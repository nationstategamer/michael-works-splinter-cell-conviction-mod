#pragma once
#include <windows.h>

namespace ce {
struct ControllerStartupStatus {
    bool installed=false, completed=false, repaired=false;
    unsigned attempts=0;
    const char* reason="Waiting for controller startup check";
};
// No input hook or profile write: only the exact-build native runtime setting.
bool initializeControllerStartup(bool twoInstanceMode) noexcept;
void pollControllerStartup(HWND gameWindow,unsigned connectedMask) noexcept;
// Invoked only after the existing signature-checked native PlayerCalcView call
// from its verified render-constructor caller, with that call's controller.
void controllerStartupNativeView(void* controller) noexcept;
ControllerStartupStatus controllerStartupStatus() noexcept;
}
