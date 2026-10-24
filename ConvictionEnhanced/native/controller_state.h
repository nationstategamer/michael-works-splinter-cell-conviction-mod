#pragma once

#include <array>
#include <cstdint>

namespace ce {

// Button values and default thresholds match XInput. This header deliberately
// has no platform dependencies, hooks, IO, or internal locks. Callers serialize
// every access (including getters) with the same input-state lock.
struct PadSnapshot {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t leftX = 0;
    std::int16_t leftY = 0;
    std::int16_t rightX = 0;
    std::int16_t rightY = 0;
};

struct ControllerUpdate {
    bool menuOpened = false;
    bool menuClosed = false;
    bool navigationReady = false;
    int selectedSlot = -1;
};

enum class CameraStopReason { None, Requested, FocusLost, Disconnected };

class ControllerState {
public:
    using Pads = std::array<PadSnapshot, 4>;
    static constexpr std::uint16_t LeftThumb = 0x0040;
    static constexpr std::uint16_t RightThumb = 0x0080;
    static constexpr std::uint16_t ToggleChord = LeftThumb | RightThumb;
    static constexpr std::uint16_t PauseButton = 0x0010;
    static constexpr std::uint64_t HoldMilliseconds = 600;
    static constexpr int LeftDeadzone = 7849;
    static constexpr int RightDeadzone = 8689;
    static constexpr int TriggerThreshold = 30;

    // Call on actual overlay visibility changes, including keyboard opening and
    // closing. An idempotent false call intentionally does not cancel a pending
    // controller chord. Controller-generated events already update this flag.
    void setMenuOpen(bool open) noexcept {
        if (cameraActive() || cameraStopping()) {
            requestCameraStop(open);
            return;
        }
        if (menuOpen() == open) return;
        mode_ = open ? Mode::Menu : Mode::None;
        navigationReady_ = false;
        holdingChord_ = false;
        if (open) {
            phase_ = Phase::Open;
        } else {
            beginDrain();
        }
    }

    // Reserve controller ownership before enabling the camera backend. If backend
    // activation fails, request a stop back to the menu and acknowledge this token.
    std::uint64_t beginFreeCamera() noexcept {
        if (!menuOpen() || selectedSlot_ < 0 || !focused_ ||
            !connected_[static_cast<unsigned>(selectedSlot_)]) return 0;
        ++nextCameraSession_;
        if (nextCameraSession_ == 0) ++nextCameraSession_;
        cameraSession_ = nextCameraSession_;
        mode_ = Mode::FreeCamera;
        phase_ = Phase::Open;
        navigationReady_ = false;
        holdingChord_ = false;
        stopReturnToMenu_ = false;
        stopReason_ = CameraStopReason::None;
        armed_.fill(false);
        return cameraSession_;
    }

    // Persistent: XInput worker observations must not lose the request before
    // the render/camera thread can restore the verified gameplay camera.
    void requestCameraStop(bool returnToMenu,
                           CameraStopReason reason = CameraStopReason::Requested) noexcept {
        if (!cameraActive() && !cameraStopping()) return;
        const bool forced = reason == CameraStopReason::FocusLost || reason == CameraStopReason::Disconnected;
        const bool alreadyForced = stopReason_ == CameraStopReason::FocusLost || stopReason_ == CameraStopReason::Disconnected;
        if (!alreadyForced || forced) {
            stopReason_ = reason;
            stopReturnToMenu_ = forced ? false : returnToMenu;
        }
        mode_ = Mode::Stopping;
        phase_ = Phase::Open;
        navigationReady_ = false;
        holdingChord_ = false;
        armed_.fill(false);
    }

    // Only the backend's matching restoration acknowledgment may release input.
    // Focus loss/disconnect always return to gameplay, never reopen a menu.
    bool acknowledgeCameraStopped(std::uint64_t session) noexcept {
        if (!cameraStopping() || session == 0 || session != cameraSession_) return false;
        const bool toMenu = stopReturnToMenu_ && focused_ && selectedSlot_ >= 0 &&
            connected_[static_cast<unsigned>(selectedSlot_)];
        mode_ = toMenu ? Mode::Menu : Mode::None;
        cameraSession_ = 0;
        stopReturnToMenu_ = false;
        stopReason_ = CameraStopReason::None;
        navigationReady_ = false;
        holdingChord_ = false;
        if (toMenu) phase_ = Phase::Open;
        else {
            beginDrain();
            if (selectedSlot_ >= 0 && !connected_[static_cast<unsigned>(selectedSlot_)]) clearOwnership();
        }
        return true;
    }

