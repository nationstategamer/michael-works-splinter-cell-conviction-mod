#include "controller_state.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using ce::ControllerState;
using ce::PadSnapshot;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
PadSnapshot pad(std::uint16_t buttons = 0) {
    PadSnapshot result;
    result.connected = true;
    result.buttons = buttons;
    return result;
}
struct Fixture {
    ControllerState state;
    ControllerState::Pads pads{};
    std::uint64_t now = 1000;
    Fixture() { pads[0] = pad(); step(); }
    ce::ControllerUpdate step(std::uint64_t elapsed = 0, bool focused = true) {
        now += elapsed;
        return state.update(now, focused, pads);
    }
    void beginChord(unsigned slot = 0) {
        pads[slot].buttons = ControllerState::ToggleChord;
        step();
    }
    void open() {
        beginChord();
        require(step(600).menuOpened, "A full chord did not open after 600 ms");
    }
    void neutralize(unsigned slot = 0) { pads[slot] = pad(); step(); }
};

void chordTimingAndPassThrough() {
    Fixture f;
    require(!f.state.shouldSuppress(0), "A neutral closed menu captured input");
    f.pads[0].buttons = ControllerState::LeftThumb;
    f.step(1000);
    require(!f.state.shouldSuppress(0), "A lone L3 click was captured");
    f.pads[0].buttons = ControllerState::RightThumb;
    f.step(1000);
    require(!f.state.shouldSuppress(0), "A lone R3 click was captured");
    f.beginChord();
    require(f.state.candidate() && f.state.shouldSuppress(0), "The complete chord was not captured immediately");
    require(!f.step(599).menuOpened && !f.state.menuOpen(), "The menu opened too early");
    require(f.step(1).menuOpened && f.state.menuOpen(), "The menu missed the 600 ms threshold");
    require(!f.state.navigationReady(), "The opening chord leaked into navigation");
    require(!f.step(5000).menuClosed, "Holding the opening chord toggled the menu closed again");
    f.neutralize();
    require(f.state.navigationReady(), "Releasing the opening chord did not enable navigation");
    f.beginChord();
    require(!f.step(599).menuClosed, "The closing chord fired before 600 ms");
    require(f.step(1).menuClosed && !f.state.menuOpen(), "The closing chord did not close the menu");
    require(f.state.draining() && f.state.shouldSuppress(0), "The closing chord leaked to gameplay");
    require(!f.step(5000).menuOpened, "The held closing chord reopened the menu");
    f.neutralize();
    require(!f.state.shouldSuppress(0), "Neutral release failed to restore input");
}

void abortedHoldDrainsEveryControl() {
    Fixture f;
    f.beginChord();
    f.pads[0].buttons = ControllerState::LeftThumb;
    f.step(250);
    require(f.state.draining() && f.state.shouldSuppress(0), "An aborted hold leaked the remaining click");
    f.pads[0] = pad(); f.pads[0].leftTrigger = 31; f.step();
    require(f.state.shouldSuppress(0), "A held trigger ended the drain");
    f.pads[0] = pad(); f.pads[0].rightTrigger = 31; f.step();
    require(f.state.shouldSuppress(0), "A held right trigger ended the drain");
    f.pads[0] = pad(); f.pads[0].leftX = 7850; f.step();
    require(f.state.shouldSuppress(0), "A left stick outside its deadzone ended the drain");
    f.pads[0] = pad(); f.pads[0].rightY = -8690; f.step();
    require(f.state.shouldSuppress(0), "A right stick outside its deadzone ended the drain");
    f.pads[0] = pad(); f.pads[0].leftX = 6000; f.pads[0].leftY = 6000; f.step();
    require(f.state.shouldSuppress(0), "A diagonal stick outside its radial deadzone ended the drain");
    f.pads[0] = pad(); f.pads[0].leftTrigger = 30; f.pads[0].rightTrigger = 30;
    f.pads[0].leftX = 7849; f.pads[0].rightY = -8689; f.step();
    require(!f.state.shouldSuppress(0), "Default XInput thresholds were not treated as neutral");
    require(!f.state.menuOpen(), "An aborted hold opened a menu");
}

