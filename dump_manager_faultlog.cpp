#include "config.h"

#include "dump_manager_faultlog.hpp"

#include "dump_utils.hpp"
#include "faultlog_dump_entry.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/File/error.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace phosphor
{
namespace dump
{
namespace faultlog
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using namespace sdbusplus::xyz::openbmc_project::Common::File::Error;
using ErrnoOpen = xyz::openbmc_project::Common::File::Open::ERRNO;
using PathOpen = xyz::openbmc_project::Common::File::Open::PATH;

using InterfaceVariant = typename sdbusplus::utility::dedup_variant_t<
    bool, uint8_t, uint16_t, int16_t, uint32_t, int32_t, uint64_t, int64_t,
    size_t, ssize_t, double, std::string, sdbusplus::message::object_path>;

using ChangedPropertiesType =
    std::vector<std::pair<std::string, InterfaceVariant>>;

using ChangedInterfacesType =
    std::vector<std::pair<std::string, ChangedPropertiesType>>;

sdbusplus::message::object_path Manager::createDump(
    phosphor::dump::DumpCreateParams params)
{
    lg2::info("In dump_manager_fault.cpp createDump");

    // Currently we ignore the parameters.
    // TODO phosphor-debug-collector/issues/22: Check parameter values and
    // exit early if we don't receive the expected parameters
    if (params.empty())
    {
        lg2::info("No additional parameters received");
    }
    else
    {
        lg2::info("Got additional parameters");
    }

    FaultDataType entryType = FaultDataType::Crashdump;
    std::string primaryLogIdStr;
    std::string additionalTypeStr;

    if (MAX_TOTAL_CRASHDUMP_ENTRIES != (MAX_TOTAL_BERT_ENTRIES +
                                        MAX_TOTAL_DIAGNOSTIC_ENTRIES))
    {
        lg2::error("Incorrect total of BERT and Diagnostic "
                   "entries with total CrashDump entries\n");
        elog<InternalFailure>();
    }
    getAndCheckCreateDumpParams(params, entryType, primaryLogIdStr,
                                additionalTypeStr);
    checkThresholdFaultLog(entryType, additionalTypeStr);

    // Get the originator id and type from params
    std::string originatorId;
    originatorTypes originatorType;

    phosphor::dump::extractOriginatorProperties(params, originatorId,
                                                originatorType);

    // Get the id
    auto id = lastEntryId + 1;
    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;

    lg2::info(
        "next entry id: {ID}, entries.size(): {SIZE}",
        "ID", id, "SIZE", entries.size());

    std::filesystem::path faultLogFilePath = primaryLogIdStr;
    uint64_t fileSize = 0;
    if (std::filesystem::exists(faultLogFilePath))
    {
        fileSize = std::filesystem::file_size(faultLogFilePath);
    }
    lg2::info("file_size: {SIZE}", "SIZE", fileSize);
    std::filesystem::path filePath = dumpDir + idString;
    if (!std::filesystem::exists(filePath))
    {
        std::filesystem::create_directory(filePath);
    }
    filePath /= FAULTLOG_FILE;
    try
    {
        lg2::info("dump_manager_faultlog.cpp: add faultlog entry");
        auto e = std::make_unique<faultlog::Entry>(
                      bus, objPath.c_str(), id, generateTimestamp(),
                      fileSize, filePath,
                      phosphor::dump::OperationStatus::Completed,
                      originatorId, originatorType, entryType,
                      primaryLogIdStr, additionalTypeStr, *this, &entries);
        e->serializeEntry();
        entries.insert(std::make_pair(id, std::move(e)));
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error("Error in creating dump entry, errormsg: {ERROR}, "
                   "OBJECTPATH: {OBJECT_PATH}, ID: {ID}",
                   "ERROR", e, "OBJECT_PATH", objPath, "ID", id);
        elog<InternalFailure>();
    }

    lastEntryId++;

    lg2::info("End of dump_manager_faultlog.cpp createDump");
    return objPath.string();
}

void Manager::deleteAll()
{
    lg2::info("In dump_manager_faultlog.hpp deleteAll");

    lastEntryId = 0;
    faultLogSize = 0;
    cperLogSize = 0;
    crashdumpSize = 0;
    bertSize = 0;
    diagnosticSize = 0;

    removeAllDataEntry();

    // Delete the persistent representation of all FaultLog entries.
    for ( auto it = entries.begin(); it != entries.end(); ++it  )
    {
        fs::path faultlogPath(FAULTLOG_DUMP_PATH);
        uint32_t id = it->first;
        faultlogPath /= std::to_string(id);
        fs::remove_all(faultlogPath);
    }

    phosphor::dump::Manager::deleteAll();

    auto iter = savedCperLogEntries.begin();
    while (iter != savedCperLogEntries.end())
    {
        auto& entry = iter->second;
        ++iter;
        entry->delete_();
    }

    iter = savedCrashdumpEntries.begin();
    while (iter != savedCrashdumpEntries.end())
    {
        auto& entry = iter->second;
        ++iter;
        entry->delete_();
    }
}

void Manager::registerFaultLogMatches()
{
    lg2::info("dump_manager_faultlog registerFaultLogMatches");

    registerCrashdumpMatch();
    registerCperLogMatch();
}

void Manager::registerCrashdumpMatch()
{
    crashdumpMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        "type='signal',interface='org.freedesktop.DBus.Properties',member='"
        "PropertiesChanged',path_namespace='/com/intel/crashdump'",

        [this](sdbusplus::message_t& msg) {
            if (msg.is_method_error())
            {
                lg2::error("dump_manager_faultlog got crashdump error!");
                return;
            }

            lg2::info("Got new crashdump notification!");

            std::string interface;
            std::string objpath;
            objpath = msg.get_path();

            ChangedPropertiesType changedProps;
            msg.read(interface, changedProps);

            if (interface == "com.intel.crashdump")
            {
                lg2::info("interface is com.intel.crashdump");

                for (const auto& [changedProp, newValue] : changedProps)
                {
                    if (changedProp == "Log")
                    {
                        const auto* val = std::get_if<std::string>(&newValue);
                        if (val == nullptr)
                        {
                            lg2::error("Couldn't get Log property");
                            return;
                        }

                        lg2::info("Log: {VAL}", "VAL", *val);

                        std::map<std::string,
                                 std::variant<std::string, uint64_t>>
                            crashdumpMap;

                        crashdumpMap.insert(std::pair<std::string, std::string>(
                            "Type", "Crashdump"));

                        crashdumpMap.insert(std::pair<std::string, std::string>(
                            "PrimaryLogId",
                            std::filesystem::path(objpath).filename()));

                        createDump(crashdumpMap);
                    }
                }
            }
        });
}

void Manager::registerCperLogMatch()
{
    cperLogMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus,
        "type='signal',path_namespace='/xyz/openbmc_project/external_storer/"
        "bios_bmc_smm_error_logger/CPER',"
        "interface='org.freedesktop.DBus.ObjectManager',member='"
        "InterfacesAdded'",

        [this](sdbusplus::message_t& msg) {
            if (msg.is_method_error())
            {
                lg2::info(
                    "dump_manager_faultlog got cperLogMatch error!");
            }

            lg2::info("Got new CPER Log notification!");

            sdbusplus::message::object_path newObjPath;

            ChangedPropertiesType changedProps;
            ChangedInterfacesType changedInterfaces;
            msg.read(newObjPath, changedInterfaces);

            lg2::info(
                "newObjPath: {PATH}", "PATH", newObjPath.str);

            for (const auto& [changedInterface, changedProps] :
                 changedInterfaces)
            {
                if (changedInterface == "xyz.openbmc_project.Common.FilePath")
                {
                    lg2::info("changedInterface is "
                              "xyz.openbmc_project.Common.FilePath");

                    for (const auto& [changedProp, newValue] : changedProps)
                    {
                        if (changedProp == "Path")
                        {
                            const auto* val =
                                std::get_if<std::string>(&newValue);

                            if (val == nullptr)
                            {
                                lg2::error("Couldn't get Path property");
                                return;
                            }

                            lg2::info("Path: {PATH}", "PATH", *val);

                            std::string cperLogPath(CPER_LOG_PATH);
                            bool badPath = false;

                            // Check path length
                            if ((*val).size() <
                                cperLogPath.size() + CPER_LOG_ID_STRING_LEN)
                            {
                                badPath = true;
                                lg2::error("CPER_LOG_ID_STRING_LEN: {LEN}",
                                            "LEN", CPER_LOG_ID_STRING_LEN);
                            }
                            // Check path prefix
                            else if ((*val).compare(0, cperLogPath.size(),
                                                    cperLogPath) != 0)
                            {
                                badPath = true;
                            }

                            if (badPath)
                            {
                                lg2::error("Unexpected CPER log path: {VAL}",
                                           "VAL", *val);
                            }
                            else
                            {
                                std::string cperId = val->substr(
                                    cperLogPath.size(), CPER_LOG_ID_STRING_LEN);
                                std::map<std::string,
                                         std::variant<std::string, uint64_t>>
                                    cperLogMap;
                                cperLogMap.insert(
                                    std::pair<std::string, std::string>(
                                        "Type", "CPER"));
                                cperLogMap.insert(
                                    std::pair<std::string, std::string>(
                                        "PrimaryLogId", cperId));
                                createDump(cperLogMap);
                            }
                        }
                    }
                }
            }
        });
}

