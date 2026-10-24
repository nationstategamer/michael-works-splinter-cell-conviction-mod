#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "controller_input.h"
#include "controller_state.h"
#include "MinHook.h"
#include <array>
#include <atomic>
#include <cstring>

namespace ce {
namespace {
static_assert(ControllerState::PauseButton == XINPUT_GAMEPAD_START, "Pause button must match the XInput Start bit");
using GetState=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
struct PacketHistory {bool observed=false, virtualized=false;DWORD packet=0;XINPUT_GAMEPAD last{};};
struct Entry {void* address{};GetState original{};std::array<PacketHistory,4> packets{};};
std::array<Entry,2> entries{};
unsigned entryCount=0;
SRWLOCK inputLock=SRWLOCK_INIT;
struct Lock {Lock(){AcquireSRWLockExclusive(&inputLock);}~Lock(){ReleaseSRWLockExclusive(&inputLock);}};
ControllerState owner;
std::array<PadSnapshot,4> cached{};
std::array<XINPUT_GAMEPAD,4> rawPads{};
std::array<std::uint64_t,4> nextProbe{};
std::array<std::uint64_t,4> sampleGeneration{};
std::uint64_t focusEpoch=0;
std::atomic<bool> enabled{false},foreground{false};
std::atomic<unsigned long long> gameReads{0};
thread_local bool bypass=false;
thread_local unsigned callDepth=0;
#ifdef CE_TEST_HOST
SRWLOCK fixtureLock=SRWLOCK_INIT;
std::array<XINPUT_STATE,4> fixture{};
std::array<bool,4> fixtureConnected{};
std::array<DWORD,4> fixtureError{};
std::atomic<std::uint64_t> testTime{1000};
std::atomic<bool> pauseNextRead{false};
HANDLE readPaused{},readRelease{};
// Separate entries exercise both API consumers without touching real controllers.
__declspec(noinline) DWORD WINAPI fakeGetStateA(DWORD slot,XINPUT_STATE* state) {
    AcquireSRWLockShared(&fixtureLock);
    DWORD result=slot>=4||!fixtureConnected[slot]?ERROR_DEVICE_NOT_CONNECTED:fixtureError[slot];
    if(result==ERROR_SUCCESS&&state)*state=fixture[slot];
    ReleaseSRWLockShared(&fixtureLock);
    if(pauseNextRead.exchange(false)){SetEvent(readPaused);WaitForSingleObject(readRelease,5000);}
    return result;
}
__declspec(noinline) DWORD WINAPI fakeGetStateB(DWORD slot,XINPUT_STATE* state) {
    auto result=fakeGetStateA(slot,state);
    if(result==ERROR_SUCCESS&&state)state->dwPacketNumber+=100;return result;
}
#endif
std::uint64_t now() noexcept {
#ifdef CE_TEST_HOST
    return testTime.load();
#else
    return GetTickCount64();
#endif
}
PadSnapshot snapshot(bool connected,const XINPUT_GAMEPAD& p) noexcept {
    PadSnapshot result{};result.connected=connected;
    result.buttons=p.wButtons;result.leftTrigger=p.bLeftTrigger;result.rightTrigger=p.bRightTrigger;
    result.leftX=p.sThumbLX;result.leftY=p.sThumbLY;result.rightX=p.sThumbRX;result.rightY=p.sThumbRY;
    return result;
}
bool equal(const XINPUT_GAMEPAD& a,const XINPUT_GAMEPAD& b) noexcept {
    return a.wButtons==b.wButtons&&a.bLeftTrigger==b.bLeftTrigger&&a.bRightTrigger==b.bRightTrigger&&
        a.sThumbLX==b.sThumbLX&&a.sThumbLY==b.sThumbLY&&a.sThumbRX==b.sThumbRX&&a.sThumbRY==b.sThumbRY;
}
void cacheResult(DWORD slot,DWORD result,const XINPUT_STATE* state) noexcept {
    if(slot>=4)return;
    if(result==ERROR_SUCCESS&&state){rawPads[slot]=state->Gamepad;cached[slot]=snapshot(true,state->Gamepad);}
    else {rawPads[slot]={};cached[slot]={};}
    ++sampleGeneration[slot];
}
void observe(DWORD slot,DWORD result,const XINPUT_STATE* state) noexcept {
    cacheResult(slot,result,state);
    owner.update(now(),foreground.load(),cached);
}
template<unsigned N> DWORD WINAPI getStateHook(DWORD slot,XINPUT_STATE* state) {
    const bool outer=callDepth++==0;
    const bool track=!bypass&&outer&&enabled.load()&&slot<4;
    std::uint64_t generation=0,epoch=0;
    bool capturedAtStart=false;
    if(track){Lock lock;generation=sampleGeneration[slot];epoch=focusEpoch;capturedAtStart=owner.shouldSuppress(slot);}
    DWORD result=entries[N].original(slot,state);
    --callDepth;
    if(bypass||!outer||!enabled.load())return result;
    ++gameReads;
    Lock lock;
    // A slow worker read must not overwrite a newer Present/focus snapshot.
    const bool fresh=track&&generation==sampleGeneration[slot]&&epoch==focusEpoch;
    if(fresh)observe(slot,result,state);
    if(slot>=4)return result;
    auto& history=entries[N].packets[slot];
    if(result!=ERROR_SUCCESS||!state){history={};return result;}
    // An in-flight captured read must not replay its buttons/axes if ownership
    // ended while the underlying API was running.
    const bool capture=owner.shouldSuppress(slot)||capturedAtStart;
    if(!history.observed){history.observed=true;history.packet=state->dwPacketNumber;history.last=state->Gamepad;}
    if(capture){
        // Use the latest raw observation if another poll won the race. Emitting
        // an artificial zero for a stale sample would release a held Options
        // button and let the next poll spuriously press it again.
        const WORD allowed=owner.capturedButtonsForGame(slot,rawPads[slot].wButtons);
        history.virtualized=true;state->Gamepad={};state->Gamepad.wButtons=allowed;
    }
    if(history.virtualized){
        if(!equal(history.last,state->Gamepad))++history.packet;
        state->dwPacketNumber=history.packet;
    } else history.packet=state->dwPacketNumber;
    history.last=state->Gamepad;
    return result;
}
}
bool initializeControllerInput() noexcept {
    void* targets[2]{};
    bool enableAttempted=false;
#ifdef CE_TEST_HOST
    readPaused=CreateEventW(nullptr,TRUE,FALSE,nullptr);readRelease=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    targets[0]=(void*)fakeGetStateA;targets[1]=(void*)fakeGetStateB;
#else
    const wchar_t* names[2]={L"xinput1_4.dll",L"xinput1_3.dll"};
    for(unsigned i=0;i<2;++i){
        auto module=GetModuleHandleW(names[i]);
        if(!module)module=LoadLibraryExW(names[i],nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(module)targets[i]=(void*)GetProcAddress(module,"XInputGetState");
    }
#endif
    if(!targets[0]||!targets[1])return false;
    for(auto target:targets){
        if(entryCount&&target==entries[0].address)continue;
        auto& entry=entries[entryCount];entry.address=target;
        void* detour=entryCount?(void*)getStateHook<1>:(void*)getStateHook<0>;
        if(MH_CreateHook(target,detour,reinterpret_cast<void**>(&entry.original))!=MH_OK)goto failed;
        ++entryCount;
    }
    for(unsigned n=0;n<entryCount;++n)if(MH_QueueEnableHook(entries[n].address)!=MH_OK)goto failed;
    enableAttempted=true;
    if(MH_ApplyQueued()!=MH_OK)goto failed;
    enabled=true;return true;
failed:
    for(unsigned n=0;n<entryCount;++n) {
        if(enableAttempted)MH_DisableHook(entries[n].address);
        else MH_RemoveHook(entries[n].address);
    }
    // A partially enabled hook may already have an in-flight caller. Keep its
    // original trampoline alive for the process lifetime; capture remains off.
    if(!enableAttempted)entryCount=0;
    return false;
}
void setControllerMenuOpen(bool open) noexcept {Lock lock;owner.setMenuOpen(open);}
std::uint64_t beginControllerFreeCamera() noexcept {Lock lock;return enabled.load()?owner.beginFreeCamera():0;}
void requestControllerCameraStop(bool returnToMenu) noexcept {Lock lock;owner.requestCameraStop(returnToMenu);}
bool acknowledgeControllerCameraStopped(std::uint64_t session) noexcept {Lock lock;return owner.acknowledgeCameraStopped(session);}
void abandonControllerInput() noexcept {enabled=false;Lock lock;owner=ControllerState{};}
void setControllerFocus(bool focused) noexcept {
    Lock lock;if(foreground.load()!=focused)++focusEpoch;foreground=focused;
    if(!focused)owner.update(now(),false,cached);
}
MenuPadFrame pollControllerInput(bool focused) noexcept {
    MenuPadFrame frame{};frame.available=enabled.load();if(!frame.available)return frame;
    const bool regainingFocus=focused&&!foreground.load();
    if(!focused)setControllerFocus(false);
    std::array<XINPUT_STATE,4> batch{};
    std::array<DWORD,4> results{};
    std::array<bool,4> queried{};
    std::array<std::uint64_t,4> generation{};
    for(DWORD slot=0;slot<4;++slot){
        bool probe=false;
        {Lock lock;probe=regainingFocus||cached[slot].connected||now()>=nextProbe[slot];generation[slot]=sampleGeneration[slot];}
        if(!probe)continue;
        bypass=true;
        results[slot]=entries[0].original(slot,&batch[slot]);queried[slot]=true;
        bypass=false;
    }
    {Lock lock;
        // Refresh the entire focus-regain snapshot before arming any controller.
        // Do not replace a newer worker-thread observation with an older sample.
        for(DWORD slot=0;slot<4;++slot)if(queried[slot]) {
            if(generation[slot]==sampleGeneration[slot])cacheResult(slot,results[slot],&batch[slot]);
            nextProbe[slot]=now()+(results[slot]==ERROR_SUCCESS?0:500);
        }
        if(foreground.load()!=focused)++focusEpoch;
        foreground=focused;owner.update(now(),focused,cached);
        frame.menuOpen=owner.menuOpen();frame.navigationReady=owner.navigationReady();frame.selectedSlot=owner.selectedSlot();
        frame.cameraActive=owner.cameraActive();frame.cameraStopping=owner.cameraStopping();
        frame.cameraSession=owner.cameraSession();frame.cameraStopReason=owner.cameraStopReason();
        frame.cameraReturnsToMenu=owner.cameraReturnsToMenu();
        frame.inputDraining=owner.draining();
        for(unsigned slot=0;slot<4;++slot)if(cached[slot].connected){
            ++frame.connected;frame.connectedMask|=1u<<slot;
            if(!ControllerState::neutral(cached[slot]))frame.allPadsNeutral=false;
        }
        if(frame.selectedSlot>=0&&frame.selectedSlot<4)frame.gamepad=rawPads[frame.selectedSlot];
    }
    frame.gameReads=gameReads.load();return frame;
}
#ifdef CE_TEST_HOST
void testSetPad(unsigned slot,const XINPUT_STATE* state,bool connected) noexcept {
    if(slot>=4)return;AcquireSRWLockExclusive(&fixtureLock);
    fixtureConnected[slot]=connected;fixtureError[slot]=ERROR_SUCCESS;fixture[slot]=state?*state:XINPUT_STATE{};ReleaseSRWLockExclusive(&fixtureLock);
}
void testSetPadError(unsigned slot,DWORD error) noexcept {if(slot>=4)return;AcquireSRWLockExclusive(&fixtureLock);fixtureError[slot]=error;ReleaseSRWLockExclusive(&fixtureLock);}
void testPauseNextRead() noexcept {ResetEvent(readPaused);ResetEvent(readRelease);pauseNextRead=true;}
bool testReadPaused() noexcept {return WaitForSingleObject(readPaused,0)==WAIT_OBJECT_0;}
void testReleaseRead() noexcept {SetEvent(readRelease);}
void testSetControllerTime(std::uint64_t time) noexcept {testTime=time;}
DWORD testGameGetState(unsigned entry,DWORD slot,XINPUT_STATE* state) noexcept {
    if(entry>=entryCount)return ERROR_INVALID_PARAMETER;
    return ((GetState)entries[entry].address)(slot,state);
}
#endif
}
