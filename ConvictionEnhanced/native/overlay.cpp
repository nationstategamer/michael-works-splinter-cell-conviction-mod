#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <atomic>
#include <array>
#include <chrono>
#include <algorithm>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"
#include "MinHook.h"
#include "controller_input.h"
#include "free_camera.h"
#include "hud_control.h"
#include "interface_preferences.h"
#include "controller_startup.h"
#include "branding.h"
#include "checkpoint_control.h"
#include "practice_storage.h"
#include "exploration_control.h"
#include "exploration_marker_math.h"
#ifdef CE_TEST_HOST
#include "imgui_internal.h"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace fs=std::filesystem;
namespace {
HMODULE moduleHandle{};
fs::path root, store, statusFile, pendingFile;
std::recursive_mutex guiMutex;
std::once_flag initFlag;
std::atomic<bool> visible{false}, ready{false}, resetting{false}, faulted{false};
bool initialized=false, consumed[256]{}, hotkeyOwned=false;
int requestedPanel=-1,activePanel=0;
constexpr int MenuPageCount=7;
bool focusFirstItem=false;
ce::MenuPadFrame menuPad;
WORD previousPadButtons=0;
WORD previousCameraButtons=0;
int cameraSpeed=1;
bool showCameraHint=true;
ce::InterfacePreferences interfacePreferences;
std::string interfaceSaveStatus="HUD choices save automatically.";
int selectedCheckpoint=-1,confirmCheckpoint=-1,selectedMissionPart=-1;
bool confirmMissionPart=false;
std::uint64_t checkpointGeneration=0,confirmCheckpointGeneration=0;
bool checkpointConfirmation=false,checkpointConfirmReleased=false;
std::string checkpointMessage;
enum class ExplorationChoice {None,SaveReturn,Teleport,Return};
ExplorationChoice explorationChoice=ExplorationChoice::None;
bool explorationConfirmation=false,explorationConfirmReleased=false;
std::uint64_t confirmExplorationGeneration=0;
std::string explorationMessage;
std::uint64_t explorationCameraSession=0;
std::chrono::steady_clock::time_point cameraFrame{};
unsigned long long diagnosticFrame=0;
#ifdef CE_TEST_HOST
bool testControllerFocused=true;
unsigned testFirstItemIds[MenuPageCount]{};
bool testExploreMarkerDrawn=false,testExploreMarkerOnScreen=false;
#endif
HWND gameWindow{};
WNDPROC previousWndProc{};
IDirect3DDevice9* gameDevice{};
using PresentFn=HRESULT(WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using ResetFn=HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
PresentFn originalPresent{};
ResetFn originalReset{};
void* presentAddress{};
void* resetAddress{};
unsigned long long frames=0, resets=0;
float frameMs=0;
std::chrono::steady_clock::time_point previousFrame{};
std::string message="Pause with Options first. Release the sticks after opening the menu.";
struct Selection { int fov=95; float deadzone=.15f; bool tactical=true,deniable=true,raw=false,blacklist=true; } selection;

std::string utf8(const std::wstring& value) {
    if(value.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);
    std::string out(n,0); WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),out.data(),n,nullptr,nullptr); return out;
}
void log(const std::string& value) noexcept {
    try { if(store.empty()) return;
        std::ofstream out(store/"overlay.log",std::ios::app); out<<value<<"\n";
    } catch(...) {}
}
void disableOnFailure() noexcept { ce::stopFreeCamera();ce::cancelCheckpointRequest();ce::cancelExplorationRequest();ce::restoreHud();visible=false;faulted=true;ce::abandonControllerInput();OutputDebugStringW(L"Michael Works Splinter Cell Conviction Mod overlay disabled after an internal error.\n"); }
struct QueueLock {
    HANDLE handle;fs::path path;
    ~QueueLock(){if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);DeleteFileW(path.c_str());}}
};
std::string read(const wchar_t* section,const wchar_t* key,const wchar_t* fallback=L"") {
    wchar_t value[2048]{}; GetPrivateProfileStringW(section,key,fallback,value,2048,statusFile.c_str()); return utf8(value);
}
std::string sha256(const fs::path& path) {
    std::ifstream file(path,std::ios::binary); if(!file) return {};
    BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    if(BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)<0) { BCryptCloseAlgorithmProvider(alg,0);return {}; }
    std::array<char,65536> chunk{}; bool ok=true;
    while(file) { file.read(chunk.data(),chunk.size()); auto n=file.gcount();
        if(n && BCryptHashData(hash,(PUCHAR)chunk.data(),(ULONG)n,0)<0) {ok=false;break;} }
    std::array<unsigned char,32> bytes{};
    if(BCryptFinishHash(hash,bytes.data(),32,0)<0) ok=false;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0);
    if(!ok) return {};
    std::ostringstream result; for(auto b:bytes) result<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)b; return result.str();
}
bool safeDirectory(const fs::path& p) {
    for(auto check=p; !check.empty() && check!=check.root_path(); check=check.parent_path()) {
        auto attributes=GetFileAttributesW(check.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    } return true;
}
void loadSelection() {
    selection.fov=GetPrivateProfileIntW(L"Selection",L"FOV",95,statusFile.c_str());
    if(selection.fov!=90&&selection.fov!=95&&selection.fov!=100) selection.fov=95;
    auto dz=read(L"Selection",L"Deadzone",L"0.15");
    selection.deadzone=(dz=="0.2"||dz=="0.20")?.20f:(dz=="0.25"?.25f:.15f);
    selection.raw=read(L"Selection",L"MouseInput",L"standard")=="raw";
    selection.blacklist=read(L"Selection",L"Controls",L"blacklist")=="blacklist";
    selection.tactical=read(L"Selection",L"Tactical",L"0")=="1";
    selection.deniable=read(L"Selection",L"Deniable",L"0")=="1";
}
bool savePending() {
    if(read(L"Status",L"CanQueue")!="1") {message="Optional presets are not ready. Close the game and complete the local baseline import in Install.cmd.";return false;}
    auto revision=read(L"Status",L"Revision");
    if(revision.size()!=64 || revision.find_first_not_of("0123456789abcdef")!=std::string::npos) {message="Invalid configuration revision; queue refused.";return false;}
    if(!safeDirectory(store)||!safeDirectory(pendingFile)) {message="Redirected state path; queue refused.";return false;}
    auto lockPath=store/"operation.lock";
    HANDLE lock=CreateFileW(lockPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock==INVALID_HANDLE_VALUE) {message="Another configuration operation is running. Try again shortly.";return false;}
    QueueLock held{lock,std::move(lockPath)};
    bool ok=false;
    if(sha256(store/"active.json")!=revision) message="Saved configuration changed. Exit and use Launch Michael Works Mod to refresh.";
    else {
        std::ostringstream text;
        text<<"[Request]\nSchema=1\nBaseRevision="<<revision<<"\n\n[Selection]\nFOV="<<selection.fov
            <<"\nDeadzone="<<(selection.deadzone>.24f?"0.25":(selection.deadzone>.19f?"0.2":"0.15"))
            <<"\nControls="<<(selection.blacklist?"blacklist":"original")<<"\nMouseInput="<<(selection.raw?"raw":"standard")
            <<"\nTactical="<<selection.tactical<<"\nDeniable="<<selection.deniable<<"\n";
        auto temporary=store/(L"overlay-queue-"+std::to_wstring(GetCurrentProcessId())+L".tmp");
        HANDLE f=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f!=INVALID_HANDLE_VALUE) {
            auto data=text.str(); DWORD written{};
            ok=WriteFile(f,data.data(),(DWORD)data.size(),&written,nullptr)&&written==data.size()&&FlushFileBuffers(f);
            CloseHandle(f);
            if(ok) ok=MoveFileExW(temporary.c_str(),pendingFile.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
            if(!ok) DeleteFileW(temporary.c_str());
        }
        message=ok?"Queued. Exit the game, then open Launch Michael Works Mod.cmd to apply.":"Could not write the pending settings. Game settings were not changed.";
    }
    log(message); return ok;
}
bool navigation(WPARAM key) {
    return key==VK_TAB||key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN||key==VK_RETURN||key==VK_SPACE||key==VK_ESCAPE||key==VK_F1||key==VK_F2||key==VK_F3||key==VK_F4||key==VK_F5||key==VK_F6||key==VK_F7;
}
void showMenu(bool show,bool synchronizeController=true) {
    visible=show;
    previousPadButtons=0;
    if(synchronizeController)ce::setControllerMenuOpen(show);
    if(initialized) {ImGui::GetIO().ClearEventsQueue();ImGui::GetIO().ClearInputKeys();}
    if(show){loadSelection();focusFirstItem=true;}
    else {checkpointConfirmation=false;explorationConfirmation=false;}
}
LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    try {
        std::lock_guard<std::recursive_mutex> guard(guiMutex);
        bool down=msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN;
        bool up=msg==WM_KEYUP||msg==WM_SYSKEYUP;
        const bool repeat=(lp&(1L<<30))!=0;
        if(down&&wp==VK_F10&&hotkeyOwned)return 0;
        if(!faulted&&down&&wp==VK_F10&&(GetKeyState(VK_CONTROL)&0x8000)&&(GetKeyState(VK_SHIFT)&0x8000)&&!(GetKeyState(VK_MENU)&0x8000)) {
            if(!repeat) {hotkeyOwned=true;showMenu(!visible.load());return 0;}
            if(hotkeyOwned)return 0;
        }
        if(up&&wp==VK_F10&&hotkeyOwned) {hotkeyOwned=false;return 0;}
        if(msg==WM_KILLFOCUS||(msg==WM_ACTIVATEAPP&&!wp)) {
            ce::stopFreeCamera();
            ce::cancelCheckpointRequest();
            ce::cancelExplorationRequest();
            ce::setControllerFocus(false);
            showMenu(false); hotkeyOwned=false;
            std::fill(std::begin(consumed),std::end(consumed),false);
        }
        if(!faulted&&down&&wp==VK_ESCAPE&&ce::freeCameraStatus().active) {
            consumed[VK_ESCAPE]=true;ce::requestControllerCameraStop(true);return 0;
        }
        if(wp<256&&up&&consumed[wp]) {
            consumed[wp]=false;
            if(initialized&&visible&&!faulted) ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp);
            return 0;
        }
        if(wp<256&&down&&consumed[wp]) {
            if(initialized&&visible&&!faulted&&!(wp>=VK_F1&&wp<=VK_F7))ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp);
            return 0;
        }
        if(!faulted&&visible&&msg==WM_KEYDOWN&&navigation(wp)&&(!repeat||consumed[wp])) {
            consumed[wp]=true;
            if(wp==VK_ESCAPE) {
                if(initialized&&ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp);
                else showMenu(false);
            }
            else if(wp>=VK_F1&&wp<=VK_F7){
                if(!initialized||!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)){
                    requestedPanel=(int)(wp-VK_F1);focusFirstItem=true;
                }
            }
            else if(initialized) ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp);
            return 0;
        }
        // Only complete keyboard presses owned by this menu are consumed. Mouse,
        // raw input and unowned system-key messages always continue.
        if(initialized && !faulted && (msg==WM_SETFOCUS||msg==WM_KILLFOCUS ||
           (visible&&(down||up)&&(wp==VK_SHIFT||wp==VK_CONTROL))))
            ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp);
    } catch(...) {disableOnFailure();}
    return CallWindowProcW(previousWndProc,hwnd,msg,wp,lp);
}
bool initializeGui(IDirect3DDevice9* device) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if(FAILED(device->GetCreationParameters(&creation))||!IsWindow(creation.hFocusWindow)) return false;
    DWORD pid{};GetWindowThreadProcessId(creation.hFocusWindow,&pid);
    if(pid!=GetCurrentProcessId()) return false;
    gameWindow=creation.hFocusWindow;
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.ConfigNavCursorVisibleAlways=true;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad|ImGuiConfigFlags_NoMouse|ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark(); auto& style=ImGui::GetStyle();style.WindowRounding=3;style.FrameRounding=2;
    style.Colors[ImGuiCol_TitleBgActive]=ImVec4(.13f,.22f,.12f,1);
    style.Colors[ImGuiCol_CheckMark]=ImVec4(.70f,.90f,.45f,1);
    if(!ImGui_ImplWin32_Init(gameWindow)||!ImGui_ImplDX9_Init(device)) {ImGui::DestroyContext();return false;}
    SetLastError(0);
    previousWndProc=(WNDPROC)SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,(LONG_PTR)windowProc);
    if(!previousWndProc) {ImGui_ImplDX9_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();return false;}
    gameDevice=device;initialized=true;loadSelection();log("Overlay renderer and keyboard message chain initialized.");return true;
}
void heading(const char* title) {ImGui::TextUnformatted(title);ImGui::Separator();ImGui::Spacing();}
bool wantsFirstFocus(int panel) {
    // SetSelected can leave the previous tab visible until the next frame. Do
    // not let that page consume the destination page's focus request.
    return focusFirstItem&&(requestedPanel<0||requestedPanel==panel);
}
void saveInterface() {
    const auto result=ce::saveInterfacePreferences(store/L"interface-preferences.ini",interfacePreferences);
    interfaceSaveStatus=result.ok?"Saved automatically for future launches.":"Could not save HUD choices: "+result.reason;
    if(!result.ok)log(interfaceSaveStatus);
}
void loadInterface() {
    const auto result=ce::loadInterfacePreferences(store/L"interface-preferences.ini");
    if(!result.ok){interfaceSaveStatus="Saved HUD choices could not be loaded: "+result.reason;log(interfaceSaveStatus);return;}
    interfacePreferences=result.value;showCameraHint=interfacePreferences.cameraHint;
    for(unsigned bit=1;bit<=64;bit<<=1)ce::setHudLayerHidden(static_cast<ce::HudLayer>(bit),(interfacePreferences.hiddenMask&bit)!=0);
    ce::setAllHudHidden(interfacePreferences.allHidden);
    interfaceSaveStatus=result.missing?"HUD choices save automatically when changed.":"Saved HUD choices loaded.";
    log(interfaceSaveStatus);
}
void restoreInterface() {
    ce::restoreHud();showCameraHint=true;interfacePreferences=ce::InterfacePreferences{};saveInterface();
}
void renderInterface() {
    heading("Interface visibility - saved automatically");
    auto hud=ce::hudStatus();
    ImGui::TextWrapped("%s",hud.reason);
    ImGui::TextWrapped("Choose what appears during gameplay. HUD choices are remembered between game launches.");
    ImGui::Spacing();
    bool allHidden=interfacePreferences.allHidden;
    ImGui::BeginDisabled(!hud.installed);
    if(wantsFirstFocus(4)&&hud.installed){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
    if(ImGui::Checkbox("Hide gameplay interface",&allHidden)){
        ce::setAllHudHidden(allHidden);interfacePreferences.allHidden=allHidden;saveInterface();
    }
#ifdef CE_TEST_HOST
    if(hud.installed)testFirstItemIds[4]=ImGui::GetItemID();
#endif
    ImGui::EndDisabled();
    // Keep individual choices while the master switch is active. Only controls
    // backed by the installed HUD backend are exposed as usable features.
    ImGui::BeginDisabled(!hud.installed||allHidden);
    const auto layer=[&](const char* label,ce::HudLayer kind) {
        const auto bit=static_cast<std::uint32_t>(kind);
        if(!(hud.supportedMask&bit))return;
        bool shown=(interfacePreferences.hiddenMask&bit)==0;
        if(ImGui::Checkbox(label,&shown)){
            ce::setHudLayerHidden(kind,!shown);
            if(shown)interfacePreferences.hiddenMask&=~bit;else interfacePreferences.hiddenMask|=bit;
            saveInterface();
        }
    };
    layer("Show crosshair",ce::HudLayer::Crosshair);
    layer("Show target and world markers",ce::HudLayer::TargetMarkers);
    layer("Show weapons and gadgets",ce::HudLayer::Inventory);
    layer("Show action prompts",ce::HudLayer::Prompts);
    layer("Show objectives and notifications",ce::HudLayer::Objectives);
    layer("Show health and detection indicators",ce::HudLayer::Status);
    layer("Show subtitles",ce::HudLayer::Subtitles);
    ImGui::EndDisabled();
    if(!hud.supportedMask)ImGui::TextDisabled("Individual HUD controls are not available yet.");
    ImGui::Separator();
    if(wantsFirstFocus(4)){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
    if(ImGui::Checkbox("Show free-camera control hint",&showCameraHint)){
        interfacePreferences.cameraHint=showCameraHint;saveInterface();
    }
#ifdef CE_TEST_HOST
    if(!hud.installed)testFirstItemIds[4]=ImGui::GetItemID();
#endif
    if(ImGui::Button("Restore all interface elements"))restoreInterface();
    ImGui::TextWrapped("%s",interfaceSaveStatus.c_str());
    ImGui::TextWrapped("The debug menu remains available with L3+R3 or Ctrl+Shift+F10. Reopen this page to restore hidden elements.");
}
bool checkpointControlsNeutral() {
    if(!menuPad.allPadsNeutral||menuPad.inputDraining||menuPad.cameraActive||menuPad.cameraStopping||hotkeyOwned)return false;
    for(bool held:consumed)if(held)return false;
    return true;
}
void startCamera();
void publishExplorationInput() {
    const auto protection=ce::practiceStorageStatus();
    ce::pollExplorationInput(gameWindow,menuPad.connectedMask,visible,checkpointControlsNeutral(),
        menuPad.inputDraining,menuPad.cameraActive||menuPad.cameraStopping,
        protection.active&&protection.healthy);
    const auto camera=ce::freeCameraStatus();
    ce::publishExplorationPreview(camera,camera.active&&camera.session==explorationCameraSession);
}
void renderExploration() {
    const auto scene=ce::explorationStatus();
    const auto camera=ce::freeCameraStatus();
    const auto protection=ce::practiceStorageStatus();
    const bool busy=scene.pending||scene.executing;
    if(!busy&&(explorationMessage=="Release the controls to save the return point."
        ||explorationMessage=="Release the controls while the destination is checked."))explorationMessage.clear();
    heading("Exploration - protected practice");
    ImGui::TextWrapped("%s",scene.reason);
    ImGui::TextWrapped("Fly to a destination, pause with Options, then hold L3+R3 to return to this menu. Sam moves only after you confirm.");
    ImGui::Text("Camera destination: %s",scene.hasCameraTarget?"selected":"not selected");
    if(scene.preview.state!=ce::ExplorationPreviewState::None){
        const auto color=scene.preview.state==ce::ExplorationPreviewState::Valid?ImVec4(.3f,1.f,.8f,1):
            scene.preview.state==ce::ExplorationPreviewState::Blocked?ImVec4(1.f,.4f,.3f,1):ImVec4(1.f,.85f,.35f,1);
        ImGui::TextColored(color,"Landing marker: %s",scene.preview.reason);
    }
    ImGui::Text("Return point: %s",scene.hasReturnPoint?"saved for this scene":"saved automatically before the first move");
    if(protection.active)ImGui::TextColored(ImVec4(.75f,.92f,.5f,1),"Practice saves are active until you exit the game.");
    else ImGui::TextWrapped("Saving a return point or moving enables separate practice saves until you exit and relaunch.");
    const bool choose=scene.available&&!busy&&camera.available&&menuPad.selectedSlot>=0&&menuPad.navigationReady;
    ImGui::Spacing();ImGui::BeginDisabled(!choose);
    if(wantsFirstFocus(6)&&choose){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
    if(ImGui::Button("Choose destination with free camera"))startCamera();
#ifdef CE_TEST_HOST
    testFirstItemIds[6]=ImGui::GetItemID();
#endif
    ImGui::EndDisabled();
    if(wantsFirstFocus(6)&&!choose)focusFirstItem=false;
    auto confirm=[&](ExplorationChoice choice){
        explorationChoice=choice;confirmExplorationGeneration=scene.generation;
        explorationConfirmation=true;explorationConfirmReleased=false;explorationMessage.clear();
        ImGui::OpenPopup("Confirm exploration action?");
    };
    const bool canMove=scene.available&&scene.currentSafe&&!busy&&protection.installed&&protection.healthy;
    ImGui::BeginDisabled(!canMove);
    if(ImGui::Button(scene.hasReturnPoint?"Replace return point###ExplorationSaveReturn":"Save return point###ExplorationSaveReturn"))confirm(ExplorationChoice::SaveReturn);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!canMove||!scene.hasCameraTarget);
    if(ImGui::Button("Teleport to camera position"))confirm(ExplorationChoice::Teleport);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!canMove||!scene.hasReturnPoint||!scene.hasReturnSupport);
    if(ImGui::Button("Return to saved position"))confirm(ExplorationChoice::Return);
    ImGui::EndDisabled();
    if(!explorationMessage.empty())ImGui::TextWrapped("%s",explorationMessage.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("The landing ring marks the checked ground. Look down while flying to see the spot below the camera. Blocked positions are refused.");
    ImGui::TextWrapped("Let Sam stop for three seconds before pausing to save, move or return. The return point resets after a checkpoint or scene change.");
    ImGui::TextWrapped("Returning restores your position. Use Mission checkpoints to reset mission progress.");
    ImGui::TextWrapped("You can return standing or crouched if there is enough room at the saved spot.");
    const auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(500.f,display.x-40),0),ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("Confirm exploration action?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        if(!explorationConfirmation)ImGui::CloseCurrentPopup();
        else {
            const bool firstReleased=!explorationConfirmReleased&&checkpointControlsNeutral();
            if(firstReleased)explorationConfirmReleased=true;
            const bool sameScene=scene.available&&scene.generation==confirmExplorationGeneration;
            const char* description=explorationChoice==ExplorationChoice::SaveReturn?
                (scene.hasReturnPoint?"Replace your saved return point with Sam's current position?":"Save Sam's current position as the return point?"):
                explorationChoice==ExplorationChoice::Teleport?"Move Sam to nearby ground beneath the selected camera position?":"Move Sam back to the saved return point?";
            ImGui::TextWrapped("%s",description);
            ImGui::TextWrapped("Original saves will be protected. Practice saving remains active for the rest of this session, including if a destination is blocked.");
            if(explorationChoice==ExplorationChoice::Teleport)ImGui::TextWrapped("The first safe return point is kept until you explicitly replace it or the scene changes.");
            if(!sameScene)ImGui::TextWrapped("The scene changed. Cancel and choose the action again.");
            if(!explorationConfirmReleased)ImGui::TextDisabled("Release the controls to continue.");
            if(!explorationMessage.empty())ImGui::TextWrapped("%s",explorationMessage.c_str());
            const bool cancel=ImGui::IsKeyPressed(ImGuiKey_Escape)||ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight);
            ImGui::BeginDisabled(!explorationConfirmReleased);
            if(firstReleased)ImGui::SetKeyboardFocusHere();
            if(ImGui::Button("Cancel")||cancel){explorationConfirmation=false;ImGui::CloseCurrentPopup();}
            if(ImGui::IsWindowAppearing())ImGui::SetItemDefaultFocus();
            ImGui::SameLine();ImGui::BeginDisabled(!sameScene||!protection.installed||!protection.healthy);
            if(ImGui::Button(protection.active?"Confirm":"Start protected exploration")) {
                if(!ce::activatePracticeStorage())explorationMessage=ce::practiceStorageStatus().reason;
                else {
                    publishExplorationInput();
                    const bool save=explorationChoice==ExplorationChoice::SaveReturn;
                    const bool queued=save?ce::requestExplorationSaveReturn(confirmExplorationGeneration):
                        explorationChoice==ExplorationChoice::Teleport?ce::requestExplorationTeleport(confirmExplorationGeneration):
                        ce::requestExplorationReturn(confirmExplorationGeneration);
                    if(queued) {
                        explorationConfirmation=false;ImGui::CloseCurrentPopup();
                        if(save)explorationMessage="Release the controls to save the return point.";
                        else {showMenu(false);explorationMessage="Release the controls while the destination is checked.";}
                    } else explorationMessage=ce::explorationStatus().reason;
                }
            }
            ImGui::EndDisabled();ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}
const char* missionDisplayName(const char* key) {
    struct Name {const char* key;const char* title;};
    static constexpr Name names[]={
        {"01_Market","Merchant's Street Market"},{"02_Museum","Kobin's Mansion"},
        {"03_Airfield","Price Airfield"},{"04_FB-Coste","Diwaniya, Iraq"},
        {"05_WashingtonMonument","Washington Monument"},{"06_WindowlessBuilding","White Box Laboratories"},
        {"07_LincolnMemorial","Lincoln Memorial"},{"08_ThirdEchelon_A","Third Echelon HQ (Part 1)"},
        {"08_ThirdEchelon_B","Third Echelon HQ (Part 2)"},{"09_AbandonedReservoir_A","Michigan Ave. Reservoir (Part 1)"},
        {"09_AbandonedReservoir_B","Michigan Ave. Reservoir (Part 2)"},{"10_StreetsAndGarden_A","Downtown District (Part 1)"},
        {"10_StreetsAndGarden_B","Downtown District (Part 2)"},{"11_WhiteHouse_A","The White House (Part 1)"},
        {"11_WhiteHouse_B","The White House (Part 2)"}};
    for(const auto& name:names)if(std::string(key)==name.key)return name.title;
    return key;
}
void renderMission() {
    const auto scene=ce::checkpointStatus();
    const auto protection=ce::practiceStorageStatus();
    heading("Mission checkpoints - protected practice");
    ImGui::TextWrapped("%s",scene.reason);
    if(scene.mission[0])ImGui::Text("Mission: %s",missionDisplayName(scene.mission));
    ImGui::TextWrapped("%s",scene.partCount>1?"Choose a recorded checkpoint, or use Mission part to switch between parts. Starting and reached checkpoints are captured automatically.":"Choose a recorded checkpoint in the loaded campaign mission. Starting and reached checkpoints are captured automatically. Use Scene Select to change missions.");
    if(protection.active) {
        ImGui::TextColored(ImVec4(.75f,.92f,.5f,1),"Practice session: campaign saves are protected.");
        ImGui::TextWrapped("Practice progress stays separate until you exit the game. Relaunch to return to your campaign saves.");
    } else ImGui::TextWrapped("Starting a section enables separate practice saves for the rest of this game session.");
    if(!protection.installed||!protection.healthy)ImGui::TextWrapped("Save protection: %s",protection.reason);
    if(scene.persistent)ImGui::TextDisabled("Recorded checkpoints are available after relaunching.");
    const unsigned count=std::min(scene.sectionCount,32u),partCount=std::min(scene.partCount,2u);
    if(checkpointGeneration!=scene.generation) {
        checkpointGeneration=scene.generation;selectedCheckpoint=count?0:-1;selectedMissionPart=scene.episode;
        for(unsigned i=0;i<count;++i)if(scene.sections[i].id==scene.currentSection)selectedCheckpoint=static_cast<int>(i);
    }
    if(selectedCheckpoint<0||static_cast<unsigned>(selectedCheckpoint)>=count)selectedCheckpoint=count?0:-1;
    const bool busy=scene.pending||scene.loading||scene.partPending||scene.partLoading;
    const bool canSelect=scene.available&&count&&!busy;
    const bool canPart=scene.partsAvailable&&partCount>1&&!busy;
    const char* partLabel="Current part";bool partValid=false;
    for(unsigned i=0;i<partCount;++i)if(scene.parts[i].episode==selectedMissionPart){partLabel=scene.parts[i].label;partValid=true;}
    if(!partValid)selectedMissionPart=scene.episode;
    ImGui::Spacing();
    if(partCount>1) {
        ImGui::BeginDisabled(!canPart);
        if(wantsFirstFocus(5)&&canPart){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
        if(ImGui::BeginCombo("Mission part",partLabel)) {
            for(unsigned i=0;i<partCount;++i){
                const bool chosen=scene.parts[i].episode==selectedMissionPart;
                if(ImGui::Selectable(scene.parts[i].label,chosen))selectedMissionPart=scene.parts[i].episode;
                if(chosen)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
#ifdef CE_TEST_HOST
        if(canPart)testFirstItemIds[5]=ImGui::GetID("Mission part");
#endif
        ImGui::EndDisabled();
    }
    const bool anotherPart=partValid&&selectedMissionPart!=scene.episode;
    auto confirm=[&](bool part){
        confirmMissionPart=part;confirmCheckpoint=part?selectedMissionPart:scene.sections[selectedCheckpoint].id;
        confirmCheckpointGeneration=scene.generation;checkpointConfirmation=true;checkpointConfirmReleased=false;
        checkpointMessage.clear();ImGui::OpenPopup("Load protected mission state?");
    };
    if(anotherPart) {
        ImGui::TextWrapped("The selected part loads from its beginning. Its recorded checkpoints appear after loading.");
        ImGui::BeginDisabled(!canPart||!protection.installed||!protection.healthy);
        if(ImGui::Button("Load selected part"))confirm(true);
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(!canSelect);
        if(wantsFirstFocus(5)&&canSelect){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
        const char* selected=selectedCheckpoint>=0?scene.sections[selectedCheckpoint].label:"Load a campaign mission first";
        if(ImGui::BeginCombo("Checkpoint",selected)) {
            for(unsigned i=0;i<count;++i){
                ImGui::PushID(scene.sections[i].id);
                const bool chosen=selectedCheckpoint==static_cast<int>(i);
                if(ImGui::Selectable(scene.sections[i].label,chosen))selectedCheckpoint=static_cast<int>(i);
                if(chosen)ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
#ifdef CE_TEST_HOST
        if(canSelect&&!canPart)testFirstItemIds[5]=ImGui::GetID("Checkpoint");
#endif
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!canSelect||!protection.installed||!protection.healthy);
        if(ImGui::Button("Load selected checkpoint"))confirm(false);
        ImGui::EndDisabled();
    }
    if(wantsFirstFocus(5)&&!canSelect&&!canPart)focusFirstItem=false;
    if(!checkpointMessage.empty())ImGui::TextWrapped("%s",checkpointMessage.c_str());
    ImGui::Spacing();ImGui::TextWrapped("Pause with Options first. D-pad / Cross chooses a checkpoint; Circle cancels. Uncaptured checkpoints are unavailable.");
    const auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(500.f,display.x-40),0),ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("Load protected mission state?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        if(!checkpointConfirmation)ImGui::CloseCurrentPopup();
        else {
            const bool firstReleased=!checkpointConfirmReleased&&checkpointControlsNeutral();
            if(firstReleased)checkpointConfirmReleased=true;
            const bool sameScene=(confirmMissionPart?scene.partsAvailable:scene.available)&&scene.generation==confirmCheckpointGeneration;
            ImGui::TextWrapped("%s",confirmMissionPart?"This loads the selected mission part from its beginning and replaces current unsaved progress.":"This loads the recorded checkpoint and replaces current unsaved progress.");
            ImGui::TextWrapped("Campaign saves will be protected. Practice progress stays separate for the rest of this session.");
            if(!sameScene)ImGui::TextWrapped("The mission changed. Cancel and choose a checkpoint again.");
            if(!protection.healthy)ImGui::TextWrapped("Save protection: %s",protection.reason);
            if(!checkpointMessage.empty())ImGui::TextWrapped("%s",checkpointMessage.c_str());
            if(!checkpointConfirmReleased)ImGui::TextDisabled("Release the controls to continue.");
            const bool cancel=ImGui::IsKeyPressed(ImGuiKey_Escape)||ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight);
            ImGui::BeginDisabled(!checkpointConfirmReleased);
            if(firstReleased)ImGui::SetKeyboardFocusHere();
            if(ImGui::Button("Cancel")||cancel){checkpointConfirmation=false;ImGui::CloseCurrentPopup();}
            if(ImGui::IsWindowAppearing())ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            ImGui::BeginDisabled(!sameScene||!protection.installed||!protection.healthy);
            if(ImGui::Button("Start practice")) {
                if(!ce::activatePracticeStorage())checkpointMessage=ce::practiceStorageStatus().reason;
                else {
                    const auto freshProtection=ce::practiceStorageStatus();
                    ce::pollCheckpointInput(gameWindow,menuPad.connectedMask,visible,checkpointControlsNeutral(),
                        freshProtection.active&&freshProtection.healthy);
                    if(!(confirmMissionPart?ce::requestPart(confirmCheckpoint,confirmCheckpointGeneration):ce::requestSection(confirmCheckpoint,confirmCheckpointGeneration)))checkpointMessage=ce::checkpointStatus().reason;
                    else {
                        checkpointMessage.clear();
                        ce::stopFreeCamera();ce::requestControllerCameraStop(false);
                        ImGui::CloseCurrentPopup();checkpointConfirmation=false;showMenu(false);
                    }
                }
            }
            ImGui::EndDisabled();ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}
void startCamera() {
    auto session=ce::beginControllerFreeCamera();
    if(!session){message="Connect the controller and release its controls before starting the camera.";return;}
    if(!ce::beginFreeCamera(session)) {
        ce::requestControllerCameraStop(true);ce::acknowledgeControllerCameraStopped(session);
        message="A live campaign or solo Deniable Ops camera is required.";return;
    }
    showMenu(false,false);cameraFrame={};previousCameraButtons=0;
    explorationCameraSession=activePanel==6?session:0;
    log("Free camera started; renderer outputs only. Controller capture retained.");
}
void updateCamera(bool focused) {
    auto state=ce::freeCameraStatus();
    if(menuPad.cameraActive&&(!focused||!state.active||!state.available))ce::requestControllerCameraStop(false);
    if(menuPad.cameraStopping||(!state.active&&menuPad.cameraActive)||!focused) {
        if(menuPad.cameraSession) {
            if(focused&&state.active&&menuPad.cameraStopping&&menuPad.cameraReturnsToMenu)
                ce::rememberExplorationCamera(state);
            ce::stopFreeCamera(menuPad.cameraSession);
            ce::acknowledgeControllerCameraStopped(menuPad.cameraSession);
            menuPad=ce::pollControllerInput(focused);
            log("Free camera stopped; normal renderer outputs restored.");
        }
        cameraFrame={};previousCameraButtons=0;return;
    }
    if(!menuPad.cameraActive){cameraFrame={};previousCameraButtons=0;return;}
    auto now=std::chrono::steady_clock::now();
    double dt=cameraFrame.time_since_epoch().count()?std::chrono::duration<double>(now-cameraFrame).count():0;
    cameraFrame=now;
    auto pad=menuPad.navigationReady?menuPad.gamepad:XINPUT_GAMEPAD{};
    WORD pressed=pad.wButtons&~previousCameraButtons;previousCameraButtons=pad.wButtons;
    if(pressed&XINPUT_GAMEPAD_B){ce::requestControllerCameraStop(false);return;}
    if(pressed&XINPUT_GAMEPAD_DPAD_UP)cameraSpeed=std::min(2,cameraSpeed+1);
    if(pressed&XINPUT_GAMEPAD_DPAD_DOWN)cameraSpeed=std::max(0,cameraSpeed-1);
    auto left=ce::normalizeStick(pad.sThumbLX,pad.sThumbLY,ce::ControllerState::LeftDeadzone);
    auto right=ce::normalizeStick(pad.sThumbRX,pad.sThumbRY,ce::ControllerState::RightDeadzone);
    ce::CameraMotion motion{left.x,left.y,ce::normalizedTrigger(pad.bRightTrigger)-ce::normalizedTrigger(pad.bLeftTrigger),right.x,right.y};
    if(!ce::moveFreeCamera(menuPad.cameraSession,motion,menuPad.navigationReady?dt:0,ce::CameraSpeeds[cameraSpeed]))ce::requestControllerCameraStop(false);
}
void pollControllerMenu() {
#ifdef CE_TEST_HOST
    const bool focused=testControllerFocused;
#else
    const bool focused=GetForegroundWindow()==gameWindow;
#endif
    menuPad=ce::pollControllerInput(focused);
    ce::pollControllerStartup(gameWindow,menuPad.connectedMask);
    updateCamera(focused);
    if(menuPad.available&&menuPad.menuOpen!=visible)showMenu(menuPad.menuOpen,false);
    const auto saveProtection=ce::practiceStorageStatus();
    ce::pollCheckpointInput(gameWindow,menuPad.connectedMask,visible,checkpointControlsNeutral(),
        saveProtection.active&&saveProtection.healthy);
    publishExplorationInput();
    auto& io=ImGui::GetIO();
    // Focus messages can arrive while the menu is hidden. Keep ImGui in sync
    // with the same actual foreground check used by controller ownership.
    io.AddFocusEvent(focused);
    if(menuPad.available&&menuPad.connected)io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags&=~ImGuiBackendFlags_HasGamepad;
    if(!visible){previousPadButtons=0;return;}
    XINPUT_GAMEPAD pad=menuPad.navigationReady?menuPad.gamepad:XINPUT_GAMEPAD{};
    const WORD pressed=pad.wButtons&~previousPadButtons;
    previousPadButtons=pad.wButtons;
    if((pressed&XINPUT_GAMEPAD_B)&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)) {
        showMenu(false);return;
    }
    if(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)) {
        if(pressed&XINPUT_GAMEPAD_LEFT_SHOULDER)requestedPanel=(activePanel+MenuPageCount-1)%MenuPageCount;
        if(pressed&XINPUT_GAMEPAD_RIGHT_SHOULDER)requestedPanel=(activePanel+1)%MenuPageCount;
        if(pressed&(XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER))focusFirstItem=true;
    }
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,(pad.wButtons&XINPUT_GAMEPAD_A)!=0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight,(pad.wButtons&XINPUT_GAMEPAD_B)!=0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,(pad.wButtons&XINPUT_GAMEPAD_DPAD_LEFT)||pad.sThumbLX < -12000);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight,(pad.wButtons&XINPUT_GAMEPAD_DPAD_RIGHT)||pad.sThumbLX > 12000);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,(pad.wButtons&XINPUT_GAMEPAD_DPAD_UP)||pad.sThumbLY > 12000);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,(pad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN)||pad.sThumbLY < -12000);
}
void renderMenu() {
    const auto size=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(670.f,size.x-30),std::min(530.f,size.y-30)),ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(size.x*.5f,size.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    if(focusFirstItem)ImGui::SetNextWindowFocus();
    ImGui::Begin(ce::DebugMenuTitle,nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
    ImGui::TextColored(ImVec4(.75f,.92f,.5f,1),"PAUSE WITH OPTIONS FIRST | Hold L3+R3 for 0.6s: toggle");
    ImGui::TextWrapped("D-pad / left stick: move | Cross: select | Circle: back / close | L1/R1: pages");
    ImGui::TextDisabled("Keyboard: Ctrl+Shift+F10 / Esc | Tab, arrows, Enter | F1-F7: pages");
    if(ImGui::BeginTabBar("MenuSections")) {
        if(ImGui::BeginTabItem("Settings",nullptr,requestedPanel==0?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=0;
            heading("Preset choices - edits require a game restart");
            const bool canQueuePresets=read(L"Status",L"CanQueue")=="1";
            if(!canQueuePresets)ImGui::TextWrapped("Optional presets need a local baseline import and setup. Camera, Interface and Exploration work independently.");
            int fov=(selection.fov-90)/5;
            if(wantsFirstFocus(0)){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
            if(ImGui::Combo("Base FOV",&fov,"90\0" "95\0" "100\0"))selection.fov=90+fov*5;
#ifdef CE_TEST_HOST
            testFirstItemIds[0]=ImGui::GetItemID();
#endif
            int dz=selection.deadzone>.24f?2:(selection.deadzone>.19f?1:0);
            if(ImGui::Combo("Stick deadzone",&dz,"0.15\0" "0.20\0" "0.25\0"))selection.deadzone=.15f+.05f*dz;
            ImGui::Checkbox("Blacklist-style controls",&selection.blacklist);
            ImGui::Checkbox("Raw mouse input (off = standard input)",&selection.raw);
            ImGui::Checkbox("Tactical stealth",&selection.tactical);
            ImGui::TextDisabled("Search: %ds | Out-of-combat recovery: %d/s after %ds",selection.tactical?60:40,selection.tactical?40:80,selection.tactical?3:0);
            ImGui::Checkbox("Deniable Ops",&selection.deniable);
            ImGui::TextDisabled("Solo Hunter reinforcements: %d | Last Stand interval: %ds",selection.deniable?3:2,selection.deniable?15:25);
            ImGui::Separator();
            ImGui::BeginDisabled(!canQueuePresets);
            if(ImGui::Button("Queue for next launch"))savePending();
            ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button("Reload saved values")){loadSelection();message="Reloaded saved values. Pending queue is unchanged.";}
            ImGui::TextWrapped("%s",message.c_str());
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Diagnostics",nullptr,requestedPanel==1?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=1;
            heading("Live overlay observations");
            ImGui::Text("Presentation rate: %.1f FPS | %.2f ms",frameMs>0?1000.f/frameMs:0,frameMs);
            ImGui::Text("Frames observed: %llu | Device resets: %llu",frames,resets);
            ImGui::Text("Fusion Fix loaded: %s",GetModuleHandleW(L"SplinterCellConviction.FusionFix.asi")?"yes":"not detected");
            ImGui::Text("Xidi support loaded: %s",GetModuleHandleW(L"Xidi.32.dll")?"yes":"not detected");
            ImGui::Text("Steam overlay loaded: %s",GetModuleHandleW(L"gameoverlayrenderer.dll")?"yes":"not detected");
            ImGui::Text("Controller menu: %s | Connected: %d | Input reads: %llu",menuPad.available?"ready":"unavailable - use keyboard",menuPad.connected,menuPad.gameReads);
            ImGui::TextWrapped("Controller startup: %s",ce::controllerStartupStatus().reason);
            const auto practice=ce::practiceStorageStatus();
            ImGui::Text("Practice saves: %s | Writes: %llu",practice.active?"isolated":practice.installed?"ready":"unavailable",practice.writes);
            ImGui::TextWrapped("Checkpoint: %s",ce::checkpointStatus().reason);
            const auto exploration=ce::explorationStatus();
            ImGui::Text("Exploration: %llu moves | %llu returns",exploration.moves,exploration.returns);
            ImGui::TextWrapped("Exploration: %s",exploration.reason);
            const auto camera=ce::freeCameraStatus();
            ImGui::Text("Camera hook: %s | Views: %llu | Overrides: %llu",camera.installed?"ready":"unavailable",camera.observations,camera.overrides);
            ImGui::TextWrapped("Camera: %s",camera.reason);
            const auto hud=ce::hudStatus();
            ImGui::Text("HUD control: %s | Observations: %llu | Suppressed: %llu",hud.installed?"ready":"unavailable",hud.observations,hud.suppressed);
            ImGui::TextWrapped("HUD: %s",hud.reason);
            if(menuPad.selectedSlot>=0)ImGui::Text("Menu controller: %d | %s",menuPad.selectedSlot+1,menuPad.navigationReady?"ready":"release controls to navigate");
            ImGui::Spacing();heading("Saved diagnostics snapshot - not live engine values");
            ImGui::Text("Checked (UTC): %s",read(L"Status",L"CheckedAtUTC",L"unknown").c_str());
            ImGui::Text("Compatibility: %s | File integrity: %s",read(L"Status",L"Compatible")=="1"?"supported":"not verified",read(L"Status",L"Integrity",L"unknown").c_str());
            std::error_code pendingError;
            bool pending=fs::exists(pendingFile,pendingError);
            ImGui::Text("Queue file: %s",pendingError?"unavailable":(pending?"present - use Launch Michael Works Mod after exiting":"none"));
            ImGui::TextWrapped("%s",read(L"Status",L"Message").c_str());
            ImGui::TextWrapped("Controller input is held from the game while this menu owns it. FPS measures presentation intervals, not engine simulation time.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Help",nullptr,requestedPanel==2?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=2;
            heading(ce::ProductName);
            ImGui::TextUnformatted(ce::CopyrightNotice);
            ImGui::TextWrapped("Original mod work. Third-party components retain their own licenses.");
            ImGui::Text("Version %s",ce::OverlayVersion);
            ImGui::Spacing();
            heading("Keep the working controller configuration");
            ImGui::TextWrapped("Pause with Options, then hold both stick clicks (L3+R3) for 0.6 seconds. Release the controls, then use the D-pad or left stick and Cross. L1/R1 switch pages. Circle goes back or closes the menu.");
            ImGui::Spacing();ImGui::TextWrapped("The menu does not pause gameplay. Its controller input is blocked from the game until the menu closes and you release the controls. Mouse input still reaches the game. Losing focus or disconnecting the controller closes the menu.");
            ImGui::Spacing();ImGui::TextWrapped("Queue settings here, close the game normally, then run ConvictionEnhanced/Launch Michael Works Mod.cmd. A normal Steam launch does not apply the queue.");
            ImGui::Spacing();ImGui::TextWrapped("The launcher validates backups, file hashes and the queue revision before applying changes. Steam Input and the game's Use Controller setting are never changed by this overlay.");
            ImGui::Spacing();ImGui::TextWrapped("The Camera page provides free camera in campaign and solo Deniable Ops. Options pauses/resumes while flying. Circle returns to your character; holding L3+R3 returns to this menu. The world is not automatically frozen.");
            ImGui::Spacing();ImGui::TextWrapped("The Interface page hides supported gameplay HUD elements and the free-camera hint immediately. Choices save automatically between launches. Restore all interface elements restores visibility and saves that choice too.");
            ImGui::Spacing();ImGui::TextWrapped("The Mission page loads recorded campaign checkpoints in a protected practice session. New checkpoints are captured as you reach them. Exit and relaunch the game to return to normal campaign saves.");
            ImGui::Spacing();ImGui::TextWrapped("Exploration saves a return position and moves your character to a chosen free-camera destination. Fly from that page, pause, then hold L3+R3 to keep the destination and return to the menu. Moves require clear ground and separate practice saves.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Camera",nullptr,requestedPanel==3?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=3;heading("Free camera - campaign and solo Deniable Ops");
            const auto camera=ce::freeCameraStatus();
            ImGui::TextWrapped("%s",camera.reason);
            ImGui::TextWrapped("Fly independently of your character. Left stick: move | Right stick: look | R2/L2: rise/lower | D-pad up/down: speed.");
            ImGui::Spacing();
            if(wantsFirstFocus(3)){ImGui::SetKeyboardFocusHere();focusFirstItem=false;}
            ImGui::Combo("Travel speed",&cameraSpeed,"Slow\0Normal\0Fast\0");
#ifdef CE_TEST_HOST
            testFirstItemIds[3]=ImGui::GetItemID();
#endif
            const bool canStart=camera.installed&&camera.available&&menuPad.selectedSlot>=0&&menuPad.navigationReady;
            ImGui::BeginDisabled(!canStart);
            if(ImGui::Button("Start free camera"))startCamera();
            ImGui::EndDisabled();
            ImGui::TextWrapped("Release the controls after starting. Press Options to close the game's pause screen. Circle returns to your character; hold L3+R3 to return here. Escape is a keyboard fallback.");
            ImGui::Spacing();ImGui::TextWrapped("Losing focus, disconnecting the controller or changing the gameplay camera stops free camera. Co-op and competitive multiplayer camera modes are unavailable.");
            ImGui::TextDisabled("Fusion Fix FOV and the normal gameplay camera remain in control after exit.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Interface",nullptr,requestedPanel==4?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=4;renderInterface();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Mission",nullptr,requestedPanel==5?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=5;renderMission();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Exploration",nullptr,requestedPanel==6?ImGuiTabItemFlags_SetSelected:0)) {
            activePanel=6;renderExploration();ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        // Keep both selection and focus pending until ImGui actually displays
        // the requested page, including when leaving a read-only page.
        if(requestedPanel<0||activePanel==requestedPanel){
            if(activePanel!=0&&activePanel!=3&&activePanel!=4&&activePanel!=5&&activePanel!=6)focusFirstItem=false;
            requestedPanel=-1;
        }
    }
    ImGui::End();
}
void renderCameraHint() {
    if(!showCameraHint)return;
    ImGui::SetNextWindowPos(ImVec2(18,18),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.8f);
    ImGui::Begin("Free camera controls",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(.75f,.92f,.5f,1),"FREE CAMERA | %s",cameraSpeed==0?"Slow":cameraSpeed==2?"Fast":"Normal");
    ImGui::TextUnformatted("LS move | RS look | R2/L2 height | D-pad up/down speed");
    ImGui::TextUnformatted("Options pause/resume | Circle exit | Hold L3+R3 menu");
    if(!menuPad.navigationReady)ImGui::TextUnformatted("Release the controls to start moving.");
    ImGui::End();
}
bool explorationMarkerWanted() {
    const auto state=ce::explorationStatus();
    const auto camera=ce::freeCameraStatus();
    return state.preview.state!=ce::ExplorationPreviewState::None
        &&camera.available&&camera.controller==state.preview.controller&&camera.actor==state.preview.pawn
        &&((camera.active&&camera.session==explorationCameraSession)
            ||(visible&&activePanel==6)||state.pending||state.executing);
}
void renderExplorationMarker() {
    if(!explorationMarkerWanted())return;
    const auto scene=ce::explorationStatus();const auto& preview=scene.preview;
    const auto camera=ce::freeCameraStatus();const auto display=ImGui::GetIO().DisplaySize;
    const bool valid=preview.state==ce::ExplorationPreviewState::Valid;
    const bool blocked=preview.state==ce::ExplorationPreviewState::Blocked;
    const ImU32 color=valid?IM_COL32(70,245,195,255):blocked?IM_COL32(255,100,75,255):IM_COL32(255,210,80,255);
    const ce::MarkerViewport viewport{0,0,display.x,display.y};ce::MarkerProjection projection{};
    const bool view=camera.available&&camera.controller==preview.controller&&camera.actor==preview.pawn
        &&camera.renderViewValid&&ce::makeMarkerProjection(
        camera.horizontalFov,viewport,camera.correctUltrawideFov,camera.pixelAspect,&projection);
    bool onScreen=false,directionShown=false;
    if(view&&preview.hasFloor&&(valid||blocked)){
        ce::MarkerWorldPoint ground{preview.floorPoint.x,preview.floorPoint.y,preview.floorPoint.z+2};
        auto draw=ImGui::GetBackgroundDrawList();ce::ExplorationMarkerRing ring{};
        if(ce::makeExplorationMarkerRing(ground,preview.radius,&ring)){
            for(std::size_t n=0;n<ring.size();++n){
                ce::MarkerScreenPoint a{},b{};
                if(ce::projectExplorationMarker(camera.renderPose,projection,viewport,ring[n],&a)
                    &&ce::projectExplorationMarker(camera.renderPose,projection,viewport,ring[(n+1)%ring.size()],&b)){
                    draw->AddLine(ImVec2(a.x,a.y),ImVec2(b.x,b.y),IM_COL32(0,0,0,220),6);
                    draw->AddLine(ImVec2(a.x,a.y),ImVec2(b.x,b.y),color,3);
                }
            }
        }
        ce::MarkerScreenPoint center{};
        onScreen=ce::projectExplorationMarker(camera.renderPose,projection,viewport,ground,&center);
        if(onScreen){
            const ImVec2 p(center.x,center.y);
            draw->AddCircleFilled(p,7,IM_COL32(0,0,0,210));draw->AddCircle(p,7,color,16,2);
            ce::MarkerWorldPoint top=ground;top.z+=50;ce::MarkerScreenPoint pin{};
            if(ce::projectExplorationMarker(camera.renderPose,projection,viewport,top,&pin))
                draw->AddLine(p,ImVec2(pin.x,pin.y),color,3);
            const char* label=valid?"TELEPORT LANDING":"LANDING BLOCKED";
            const auto size=ImGui::CalcTextSize(label);
            const ImVec2 text(std::clamp(center.x+12,4.f,std::max(4.f,display.x-size.x-4)),
                std::clamp(center.y-size.y-8,4.f,std::max(4.f,display.y-size.y-4)));
            draw->AddRectFilled(ImVec2(text.x-4,text.y-2),ImVec2(text.x+size.x+4,text.y+size.y+2),IM_COL32(0,0,0,210),3);
            draw->AddText(text,color,label);
        }else{
            ce::MarkerBearing bearing{};
            if(ce::explorationMarkerBearing(camera.renderPose,projection,ground,&bearing)){
                const float halfX=std::max(1.f,display.x*.5f-34),halfY=std::max(1.f,display.y*.5f-34);
                const float edge=1.f/std::max(std::fabs(bearing.x)/halfX,std::fabs(bearing.y)/halfY);
                const ImVec2 tip(display.x*.5f+bearing.x*edge,display.y*.5f+bearing.y*edge);
                const ImVec2 base(tip.x-bearing.x*17,tip.y-bearing.y*17);
                draw->AddTriangleFilled(tip,ImVec2(base.x-bearing.y*8,base.y+bearing.x*8),
                    ImVec2(base.x+bearing.y*8,base.y-bearing.x*8),color);
                directionShown=true;
            }
        }
    }
    ImGui::SetNextWindowPos(ImVec2(18,std::max(18.f,display.y-18)),ImGuiCond_Always,ImVec2(0,1));
    ImGui::SetNextWindowBgAlpha(.85f);
    ImGui::Begin("Exploration landing marker",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize
        |ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color),"%s",valid?"LANDING READY":blocked?"LANDING BLOCKED":"CHECKING LANDING");
    ImGui::TextUnformatted(preview.fromCamera?"Ground below the free camera":"Selected teleport destination");
    ImGui::TextWrapped("%s",preview.reason);
    if(valid&&!onScreen)ImGui::TextUnformatted(directionShown?"Follow the edge arrow; look down while flying.":"Look toward the landing spot to see the ground ring.");
    if(valid&&!view)ImGui::TextUnformatted("Waiting for the current camera projection.");
    ImGui::End();
#ifdef CE_TEST_HOST
    testExploreMarkerDrawn=true;testExploreMarkerOnScreen=valid&&onScreen;
#endif
}
// Render targets/depth/viewport are not fully covered by D3D9 state blocks.
// This scope restores them and ends our scene on every path, including exceptions.
struct OverlayScene {
    IDirect3DDevice9* device;
    IDirect3DSurface9* targets[4]{};IDirect3DSurface9* depth{};IDirect3DSurface9* back{};
    D3DVIEWPORT9 viewport{};D3DSURFACE_DESC backDescription{};DWORD count=0;bool scene=false,changed=false;
    explicit OverlayScene(IDirect3DDevice9* d):device(d){}
    bool begin() {
        if(FAILED(device->BeginScene()))return false;scene=true;
        D3DCAPS9 caps{};
        if(FAILED(device->GetDeviceCaps(&caps))||!caps.NumSimultaneousRTs||FAILED(device->GetViewport(&viewport)))return false;
        count=std::min<DWORD>(4,caps.NumSimultaneousRTs);
        if(FAILED(device->GetRenderTarget(0,&targets[0])))return false;
        for(DWORD n=1;n<count;++n){auto hr=device->GetRenderTarget(n,&targets[n]);if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return false;}
        auto depthResult=device->GetDepthStencilSurface(&depth);
        if(FAILED(depthResult)&&depthResult!=D3DERR_NOTFOUND)return false;
        if(FAILED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back)))return false;
        if(FAILED(back->GetDesc(&backDescription))||!backDescription.Width||!backDescription.Height)return false;
        changed=true;
        for(DWORD n=1;n<count;++n)if(FAILED(device->SetRenderTarget(n,nullptr)))return false;
        if(FAILED(device->SetDepthStencilSurface(nullptr))||FAILED(device->SetRenderTarget(0,back)))return false;
        return true;
    }
    ~OverlayScene() {
        if(changed){device->SetRenderTarget(0,targets[0]);for(DWORD n=1;n<count;++n)device->SetRenderTarget(n,targets[n]);device->SetDepthStencilSurface(depth);device->SetViewport(&viewport);}
        if(back)back->Release();for(auto target:targets)if(target)target->Release();if(depth)depth->Release();
        if(scene)device->EndScene();
    }
};
HRESULT WINAPI presentHook(IDirect3DDevice9* device,const RECT* src,const RECT* dst,HWND overrideWindow,const RGNDATA* dirty) {
    try {
        std::lock_guard<std::recursive_mutex> guard(guiMutex);
        if(!faulted&&!resetting&&!initialized) initializeGui(device);
        if(!faulted&&!resetting&&initialized&&device==gameDevice) {
            pollControllerMenu();
            auto now=std::chrono::steady_clock::now();
            if(previousFrame.time_since_epoch().count()) {float dt=std::chrono::duration<float,std::milli>(now-previousFrame).count();if(dt>0&&dt<1000)frameMs=frameMs?frameMs*.9f+dt*.1f:dt;}
            previousFrame=now;++frames;
#ifdef CE_TEST_HOST
            testExploreMarkerDrawn=false;testExploreMarkerOnScreen=false;
#endif
            if((visible||(showCameraHint&&ce::freeCameraStatus().active)||explorationMarkerWanted())&&SUCCEEDED(device->TestCooperativeLevel())) {
                OverlayScene scene(device);
                if(scene.begin()) {
                    ImGui_ImplDX9_NewFrame();ImGui_ImplWin32_NewFrame();
                    // The backbuffer may differ from the Win32 client size in
                    // windowed/scaled modes. World projection and overlay must
                    // use the pixels actually being rendered, including edges.
                    ImGui::GetIO().DisplaySize=ImVec2(static_cast<float>(scene.backDescription.Width),static_cast<float>(scene.backDescription.Height));
                    ImGui::NewFrame();
                    renderExplorationMarker();
                    if(visible)renderMenu();else if(ce::freeCameraStatus().active)renderCameraHint();
                    ImGui::Render();ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                }
            }
            if(frames-diagnosticFrame>=300) {
                diagnosticFrame=frames;auto camera=ce::freeCameraStatus();
                std::ofstream out(store/"camera-diagnostics.ini",std::ios::trunc);
                out<<"[Camera]\nInstalled="<<camera.installed<<"\nAvailable="<<camera.available<<"\nActive="<<camera.active<<"\nViews="<<camera.observations<<"\nOverrides="<<camera.overrides<<"\nController="<<std::hex<<camera.controller<<"\nActor="<<camera.actor<<"\nCamera="<<camera.camera<<std::dec<<"\nReason="<<camera.reason<<"\n";
                const auto hud=ce::hudStatus();
                std::ofstream hudOut(store/"hud-diagnostics.ini",std::ios::trunc);
                hudOut<<"[HUD]\nInstalled="<<hud.installed<<"\nAllHidden="<<hud.allHidden<<"\nHiddenMask="<<hud.hiddenMask<<"\nObservedMask="<<hud.observedMask<<"\nObservations="<<hud.observations<<"\nSuppressed="<<hud.suppressed<<"\nReason="<<hud.reason<<"\n";
                const auto startup=ce::controllerStartupStatus();
                std::ofstream inputOut(store/"controller-startup-diagnostics.ini",std::ios::trunc);
                inputOut<<"[ControllerStartup]\nInstalled="<<startup.installed<<"\nCompleted="<<startup.completed<<"\nRepaired="<<startup.repaired<<"\nAttempts="<<startup.attempts<<"\nConnectedMask="<<menuPad.connectedMask<<"\nMenuReady="<<menuPad.navigationReady<<"\nMenuOpen="<<menuPad.menuOpen<<"\nSelectedSlot="<<menuPad.selectedSlot<<"\nDraining="<<menuPad.inputDraining<<"\nAllNeutral="<<menuPad.allPadsNeutral<<"\nGameReads="<<menuPad.gameReads<<"\nFocused="<<(GetForegroundWindow()==gameWindow)<<"\nReason="<<startup.reason<<"\n";
                const auto checkpoint=ce::checkpointStatus();const auto practice=ce::practiceStorageStatus();
                std::ofstream missionOut(store/"checkpoint-diagnostics.ini",std::ios::trunc);
                missionOut<<"[Checkpoint]\nInstalled="<<checkpoint.installed<<"\nAvailable="<<checkpoint.available<<"\nPending="<<checkpoint.pending<<"\nLoading="<<checkpoint.loading<<"\nGeneration="<<checkpoint.generation<<"\nMission="<<checkpoint.mission<<"\nAct="<<checkpoint.act<<"\nEpisode="<<checkpoint.episode<<"\nCurrent="<<checkpoint.currentSection<<"\nCurrentHardCheckpoint="<<checkpoint.currentHardCheckpoint<<"\nPartsAvailable="<<checkpoint.partsAvailable<<"\nPartPending="<<checkpoint.partPending<<"\nPartLoading="<<checkpoint.partLoading<<"\nPartCount="<<checkpoint.partCount<<"\nTargetPartEpisode="<<checkpoint.targetPartEpisode<<"\nChoices="<<checkpoint.sectionCount<<"\nReason="<<checkpoint.reason<<"\n[PracticeSaves]\nInstalled="<<practice.installed<<"\nActive="<<practice.active<<"\nHealthy="<<practice.healthy<<"\nReads="<<practice.reads<<"\nWrites="<<practice.writes<<"\nDeletes="<<practice.deletes<<"\nBlocked="<<practice.blocked<<"\nReason="<<practice.reason<<"\n";
                const auto exploration=ce::explorationStatus();
                std::ofstream explorationOut(store/"exploration-diagnostics.ini",std::ios::trunc);
                explorationOut<<"[Exploration]\nInstalled="<<exploration.installed<<"\nAvailable="<<exploration.available
                    <<"\nCurrentSafe="<<exploration.currentSafe<<"\nHasTarget="<<exploration.hasCameraTarget<<"\nHasReturn="<<exploration.hasReturnPoint
                    <<"\nPending="<<exploration.pending<<"\nExecuting="<<exploration.executing<<"\nGeneration="<<exploration.generation
                    <<"\nMoves="<<exploration.moves<<"\nReturns="<<exploration.returns
                    <<"\nCurrent="<<exploration.current.x<<","<<exploration.current.y<<","<<exploration.current.z
                    <<"\nTarget="<<exploration.cameraTarget.x<<","<<exploration.cameraTarget.y<<","<<exploration.cameraTarget.z
                    <<"\nReturn="<<exploration.returnPoint.x<<","<<exploration.returnPoint.y<<","<<exploration.returnPoint.z
                    <<"\nCurrentHalfHeight="<<exploration.currentHalfHeight<<"\nHasReturnSupport="<<exploration.hasReturnSupport<<"\nReturnFloorZ="<<exploration.returnFloorZ
                    <<"\nPreviewState="<<static_cast<unsigned>(exploration.preview.state)<<"\nPreviewFloor="<<exploration.preview.floorPoint.x<<","<<exploration.preview.floorPoint.y<<","<<exploration.preview.floorPoint.z
                    <<"\nPreviewReason="<<exploration.preview.reason
                    <<"\nReason="<<exploration.reason<<"\n";
            }
        }
    } catch(...) {disableOnFailure();}
    return originalPresent(device,src,dst,overrideWindow,dirty);
}
HRESULT WINAPI resetHook(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* pp) {
    resetting=true;
    ce::stopFreeCamera();ce::requestControllerCameraStop(false);
    ce::cancelCheckpointRequest();
    ce::cancelExplorationRequest();
    try {std::lock_guard<std::recursive_mutex> guard(guiMutex);
        if(initialized&&device==gameDevice) ImGui_ImplDX9_InvalidateDeviceObjects();
    } catch(...) {disableOnFailure();}
    // Reset can dispatch window messages; never hold our mutex across the game call.
    HRESULT result=originalReset(device,pp);
    try {std::lock_guard<std::recursive_mutex> guard(guiMutex);
        if(!faulted&&initialized&&device==gameDevice&&SUCCEEDED(result)) {ImGui_ImplDX9_CreateDeviceObjects();++resets;}
    } catch(...) {disableOnFailure();}
    resetting=false;
    return result;
}
DWORD WINAPI worker(void*) {
    try {
    wchar_t modulePath[32768]{};GetModuleFileNameW(moduleHandle,modulePath,32768);
#ifdef CE_TEST_HOST
    wchar_t testRoot[32768]{};GetEnvironmentVariableW(L"CE_TEST_GAME_ROOT",testRoot,32768);root=testRoot;
#else
    root=fs::path(modulePath).parent_path().parent_path().parent_path().parent_path();
#endif
    store=root/L"ConvictionEnhanced-state";statusFile=store/L"overlay-status.ini";pendingFile=store/L"overlay-pending.ini";
    if(!safeDirectory(store)||!fs::is_directory(store)) return 0;
#ifndef CE_TEST_HOST
    if(sha256(root/L"src/system/conviction_game.exe")!="beebb33559e064ec2f78065a4e9bc8dfe8844fa8711ea4afbcb880616170936f" ||
       sha256(root/L"src/system/scripts/SplinterCellConviction.FusionFix.asi")!="41291418dd5b5e69c6758f81b06dff6ced8f54344661efc148a6146c5c084ce3") {
        log("Unsupported executable or Fusion Fix fingerprint: overlay disabled.");return 0;
    }
#endif
    log(std::string(ce::ProductName)+" overlay "+ce::OverlayVersion+": initializing protected exploration, checkpoints, camera and saved HUD controls.");
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=moduleHandle;wc.lpszClassName=L"CEPrivateDeviceProbe";
    RegisterClassW(&wc);
    HWND probe=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,moduleHandle,nullptr);
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);IDirect3DDevice9* device{};
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=probe;
    pp.BackBufferFormat=D3DFMT_UNKNOWN;pp.BackBufferWidth=64;pp.BackBufferHeight=64;
    HRESULT hr=E_FAIL;
    if(d3d)hr=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,probe,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
    if(SUCCEEDED(hr)) {
        auto table=*reinterpret_cast<void***>(device);presentAddress=table[17];resetAddress=table[16];
        auto mh=MH_Initialize();
        if((mh==MH_OK||mh==MH_ERROR_ALREADY_INITIALIZED) &&
           MH_CreateHook(presentAddress,presentHook,reinterpret_cast<void**>(&originalPresent))==MH_OK&&
           MH_CreateHook(resetAddress,resetHook,reinterpret_cast<void**>(&originalReset))==MH_OK) {
            MH_QueueEnableHook(presentAddress);MH_QueueEnableHook(resetAddress);
            if(MH_ApplyQueued()==MH_OK) {
                bool controller=ce::initializeControllerInput();
                ce::initializePracticeStorage(store);
                log(ce::practiceStorageStatus().reason);
                ce::initializeCheckpointControl(store);
                log(ce::checkpointStatus().reason);
                bool camera=ce::initializeFreeCamera();
                log(camera?"Renderer camera hook ready; awaiting a single-player view.":"Camera hook unavailable; existing settings and controller menu remain available.");
                bool hud=ce::initializeHudControl();
                log(hud?"HUD visibility controls ready.":"HUD controls unavailable; debug menu and free-camera hint controls remain available.");
                {std::lock_guard<std::recursive_mutex> guard(guiMutex);
                    const auto fusionIni=root/L"src/system/scripts/SplinterCellConviction.FusionFix.ini";
                    ce::initializeControllerStartup(GetPrivateProfileIntW(L"2INSTANCESPLITSCREEN",L"Enable",0,fusionIni.c_str())!=0);
                    ce::initializeExploration(GetPrivateProfileIntW(L"2INSTANCESPLITSCREEN",L"Enable",0,fusionIni.c_str())!=0);
                    log(ce::controllerStartupStatus().reason);log(ce::explorationStatus().reason);loadInterface();}
                ready=true;log(controller?"Controller menu ready (XInput 1.4 + 1.3). Pause, then hold L3+R3 for 0.6s.":"Controller capture unavailable; keyboard menu remains available.");
                log("Present/Reset hooks ready. Keyboard fallback: Ctrl+Shift+F10.");
            }
        }
        if(!ready) {MH_RemoveHook(presentAddress);MH_RemoveHook(resetAddress);log("Overlay hook setup failed; game input unchanged.");}
        device->Release();
    } else log("D3D9 probe failed; overlay disabled.");
    if(d3d)d3d->Release();if(probe)DestroyWindow(probe);UnregisterClassW(wc.lpszClassName,moduleHandle);
    } catch(...) {disableOnFailure();}
    return 0;
}
}
extern "C" __declspec(dllexport) void InitializeASI() {
    try {std::call_once(initFlag,[]{HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(t)CloseHandle(t);});}
    catch(...) {disableOnFailure();}
}
#ifdef CE_TEST_HOST
extern "C" __declspec(dllexport) BOOL CETestReady(){return ready;}
extern "C" __declspec(dllexport) void CETestCameraObserve(unsigned id,ce::CameraPose* pose,BOOL valid){ce::testCameraView(id,pose,valid!=FALSE);}
extern "C" __declspec(dllexport) void CETestCameraContextObserve(unsigned id,ce::CameraPose* pose,const char* map,const char* connection,BOOL split,BOOL primary){ce::testCameraContextView(id,pose,map,connection,split!=FALSE,primary!=FALSE);}
extern "C" __declspec(dllexport) BOOL CETestCheckpointAvailable(){return ce::checkpointStatus().available;}
extern "C" __declspec(dllexport) void CETestCameraClock(unsigned long long t){ce::testCameraClock(t);}
extern "C" __declspec(dllexport) void CETestCameraFov(float fov){ce::testCameraFov(fov);}
extern "C" __declspec(dllexport) void CETestCameraStart(){std::lock_guard<std::recursive_mutex> guard(guiMutex);startCamera();}
extern "C" __declspec(dllexport) BOOL CETestCameraActive(){return ce::freeCameraStatus().active;}
extern "C" __declspec(dllexport) unsigned CETestHudMask(){return ce::hudStatus().hiddenMask;}
extern "C" __declspec(dllexport) BOOL CETestHudAllHidden(){return ce::hudStatus().allHidden;}
extern "C" __declspec(dllexport) BOOL CETestHudHint(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return showCameraHint;}
extern "C" __declspec(dllexport) void CETestHudRestore(){std::lock_guard<std::recursive_mutex> guard(guiMutex);restoreInterface();}
extern "C" __declspec(dllexport) BOOL CETestVisible(){return visible;}
extern "C" __declspec(dllexport) void CETestShow(BOOL show){std::lock_guard<std::recursive_mutex> guard(guiMutex);showMenu(show!=FALSE);}
extern "C" __declspec(dllexport) int CETestQueuedEvents(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return ImGui::GetCurrentContext()->InputEventsQueue.Size;}
extern "C" __declspec(dllexport) int CETestPage(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return activePanel;}
extern "C" __declspec(dllexport) void CETestPad(unsigned slot,const XINPUT_STATE* state,BOOL connected){ce::testSetPad(slot,state,connected!=FALSE);}
extern "C" __declspec(dllexport) void CETestPadTime(unsigned long long time){ce::testSetControllerTime(time);ce::testExplorationClock(time);}
extern "C" __declspec(dllexport) void CETestPadFocus(BOOL focus){testControllerFocused=focus!=FALSE;ce::testCheckpointFocused(focus!=FALSE);ce::testExplorationFocused(focus!=FALSE);}
extern "C" __declspec(dllexport) DWORD CETestGamePad(unsigned entry,DWORD slot,XINPUT_STATE* state){return ce::testGameGetState(entry,slot,state);}
extern "C" __declspec(dllexport) unsigned CETestNavId(){return ImGui::GetCurrentContext()->NavId;}
extern "C" __declspec(dllexport) unsigned CETestPageFirstId(int page){std::lock_guard<std::recursive_mutex> guard(guiMutex);return page>=0&&page<MenuPageCount?testFirstItemIds[page]:0;}
extern "C" __declspec(dllexport) void CETestCheckpointView(unsigned id,int current,const int* ids,unsigned count,BOOL valid){ce::testCheckpointView(id,current,ids,count,valid!=FALSE);}
extern "C" __declspec(dllexport) unsigned CETestCheckpointDispatches(){return ce::testCheckpointDispatches();}
extern "C" __declspec(dllexport) int CETestCheckpointLast(){return ce::testCheckpointLastDispatch();}
extern "C" __declspec(dllexport) BOOL CETestCheckpointPending(){return ce::checkpointStatus().pending;}
extern "C" __declspec(dllexport) void CETestExploreView(unsigned id,float x,float y,float z,BOOL valid,BOOL safe){ce::testExplorationView(id,{x,y,z},valid!=FALSE,safe!=FALSE);}
extern "C" __declspec(dllexport) void CETestExploreContextView(unsigned id,float x,float y,float z,const char* map,const char* connection,BOOL split,BOOL safe){ce::testExplorationContextView(id,{x,y,z},map,connection,split!=FALSE,safe!=FALSE);}
extern "C" __declspec(dllexport) void CETestExploreTick(){ce::testExplorationTick();}
extern "C" __declspec(dllexport) void CETestExploreFailure(unsigned flags){ce::testExplorationFailure(flags);}
extern "C" __declspec(dllexport) void CETestExploreBody(float height,float currentFloor,float destinationFloor){ce::testExplorationBody(height,currentFloor,destinationFloor);}
extern "C" __declspec(dllexport) BOOL CETestExploreSupport(float* floorZ){const auto s=ce::explorationStatus();*floorZ=s.returnFloorZ;return s.hasReturnSupport;}
extern "C" __declspec(dllexport) unsigned CETestExploreNativeMoves(){return ce::testExplorationNativeMoves();}
extern "C" __declspec(dllexport) BOOL CETestExploreHasReturn(){return ce::explorationStatus().hasReturnPoint;}
extern "C" __declspec(dllexport) BOOL CETestExploreTarget(){return ce::explorationStatus().hasCameraTarget;}
extern "C" __declspec(dllexport) BOOL CETestExplorePending(){return ce::explorationStatus().pending;}
extern "C" __declspec(dllexport) BOOL CETestExploreMessageEmpty(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return explorationMessage.empty();}
extern "C" __declspec(dllexport) unsigned CETestExplorePreviewState(){return static_cast<unsigned>(ce::explorationStatus().preview.state);}
extern "C" __declspec(dllexport) void CETestExplorePreviewPoint(float* xyz){const auto s=ce::explorationStatus();xyz[0]=s.preview.floorPoint.x;xyz[1]=s.preview.floorPoint.y;xyz[2]=s.preview.floorPoint.z;}
extern "C" __declspec(dllexport) BOOL CETestExploreMarkerDrawn(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return testExploreMarkerDrawn;}
extern "C" __declspec(dllexport) BOOL CETestExploreMarkerOnScreen(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return testExploreMarkerOnScreen;}
extern "C" __declspec(dllexport) void CETestExploreCurrent(float* xyz){const auto s=ce::explorationStatus();xyz[0]=s.current.x;xyz[1]=s.current.y;xyz[2]=s.current.z;}
extern "C" __declspec(dllexport) void CETestExploreAnchor(float* xyz){const auto s=ce::explorationStatus();xyz[0]=s.returnPoint.x;xyz[1]=s.returnPoint.y;xyz[2]=s.returnPoint.z;}
extern "C" __declspec(dllexport) BOOL CETestPracticeActive(){return ce::practiceStorageStatus().active;}
extern "C" __declspec(dllexport) void CETestPartConfigure(int episode){ce::testCheckpointMissionPart(episode);}
extern "C" __declspec(dllexport) unsigned CETestPartDispatches(){return ce::testCheckpointPartDispatches();}
extern "C" __declspec(dllexport) int CETestPartLast(){return ce::testCheckpointLastPartDispatch();}
extern "C" __declspec(dllexport) BOOL CETestPartPending(){const auto s=ce::checkpointStatus();return s.partPending||s.partLoading;}
extern "C" __declspec(dllexport) int CETestSelectedPart(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return selectedMissionPart;}
extern "C" __declspec(dllexport) int CETestCheckpointChoice(){std::lock_guard<std::recursive_mutex> guard(guiMutex);const auto s=ce::checkpointStatus();return selectedCheckpoint>=0&&static_cast<unsigned>(selectedCheckpoint)<s.sectionCount?s.sections[selectedCheckpoint].id:-1;}
extern "C" __declspec(dllexport) BOOL CETestPopup(){return ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);}
extern "C" __declspec(dllexport) int CETestFov(){return selection.fov;}
extern "C" __declspec(dllexport) BOOL CETestPadReady(){return menuPad.navigationReady;}
extern "C" __declspec(dllexport) void CETestPadError(unsigned slot,DWORD error){ce::testSetPadError(slot,error);}
extern "C" __declspec(dllexport) void CETestPauseRead(){ce::testPauseNextRead();}
extern "C" __declspec(dllexport) BOOL CETestReadPaused(){return ce::testReadPaused();}
extern "C" __declspec(dllexport) void CETestReleaseRead(){ce::testReleaseRead();}
extern "C" __declspec(dllexport) BOOL CETestQueue(){std::lock_guard<std::recursive_mutex> guard(guiMutex);return savePending();}
extern "C" __declspec(dllexport) unsigned long long CETestFrames(){return frames;}
extern "C" __declspec(dllexport) unsigned long long CETestResets(){return resets;}
#endif
BOOL WINAPI DllMain(HMODULE h,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){moduleHandle=h;DisableThreadLibraryCalls(h);}return TRUE;
}
