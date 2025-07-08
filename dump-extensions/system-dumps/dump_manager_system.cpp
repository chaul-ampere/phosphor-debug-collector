#include "config.h"

#include "dump_manager_system.hpp"

#include "dump_utils.hpp"
#include "system_dump_entry.hpp"
#include "xyz/openbmc_project/Common/error.hpp"
#include <xyz/openbmc_project/ObjectMapper/client.hpp>

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
using ObjectPath = std::string;
using ServiceName = std::string;
using Interfaces = std::vector<std::string>;
using MapperServiceMap = std::vector<std::pair<ServiceName, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;

constexpr auto PLDM_SERVICE = "xyz.openbmc_project.PLDM";
constexpr auto FILE_INTERFACE = "xyz.openbmc_project.Common.File";

void Manager::notify(uint32_t dumpId, uint64_t size)
{
    // Get the timestamp
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

        // If there's already a completed entry with the input source id and
        // size, ignore this notification
        // if ((sysEntry->sourceDumpId() == dumpId) && (sysEntry->size() == size))
        if (sysEntry->sourceDumpId() == dumpId)
        {
            if (sysEntry->status() ==
                phosphor::dump::OperationStatus::Completed)
            {
                lg2::info(
                    "System dump entry with source dump id:{SOURCE_ID} and "
                    "size: {SIZE} is already present with entry id:{ID}",
                    "SOURCE_ID", dumpId, "SIZE", size, "ID",
                    sysEntry->getDumpId());
                return;
            }
            else
            {
                lg2::error("A duplicate notification for an incomplete dump "
                           "dump id: {SOURCE_ID} entry id: {ID}",
                           "SOURCE_D", dumpId, "ID", sysEntry->getDumpId());
                upEntry = sysEntry;
                break;
            }
        }
        // else if (sysEntry->sourceDumpId() == dumpId)
        // {
        //     // If the dump id is the same but the size is different, then this
        //     // is a new dump. So, delete the stale entry and prepare to create a
        //     // new one.
        //     lg2::info("A previous dump entry found with same source id: "
        //               "{SOURCE_ID}, deleting it, entry id: {DUMP_ID}",
        //               "SOURCE_ID", dumpId, "DUMP_ID", sysEntry->getDumpId());
        //     sysEntry->delete_();
        //     // No 'break' here, as we need to continue checking other entries.
        // }

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
        lg2::info(
            "System Dump Notify: Updating dumpId:{ID} Source Id:{SOURCE_ID} "
            "Size:{SIZE}",
            "ID", upEntry->getDumpId(), "SOURCE_ID", dumpId, "SIZE", size);
        upEntry->update(timeStamp, size, dumpId, OperationStatus::Completed);
        return;
    }

    // Get the id
    auto id = lastEntryId + 1;
    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;
    std::filesystem::path dumpPath(dumpDir);
    dumpPath /= idString;
    

    try
    {
        lg2::info("System Dump Notify: creating new dump "
                  "entry dumpId:{ID} Source Id:{SOURCE_ID} Size:{SIZE}",
                  "ID", id, "SOURCE_ID", dumpId, "SIZE", size);
        entries.insert(std::make_pair(
            id, std::make_unique<system::Entry>(
                    bus, objPath.c_str(), id, timeStamp, size, dumpPath, dumpId,
                    phosphor::dump::OperationStatus::Completed,
                    std::string(), originatorTypes::Internal, *this)));
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error(
            "Error in creating system dump entry, errormsg: {ERROR}, "
            "OBJECTPATH: {OBJECT_PATH}, ID: {ID}, TIMESTAMP: {TIMESTAMP}, "
            "SIZE: {SIZE}, SOURCEID: {SOURCE_ID}",
            "ERROR", e, "OBJECT_PATH", objPath, "ID", id, "TIMESTAMP",
            timeStamp, "SIZE", size, "SOURCE_ID", dumpId);
        report<InternalFailure>();
        return;
    }
    lastEntryId++;
    return;
}

