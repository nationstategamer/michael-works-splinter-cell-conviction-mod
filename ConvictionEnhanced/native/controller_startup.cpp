#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "controller_startup.h"
#include "controller_startup_policy.h"

namespace ce { namespace {
SRWLOCK startupLock=SRWLOCK_INIT;
struct Lock {Lock(){AcquireSRWLockExclusive(&startupLock);}~Lock(){ReleaseSRWLockExclusive(&startupLock);}};
ControllerStartupStatus status;
ControllerStartupPolicy policy;
std::uintptr_t base=0;
HWND inputWindow=nullptr;
unsigned inputMask=0;
std::uint64_t inputTime=0;
using ApplySetting=void(__thiscall*)(void*,int,int,int);
bool match(std::uintptr_t address,const void* bytes,std::size_t size) noexcept {
    __try {return std::memcmp(reinterpret_cast<const void*>(address),bytes,size)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool signature() noexcept {
    // Entire dispatch through the UseController branch, plus its sole epilogue.
    const unsigned char apply[]={0x55,0x8b,0xec,0x8b,0x45,0x08,0x83,0xe8,0x00,0x56,0x8b,0xf1,0x0f,0x84,0x98,0,0,0,
        0x48,0x74,0x66,0x83,0xe8,0x03,0x74,0x32,0x83,0xe8,0x08,0x0f,0x85,0xa4,0,0,0,
        0x8b,0x4d,0x0c,0x8d,0x86,0x04,0x06,0,0,0x8b,0xb6,0x30,0x04,0,0,0x8d,0x14,0x09,
        0x33,0x10,0x83,0xe2,0x02,0x31,0x10,0x85,0xf6,0x0f,0x84,0x83,0,0,0,0x89,0x8e,0x88,0x03,0,0,0xeb,0x7b};
    const unsigned char tail[]={0x5e,0x5d,0xc2,0x0c,0};
    unsigned char hasPad[]={0xa1,0,0,0,0,0xc3};
    const auto found=base+0x1031e4c;std::memcpy(hasPad+1,&found,4);
    return match(base+0x11762b,apply,sizeof(apply))&&match(base+0x1176f2,tail,sizeof(tail))
        &&match(base+0x4f4417,hasPad,sizeof(hasPad));
}
bool inspect(HWND window,unsigned connectedMask,void* nativeController,ControllerStartupSnapshot* out,const char** why,bool* otherViewport) noexcept {
    *otherViewport=false;
    *out={};out->nowMs=GetTickCount64();out->supported=status.installed;
    out->rawPadReady=connectedMask!=0;
    DWORD pid=0;if(window)GetWindowThreadProcessId(window,&pid);
    // The window and renderer use different threads in this build. Execution
    // comes from the verified native view callback, never the Present poll.
    out->onGameThread=nativeController&&pid==GetCurrentProcessId();
    out->foreground=window&&GetForegroundWindow()==window;
    if(!out->onGameThread){*why="Waiting for the native game callback";return false;}
    if(!out->foreground){*why="Waiting for game focus";return false;}
    __try {
        auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};
        auto integer=[](std::uintptr_t p){return *reinterpret_cast<const int*>(p);};
        auto engine=!at(base+0xfa3178)?at(base+0xfa3370):0;
        if(!engine)engine=at(base+0xfa336c);
        if(!engine){*why="Waiting for the game engine";return false;}
        if(at(engine+0x1fc)&0x100){*why="Startup repair inactive in split-screen";return false;}
        const auto connection=at(base+0x101f8c0);
        if(!connection){*why="Waiting for a solo session";return false;}
        const auto block=at(connection+0x284);
        if(!block||integer(block+4)!=4||integer(block+8)<4||integer(block+8)>64
            ||!match(at(block+12),"SOLO",4)) {*why="Startup repair requires a solo session";return false;}
        out->sessionSolo=true;
        const unsigned char getter[]={0x8b,0x81,0x64,0x02,0,0,0xc3};
        if(!match(at(at(engine)+0x1f0),getter,sizeof(getter))){*why="Profile accessor differs from supported build";return false;}
        const auto manager=at(engine+0x264);
        if(!manager){*why="Waiting for the saved profile";return false;}
        const int id=integer(manager+0x230);
        if(id<0||id>3||integer(manager+0x234)!=-1){*why="Waiting for one loaded player profile";return false;}
        const auto profile=at(manager+0xa8+4*id);
        if(!profile){*why="Waiting for the saved profile";return false;}
        const int saved=integer(profile+0x24);
        if(saved!=0&&saved!=1){*why="Controller preference is not ready";return false;}
        out->profileLoaded=true;out->savedUseController=saved==1;
        const auto client=at(engine+0x44);
        if(!client||integer(client+0x30)<1||integer(client+0x30)>8||!at(client+0x2c))return false;
        const auto viewport=at(at(client+0x2c));
        if(!viewport||at(viewport+0x18)!=client)return false;
        const auto controller=at(viewport+0x30);
        if(!controller||at(controller+0x430)!=viewport)return false;
        if(controller!=reinterpret_cast<std::uintptr_t>(nativeController)){*otherViewport=true;return false;}
        if(at(at(controller)+0x524)!=base+0x11762b||at(at(viewport)+0x70)!=base+0x4f4417) {
            *why="Controller functions differ from supported build";return false;
        }
        const int active=integer(viewport+0x388);
        if(active!=0&&active!=1)return false;
        out->identity={engine,viewport,controller,profile};
        out->controllerEnabled=(at(controller+0x604)&2)!=0;out->viewportEnabled=active==1;
        // Equivalent to the verified HasControllerConnected method, without
        // invoking any device-enumeration, input hook or profile dispatcher.
        bool nativePad=false;
        for(unsigned i=0;i<2;++i)nativePad=nativePad||(at(base+0x1031e28+4*i)
            &&at(base+0x1031e34+4*i)==1&&at(base+0x1031e3c+4*i)==1);
        out->nativePadReady=at(base+0x1031e4c)!=0&&nativePad&&integer(viewport+0x414)==1;
        if(at(base+0x101f8c0)!=connection||at(connection+0x284)!=block||at(manager+0xa8+4*id)!=profile)return false;
        *why=!saved?"Use Controller is off in the saved profile":!out->rawPadReady?"Waiting for the controller":
            !out->nativePadReady?"Waiting for the game to detect the controller":
            out->controllerEnabled!=out->viewportEnabled?"Controller mode is changing; no repair applied":
            "Checking that controller startup is stable";
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){*why="Waiting for stable controller objects";return false;}
}
bool apply(const ControllerStartupSnapshot& sample) noexcept {
    __try {
        reinterpret_cast<ApplySetting>(base+0x11762b)(reinterpret_cast<void*>(sample.identity.controller),0x0c,1,0);
        return (*reinterpret_cast<const unsigned*>(sample.identity.controller+0x604)&2)!=0
            &&*reinterpret_cast<const int*>(sample.identity.viewport+0x388)==1;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool initializeControllerStartup(bool twoInstanceMode) noexcept {
    Lock lock;
#ifdef CE_TEST_HOST
    (void)twoInstanceMode;status.reason="Native startup repair inactive in isolated test host";return false;
#else
    if(twoInstanceMode){status.reason="Startup repair inactive in Fusion Fix two-instance mode";return false;}
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    status.installed=signature();status.reason=status.installed?"Waiting for the saved controller preference":"Native controller setting differs from supported build";
    return status.installed;
#endif
}
void pollControllerStartup(HWND gameWindow,unsigned connectedMask) noexcept {
    Lock lock;
    inputWindow=gameWindow;inputMask=connectedMask;inputTime=GetTickCount64();
}
void controllerStartupNativeView(void* nativeController) noexcept {
    Lock lock;if(!status.installed||policy.completed())return;
    const auto currentTime=GetTickCount64();
    if(!inputTime||currentTime<inputTime||currentTime-inputTime>1000) {
        ControllerStartupSnapshot unavailable{};policy.observe(unavailable);
        status.reason="Waiting for fresh controller input";return;
    }
    ControllerStartupSnapshot sample{};const char* why="Waiting for the primary controller";
    bool otherViewport=false;
    if(!inspect(inputWindow,inputMask,nativeController,&sample,&why,&otherViewport))sample.profileLoaded=false;
    if(otherViewport)return; // a secondary view must not reset primary stability
    status.reason=why;
    const auto decision=policy.observe(sample);
    if(decision==ControllerStartupDecision::AlreadyEnabled) {
        status.completed=true;status.reason="Controller was already active; no repair needed";return;
    }
    if(decision!=ControllerStartupDecision::Repair)return;
    // Read all guards again immediately before the one native call. Never
    // carry an engine pointer from an earlier Present into a write.
    ControllerStartupSnapshot fresh{};
    if(!signature()||!inspect(inputWindow,inputMask,nativeController,&fresh,&why,&otherViewport)
        ||!(fresh.identity==sample.identity)||policy.observe(fresh)!=ControllerStartupDecision::Repair)return;
    if(!policy.markAttempt())return;
    status.attempts=1;status.completed=true;status.repaired=apply(fresh);
    status.reason=status.repaired?"Controller activated from saved preference":"Controller activation could not be verified; use the game setting";
}
ControllerStartupStatus controllerStartupStatus() noexcept {Lock lock;return status;}
}
