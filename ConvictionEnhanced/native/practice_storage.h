#pragma once
// Copyright (c) 2026 Michael Works. Original mod work; see COPYRIGHT.txt.
#include <filesystem>
#include <cstdint>

namespace ce {
struct PracticeStorageStatus {
    bool installed=false,active=false,healthy=false;
    std::uint64_t reads=0,writes=0,deletes=0,blocked=0;
    const char* reason="Practice save protection is not initialized";
};
// Register all six exact-build Orbit payload hooks after MinHook initialization.
// Inactive hooks pass through. Activation never has a process-lifetime inverse.
bool initializePracticeStorage(const std::filesystem::path& modStateRoot) noexcept;
bool activatePracticeStorage() noexcept;
PracticeStorageStatus practiceStorageStatus() noexcept;
// Privacy-safe stable binding for the validated Ubisoft account/save-group.
// Available before activation; no raw account name or source path is returned.
bool practiceAccountKey(char out[65]) noexcept;

#ifdef CE_TEST_HOST
enum class PracticeStorageFailure {None,Write,Flush,Replace};
struct PracticeOriginalCounts {unsigned reads=0,writes=0,exists=0,deletes=0,bulkDeletes=0,rawDeletes=0;};
void testPracticeReset() noexcept;
void testPracticeSource(const char* name,const void* data,unsigned size);
unsigned testPracticeWrite(const char* name,const void* data,unsigned size) noexcept;
unsigned testPracticeRead(const char* name,void* data,unsigned size) noexcept;
bool testPracticeExists(const char* name) noexcept;
bool testPracticeDelete(const char* name) noexcept;
bool testPracticeBulkDelete(const char* name) noexcept;
bool testPracticeRawIdDelete(unsigned saveId) noexcept;
PracticeOriginalCounts testPracticeOriginalCounts() noexcept;
void testPracticeFailure(PracticeStorageFailure failure) noexcept;
std::filesystem::path testPracticeSessionPath();
void testPracticePauseNextOriginalWrite() noexcept;
bool testPracticeOriginalWritePaused() noexcept;
void testPracticeReleaseOriginalWrite() noexcept;
void testPracticeOriginalDirectory(const std::filesystem::path& directory);
void testPracticeSourceId(unsigned saveId,const char* logicalName);
#endif
}
