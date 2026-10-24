#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <windows.h>
#include <cstdint>
#include <filesystem>

namespace ce {
struct CheckpointSection {int id=-1;char label[96]{};int checkpoint=-1;bool initial=false;};
struct CheckpointPart {int episode=-1;char label[48]{};};
struct CheckpointStatus {
    bool installed=false,available=false,pending=false,loading=false;
    bool persistent=false;
    bool partsAvailable=false,partPending=false,partLoading=false;
    unsigned partCount=0;
    CheckpointPart parts[2]{};
    int targetPartEpisode=-1;
    std::uint64_t generation=0;
    int act=-1,episode=-1,currentSection=-1;
    int currentHardCheckpoint=-1;
    char mission[96]{};
    unsigned sectionCount=0;
    CheckpointSection sections[32]{};
    const char* reason="Load a campaign checkpoint to choose a mission section";
};
bool initializeCheckpointControl(const std::filesystem::path& modStateRoot={}) noexcept;
CheckpointStatus checkpointStatus() noexcept;
bool requestSection(int section,std::uint64_t generation) noexcept;
bool requestPart(int targetEpisode,std::uint64_t generation) noexcept;
void cancelCheckpointRequest() noexcept;
// Present publishes only recent input state. No native load runs from Present.
void pollCheckpointInput(HWND window,unsigned connectedMask,bool menuOpen,
    bool neutral,bool practiceActiveHealthy) noexcept;
// Called after the existing original PlayerCalcView, only for its verified
// render caller and extra==0. The backend matches self to the primary controller.
void checkpointNativeView(void* controller) noexcept;
#ifdef CE_TEST_HOST
void testCheckpointClock(std::uint64_t time) noexcept;
void testCheckpointFocused(bool focused) noexcept;
void testCheckpointStorage(bool activeHealthy) noexcept;
void testCheckpointView(std::uint32_t identity,int currentSection,const int* sections,
    unsigned count,bool valid=true) noexcept;
unsigned testCheckpointDispatches() noexcept;
int testCheckpointLastDispatch() noexcept;
void testCheckpointReset() noexcept;
int testCheckpointTickRestore(bool expectedCaller=true,bool sourceMatches=true,
    int originalResult=1,int restoredCheckpoint=-999,bool mapMatches=true) noexcept;
void testCheckpointCorruptPending() noexcept;
unsigned testCheckpointOriginalCalls() noexcept;
bool testCheckpointUsedOwned() noexcept;
bool testCheckpointReentered() noexcept;
void testCheckpointAnchorFailure(bool failure) noexcept;
unsigned testCheckpointAnchorCopies() noexcept;
bool testCheckpointProvenance(int sourceAct,int sourceEpisode,int currentAct,
    int currentEpisode,bool samePayload) noexcept;
void testCheckpointChangeMaskDuringOriginal(bool change) noexcept;
void testCheckpointMissionPart(int episode,bool mapsValid=true,unsigned difficulty=1,
    unsigned profileIdentity=1) noexcept;
unsigned testCheckpointPartDispatches() noexcept;
int testCheckpointLastPartDispatch() noexcept;
bool testCheckpointPartReentered() noexcept;
// Failure flags: 1 bindings, 2 queued URL, 4 profile episode, 8 difficulty.
void testCheckpointPartFailure(unsigned flags) noexcept;
void testCheckpointPartArchives(bool ready) noexcept;
// Arrival proof: 1 valid target payload, 2 wrong episode, 4 corrupt zlib,
// 8 archive hard ID differs from the loaded current hard ID.
void testCheckpointPartArrivalProvenance(unsigned flags) noexcept;
#endif
}