void Manager::getAndCheckCreateDumpParams(
    const phosphor::dump::DumpCreateParams& params, FaultDataType& entryType,
    std::string& primaryLogIdStr, std::string& additionalTypeName)
{
    using InvalidArgument =
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
    using Argument = xyz::openbmc_project::Common::InvalidArgument;
    std::string value;

    auto iter = params.find("Type");
    if (iter == params.end())
    {
        lg2::error("Required argument Type is missing");
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("TYPE"),
                              Argument::ARGUMENT_VALUE("MISSING"));
    }
    else
    {
        try
        {
            value = std::get<std::string>(iter->second);
        }
        catch (const std::bad_variant_access& e)
        {
            // Exception will be raised if the input is not string
            lg2::error("An invalid Type string is passed errormsg({TYPE})",
                       "TYPE", e.what());
            elog<InvalidArgument>(Argument::ARGUMENT_NAME("TYPE"),
                                  Argument::ARGUMENT_VALUE("INVALID INPUT"));
        }

        if (value == "Crashdump")
        {
            entryType = FaultDataType::Crashdump;
        }
        else if (value == "CPER")
        {
            entryType = FaultDataType::CPER;
        }
        else if (value == "BERT")
        {
            entryType = FaultDataType::Crashdump;
            additionalTypeName = "BERT";
        }
        else if (value == "Diagnostic")
        {
            entryType = FaultDataType::Crashdump;
            additionalTypeName = "Diagnostic";
        }
        else
        {
            lg2::error("Unexpected entry type, not handled");
            elog<InvalidArgument>(Argument::ARGUMENT_NAME("TYPE"),
                                  Argument::ARGUMENT_VALUE("UNEXPECTED TYPE"));
        }
    }

    iter = params.find("PrimaryLogId");
    if (iter == params.end())
    {
        lg2::error("Required argument PrimaryLogId is missing");
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("PRIMARYLOGID"),
                              Argument::ARGUMENT_VALUE("MISSING"));
    }
    else
    {
        try
        {
            value = std::get<std::string>(iter->second);
        }
        catch (const std::bad_variant_access& e)
        {
            // Exception will be raised if the input is not string
            lg2::error(
                "An invalid PrimaryLogId string is passed errormsg({TYPE})",
                "TYPE", e.what());
            elog<InvalidArgument>(Argument::ARGUMENT_NAME("PRIMARYLOGID"),
                                  Argument::ARGUMENT_VALUE("INVALID INPUT"));
        }

        if (value.empty())
        {
            lg2::error("Got empty PrimaryLogId string");
            elog<InvalidArgument>(Argument::ARGUMENT_NAME("PRIMARYLOGID"),
                                  Argument::ARGUMENT_VALUE("EMPTY STRING"));
        }

        primaryLogIdStr = value;
    }
}