    // Poll every connected slot once per presentation using ORIGINAL, unfiltered
    // XInput calls. nowMs must be a monotonic millisecond clock. The first neutral
    // sample after startup, focus regain, or connection arms chord recognition.
    ControllerUpdate update(std::uint64_t nowMs, bool focused, const Pads& pads) noexcept {
        ControllerUpdate result;
        const bool focusChanged = focused_ != focused;
        focused_ = focused;
        for (unsigned i = 0; i < pads.size(); ++i) {
            if (focusChanged || connected_[i] != pads[i].connected || !focused)
                armed_[i] = false;
            connected_[i] = pads[i].connected;
            if (focused && pads[i].connected && neutral(pads[i])) armed_[i] = true;
        }

        if (!focused) {
            if (cameraActive() || cameraStopping()) {
                requestCameraStop(false, CameraStopReason::FocusLost);
                return finish(result);
            }
            if (menuOpen()) {
                setMenuOpen(false);
                result.menuClosed = true;
            } else if (phase_ == Phase::Candidate) {
                beginDrain();
            }
            drainFrom(pads);
            return finish(result);
        }

        if (selectedSlot_ >= 0 && !pads[static_cast<unsigned>(selectedSlot_)].connected) {
            if (cameraActive() || cameraStopping()) {
                requestCameraStop(false, CameraStopReason::Disconnected);
                return finish(result);
            }
            if (menuOpen()) result.menuClosed = true;
            mode_ = Mode::None;
            armed_.fill(false);
            clearOwnership();
            // Never transfer an open menu or an interrupted hold to another pad
            // in the same update. A new neutral, deliberate chord is required.
            return finish(result);
        }

        if (cameraStopping()) return finish(result);

        if (phase_ == Phase::Draining) {
            drainFrom(pads);
            return finish(result);
        }

        if (cameraActive()) {
            const auto& pad = pads[static_cast<unsigned>(selectedSlot_)];
            if (!navigationReady_) {
                if (neutral(pad)) navigationReady_ = true;
            } else if (chordDown(pad)) {
                if (!holdingChord_) {
                    holdingChord_ = true;
                    holdStart_ = nowMs;
                } else if (heldLongEnough(nowMs)) {
                    requestCameraStop(true);
                }
            } else {
                holdingChord_ = false;
            }
            return finish(result);
        }

        if (menuOpen()) {
            if (selectedSlot_ < 0) {
                // Keyboard opening captures the first connected pad immediately,
                // but it cannot navigate until all held controls are released.
                for (unsigned i = 0; i < pads.size(); ++i) {
                    if (pads[i].connected) {
                        selectedSlot_ = static_cast<int>(i);
                        phase_ = Phase::Open;
                        break;
                    }
                }
            }
            if (selectedSlot_ >= 0) {
                const auto& pad = pads[static_cast<unsigned>(selectedSlot_)];
                if (!navigationReady_) {
                    if (neutral(pad)) navigationReady_ = true;
                } else if (chordDown(pad)) {
                    if (!holdingChord_) {
                        holdingChord_ = true;
                        holdStart_ = nowMs;
                    } else if (heldLongEnough(nowMs)) {
                        setMenuOpen(false);
                        result.menuClosed = true;
                    }
                } else {
                    holdingChord_ = false;
                }
            }
            return finish(result);
        }

        if (phase_ == Phase::Candidate) {
            const auto& pad = pads[static_cast<unsigned>(selectedSlot_)];
            if (!chordDown(pad)) {
                beginDrain();
                drainFrom(pads);
            } else if (heldLongEnough(nowMs)) {
                mode_ = Mode::Menu;
                phase_ = Phase::Open;
                holdingChord_ = false;
                navigationReady_ = false;
                result.menuOpened = true;
            }
            return finish(result);
        }

        for (unsigned i = 0; i < pads.size(); ++i) {
            if (pads[i].connected && armed_[i] && chordDown(pads[i])) {
                selectedSlot_ = static_cast<int>(i);
                phase_ = Phase::Candidate;
                armed_[i] = false;
                holdStart_ = nowMs;
                holdingChord_ = true;
                // Suppression starts with the complete chord, before the menu
                // opens. Individual stick clicks are never captured here.
                break;
            }
        }
        return finish(result);
    }

