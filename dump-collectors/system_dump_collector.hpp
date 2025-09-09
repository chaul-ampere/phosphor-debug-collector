#pragma once
#include <sdbusplus/bus.hpp>
#include "dump_manager_system.hpp"
#include "system_dump_entry.hpp"

namespace phosphor
{
namespace dump
{
namespace collector
{
namespace system
{

using Entry = phosphor::dump::system::Entry;
using Manager = phosphor::dump::system::Manager;

void collectDump(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus, sdbusplus::slot_t& slot);
void handleNotifiedDump(Entry* entry);

} // namespace system
} // namespace collector
} // namespace dump
} // namespace phosphor