#include "config.h"
#include "common.hpp"
#include <xyz/openbmc_project/ObjectMapper/client.hpp>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>

namespace phosphor
{
namespace dump
{
namespace collector
{
namespace system
{

bool collectFileObject(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus, sdbusplus::slot_t& slot,
                        std::function<
                        void(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus,
                            sdbusplus::slot_t& slot, sdbusplus::message_t&& msg)> callback)
{
    auto method = bus.new_method_call(ObjectMapper::default_service,
                                      ObjectMapper::instance_path,
                                      ObjectMapper::interface, "GetSubTree");
    
    std::string searchPath = "/";
    int depth = 0;
    std::vector<std::string> interfaceList = {FILE_INTERFACE};
    method.append(searchPath.c_str(), depth, interfaceList);

    try
    {
        slot = method.call_async(std::bind_front(callback, manager, tmpEntry, std::ref(bus), std::ref(slot)));
        if (!slot)
        {
            return false;
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to collect system dump files - error {ERROR}", "ERROR", e);
        return false;
    }
    return true;
}

void handleNotifiedDump(Entry* entry)
{
    std::filesystem::path dumpPath(entry->getDumpFilePath());
    std::filesystem::path tmpPath(TMP_PATH);
    tmpPath /= std::to_string(entry->sourceDumpId());
    try
    {
        if (!std::filesystem::exists(tmpPath))
        {
            lg2::error("Temporary dump file does not exist {FILE}", "FILE", tmpPath.string());
            entry->status(OperationStatus::Failed);
            return;
        }
        if (!std::filesystem::exists(dumpPath.parent_path()))
        {
            std::filesystem::create_directories(dumpPath.parent_path());
        }
        std::filesystem::copy_file(tmpPath, dumpPath, std::filesystem::copy_options::overwrite_existing);
    }
    catch (std::filesystem::filesystem_error& e)
    {
        lg2::info("FileSystem error: {ERROR}", "ERROR", e.what());
        entry->status(OperationStatus::Failed);
    }
    try
    {
        if (std::filesystem::exists(TMP_PATH))
        {
            std::filesystem::remove_all(TMP_PATH);
        }
    }
    catch (std::filesystem::filesystem_error& e)
    {
        lg2::error("Failed to remove temporary dump directory {PATH} - error {ERROR}",
                    "PATH", TMP_PATH, "ERROR", e.what());
    }
}

template <typename T>
T getDumpProperty(const std::string& objPath, const std::string& property, sdbusplus::bus_t& bus)
{
    auto propVal = readDBusProperty<std::variant<T>>(bus, PLDM_SERVICE, objPath.c_str(),
                                                    FILE_INTERFACE, property);
    return std::get<T>(propVal);
}

std::optional<FileInf::SourceType> collectDumpSource(const std::string& path, sdbusplus::bus_t& bus)
{
    auto sourceStr = getDumpProperty<std::string>(path, "Source", bus);
    return FileInf::convertStringToSourceType(sourceStr);
}

std::string collectFileName(const std::string& path, sdbusplus::bus_t& bus)
{
    return getDumpProperty<std::string>(path, "Name", bus);
}

size_t downloadDumpData(const std::string& objPath, const std::string& downloadPath, sdbusplus::bus_t& bus)
{
    auto method = bus.new_method_call(PLDM_SERVICE, objPath.c_str(),
                                      FILE_INTERFACE, READ_METHOD);
    uint32_t offset = 0;
    uint32_t length = 0;
    bool exclusivity = false;
    method.append(offset, length, exclusivity);
    auto reply = bus.call(method);
    sdbusplus::message::unix_fd unixFd;
    reply.read(unixFd);
    auto fd = dup(unixFd);
    if (fd == -1)
    {
        lg2::error("Failed to dup D-Bus unix fd {FD}", "FD", (unsigned)(unixFd));
        return 0;
    }

    fd_set read_fds;
    struct timeval timeout;
    std::vector<char> readBuffer;
    while (true)
    {
        std::array<char, 256> buffer;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        timeout.tv_sec = 0;
        timeout.tv_usec = 2000;
        int retval = select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (retval < 0)
        {
            close(fd);
            lg2::error(
                "Failed to select unix socket, error number - {ERROR_NO}",
                "ERROR_NO", errno);
            return 0;
        }
        if (retval == 0)
        {
            continue;
        }
        if ((retval > 0) && (FD_ISSET(fd, &read_fds)))
        {
            int bytes = read(fd, buffer.data(), buffer.size());
            if (bytes < 0)
            {
                lg2::error("Failed to read from unix socket, error number - {ERROR_NO}",
                            "ERROR_NO", errno);
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    lg2::error("No data available, but the server socket is still open. Try reading again");
                    continue;
                }
                close(fd);
                lg2::error(
                    "Failed to read from unix socket, error number - {ERROR_NO}",
                    "ERROR_NO", errno);
                return 0;
            }

            readBuffer.insert(readBuffer.end(), buffer.begin(), buffer.begin() + bytes);
            if (bytes == 0)
            {
                lg2::info("The other end closed. Reading is done.");
                lg2::info("Total bytes read - {BYTES}", "BYTES", readBuffer.size());
                break;
            }
        }
        else
        {
            lg2::error("Read FD is not set. Try reading again");
            continue;
        }
    }
    close(fd);
    
    lg2::info("Writing {NUM} bytes out to file {FILE}", "NUM", readBuffer.size(), "FILE", downloadPath);
    if (!saveDumpData(downloadPath, &readBuffer))
    {
        return 0;
    }
    return readBuffer.size();
}

bool saveDumpData(std::string downloadPath, const std::vector<char>* readBuffer)
{
    std::filesystem::path outputPath(downloadPath);
    if (!std::filesystem::exists(outputPath.parent_path()))
    {
        std::filesystem::create_directories(outputPath.parent_path());
    }

    std::ofstream outputFile;
    outputFile.open(outputPath);
    if (outputFile.is_open())
    {
        outputFile.write(readBuffer->data(), readBuffer->size());
        if (!outputFile.good())
        {
            lg2::error("Error writing to file {FILE}.", "FILE", outputPath.string());
            outputFile.close();
            return false;
        }
    }
    else
    {
        lg2::error("Error opening to file {FILE}.", "FILE", outputPath.string());
        return false;
    }
    outputFile.close();
    return true;
}

uint32_t hashStringToUInt32(const std::string& str) {
    std::hash<std::string> hasher;
    std::size_t hash_value = hasher(str);
    return static_cast<uint32_t>(hash_value);
}

} // namespace system
} // namespace collector
} // namespace dump
} // namespace phosphor
