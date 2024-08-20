#pragma once

#include "config.h"

#include "dump_manager.hpp"
#include "faultlog_dump_entry.hpp"

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/server/object.hpp>
#include <filesystem>
#include <xyz/openbmc_project/Dump/Create/server.hpp>

namespace phosphor
{
namespace dump
{
namespace faultlog
{

using CreateIface = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Dump::server::Create>;
namespace fs = std::filesystem;

/** @class Manager
 *  @brief FaultLog Dump manager implementation.
 */
class Manager :
    virtual public CreateIface,
    virtual public phosphor::dump::Manager
{
  public:
    Manager() = delete;
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;
    virtual ~Manager() = default;

    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     *  @param[in] baseEntryPath - Base path for dump entry.
     *  @param[in] filePath - Path where the dumps are stored.
     */
    Manager(sdbusplus::bus_t& bus, const char* path,
            const std::string& baseEntryPath, const char* filePath) :
        CreateIface(bus, path),
        phosphor::dump::Manager(bus, path, baseEntryPath), dumpDir(filePath)
    {
        std::error_code ec;

        std::filesystem::create_directory(FAULTLOG_DUMP_PATH, ec);

        if (ec)
        {
            auto dir = FAULTLOG_DUMP_PATH;
            lg2::error(
                "dump_manager_faultlog directory {DIRECTORY} not created. "
                "error_code = {ERRNO} ({ERROR_MESSAGE})",
                "DIRECTORY", dir, "ERRNO", ec.value(), "ERROR_MESSAGE",
                ec.message());
        }

        registerFaultLogMatches();
        faultLogSize = 0;
        cperLogSize = 0;
        crashdumpSize = 0;
        bertSize = 0;
        diagnosticSize = 0;
    }

    void restore() override;

    /** @brief  Delete all fault log entries and their corresponding fault log
     * dump files */
    void deleteAll() override;

    /** @brief Method to create a new fault log dump entry
     *  @param[in] params - Key-value pair input parameters
     *
     *  @return object_path - The path to the new dump entry.
     */
    sdbusplus::message::object_path createDump(
        phosphor::dump::DumpCreateParams params) override;

  private:
    static constexpr uint32_t MAX_NUM_FAULT_LOG_ENTRIES =
            MAX_TOTAL_CPER_LOG_ENTRIES + MAX_TOTAL_CRASHDUMP_ENTRIES;

    /** @brief Map of saved CPER log entry dbus objects based on entry id */
    std::map<uint32_t, std::unique_ptr<phosphor::dump::Entry>>
        savedCperLogEntries;

    /** @brief Map of saved crashdump entry dbus objects based on entry id */
    std::map<uint32_t, std::unique_ptr<phosphor::dump::Entry>>
        savedCrashdumpEntries;

    uint32_t faultLogSize;
    uint32_t cperLogSize;
    uint32_t crashdumpSize;
    uint32_t bertSize;
    uint32_t diagnosticSize;

    /** @brief Path to the dump file*/
    std::string dumpDir;

    /** @brief D-Bus match for crashdump completion signal */
    std::unique_ptr<sdbusplus::bus::match_t> crashdumpMatch;

    /** @brief D-Bus match for CPER log added signal */
    std::unique_ptr<sdbusplus::bus::match_t> cperLogMatch;

    /** @brief Register D-Bus match rules to detect fault events */
    void registerFaultLogMatches();
    /** @brief Register D-Bus match rules to detect new crashdumps */
    void registerCrashdumpMatch();
    /** @brief Register D-Bus match rules to detect CPER logs */
    void registerCperLogMatch();

    /** @brief Get and check parameters for createDump() function (throws
     * exception on error)
     *  @param[in] params - Key-value pair input parameters
     *  @param[out] entryType - Log entry type (corresponding to type of data in
     * primary fault data log)
     *  @param[out] primaryLogIdStr - Id of primary fault data log
     *  @param[out] additionalTypeName - OEM format of a entryType
     */
    void getAndCheckCreateDumpParams(
        const phosphor::dump::DumpCreateParams& params,
        FaultDataType& entryType, std::string& primaryLogIdStr,
        std::string& additionalTypeName);

    /** @brief Generate the current timestamp, adjusting as needed to ensure an
     * increase compared to the last fault log entry's timestamp
     *
     *  @return timestamp - microseconds since epoch
     */
    uint64_t generateTimestamp();

    /** @brief Save earliest fault log entry (if it qualifies to be saved) and
     * remove it from the main fault log entries map.
     *
     *  More specifically, move the earliest entry from the fault log
     *  entries map to the saved entries map based on its type. Before
     *  moving it, this function checks (1) whether a saved entries map
     *  exists for the entry type, and if so, then (2) whether the
     *  saved entries map is already full. If the entry can't be saved,
     *  then it's simply deleted from the main fault log entries map.
     */
    void saveEarliestEntry();

    /** @brief Remove all FaultLog data */
    void removeAllDataEntry();

    /** @brief Remove earliest FaultLog data match with FaultLog type */
    void removeEarliestDataEntry(FaultDataType type);

    /** @brief Remove earliest FaultLog data and entry */
    void removeEarliestEntry(std::string &additionalTypeStr);

    /** @brief Check threshold condition of FaultLog */
    void checkThresholdFaultLog(FaultDataType type,
                                std::string &additionalTypeStr);

    /** @brief Restore the counter of last entry, cper and crashdump */
    void restoreCounter();
};

} // namespace faultlog
} // namespace dump
} // namespace phosphor