sdbusplus::message::object_path
    Manager::createDump(phosphor::dump::DumpCreateParams params)
{
    using ObjectMapper = sdbusplus::client::xyz::openbmc_project::ObjectMapper<>;
    lg2::error("Create system dump");
    // using ObjectMapper = sdbusplus::client::xyz::openbmc_project::ObjectMapper<>;

    if (params.empty())
    {
        lg2::warning("No additional parameters received");
    }

    // Get the originator id and type from params
    std::string originatorId;
    originatorTypes originatorType;

    phosphor::dump::extractOriginatorProperties(params, originatorId,
                                                originatorType);

    std::string oemDiagnosticDataType = "socket";
    // extractDiagnosticType(params, oemDiagnosticDataType);

    auto id = lastEntryId + 1;
    std::string arg = std::to_string(id) + "_" + oemDiagnosticDataType;

    auto method = bus.new_method_call(ObjectMapper::default_service,
                                      ObjectMapper::instance_path,
                                      ObjectMapper::interface, "GetSubTree");
    
    std::string searchPath = "/";
    int depth = 0;
    std::vector<std::string> interfaceList = {FILE_INTERFACE};
    method.append(searchPath.c_str(), depth, interfaceList);

    auto callback = [this](sdbusplus::message_t&& msg) {
        lg2::info("GetSubTree callback reached");
        if (msg.is_method_error())
        {
            lg2::error("Async DBus call failed for GetSubTree of file objects, ERROR - {ERROR}",
                       "ERROR", msg.get_error()->message);
        }
        uint32_t sourceDumpId = 0;
        GetSubTreeResponse resp;
        msg.read(resp);
        for (const auto& [objPath, serviceMap] : resp)
        {
            for (const auto& [service, interfaces] : serviceMap)
            {
                if (service == PLDM_SERVICE)
                {
                    sdbusplus::message::object_path path(objPath);
                    if (path.filename() == "ddr_adg")
                    {
                        sourceDumpId = 1;
                    }
                    // else if (path.filename() == "hsio_adg")
                    // {
                    //     sourceDumpId = 2;
                    // }
                    // else if (path.filename() == "mpro0")
                    // {
                    //     sourceDumpId = 3;
                    // }
                    // else if (path.filename() == "secpro0")
                    // {
                    //     sourceDumpId = 4;
                    // }
                    else
                    {
                        continue;
                    }
                    lg2::info("Notifying dump {SRCID}", "SRCID", sourceDumpId);
                    this->notify(sourceDumpId, 2000);
                }
            }
        }
        for (auto& entry : this->entries)
        {
            std::filesystem::path dumpPath(dumpDir);
            dumpPath /= std::to_string(entry.first);
            lg2::info("Offloading dump {ID}", "ID", entry.first);
            entry.second->initiateOffload(dumpPath);
        }
    };
    
    if (_slot)
    {
        _slot = sdbusplus::slot_t(nullptr);
    }
    try
    {
        _slot = method.call_async(callback);
        lg2::error("Called GetSubTree !!");
        if (_slot)
        {
            lg2::error("slot contains real pointer !!");
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to call GetSubTree - {EC}\n", "EC", e);
    }

    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;
    uint64_t timeStamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    std::filesystem::path dumpPath(dumpDir);
    dumpPath /= idString;

    lg2::error("Creating entry {ENTRY}\n", "ENTRY", objPath);

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
        lg2::error("Error in creating system dump entry, errormsg: {ERROR}, "
                   "OBJECTPATH: {OBJECT_PATH}, ID: {ID}",
                   "ERROR", e, "OBJECT_PATH", objPath, "ID", id);
        elog<InternalFailure>();
        return std::string();
    }

    lastEntryId++;

    return objPath.string();
}

void Manager::extractDiagnosticType(phosphor::dump::DumpCreateParams params,
                                    std::string& oemDiagType)
{
    using InvalidArgument =
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
    using Argument = xyz::openbmc_project::Common::InvalidArgument;
    std::string value;

    auto iter = params.find("OEMDiagnosticDataType");
    if (iter == params.end())
    {
        lg2::error("Required argument OEMDiagnosticDataType is missing");
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("OEMDiagnosticDataType"),
                              Argument::ARGUMENT_VALUE("MISSING"));
    }
    else
    {
        try
        {
            oemDiagType = std::get<std::string>(iter->second);
        }
        catch (const std::bad_variant_access& e)
        {
            // Exception will be raised if the input is not string
            lg2::error("An invalid Type string is passed errormsg:{ERR}", "ERR",
                       e.what());
            elog<InvalidArgument>(
                Argument::ARGUMENT_NAME("OEMDiagnosticDataType"),
                Argument::ARGUMENT_VALUE("INVALID INPUT"));
        }
    }
}

} // namespace system
} // namespace dump
} // namespace phosphor
