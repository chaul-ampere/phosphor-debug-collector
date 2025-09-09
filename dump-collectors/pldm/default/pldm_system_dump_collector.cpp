#include "config.h"
#include "system_dump_collector.hpp"
#include "dump_manager_system.hpp"
#include "system_dump_entry.hpp"
#include "common.hpp"

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

void handleCollectedDump(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus, sdbusplus::slot_t& slot, sdbusplus::message_t&& msg)
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
    bool collected = false;
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

                auto sourceDumpId = hashStringToUInt32(objPath);

                std::filesystem::path tmpPath(TMP_PATH);
                tmpPath /= std::to_string(sourceDumpId);
                lg2::info("Downloading dump data from {PATH} to {FILE}.",
                          "PATH", objPath, "FILE", tmpPath.string());
                auto size = downloadDumpData(objPath, tmpPath.string(), bus);
                if (!size)
                {
                    lg2::error("Failed to download dump data from file object {PATH}.", "PATH", objPath);
                    continue;
                }
                manager->notify(sourceDumpId, size);
                collected = true;
            }
        }
    }

    if (!collected)
    {
        tmpEntry->delete_();
    }
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