void focusAndReconnectArming() {
    Fixture f;
    f.step(0, false);
    f.pads[0].buttons = ControllerState::ToggleChord;
    f.step(5000, true);
    require(!f.state.candidate() && !f.state.shouldSuppress(0), "A held chord armed on focus regain");
    f.step(5000);
    require(!f.state.menuOpen(), "A held chord across focus opened later");
    f.neutralize(); f.open();
    require(f.step(0, false).menuClosed, "Focus loss did not close the menu");
    require(f.state.shouldSuppress(0), "Focus loss released a held chord to gameplay");
    f.step(1000, true);
    require(!f.state.menuOpen(), "Refocusing with a held chord reopened the menu");
    f.neutralize();
    require(!f.state.shouldSuppress(0), "A neutral refocused controller did not release ownership");
    f.pads[0] = {}; f.step();
    f.pads[0] = pad(ControllerState::ToggleChord); f.step(); f.step(5000);
    require(!f.state.candidate() && !f.state.menuOpen(), "A held reconnect chord was accepted");
    f.neutralize(); f.open();
    f.pads[0] = {};
    require(f.step().menuClosed && !f.state.shouldSuppress(0), "Disconnect did not close and relinquish the selected pad");
}

void initialHeldChordAndBackwardsTime() {
    ControllerState state;
    ControllerState::Pads pads{};
    pads[0] = pad(ControllerState::ToggleChord);
    state.update(1000, true, pads); state.update(5000, true, pads);
    require(!state.candidate() && !state.menuOpen(), "Startup accepted a chord held before arming");
    pads[0] = pad(); state.update(6000, true, pads);
    pads[0].buttons = ControllerState::ToggleChord; state.update(7000, true, pads);
    require(!state.update(6500, true, pads).menuOpened, "A backwards clock opened the menu");
    require(state.update(7600, true, pads).menuOpened, "A clock correction lost a valid hold");
}

void keyboardOpenAndCircleDrain() {
    Fixture f;
    f.pads[0] = pad(0x1000); // Cross held before keyboard opening.
    f.state.setMenuOpen(true); f.step();
    require(f.state.shouldSuppress(0) && !f.state.navigationReady(), "Keyboard opening did not capture and wait for neutral");
    f.neutralize();
    require(f.state.navigationReady(), "Keyboard opening never became ready after neutral");
    f.pads[0] = pad(0x2000); // Circle is handled by ImGui/overlay, never this state.
    require(!f.step().menuClosed, "Controller state stole Circle from ImGui");
    f.state.setMenuOpen(false);
    require(f.state.draining() && f.state.shouldSuppress(0), "External close leaked held Circle");
    f.step(1000);
    require(f.state.shouldSuppress(0), "A time limit released Circle before neutral");
    f.state.observeForDrain(1, pad());
    require(f.state.shouldSuppress(0), "Another controller cleared the active drain");
    f.state.observeForDrain(0, pad(0x2000));
    require(f.state.shouldSuppress(0), "Hook observation cleared a nonneutral drain");
    f.state.observeForDrain(0, pad());
    require(!f.state.shouldSuppress(0), "Hook observation failed to clear a neutral drain");
    require(!f.state.navigationReady(), "Navigation stayed ready after close");
}

void multipleControllersStayIndependent() {
    Fixture f;
    f.pads[1] = pad(); f.step();
    f.beginChord(1);
    require(f.state.selectedSlot() == 1 && !f.state.shouldSuppress(0) && f.state.shouldSuppress(1),
        "The candidate did not exclusively own its starting pad");
    f.pads[0].buttons = ControllerState::ToggleChord;
    require(f.step(600).menuOpened && f.state.selectedSlot() == 1, "Another pad stole the opening chord");
    f.pads[0] = pad(0x1000); f.step();
    require(!f.state.shouldSuppress(0) && f.state.selectedSlot() == 1, "An active menu captured another controller");
    f.neutralize(1);
    f.pads[1] = {};
    require(f.step().menuClosed && f.state.selectedSlot() == -1, "Disconnect silently transferred an open menu to another pad");
    f.state.setMenuOpen(true); f.step();
    require(f.state.selectedSlot() == 0 && !f.state.navigationReady(), "Keyboard opening did not select the first connected pad");
    f.pads[1] = pad(); f.step();
    require(f.state.selectedSlot() == 0, "A later connection stole the keyboard-open menu");
}

