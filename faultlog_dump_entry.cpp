#include "faultlog_dump_entry.hpp"

#include <phosphor-logging/lg2.hpp>
#include <nlohmann/json.hpp>

namespace phosphor
{
namespace dump
{
namespace faultlog
{

void Entry::delete_()
{
    lg2::info("In faultlog_dump_entry.cpp delete_()");

    // Delete Dump file from Permanent location
    try
    {
        std::filesystem::remove(file);
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        // Log Error message and continue
        lg2::error("Failed to delete dump file, errormsg: {ERROR}", "ERROR", e);
    }

    // Remove Dump entry D-bus object
    parentMap->erase(id);
}

void Entry::serializeFaultLogEntry()
{
    // Folder for serialized entry
    std::filesystem::path dir = file.parent_path() / PRESERVE;

    // Serialized entry file
    std::filesystem::path serializePath = dir / FAULTLOG_SERIALIZE_FILE;
    try
    {
        if (!std::filesystem::exists(dir))
        {
            std::filesystem::create_directories(dir);
        }

        std::ofstream os(serializePath, std::ios::binary);
        if (!os.is_open())
        {
            lg2::error("Failed to open file for serialization: {PATH} ", "PATH",
                       serializePath);
            return;
        }
        nlohmann::json j;
        j["dumpId"] = id;
        j["type"] = type();
        j["primaryLogId"] = primaryLogId();
        j["additionalTypeName"] = additionalTypeName();

        os << j.dump();
    }
    catch (const std::exception& e)
    {
        lg2::error("Serialization error: {PATH} {ERROR} ", "PATH",
                   serializePath, "ERROR", e);

        // Remove the serialization folder if that got created
        // Ignore the error since folder may not be created
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

void Entry::serializeEntry()
{
    // Serialize faultlog entry
    serializeFaultLogEntry();
    // Serialize base dump entry
    serialize();
}

void Entry::derializeFaultLogEntry()
{
    try
    {
        // .preserve folder
        std::filesystem::path dir = file.parent_path() / PRESERVE;
        if (!std::filesystem::exists(dir))
        {
            lg2::info("Serialization directory: {SERIAL_DIR} doesnt exist, "
                      "skip deserialization",
                      "SERIAL_DIR", dir);
            return;
        }

        // Serialized entry
        std::filesystem::path serializePath = dir / FAULTLOG_SERIALIZE_FILE;
        std::ifstream is(serializePath, std::ios::binary);
        if (!is.is_open())
        {
            lg2::error("Failed to open file for deserialization: {PATH}",
                       "PATH", serializePath);
            return;
        }
        nlohmann::json j;
        is >> j;

        uint32_t storedId;
        j.at("dumpId").get_to(storedId);
        if (storedId == id)
        {
            type(j["type"].get<FaultDataType>());
            primaryLogId(j["primaryLogId"].get<std::string>());
            additionalTypeName(j["additionalTypeName"].get<std::string>());
            uint64_t fileSize = 0;
            std::filesystem::path faultLogFilePath;
            faultLogFilePath = primaryLogId();
            if (std::filesystem::exists(faultLogFilePath))
            {
                fileSize = std::filesystem::file_size(faultLogFilePath);
            }
            size(fileSize);
        }
        else
        {
            lg2::error("The id ({ID_IN_FILE}) is not matching the dump id "
                       "({DUMPID}); skipping deserialization.",
                       "ID_IN_FILE", storedId, "DUMPID", id);

            // Id is not matching, this could be due to file corruption
            // deleting the .preserve folder.
            // Attempt to delete the folder and ignore any error.
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Deserialization error: {PATH}, {ERROR}", "PATH",
                   file.parent_path(), "ERROR", e);
    }
}

void Entry::deserializeEntry()
{
    // Deserialize faultlog entry
    derializeFaultLogEntry();
    // Deserialize base dump entry
    deserialize(file.parent_path());
}


} // namespace faultlog
} // namespace dump
} // namespace phosphor
