#include "dump_manager.hpp"
#include <iostream>

namespace phosphor
{
namespace dump
{

void Manager::erase(uint32_t entryId)
{
    try{
        entries.erase(entryId);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception when erasing entry id " << entryId << " e: " << e.what() << "\n";
    }
}

void Manager::deleteAll()
{
    auto iter = entries.begin();
    while (iter != entries.end())
    {
        auto& entry = iter->second;
        ++iter;
        entry->delete_();
    }
}

} // namespace dump
} // namespace phosphor