void noControllerAndIdempotentVisibility() {
    ControllerState state;
    ControllerState::Pads pads{};
    state.setMenuOpen(true);
    auto result = state.update(100, true, pads);
    require(state.menuOpen() && result.selectedSlot == -1 && !result.navigationReady,
        "A keyboard-only menu required a controller");
    pads[3] = pad(0x1000); state.update(200, true, pads);
    require(state.shouldSuppress(3) && !state.navigationReady(), "Connecting into a keyboard menu leaked held input");
    pads[3] = pad(); state.update(300, true, pads);
    require(state.navigationReady(), "A connected controller failed to become navigation-ready");
    state.setMenuOpen(true);
    require(state.navigationReady(), "Idempotent open reset navigation");
    state.setMenuOpen(false); state.update(400, true, pads);
    pads[3].buttons = ControllerState::ToggleChord; state.update(500, true, pads);
    state.setMenuOpen(false);
    require(state.candidate() && state.shouldSuppress(3), "Idempotent closed synchronization cancelled a chord");
    require(state.update(1100, true, pads).menuOpened, "Idempotent close interrupted the hold timer");
}

void interruptedCandidateAndReopeningDrain() {
    Fixture f;
    f.beginChord();
    require(!f.step(200, false).menuClosed, "Focus loss reported closing a menu that was never open");
    require(f.state.draining() && f.state.shouldSuppress(0), "Focus loss did not drain an interrupted candidate");
    f.neutralize(); f.state.setMenuOpen(true); f.step();
    f.pads[0] = pad(0x2000); f.state.setMenuOpen(false); f.step();
    f.state.setMenuOpen(true); f.step();
    require(f.state.menuOpen() && f.state.shouldSuppress(0) && !f.state.navigationReady(),
        "Reopening during drain failed to retain ownership and wait for neutral");
    f.neutralize();
    require(f.state.navigationReady(), "Reopened menu remained blocked after neutral");
    f.state.setMenuOpen(false); f.pads[0] = {}; f.state.observeForDrain(0, f.pads[0]);
    require(!f.state.shouldSuppress(0), "Disconnect observation retained stale drain ownership");
}

void heldSecondPadCannotTakeOver() {
    Fixture f;
    f.pads[1] = pad(); f.step();
    f.open(); f.neutralize();
    f.pads[1].buttons = ControllerState::ToggleChord; f.step();
    f.state.setMenuOpen(false); f.step(); f.step(1000);
    require(!f.state.menuOpen() && !f.state.candidate() && !f.state.shouldSuppress(1),
        "A second pad's existing chord took over after closing");
    f.pads[1] = pad(); f.step(); f.beginChord(1);
    require(f.step(600).menuOpened && f.state.selectedSlot() == 1,
        "The second pad could not open after a deliberate neutral and new chord");
    f.neutralize(1); f.pads[0].buttons = ControllerState::ToggleChord; f.step();
    f.pads[1] = {}; f.step(); f.step(1000);
    require(!f.state.menuOpen() && !f.state.candidate(),
        "A second pad's held chord took over after selected-pad disconnection");
}
std::uint64_t beginCamera(Fixture& f) {
    f.state.setMenuOpen(true); f.step(); f.neutralize();
    f.pads[0].buttons = 0x1000; f.step(); // Cross selecting the menu action.
    const auto token = f.state.beginFreeCamera();
    require(token != 0 && f.state.cameraActive() && !f.state.menuOpen(), "Free camera did not replace menu mode");
    require(f.state.selectedSlot() == 0 && f.state.shouldSuppress(0), "Camera entry lost controller ownership");
    require(!f.state.navigationReady(), "Cross selecting free camera leaked into camera input");
    return token;
}

void cameraEntryAndRestoreAcknowledgment() {
    Fixture f;
    require(f.state.beginFreeCamera() == 0, "Closed gameplay unexpectedly entered free camera");
    const auto token = beginCamera(f);
    f.step(1000);
    require(!f.state.navigationReady(), "A held activation button became camera-ready");
    f.neutralize();
    require(f.state.navigationReady(), "Free camera did not become ready after neutral");
    f.pads[0].buttons = 0x2000; f.step(); // Circle handled by caller.
    f.state.requestCameraStop(false);
    require(f.state.cameraStopping() && !f.state.cameraActive() && !f.state.navigationReady(), "Stop did not suspend camera input");
    require(f.state.cameraSession() == token && f.state.shouldSuppress(0), "Pending restoration released input/session");
    require(!f.state.acknowledgeCameraStopped(token + 1), "Mismatched restoration token was accepted");
    f.neutralize(); f.step(10000);
    require(f.state.cameraStopping() && f.state.shouldSuppress(0), "Neutral/time released input before camera acknowledgment");
    f.pads[0].buttons = 0x2000; f.step();
    require(f.state.acknowledgeCameraStopped(token), "Matching restoration acknowledgment failed");
    require(f.state.draining() && f.state.shouldSuppress(0), "Held Circle leaked after camera restoration");
    require(!f.state.cameraStopping() && f.state.cameraSession() == 0, "Acknowledged camera session stayed active");
    f.neutralize();
    require(!f.state.shouldSuppress(0) && !f.state.menuOpen(), "Restored camera failed to return gameplay input");
    const auto next = beginCamera(f);
    require(next != token, "A later camera session reused a restoration token");
    f.state.requestCameraStop(false);
    require(!f.state.acknowledgeCameraStopped(token) && f.state.cameraStopping(), "Old acknowledgment released a later session");
}

