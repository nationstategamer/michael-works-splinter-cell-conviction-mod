#pragma once
#include <windows.h>
#include <Xinput.h>
#include <cstdint>
#include "controller_state.h"

namespace ce {
struct MenuPadFrame {
    bool available=false, menuOpen=false, navigationReady=false;
    bool cameraActive=false, cameraStopping=false, cameraReturnsToMenu=false;
    std::uint64_t cameraSession=0;
    CameraStopReason cameraStopReason=CameraStopReason::None;
    int selectedSlot=-1, connected=0;
    unsigned connectedMask=0;
    bool allPadsNeutral=true,inputDraining=false;
    XINPUT_GAMEPAD gamepad{};
    unsigned long long gameReads=0;
};
// Call after MinHook is initialized, before advertising controller support.
bool initializeControllerInput() noexcept;
void setControllerMenuOpen(bool open) noexcept;
std::uint64_t beginControllerFreeCamera() noexcept;
void requestControllerCameraStop(bool returnToMenu) noexcept;
bool acknowledgeControllerCameraStopped(std::uint64_t session) noexcept;
void abandonControllerInput() noexcept;
void setControllerFocus(bool focused) noexcept;
MenuPadFrame pollControllerInput(bool focused) noexcept;
#ifdef CE_TEST_HOST
void testSetPad(unsigned slot,const XINPUT_STATE* state,bool connected) noexcept;
void testSetControllerTime(std::uint64_t time) noexcept;
void testSetPadError(unsigned slot,DWORD error) noexcept;
void testPauseNextRead() noexcept;
bool testReadPaused() noexcept;
void testReleaseRead() noexcept;
DWORD testGameGetState(unsigned entry,DWORD slot,XINPUT_STATE* state) noexcept;
#endif
}
