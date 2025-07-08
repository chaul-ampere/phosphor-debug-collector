#include "system_dump_entry.hpp"

#include "dump_utils.hpp"
#include "host_transport_exts.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <unistd.h>     // For fork(), close(), read(), write()
#include <iostream>     // For input/output operations
#include <string>
#include <cstring> // For memset
#include <fstream>

// For memfd and mmap
#include <sys/mman.h>

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

void Entry::initiateOffload(std::string uri)
{
    lg2::info("System dump offload request id: {ID} uri: {URI} "
              "source dumpid: {SOURCE_DUMP_ID}",
              "ID", id, "URI", uri, "SOURCE_DUMP_ID", sourceDumpId());
    if (slotMap.contains(id))
    {
        lg2::error("The previous offloading has not finished yet");
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }
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

    lg2::info("About to call read");

    auto callback = [this](sdbusplus::message_t&& msg) {
        lg2::info("Read callback reached");
        if (msg.is_method_error())
        {
            lg2::error("Async DBus call failed for Read of file objects, ERROR - {ERROR}",
                       "ERROR", msg.get_error()->message);
        }
        uint64_t timeStamp =  std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        sdbusplus::message::unix_fd unixFd;
        msg.read(unixFd);
        int memfd = dup(unixFd);

        if (memfd < 0)
        {
            lg2::error("Failed to dup D-Bus unix socket {SOCKET}", "SOCKET", (unsigned)unixFd);
            update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
            close(memfd);
            return;
        }

        off_t size = lseek(memfd, 0, SEEK_END);

        if (size < 0)
        {
            lg2::error("Failed to determine file size of memfd");
            update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
            close(memfd);
            return;
        }

        // *sizeOut = size;

        lg2::info("File size: {SIZE}", "SIZE", (uint64_t)size);

        void* data = mmap(nullptr, size, PROT_READ, MAP_SHARED, memfd, 0);

        if (data == MAP_FAILED)
        {
            lg2::error("Could not mmap the memfd");
            update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
            close(memfd);
            return;
        }

        using mmapUniquePtr = std::unique_ptr<void, std::function<void(void*)>>;
        mmapUniquePtr dataUnique(data, [size](void* arg) {
            if (munmap(arg, size) != 0)
            {
                lg2::error("Failed to un map the PLDM package");
            }
        });

        std::filesystem::path dumpPath(offloadUri());

        std::ofstream outputFile;

        lg2::error("Writing {NUM} bytes out to file {FILE}\n", "NUM", (uint64_t)size, "FILE", dumpPath);
        if (!std::filesystem::exists(dumpPath.parent_path()))
        {
            std::filesystem::create_directories(dumpPath.parent_path());
        }
        outputFile.open(dumpPath);

        if (outputFile.is_open())
        {
            outputFile.write(static_cast<char*>(dataUnique.get()), (uint64_t)size);
            if (!outputFile.good())
            {
                lg2::error("Error writing to file!");
                outputFile.close();
                update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
                close(memfd);
                return;
            }
        }
        else
        {
            // Handle error, file could not be opened
            lg2::error("Error opening to file!");
            close(memfd);
            update(timeStamp, 0, sourceDumpId(), OperationStatus::Failed);
            return;
        }

        update(timeStamp, (uint64_t)size, sourceDumpId(), OperationStatus::Completed);
        offloaded(true);
        outputFile.close();
        close(memfd);
        if (this->slotMap.contains(this->id))
        {
            this->slotMap.erase(this->id);
        }
    };

    // if (_slot)
    // {
    //     _slot = sdbusplus::slot_t(nullptr);
    // }
    auto& slot = slotMap[id];
    slot = sdbusplus::slot_t(nullptr);

    try
    {
        slot = method.call_async(callback);
        lg2::error("Called Read !!");
        if (slot)
        {
            lg2::error("slot contains real pointer !!");
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to call Read - {EC}\n", "EC", e);
    }
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