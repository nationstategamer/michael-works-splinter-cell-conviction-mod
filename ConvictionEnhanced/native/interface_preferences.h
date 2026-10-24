#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace ce {
struct InterfacePreferences {
    std::uint32_t hiddenMask=0;
    bool allHidden=false;
    bool cameraHint=true;
};
struct InterfacePreferencesResult {
    bool ok=false;
    bool missing=false;
    InterfacePreferences value{};
    std::string reason;
};
// The caller supplies the mod's own state file. These functions never create
// parent directories or modify game/profile INIs. Invalid files remain intact.
InterfacePreferencesResult loadInterfacePreferences(const std::filesystem::path& file);
InterfacePreferencesResult saveInterfacePreferences(const std::filesystem::path& file,const InterfacePreferences& value);
#ifdef CE_TEST_HOST
enum class InterfacePreferencesFailure {None,Write,Flush,Replace};
void testInterfacePreferencesFailure(InterfacePreferencesFailure point);
#endif
}