void cameraChordReturnsToMenuAndActivationFailure() {
    Fixture f;
    const auto token = beginCamera(f);
    f.neutralize(); f.beginChord();
    f.step(599);
    require(f.state.cameraActive() && !f.state.cameraStopping(), "Camera chord stopped before hold threshold");
    f.step(1);
    require(f.state.cameraStopping() && f.state.cameraReturnsToMenu() && !f.state.menuOpen(), "Camera chord did not request asynchronous return to menu");
    f.step(10000);
    require(f.state.cameraStopping() && !f.state.menuOpen(), "Held camera chord bypassed restoration acknowledgment");
    require(f.state.acknowledgeCameraStopped(token), "Camera-to-menu acknowledgment failed");
    require(f.state.menuOpen() && f.state.shouldSuppress(0) && !f.state.navigationReady(), "Camera-to-menu transition leaked held chord");
    f.neutralize();
    require(f.state.navigationReady(), "Returned menu did not accept neutral input");
    const auto failedActivation = f.state.beginFreeCamera();
    require(failedActivation != 0, "Second camera reservation failed");
    f.state.requestCameraStop(true);
    require(f.state.acknowledgeCameraStopped(failedActivation) && f.state.menuOpen(), "Failed backend activation could not return to menu");
    require(f.state.shouldSuppress(0) && !f.state.navigationReady(), "Activation failure bypassed neutral barrier");
}

void cameraFocusDisconnectAndMenuRequests() {
    Fixture f;
    const auto token = beginCamera(f); f.neutralize();
    f.state.setMenuOpen(true);
    require(f.state.cameraStopping() && f.state.cameraReturnsToMenu(), "Keyboard menu opening bypassed camera restore");
    f.step(0, false);
    require(f.state.cameraStopping() && !f.state.cameraReturnsToMenu() &&
            f.state.cameraStopReason() == ce::CameraStopReason::FocusLost, "Focus loss did not force persistent camera exit");
    f.state.setMenuOpen(true); f.state.requestCameraStop(true);
    require(!f.state.cameraReturnsToMenu(), "A menu request overrode a forced focus-loss exit");
    f.step(1000, true); f.neutralize();
    require(f.state.cameraStopping() && f.state.shouldSuppress(0), "Focus regain resumed or released pending free camera");
    require(f.state.acknowledgeCameraStopped(token), "Focus-loss restore acknowledgment failed");
    f.neutralize();
    require(!f.state.menuOpen() && !f.state.shouldSuppress(0), "Focus-loss exit reopened a menu");

    const auto disconnected = beginCamera(f); f.neutralize();
    f.pads[1] = pad(); f.pads[0] = {}; f.step();
    require(f.state.cameraStopping() && f.state.selectedSlot() == 0 && f.state.cameraSession() == disconnected,
            "Disconnect discarded camera restoration ownership");
    require(f.state.cameraStopReason() == ce::CameraStopReason::Disconnected, "Disconnect stop reason was lost");
    f.pads[0] = pad(0x2000); f.step();
    require(f.state.cameraStopping() && f.state.shouldSuppress(0) && !f.state.shouldSuppress(1), "Reconnect resumed or transferred free camera");
    f.state.requestCameraStop(true);
    require(!f.state.cameraReturnsToMenu(), "Reconnection allowed forced exit to become menu return");
    require(f.state.acknowledgeCameraStopped(disconnected) && f.state.draining(), "Reconnected held input bypassed drain");
    f.neutralize();
    require(!f.state.cameraActive() && !f.state.shouldSuppress(0), "Reconnection auto-resumed free camera");

    const auto missing = beginCamera(f); f.pads[0] = {}; f.step();
    require(f.state.acknowledgeCameraStopped(missing) && f.state.selectedSlot() == -1,
            "Acknowledging a disconnected camera owner retained stale ownership");
    f.pads[0] = pad(ControllerState::ToggleChord); f.step(); f.step(2000);
    require(!f.state.cameraActive() && !f.state.menuOpen(), "A held reconnect chord reopened after camera exit");
}