uint64_t Manager::generateTimestamp()
{
    uint64_t timestamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    if (!entries.empty())
    {
        auto latestEntry = entries.crbegin();
        auto latestEntryPtr = (latestEntry->second).get();
        uint64_t latestEntryTimestamp =
            dynamic_cast<faultlog::Entry*>(latestEntryPtr)->startTime();
        if (latestEntryTimestamp >= timestamp)
        {
            // Ensure unique and increasing timestamps
            timestamp = latestEntryTimestamp + 1;
        }
    }

    return timestamp;
}

void Manager::saveEarliestEntry()
{
    auto earliestEntry = entries.begin();
    uint32_t earliestEntryId = earliestEntry->first;
    auto earliestEntryPtr = (earliestEntry->second).get();
    FaultDataType earliestEntryType =
        dynamic_cast<faultlog::Entry*>(earliestEntryPtr)->type();

    size_t maxNumSavedEntries = 0;
    std::map<uint32_t, std::unique_ptr<phosphor::dump::Entry>>* savedEntries;
    bool validSavedEntryType = true;

    switch (earliestEntryType)
    {
        case FaultDataType::CPER:
            maxNumSavedEntries = MAX_NUM_SAVED_CPER_LOG_ENTRIES;
            savedEntries = &savedCperLogEntries;
            break;
        case FaultDataType::Crashdump:
            maxNumSavedEntries = MAX_NUM_SAVED_CRASHDUMP_ENTRIES;
            savedEntries = &savedCrashdumpEntries;
            break;
        default:
            validSavedEntryType = false;
    }

    if (validSavedEntryType)
    {
        lg2::info(
            "dump_manager_faultlog.cpp: in saveEarliestEntry(). "
            "entry id: {ID}, type: {TYPE}, savedEntries->size(): {SIZE}",
            "ID", earliestEntryId,
            "TYPE", static_cast<uint32_t>(earliestEntryType),
            "SIZE", savedEntries->size());

        // Check whether saved entries map has space for a new entry
        if (savedEntries->size() < maxNumSavedEntries)
        {
            dynamic_cast<phosphor::dump::faultlog::Entry*>(
                earliestEntry->second.get())
                ->parentMap = savedEntries;

            // Insert earliest entry into saved entries map
            savedEntries->insert(std::make_pair(
                earliestEntryId, std::move(earliestEntry->second)));
        }
        else
        {
            // Delete earliest entry from fault log entries map
            (entries.at(earliestEntryId))->delete_();
        }
    }

    // Erase from fault log entries map
    entries.erase(earliestEntryId);
}

