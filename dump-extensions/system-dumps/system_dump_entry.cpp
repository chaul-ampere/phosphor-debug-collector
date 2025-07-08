#include "system_dump_entry.hpp"

#include "dump_utils.hpp"
#include "host_transport_exts.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <sys/socket.h> // For socketpair()
#include <unistd.h>     // For fork(), close(), read(), write()
#include <iostream>     // For input/output operations
#include <string>
#include <cstring> // For memset
#include <fstream>

namespace phosphor
{
namespace dump
{
namespace system
{
constexpr auto TRANSPORT_DUMP_TYPE_IDENTIFIER = 3;
using namespace phosphor::logging;
using namespace sdeventplus::source;

using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using ObjectPath = std::string;
using ServiceName = std::string;
using Interfaces = std::vector<std::string>;
using MapperServiceMap = std::vector<std::pair<ServiceName, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;


constexpr auto PLDM_SERVICE = "xyz.openbmc_project.PLDM";
constexpr auto FILE_INTERFACE = "xyz.openbmc_project.Common.File";
constexpr auto READ_METHOD = "Read";

void Entry::socketPollCallback([[maybe_unused]]sdeventplus::source::IO& es, int fd,
                            uint32_t revents)
{
    
    // Get the timestamp
    uint64_t timeStamp =  std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    lg2::info("pollCallback for {ENTRY}", "ENTRY", id);
    if (revents & EPOLLERR)
    {
        lg2::info("Poll error or hang-up of the fd");
        esource.reset();
        close(fd);
        update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
        return;
    }

    if (!(revents & EPOLLIN))
    {
        lg2::info("Event not EPOLLIN");
        return;
    }

    if (revents & (EPOLLRDHUP | EPOLLHUP))
    {
        lg2::info("The remote peer has closed.");
    }
    
    if (fd < 0)
    {
        lg2::error("fd not valid");
        esource.reset();
        close(fd);
        update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
        return;
    }

    std::array<char, 256> buffer;

    lg2::info("Reading from socket");
    int bytes = read(fd, buffer.data(), buffer.size());
    if (bytes < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            lg2::error("No data available, but the server socket is still open. Try reading again");
            return;
        }
        esource.reset();
        close(fd);
        lg2::error(
            "Failed to read from unix socket, error number - {ERROR_NO}",
            "ERROR_NO", errno);
        update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
        return;
    }
    
    if (bytes >= 0)
    {
        lg2::error("Writing {NUM} bytes to buffer\n", "NUM", bytes);
        readBuffer.insert(readBuffer.end(), buffer.begin(), buffer.begin() + bytes);
        if (bytes > 0)
        {
            return;
        }
    }
    
    lg2::info("The other end closed. Reading is done.");
    lg2::info("Total bytes read - {BYTES}", "BYTES", readBuffer.size());

    std::filesystem::path dumpPath(offloadUri());

    std::ofstream outputFile;

    lg2::error("Writing {NUM} bytes out to file {FILE}\n", "NUM", readBuffer.size(), "FILE", dumpPath);
    if (!std::filesystem::exists(dumpPath.parent_path()))
    {
        std::filesystem::create_directories(dumpPath.parent_path());
    }
    outputFile.open(dumpPath);

    if (outputFile.is_open())
    {
        outputFile.write(readBuffer.data(), readBuffer.size());
        if (!outputFile.good())
        {
            lg2::error("Error writing to file!");
            outputFile.close();
            esource.reset();
            close(fd);
            update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
            return;
        }
    }
    else
    {
        // Handle error, file could not be opened
        lg2::error("Error opening to file!");
        esource.reset();
        close(fd);
        update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
        return;
    }