void cameraRequiresValidController() {
    ControllerState state;
    ControllerState::Pads pads{};
    state.setMenuOpen(true); state.update(1000, true, pads);
    require(state.beginFreeCamera() == 0, "A keyboard-only menu started controller free camera without a pad");
    pads[3] = pad(); state.update(1100, true, pads);
    const auto token = state.beginFreeCamera();
    require(token && state.selectedSlot() == 3 && state.shouldSuppress(3) && !state.shouldSuppress(0),
            "Camera entry ignored nonzero controller ownership");
    state.setMenuOpen(false);
    require(state.cameraStopping() && !state.cameraReturnsToMenu(), "Keyboard close released free camera immediately");
    state.observeForDrain(3, pad());
    require(state.shouldSuppress(3) && state.cameraStopping(), "Drain helper bypassed camera acknowledgment");
}

void cameraPauseButtonPassThrough() {
    Fixture f;
    constexpr auto start = ControllerState::PauseButton;
    constexpr std::uint16_t everyButton = 0xffff;
    f.state.setMenuOpen(true); f.step();
    require(f.state.capturedButtonsForGame(0, everyButton) == 0, "Menu leaked a pause button");
    const auto session = beginCamera(f);
    f.pads[0].buttons = start; f.step();
    require(f.state.capturedButtonsForGame(0, everyButton) == 0, "Held pause button crossed camera entry barrier");
    f.neutralize();
    require(f.state.capturedButtonsForGame(0, everyButton) == start, "Ready camera did not allow only Options/Start");
    require(f.state.capturedButtonsForGame(1, everyButton) == 0, "Camera pause exception applied to another slot");
    require(f.state.capturedButtonsForGame(0, 0) == 0, "Pause release was not reflected");
    f.pads[0].buttons = start | 0x2000; f.step();
    require(f.state.capturedButtonsForGame(0, f.pads[0].buttons) == start, "Circle leaked alongside Options");
    f.state.requestCameraStop(false);
    require(f.state.capturedButtonsForGame(0, everyButton) == 0, "Pending camera exit left pause held");
    require(f.state.acknowledgeCameraStopped(session), "Pause test camera restoration failed");
    require(f.state.shouldSuppress(0) && f.state.capturedButtonsForGame(0, everyButton) == 0,
            "Held pause input escaped during restoration drain");
    f.neutralize();
    require(!f.state.shouldSuppress(0), "Pause release did not restore normal gameplay input");

    const auto focusSession = beginCamera(f); f.neutralize();
    f.pads[0].buttons = start; f.step();
    require(f.state.capturedButtonsForGame(0, start) == start, "Pause did not pass before focus loss");
    f.step(0, false);
    require(f.state.cameraStopping() && f.state.capturedButtonsForGame(0, start) == 0,
            "Focus loss failed to release the game's pause bit");
    f.state.acknowledgeCameraStopped(focusSession);
    f.step(1000, true);
    require(f.state.shouldSuppress(0) && f.state.capturedButtonsForGame(0, start) == 0,
            "Refocus replayed held pause before neutral");
    f.neutralize();
    require(!f.state.shouldSuppress(0), "Refocused pause release retained capture");
}
} // namespace

int main() {
    try {
        chordTimingAndPassThrough();
        abortedHoldDrainsEveryControl();
        focusAndReconnectArming();
        initialHeldChordAndBackwardsTime();
        keyboardOpenAndCircleDrain();
        multipleControllersStayIndependent();
        noControllerAndIdempotentVisibility();
        interruptedCandidateAndReopeningDrain();
        heldSecondPadCannotTakeOver();
        cameraEntryAndRestoreAcknowledgment();
        cameraChordReturnsToMenuAndActivationFailure();
        cameraFocusDisconnectAndMenuRequests();
        cameraRequiresValidController();
        cameraPauseButtonPassThrough();
        std::cout << "PASS: " << checks << " controller state checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
