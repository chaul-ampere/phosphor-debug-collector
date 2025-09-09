#include "config.h"
#include "dump_manager_system.hpp"
#include "system_dump_entry.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include "system_dump_collector.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>

namespace phosphor
{
namespace dump
{
namespace system
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;

void Manager::notify(uint32_t dumpId, uint64_t size)
{
    uint64_t timeStamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    // A system dump can be created due to a fault in the server or by a user
    // request. A system dump by fault is first reported here, but for a
    // user-requested dump, an entry will be created first with an invalid
    // source id. Since only one system dump creation is allowed at a time, if
    // there's an entry with an invalid sourceId, we will update that entry.
    phosphor::dump::system::Entry* upEntry = nullptr;
    for (auto& entry : entries)
    {
        phosphor::dump::system::Entry* sysEntry =
            dynamic_cast<phosphor::dump::system::Entry*>(entry.second.get());

        if ((sysEntry->sourceDumpId() == dumpId))
        {
            if ((sysEntry->status() ==
                phosphor::dump::OperationStatus::Completed)
                || (sysEntry->status() ==
                phosphor::dump::OperationStatus::Failed))
            {
                // If the dump id is the same but status is Failed or Completed, then this
                // is a new dump. So, delete the stale entry and prepare to create a
                // new one.
                lg2::info("A previous complete dump entry {ID} found with the "
                          "same source dump ID {SOURCE_ID}, deleting.",
                          "SOURCE_ID", dumpId, "ID", sysEntry->getDumpId());
                sysEntry->delete_();
                continue;
                // No 'break' here, as we need to continue checking other entries.
            }
            else
            {
                lg2::error("A duplicate notification for an incomplete dump "
                           "entry {ID} with the same source ID {SOURCE_ID}",
                           "SOURCE_ID", dumpId, "ID", sysEntry->getDumpId());
                upEntry = sysEntry;
                break;
            }
        }

        // Save the first entry with INVALID_SOURCE_ID, but continue in the loop
        // to ensure the new entry is not a duplicate.
        if ((sysEntry->sourceDumpId() == INVALID_SOURCE_ID) &&
            (upEntry == nullptr))
        {
            upEntry = sysEntry;
        }
    }

    if (upEntry != nullptr)
    {
        lg2::info("Updating system dump entry {ID} with source dump "
                  "ID {SOURCE_ID}, size {SIZE}",
                  "ID", upEntry->getDumpId(), "SOURCE_ID", dumpId,
                  "SIZE", size);
        upEntry->update(timeStamp, size, dumpId);
        phosphor::dump::collector::system::handleNotifiedDump(upEntry);
        return;
    }

    auto id = lastEntryId + 1;
    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;
    std::filesystem::path dumpPath(dumpDir);
    dumpPath /= idString;

    try
    {
        lg2::info("Creating new system dump entry with "
                  "ID {ID}, source dump ID {SOURCE_ID}, and size {SIZE}",
                  "ID", id, "SOURCE_ID", dumpId, "SIZE", size);
        entries.insert(std::make_pair(id, std::make_unique<system::Entry>(
                    bus, objPath.c_str(), id, timeStamp, size, dumpPath, dumpId,
                    phosphor::dump::OperationStatus::Completed,
                    std::string(), originatorTypes::Internal, *this)));
        phosphor::dump::collector::system::handleNotifiedDump(dynamic_cast<phosphor::dump::system::Entry*>(entries[id].get()));
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error("Error in creating system dump entry with "
                    "ID {ID}, source dump ID {SOURCE_ID}, and size {SIZE}"
                    " - error {ERROR}", "ID", id, "SOURCE_ID", dumpId,
                    "SIZE", size, "ERROR", e);
        report<InternalFailure>();
        return;
    }
    lastEntryId++;
    return;
}

sdbusplus::message::object_path
    Manager::createDump(phosphor::dump::DumpCreateParams params)
{
    lg2::error("Create system dump");

    if (params.empty())
    {
        lg2::warning("No additional parameters received");
    }

    for (auto& entry : entries)
    {
        phosphor::dump::system::Entry* sysEntry =
            dynamic_cast<phosphor::dump::system::Entry*>(entry.second.get());
        if (sysEntry->sourceDumpId() == INVALID_SOURCE_ID)
        {
            lg2::error("There's one existing temporary system dump under-processing."
                       " Multiple system dump creations are not allowed.");
            elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
            return std::string();
        }
    }

    // Get the originator id and type from params
    std::string originatorId;
    originatorTypes originatorType;
    phosphor::dump::extractOriginatorProperties(params, originatorId,
                                                originatorType);

    auto id = lastEntryId + 1;
    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;
    uint64_t timeStamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    std::filesystem::path dumpPath(dumpDir);
    dumpPath /= idString;

    try
    {
        entries.insert(std::make_pair(
            id, std::make_unique<system::Entry>(
                    bus, objPath.c_str(), id, timeStamp, 0, dumpPath,
                    INVALID_SOURCE_ID,
                    phosphor::dump::OperationStatus::InProgress, originatorId,
                    originatorType, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error("Error in creating system dump entry, error msg: {ERROR}, "
                   "OBJECTPATH: {OBJECT_PATH}, ID: {ID}",
                   "ERROR", e, "OBJECT_PATH", objPath, "ID", id);
        elog<InternalFailure>();
        return std::string();
    }

    phosphor::dump::collector::system::collectDump(this,
                dynamic_cast<phosphor::dump::system::Entry*>(entries[id].get()), bus, _slot);

    lastEntryId++;

    return objPath.string();
}

} // namespace system
} // namespace dump
} // namespace phosphor
