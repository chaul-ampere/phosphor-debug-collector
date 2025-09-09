#include "config.h"
#include "system_dump_collector.hpp"
#include "dump_manager_system.hpp"
#include "system_dump_entry.hpp"
#include "common.hpp"

#include "minizip/zip.h"
#include <algorithm>
#include <cctype>

namespace phosphor
{
namespace dump
{
namespace collector
{
namespace system
{
constexpr auto TMP_DATA_PATH = "/tmp/system-dumps/dump-data";
constexpr auto DEFAULT_SOURCE_DUMP_ID = 1000;

void addFileToZip(zipFile zf, const std::filesystem::path& filePath, const std::filesystem::path& basePath) {
    // Open file and read contents
    FILE* file = fopen(filePath.string().c_str(), "rb");
    if (!file) {
        lg2::error("Failed to open file {FILE}!", "FILE", filePath);
        return;
    }
    // Compute relative path inside zip
    std::filesystem::path relativePath = std::filesystem::relative(filePath, basePath);
    zip_fileinfo zi{};
    zipOpenNewFileInZip(zf, relativePath.string().c_str(), &zi,
                        nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION);
    char buffer[4096];
    int bytesRead = 0;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        zipWriteInFileInZip(zf, buffer, bytesRead);
    }
    zipCloseFileInZip(zf);
    fclose(file);
}

size_t zipDumpData(std::string source, std::string dest)
{
    std::filesystem::path sourcePath(source);
    if (!std::filesystem::exists(sourcePath))
    {
        lg2::error("Collecting: Temporary dump file does not exist {FILE}", "FILE", source);
        return 0;
    }

    std::filesystem::path destPath(dest);
    zipFile zf = zipOpen(destPath.string().c_str(), APPEND_STATUS_CREATE);
    if (!zf) {
        lg2::error("Could not create zip file {FILE}.", "FILE", dest);
        return 0;
    }
    for (auto& entry : std::filesystem::recursive_directory_iterator(sourcePath)) {
        if (std::filesystem::is_regular_file(entry.path())) {
            addFileToZip(zf, entry.path(), source);
        }
    }
    zipClose(zf, nullptr);
    return std::filesystem::file_size(destPath);
}

void handleCollectedDump(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus,
                        sdbusplus::slot_t& slot, sdbusplus::message_t&& msg)
{
    slot = sdbusplus::slot_t(nullptr);

    if (msg.is_method_error())
    {
        lg2::error("Async DBus call failed for GetSubTree of file objects - error {ERROR}",
                    "ERROR", msg.get_error()->message);
        tmpEntry->delete_();
    }

    GetSubTreeResponse resp;
    msg.read(resp);
    for (const auto& [objPath, serviceMap] : resp)
    {
        for (const auto& [service, interfaces] : serviceMap)
        {
            if (service == PLDM_SERVICE)
            {
                auto dumpSource = collectDumpSource(objPath, bus);
                if (!dumpSource ||
                    (dumpSource.value() != FileInf::SourceType::ComputerSystem))
                {
                    continue;
                }
                std::string terminusName = std::filesystem::path(objPath).parent_path().parent_path().filename();
                std::transform(terminusName.begin(), terminusName.end(), terminusName.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                auto name = collectFileName(objPath, bus);
                std::filesystem::path tmpPath(TMP_DATA_PATH);
                tmpPath /= terminusName;
                tmpPath /= name;
                lg2::info("Downloading dump data from {PATH} to {FILE}.",
                          "PATH", objPath, "FILE", tmpPath.string());
                auto size = downloadDumpData(objPath, tmpPath.string(), bus);
                if (!size)
                {
                    lg2::error("Failed to download dump data from file object {PATH}.", "PATH", objPath);
                    continue;
                }
            }
        }
    }

    uint32_t sourceDumpId = DEFAULT_SOURCE_DUMP_ID;
    std::string downloadPath(TMP_PATH);
    downloadPath += "/" + std::to_string(sourceDumpId);
    auto dumpSize = zipDumpData(TMP_DATA_PATH, downloadPath);
    std::filesystem::remove_all(std::filesystem::path(TMP_DATA_PATH));
    if (!dumpSize)
    {
        tmpEntry->delete_();
        return;
    }
    manager->notify(sourceDumpId, dumpSize);
}

void collectDump(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus, sdbusplus::slot_t& slot)
{
    if (!collectFileObject(manager, tmpEntry, bus, slot, handleCollectedDump))
    {
        tmpEntry->delete_();
    }
}

} // namespace system
} // namespace collector
} // namespace dump
} // namespace phosphor
