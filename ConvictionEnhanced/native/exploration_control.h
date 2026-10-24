#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include "exploration_policy.h"
#include "free_camera.h"

namespace ce {
enum class ExplorationPreviewState {None,Pending,Valid,Blocked};
struct ExplorationPreview {
    ExplorationPreviewState state=ExplorationPreviewState::None;
    std::uint64_t revision=0,generation=0,session=0;
    std::uintptr_t controller=0,pawn=0;
    bool fromCamera=false,hasFloor=false;
    ExplorationPosition target{},landingCenter{},floorPoint{};
    float radius=0;
    const char* reason="Choose an exploration camera destination";
};
struct ExplorationStatus {
    bool installed=false,available=false,currentSafe=false;
    bool hasCameraTarget=false,hasReturnPoint=false,pending=false,executing=false;
    bool hasReturnSupport=false;
    float currentHalfHeight=0,returnFloorZ=0;
    std::uint64_t generation=0,moves=0,returns=0;
    ExplorationPosition current{},cameraTarget{},returnPoint{};
    ExplorationPreview preview{};
    const char* reason="Load solo gameplay to use exploration tools";
};
bool initializeExploration(bool twoInstanceMode) noexcept;
ExplorationStatus explorationStatus() noexcept;
void pollExplorationInput(HWND window,unsigned connectedMask,bool menuOpen,
    bool neutral,bool inputDraining,bool cameraOwned,bool practiceHealthy) noexcept;
// Capture only the deliberate free-camera -> debug-menu handoff, before stopping
// the camera. This saves a target and never changes a game object or world state.
bool rememberExplorationCamera(const FreeCameraStatus& camera) noexcept;
// Read-only candidate publication; no collision calls, save activation or world
// mutation occur here. Call each frame after pollExplorationInput. False drops
// the live flight candidate and previews the deliberately selected destination.
void publishExplorationPreview(const FreeCameraStatus& camera,bool explorationFlight) noexcept;
bool requestExplorationTeleport(std::uint64_t generation) noexcept;
bool requestExplorationReturn(std::uint64_t generation) noexcept;
bool requestExplorationSaveReturn(std::uint64_t generation) noexcept;
void cancelExplorationRequest() noexcept;
// Read-only publication from the existing verified primary view callback.
// Relocation and collision queries run only after the native engine tick.
void explorationNativeView(void* controller,bool validPawnView=true) noexcept;
#ifdef CE_TEST_HOST
void testExplorationReset() noexcept;
void testExplorationClock(std::uint64_t milliseconds) noexcept;
void testExplorationFocused(bool focused) noexcept;
// Persistent native-host geometry. Recenter the fixture on currentFloorZ with
// the requested body height, preserving XY, and publish the new stance at once.
void testExplorationBody(float halfHeight,float currentFloorZ,float destinationFloorZ) noexcept;
void testExplorationClearReturnSupport() noexcept;
void testExplorationView(std::uint32_t identity,const ExplorationPosition& position,
    bool valid=true,bool currentSafe=true) noexcept;
void testExplorationContextView(std::uint32_t identity,const ExplorationPosition& position,
    const char* map,const char* connection,bool splitScreen=false,bool currentSafe=true) noexcept;
// 1 no ground, 2 steep floor, 4 blocked capsule, 8 native move failure,
// 16 identity change during move, 32 native position mismatch, 64 query error,
// 128 stance change / 256 shape change / 512 controller change /
// 1024 protection failure during landing validation.
void testExplorationFailure(unsigned flags) noexcept;
void testExplorationTick() noexcept;
bool testExplorationNestedTick() noexcept;
void testExplorationNativeProtection(bool healthy) noexcept;
bool testExplorationStaticFloor(std::uintptr_t table,unsigned flags,unsigned physics,bool sameLevel,
    std::uintptr_t ownBase,const ExplorationPosition& velocity) noexcept;
unsigned testExplorationNativeMoves() noexcept;
unsigned testExplorationLandingQueries() noexcept;
ExplorationPosition testExplorationLastDestination() noexcept;
#endif
}
