#pragma once

#include "dump_entry.hpp"
#include "xyz/openbmc_project/Dump/Entry/System/server.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdeventplus/source/io.hpp>
#include <memory>

namespace phosphor
{
namespace dump
{
namespace system
{

constexpr uint32_t INVALID_SOURCE_ID = 0xFFFFFFFF;

template <typename T>
using ServerObject = typename sdbusplus::server::object_t<T>;

using EntryIfaces = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Dump::Entry::server::System>;

using originatorTypes = sdbusplus::xyz::openbmc_project::Common::server::
    OriginatedBy::OriginatorTypes;

using OperationStatus = sdbusplus::xyz::openbmc_project::Common::server::Progress::OperationStatus;

class Manager;

/** @class Entry
 *  @brief System Dump Entry implementation.
 *  @details A concrete implementation for the
 *  xyz.openbmc_project.Dump.Entry DBus API
 */
class Entry : virtual public phosphor::dump::Entry, virtual public EntryIfaces
{
  public:
    Entry() = delete;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;
    ~Entry(){
        esource.reset();
    };

    /** @brief Constructor for the Dump Entry Object
     *  @param[in] bus - Bus to attach to.
     *  @param[in] objPath - Object path to attach to
     *  @param[in] dumpId - Dump id.
     *  @param[in] timeStamp - Dump creation timestamp
     *             since the epoch.
     *  @param[in] dumpSize - Dump size in bytes.
     *  @param[in] sourceId - DumpId provided by the source.
     *  @param[in] status - status  of the dump.
     *  @param[in] originatorId - Id of the originator of the dump
     *  @param[in] originatorType - Originator type
     *  @param[in] parent - The dump entry's parent.
     */
    Entry(sdbusplus::bus_t& bus, const std::string& objPath, uint32_t dumpId,
          uint64_t timeStamp, uint64_t dumpSize,
          const std::filesystem::path& file, const uint32_t sourceId,
          phosphor::dump::OperationStatus status,
          std::string originatorId, originatorTypes originatorType,
          phosphor::dump::Manager& parent) :
        phosphor::dump::Entry(bus, objPath.c_str(), dumpId, timeStamp, dumpSize,
                              file, status, originatorId, originatorType,
                              parent),
        EntryIfaces(bus, objPath.c_str(), EntryIfaces::action::defer_emit)
    {
        sourceDumpId(sourceId);
        // oemDiagnosticDataType(oemDiagType);
        // Emit deferred signal.
        this->phosphor::dump::system::EntryIfaces::emit_object_added();
    };

    /** @brief Method to initiate the offload of dump
     *  @param[in] uri - URI to offload dump.
     */
    void initiateOffload(std::string uri) override;

    /** @brief Method to update an existing dump entry
     *  @param[in] timeStamp - Dump creation timestamp
     *  @param[in] dumpSize - Dump size in bytes.
     *  @param[in] sourceId - DumpId provided by the source.
     */
    void update(const uint64_t& timeStamp, const uint64_t& dumpSize,
                const uint32_t sourceId, const OperationStatus& opStatus)
    {
        elapsed(timeStamp);
        completedTime(timeStamp);
        size(dumpSize);
        sourceDumpId(sourceId);
        status(opStatus);
    }

    /**
     * @brief Delete host system dump and it entry dbus object
     */
    void delete_() override;
private:

    void socketPollCallback([[maybe_unused]]sdeventplus::source::IO& es, int fd,
                            uint32_t revents);

    /** @brief The event source object reference */
     std::unique_ptr<sdeventplus::source::IO> esource{nullptr};

    std::vector<char> readBuffer;
};

} // namespace system
} // namespace dump
} // namespace phosphor