void Manager::checkThresholdFaultLog(FaultDataType entryType,
                                     std::string &additionalTypeStr)
{
    if (faultLogSize == MAX_TOTAL_FAULT_LOG_ENTRIES)
    {
        removeEarliestEntry(additionalTypeStr);
    }
    else
    {
        faultLogSize++;
    }

    if (entryType == FaultDataType::CPER)
    {
        if (cperLogSize == MAX_TOTAL_CPER_LOG_ENTRIES)
            removeEarliestDataEntry(entryType);
        else
            cperLogSize++;
    }
    else if (entryType == FaultDataType::Crashdump)
    {
        if (additionalTypeStr.empty())
        {
            if (crashdumpSize == MAX_TOTAL_CRASHDUMP_ENTRIES)
                removeEarliestDataEntry(entryType);
            else
                crashdumpSize++;
        }
        else
        {
            /* OEM */
            if (additionalTypeStr == "BERT")
            {
                if (bertSize == MAX_TOTAL_BERT_ENTRIES)
                    removeEarliestDataEntry(entryType);
                else
                    bertSize++;
            }
            if (additionalTypeStr == "Diagnostic")
            {
                if (diagnosticSize == MAX_TOTAL_DIAGNOSTIC_ENTRIES)
                    removeEarliestDataEntry(entryType);
                else
                    diagnosticSize++;
            }
        }
    }
    else
    {
        lg2::error("Incorrect entry type");
        elog<InternalFailure>();
    }
}

void Manager::removeAllDataEntry()
{
    for ( auto it = entries.begin(); it != entries.end(); ++it  )
    {
        auto secondPtr = it->second.get();
        std::string faultLogFilePath =
                dynamic_cast<faultlog::Entry*>(secondPtr)->primaryLogId();
        // Remove fault log file
        if (std::filesystem::exists(faultLogFilePath.c_str()))
        {
            std::filesystem::remove(faultLogFilePath.c_str());
        }
    }
}

void Manager::removeEarliestDataEntry(FaultDataType type)
{
    for ( auto it = entries.begin(); it != entries.end(); ++it  )
    {
        auto secondPtr = it->second.get();
        FaultDataType entryType =
                dynamic_cast<faultlog::Entry*>(secondPtr)->type();
        std::string faultLogFilePath =
                dynamic_cast<faultlog::Entry*>(secondPtr)->primaryLogId();
        if (entryType == type)
        {
            // Remove fault log file
            if (std::filesystem::exists(faultLogFilePath.c_str()))
            {
                std::filesystem::remove(faultLogFilePath.c_str());
                break;
            }
        }
    }
}

