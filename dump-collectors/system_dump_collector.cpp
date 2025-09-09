#include "system_dump_collector.hpp"

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

void collectDump([[maybe_unused]] Manager* manager, [[maybe_unused]] Entry* tmpEntry, [[maybe_unused]] sdbusplus::bus_t& bus, [[maybe_unused]] sdbusplus::slot_t& slot)
{
   lg2::info("Default handler for system dump collecting is not supported.");
}

void handleNotifiedDump([[maybe_unused]] Entry* entry)
{
    lg2::info("Default handler for notified system dump is not supported.");
}
} // namespace system
} // namespace collector
} // namespace dump
} // namespace phosphor