    // A filtering hook may finish a drain as soon as its ORIGINAL state is
    // neutral. It must not feed filtered zero states back into this method.
    // This only relinquishes ownership; visibility/events stay on the UI path.
    void observeForDrain(unsigned slot, const PadSnapshot& pad) noexcept {
        if (phase_ == Phase::Draining && selectedSlot_ == static_cast<int>(slot) &&
            (!pad.connected || neutral(pad))) {
            clearOwnership();
        }
    }

    bool shouldSuppress(unsigned slot) const noexcept {
        return selectedSlot_ == static_cast<int>(slot) && phase_ != Phase::Idle;
    }
    // Free camera permits only the game's pause/resume button after the entry
    // neutral barrier. Menu, restoration and release-drain emit no buttons.
    std::uint16_t capturedButtonsForGame(unsigned slot, std::uint16_t buttons) const noexcept {
        return cameraActive() && navigationReady_ && shouldSuppress(slot)
            ? static_cast<std::uint16_t>(buttons & PauseButton) : 0;
    }
    bool menuOpen() const noexcept { return mode_ == Mode::Menu; }
    bool cameraActive() const noexcept { return mode_ == Mode::FreeCamera; }
    bool cameraStopping() const noexcept { return mode_ == Mode::Stopping; }
    std::uint64_t cameraSession() const noexcept { return cameraSession_; }
    CameraStopReason cameraStopReason() const noexcept { return stopReason_; }
    bool cameraReturnsToMenu() const noexcept { return cameraStopping() && stopReturnToMenu_; }
    bool navigationReady() const noexcept { return (menuOpen() || cameraActive()) && navigationReady_; }
    int selectedSlot() const noexcept { return selectedSlot_; }
    bool candidate() const noexcept { return phase_ == Phase::Candidate; }
    bool draining() const noexcept { return phase_ == Phase::Draining; }

    static bool neutral(const PadSnapshot& pad) noexcept {
        return pad.connected && pad.buttons == 0 &&
            pad.leftTrigger <= TriggerThreshold && pad.rightTrigger <= TriggerThreshold &&
            insideDeadzone(pad.leftX, pad.leftY, LeftDeadzone) &&
            insideDeadzone(pad.rightX, pad.rightY, RightDeadzone);
    }

private:
    enum class Mode { None, Menu, FreeCamera, Stopping };
    enum class Phase { Idle, Candidate, Open, Draining };
    Phase phase_ = Phase::Idle;
    Mode mode_ = Mode::None;
    bool navigationReady_ = false;
    bool focused_ = false;
    bool holdingChord_ = false;
    int selectedSlot_ = -1;
    std::uint64_t holdStart_ = 0;
    std::array<bool, 4> armed_{};
    std::array<bool, 4> connected_{};
    std::uint64_t nextCameraSession_ = 0;
    std::uint64_t cameraSession_ = 0;
    bool stopReturnToMenu_ = false;
    CameraStopReason stopReason_ = CameraStopReason::None;

    static bool insideDeadzone(std::int16_t x, std::int16_t y, int limit) noexcept {
        const std::int64_t wideX = x, wideY = y;
        return wideX * wideX + wideY * wideY <= static_cast<std::int64_t>(limit) * limit;
    }
    static bool chordDown(const PadSnapshot& pad) noexcept {
        return (pad.buttons & ToggleChord) == ToggleChord;
    }
    bool heldLongEnough(std::uint64_t nowMs) const noexcept {
        // A backwards clock cannot accidentally satisfy an unsigned subtraction.
        return nowMs >= holdStart_ && nowMs - holdStart_ >= HoldMilliseconds;
    }
    void clearOwnership() noexcept {
        phase_ = Phase::Idle;
        selectedSlot_ = -1;
        navigationReady_ = false;
        holdingChord_ = false;
    }
    void beginDrain() noexcept {
        navigationReady_ = false;
        holdingChord_ = false;
        // A chord held on a second pad while this menu was open must not become
        // an implicit handoff when the selected pad relinquishes ownership.
        armed_.fill(false);
        if (selectedSlot_ >= 0) {
            phase_ = Phase::Draining;
        } else {
            phase_ = Phase::Idle;
        }
    }
    void drainFrom(const Pads& pads) noexcept {
        if (phase_ != Phase::Draining || selectedSlot_ < 0) return;
        const auto& pad = pads[static_cast<unsigned>(selectedSlot_)];
        if (!pad.connected || neutral(pad)) clearOwnership();
    }
    ControllerUpdate finish(ControllerUpdate result) const noexcept {
        result.navigationReady = navigationReady();
        result.selectedSlot = selectedSlot_;
        return result;
    }
};

} // namespace ce
