#include "system_dump_entry.hpp"

#include "dump_utils.hpp"
#include "dump_offload.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <string>
#include <fstream>

namespace phosphor
{
namespace dump
{
namespace system
{
using namespace phosphor::logging;
using namespace sdeventplus::source;

using namespace sdbusplus::xyz::openbmc_project::Common::Error;

void Entry::initiateOffload(std::string uri)
{
    lg2::info("System dump entry {ID} offload request to URI {URI}.",
              "ID", id, "URI", uri);
    if (offloaded())
    {
        lg2::info("System dump entry {ID} was already offloaded "
                  "to URI {URI}.", "ID", id, "URI", offloadUri());
        return;
    }
    phosphor::dump::Entry::initiateOffload(uri);
    phosphor::dump::offload::requestOffload(file, id, uri);
    offloaded(true);
}

void Entry::delete_()
{
    // Offload URI will be set during dump offload
    // Prevent delete when offload is in progress
    if ((!offloadUri().empty()) && (phosphor::dump::isHostRunning()))
    {
        lg2::error("Dump offload is in progress for entry {ID}.",
                   "DUMP_ID", id);
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    lg2::info("Delete system dump entry {ID}.", "DUMP_ID", id);

    // Remove Dump entry D-bus object
    phosphor::dump::Entry::delete_();
}
} // namespace system
} // namespace dump
} // namespace phosphor