#pragma once
// Copyright (c) 2026 Michael Works. Original mod work; see COPYRIGHT.txt.
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace ce {
struct CheckpointLibraryRecord {
    int act=-1,episode=-1,checkpoint=-1;
    bool initial=false;
    std::string map;
    std::vector<unsigned char> bytes;
};
struct CheckpointLibraryResult {
    bool ok=false;
    std::string reason;
};
inline constexpr unsigned MaxCheckpointLibraryRecords=32;
// All disk access stays inside stateRoot/checkpoint-library. The root must
// already exist on a local drive and contain no reparse/alias components.
CheckpointLibraryResult initializeCheckpointLibrary(const std::filesystem::path& stateRoot) noexcept;
CheckpointLibraryResult saveCheckpointRecord(const std::string& profileHash64,
    const CheckpointLibraryRecord& record) noexcept;
// Failure clears out; no partial/corrupt group is returned. A missing mission
// group is a successful empty result. Only canonical campaign maps are stored.
CheckpointLibraryResult loadCheckpointRecords(const std::string& profileHash64,
    const std::string& map,int act,int episode,
    std::vector<CheckpointLibraryRecord>& out) noexcept;
#ifdef CE_TEST_HOST
enum class CheckpointLibraryFailure {None,Write,Flush,Replace};
void testCheckpointLibraryReset() noexcept;
void testCheckpointLibraryFailure(CheckpointLibraryFailure failure) noexcept;
void testCheckpointLibraryBudget(std::uint64_t bytes) noexcept;
std::filesystem::path testCheckpointLibraryPath();
#endif
}