void Manager::removeEarliestEntry(std::string &additionalTypeStr)
{
    auto it = entries.begin();
    auto secondPtr = it->second.get();
    FaultDataType entryType =
            dynamic_cast<faultlog::Entry*>(secondPtr)->type();
    std::string faultLogFilePath =
            dynamic_cast<faultlog::Entry*>(secondPtr)->primaryLogId();

    if (std::filesystem::exists(faultLogFilePath.c_str()))
    {
        std::filesystem::remove(faultLogFilePath.c_str());
        switch (entryType) {
        case FaultDataType::CPER:
            cperLogSize--;
            break;
        case FaultDataType::Crashdump:
            if (additionalTypeStr.empty())
                crashdumpSize--;
            else
            {
                /* OEM */
                if (additionalTypeStr == "BERT")
                    bertSize--;
                if (additionalTypeStr == "Diagnostic")
                    diagnosticSize--;
            }
            break;
        default:
            lg2::error("Incorrect FaultLog Entry Type");
            elog<InternalFailure>();
            break;
        }
    }

    /* Delete the persistent representation of this FaultLog entry */
    fs::path faultlogPath(FAULTLOG_DUMP_PATH);
    uint32_t id = it->first;
    faultlogPath /= std::to_string(id);
    fs::remove_all(faultlogPath);

    /* Delete the FaultLog entry */
    entries.erase(it);
}

void Manager::restore()
{
    lg2::info("dump_manager_faultlog restore is called");
    fs::path dir(FAULTLOG_DUMP_PATH);
    if (!fs::exists(dir) || fs::is_empty(dir))
    {
        return;
    }

    for (auto& file : fs::directory_iterator(dir))
    {
        auto id = file.path().filename().c_str();
        auto idNum = std::stol(id);
        auto idString = std::to_string(idNum);
        auto objPath = std::filesystem::path(baseEntryPath) / idString;
        std::filesystem::path filePath = file.path();
        FaultDataType entryType;
        std::string primaryLogIdStr;
        std::string additionalTypeStr;
        std::string originatorId;
        originatorTypes originatorType;

        filePath /= FAULTLOG_FILE;
        auto e = std::make_unique<faultlog::Entry>(
                      bus, objPath.c_str(), idNum, generateTimestamp(),
                      0, filePath,
                      phosphor::dump::OperationStatus::Completed,
                      originatorId, originatorType, entryType,
                      primaryLogIdStr, additionalTypeStr, *this, &entries);

        e->deserializeEntry();
        e->serializeEntry();
        entries.insert(std::make_pair(idNum, std::move(e)));
    }

    if (!entries.empty())
    {
        // Restore the counter of last entry, cper and crashdump
        restoreCounter();
    }
}

void Manager::restoreCounter()
{
    lastEntryId = entries.rbegin()->first;

    for ( auto it = entries.begin(); it != entries.end(); ++it  )
    {
        faultLogSize++;
        if (faultLogSize > MAX_TOTAL_FAULT_LOG_ENTRIES)
        {
            faultLogSize = MAX_TOTAL_FAULT_LOG_ENTRIES;
        }
        auto secondPtr = it->second.get();
        FaultDataType entryType =
                dynamic_cast<faultlog::Entry*>(secondPtr)->type();
        std::string primaryLogId =
                dynamic_cast<faultlog::Entry*>(secondPtr)->primaryLogId();
        std::string additionalTypeStr =
                dynamic_cast<faultlog::Entry*>(secondPtr)->additionalTypeName();
        // Remove fault log file
        switch (entryType) {
        case FaultDataType::CPER:
            cperLogSize++;
            if (cperLogSize > MAX_TOTAL_CPER_LOG_ENTRIES)
            {
                cperLogSize = MAX_TOTAL_CPER_LOG_ENTRIES;
            }
            break;
        case FaultDataType::Crashdump:
            crashdumpSize++;
            if (crashdumpSize > MAX_TOTAL_CRASHDUMP_ENTRIES)
            {
                crashdumpSize = MAX_TOTAL_CRASHDUMP_ENTRIES;
            }
            if (!additionalTypeStr.empty())
            {
                /* OEM */
                if (additionalTypeStr == "BERT")
                {
                    bertSize++;
                    if (bertSize > MAX_TOTAL_BERT_ENTRIES)
                    {
                        bertSize = MAX_TOTAL_BERT_ENTRIES;
                    }
                }
                if (additionalTypeStr == "Diagnostic")
                {
                    diagnosticSize++;
                    if (diagnosticSize > MAX_TOTAL_DIAGNOSTIC_ENTRIES)
                    {
                        diagnosticSize = MAX_TOTAL_DIAGNOSTIC_ENTRIES;
                    }
                }
            }
            break;
        default:
            lg2::error("Incorrect FaultLog Entry Type");
            elog<InternalFailure>();
            break;
        }
    }
}


} // namespace faultlog
} // namespace dump
} // namespace phosphor
