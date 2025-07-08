#include "config.h"

#include "dump-extensions.hpp"

#include "dump-extensions/system-dumps/system_dumps_config.h"

#include "dump_manager_system.hpp"

namespace phosphor
{
namespace dump
{

void loadExtensions(sdbusplus::bus_t& bus, DumpManagerList& dumpList)
{
    dumpList.push_back(std::make_unique<phosphor::dump::system::Manager>(
        bus, SYSTEM_DUMP_OBJPATH, SYSTEM_DUMP_OBJ_ENTRY, SYSTEM_DUMP_PATH));
}
} // namespace dump
} // namespace phosphor