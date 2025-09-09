#pragma once
#include "dump_utils.hpp"
#include "system_dump_collector.hpp"
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <sdeventplus/source/io.hpp>
#include <sdbusplus/slot.hpp>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <filesystem>
#include <functional>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/lg2.hpp>

#include <xyz/openbmc_project/ObjectMapper/client.hpp>
#include <xyz/openbmc_project/PLDM/File/client.hpp>

namespace phosphor
{
namespace dump
{
namespace collector
{
namespace system
{

using namespace phosphor::logging;
using ObjectPath = std::string;
using ServiceName = std::string;
using Interfaces = std::vector<std::string>;
using MapperServiceMap = std::vector<std::pair<ServiceName, Interfaces>>;
using GetSubTreeResponse = std::vector<std::pair<ObjectPath, MapperServiceMap>>;
using ObjectMapper = sdbusplus::client::xyz::openbmc_project::ObjectMapper<>;
using FileInf = sdbusplus::client::xyz::openbmc_project::pldm::File<>;

constexpr auto PLDM_SERVICE = "xyz.openbmc_project.PLDM";
constexpr auto FILE_INTERFACE = FileInf::interface;
constexpr auto READ_METHOD = "Open";
constexpr auto TMP_PATH = "/tmp/system-dumps";

bool collectFileObject(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus, sdbusplus::slot_t& slot,
                        std::function<
                        void(Manager* manager, Entry* tmpEntry, sdbusplus::bus_t& bus,
                            sdbusplus::slot_t& slot, sdbusplus::message_t&& msg)> callback);
void handleNotifiedDump(Entry* entry);
template <typename T>
T getDumpProperty(const std::string& objPath, const std::string& property, sdbusplus::bus_t& bus);
std::optional<FileInf::SourceType> collectDumpSource(const std::string& path, sdbusplus::bus_t& bus);
std::string collectFileName(const std::string& path, sdbusplus::bus_t& bus);
size_t downloadDumpData(const std::string& objPath, const std::string& downloadPath, sdbusplus::bus_t& bus);
bool saveDumpData(std::string downloadPath, const std::vector<char>* readBuffer);
uint32_t hashStringToUInt32(const std::string& str);

} // namespace system
} // namespace collector
} // namespace dump
} // namespace phosphor