    update(timeStamp, readBuffer.size(), sourceDumpId(), OperationStatus::Completed);
    offloaded(true);
    outputFile.close();
    esource.reset();
    close(fd);

}

void Entry::initiateOffload(std::string uri)
{
    // using ErrorOpen = xyz::openbmc_project::Common::File::Open;
    // using ErrorWrite = xyz::openbmc_project::Common::File::Write;

    if (esource)
    {
        lg2::error("The previous offloading has not finished yet");
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    lg2::info("System dump offload request id: {ID} uri: {URI} "
              "source dumpid: {SOURCE_DUMP_ID}",
              "ID", id, "URI", uri, "SOURCE_DUMP_ID", sourceDumpId());
    phosphor::dump::Entry::initiateOffload(uri);
    std::string objPath = "/xyz/openbmc_project/file/S0/diagnostic/";
    switch (sourceDumpId())
    {
        case 1:
            objPath += "ddr_adg";
            break;
        case 2:
            objPath += "hsio_adg";
            break;
        case 3:
            objPath += "mpro0";
            break;
        case 4:
            objPath += "secpro0";
            break;
        default:
            break;
    }

    auto bus = sdbusplus::bus::new_default();
    auto method = bus.new_method_call(PLDM_SERVICE, objPath.c_str(),
                                      FILE_INTERFACE, READ_METHOD);
    uint32_t offset = 0;
    uint32_t length = 2000;
    bool exclusivity = false;
    method.append(offset, length, exclusivity);
    auto reply = bus.call(method);
    sdbusplus::message::unix_fd unixFd;
    reply.read(unixFd);

    int fd = dup(unixFd);
    if (fd == -1)
    {
        lg2::error("Failed to dup D-Bus unix socket {SOCKET}", "SOCKET", (unsigned)unixFd);
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    auto event = bus.get_event();
    readBuffer.clear();

    esource = std::make_unique<sdeventplus::source::IO>(event, fd, EPOLLIN | EPOLLRDHUP,
                        std::bind_front(&Entry::socketPollCallback, this));
    if (!esource)
    {
        lg2::error("Failed to add sd event");
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    // fd_set read_fds;
    // struct timeval timeout;
    // std::vector<char> readBuffer;

    // while (true)
    // {
    //     std::array<char, 256> buffer;

    //     FD_ZERO(&read_fds);
    //     FD_SET(fd, &read_fds);

    //     timeout.tv_sec = 0.2;
    //     timeout.tv_usec = 0;
    //     int retval = select(fd + 1, &read_fds, nullptr, nullptr, &timeout);


    //     if (retval < 0)
    //     {
    //         close(fd);
    //         lg2::error(
    //             "Failed to select unix socket, error number - {ERROR_NO}",
    //             "ERROR_NO", errno);
    //         elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    //     }
    //     if (retval == 0)
    //     {
    //         // lg2::error("select() timed-out. - {ERROR_NO}", "ERROR_NO", errno);
    //         continue;
    //     }
    //     if ((retval > 0) && (FD_ISSET(fd, &read_fds)))
    //     {
    //         lg2::info("Reading from socket");
    //         int bytes = read(fd, buffer.data(), buffer.size());
    //         if (bytes < 0)
    //         {
    //             lg2::error("Failed to read from unix socket, error number - {ERROR_NO}",
    //                         "ERROR_NO", errno);
    //             if (errno == EAGAIN || errno == EWOULDBLOCK)
    //             {
    //                 lg2::error("No data available, but the server socket is still open. Try reading again");
    //                 continue;
    //             }
    //             close(fd);
    //             lg2::error(
    //                 "Failed to read from unix socket, error number - {ERROR_NO}",
    //                 "ERROR_NO", errno);
    //             elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    //         }

    //         lg2::error("Writing {NUM} bytes to buffer\n", "NUM", bytes);
    //         readBuffer.insert(readBuffer.end(), buffer.begin(), buffer.begin() + bytes);

    //         if (bytes == 0)
    //         {
    //             lg2::info("The other end closed. Reading is done.");
    //             lg2::info("Total bytes read - {BYTES}", "BYTES", readBuffer.size());
    //             break;
    //         }
    //     }
    //     else
    //     {
    //         lg2::error("Read FD is not set. Try reading again");
    //         retries++;
    //         continue;
    //     }
    // }

    // std::filesystem::path dumpPath(uri);

    // std::ofstream outputFile;

    // lg2::error("Writing {NUM} bytes out to file {FILE}\n", "NUM", readBuffer.size(), "FILE", dumpPath);
    // if (!std::filesystem::exists(dumpPath.parent_path()))
    // {
    //     std::filesystem::create_directories(dumpPath.parent_path());
    // }
    // outputFile.open(dumpPath);

    // if (outputFile.is_open())
    // {
    //     outputFile.write(readBuffer.data(), readBuffer.size());
    //     if (!outputFile.good())
    //     {
    //         lg2::error("Error writing to file!");
    //         outputFile.close();
    //         elog<sdbusplus::xyz::openbmc_project::Common::File::Error::Write>(ErrorWrite::ERRNO(errno), ErrorWrite::PATH(dumpPath.c_str()));
    //     }
    // }
    // else
    // {
    //     close(fd);
    //     outputFile.close();
    //     // Handle error, file could not be opened
    //     lg2::error("Error opening to file!");
    //     elog<sdbusplus::xyz::openbmc_project::Common::File::Error::Open>(ErrorOpen::ERRNO(errno), ErrorOpen::PATH(dumpPath.c_str()));
    // }

    // close(fd);
    // outputFile.close();
    // phosphor::dump::host::requestOffload(sourceDumpId());
}

void Entry::delete_()
{
    auto srcDumpID = sourceDumpId();
    auto dumpId = id;

    // Offload URI will be set during dump offload
    // Prevent delete when offload is in progress
    if ((!offloadUri().empty()) && (phosphor::dump::isHostRunning()))
    {
        lg2::error("Dump offload is in progress id: {DUMP_ID} "
                   "srcdumpid: {SRC_DUMP_ID}",
                   "DUMP_ID", dumpId, "SRC_DUMP_ID", srcDumpID);
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    lg2::info("System dump delete id: {DUMP_ID} srcdumpid: {SRC_DUMP_ID}",
              "DUMP_ID", dumpId, "SRC_DUMP_ID", srcDumpID);

    // Remove host system dump when host is up by using source dump id
    // which is present in system dump entry dbus object as a property.
    if ((phosphor::dump::isHostRunning()) && (srcDumpID != INVALID_SOURCE_ID))
    {
        try
        {
            phosphor::dump::host::requestDelete(srcDumpID,
                                                TRANSPORT_DUMP_TYPE_IDENTIFIER);
        }
        catch (const std::exception& e)
        {
            lg2::error("Error deleting dump from host id: {DUMP_ID} "
                       "host id: {SRC_DUMP_ID} error: {ERROR}",
                       "DUMP_ID", dumpId, "SRC_DUMP_ID", srcDumpID, "ERROR", e);
            elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
        }
    }

    // Remove Dump entry D-bus object
    phosphor::dump::Entry::delete_();
}
} // namespace system
} // namespace dump
} // namespace phosphor