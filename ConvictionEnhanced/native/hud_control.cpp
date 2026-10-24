#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <vector>
#include "MinHook.h"
#include "hud_control.h"
#include "free_camera.h"

namespace ce { namespace {
constexpr unsigned allLayers=127,masterBit=128;
std::atomic<unsigned> choices{0},seen{0};
std::atomic<bool> installed{false},faulted{false};
std::atomic<std::uint64_t> observations{0},suppressed{0};
std::uintptr_t base=0;
struct Class {unsigned rva,layer;const char* name;bool paths;};
// Exact native RTTI vtables. Roots and non-layer HUD movies are master-only.
// Each callback is checked against the current movie before its paths are read.
constexpr Class classes[]={
 {0xdb3598,0,"FlashSPHud",false},{0xdb4524,0,"FlashMPHud",false},{0xdb489c,0,"FlashHud",false},
 {0xdb8aac,1,"FlashHudReticle",true},
 {0xdb2974,2,"FlashHudMarkerManager",true},{0xdb6a1c,2,"FlashHudMarker",true},{0xdb808c,2,"FlashHudWaypointSystem",true},
 {0xdb344c,4,"FlashHudInventory",true},{0xdb6124,4,"FlashHudWeapon",true},
 {0xdb4fd4,8,"FlashHudButtons",true},{0xdb56fc,8,"FlashHudTutorial",true},
 {0xdb460c,16,"FlashHudObjectives",true},{0xdb5bb4,16,"FlashHudGameMessage",true},
 {0xdb649c,16,"FlashHudScreenText",true},{0xdb7334,16,"FlashHudHunter",true},
 {0xdb775c,16,"FlashHudLastStand",true},{0xdb85cc,16,"FlashHudChallenge",true},
 {0xdb35e4,16,"FlashHudPEC",true},{0xdb4acc,16,"FlashHudFriendsNotification",true},
 {0xdb7ef4,16,"FlashHudHacking",true},{0xdb7cc4,16,"FlashHudEMPEscort",true},
 {0xdb7b04,16,"FlashHudFaceOff",true},{0xdb42ec,16,"FlashHudRevive",true},
 {0xdb5d6c,32,"FlashHudDetection",true},{0xdb8964,32,"FlashHudHealth",true},
 {0xdb87c4,32,"FlashHudHealthFeedback",true},{0xdb83cc,32,"FlashHudScreenHalo",true},
 {0xdb8dc4,64,"FlashHudSubtitles",true},
 {0xdb8184,0,"FlashHudCommunication",true},{0xdb601c,0,"FlashHudPiP",true},
 {0xdb8fac,0,"FlashHudSnakeCam",true},{0xdb6cfc,0,"FlashHudSpyingCam",true},
 {0xdb272c,0,"FlashHudCoste",true},{0xdb6b5c,0,"FlashHudCosteEffect",true},
 {0xdb758c,0,"FlashHudTurret",true},{0xdb67f4,0,"FlashBeaconManager",false}
};
const Class* classify(std::uintptr_t vt){for(const auto& c:classes)if(vt==base+c.rva)return &c;return nullptr;}
struct Callback {std::uintptr_t object;const Class* kind;};
struct Movie {Callback callbacks[128]{};unsigned count=0,layers=0;};
bool inspectMovie(void* raw,Movie* out) noexcept {
    __try {
        const auto p=reinterpret_cast<std::uintptr_t>(raw);
        if(!p||*reinterpret_cast<const std::uintptr_t*>(p)!=base+0xcde5d4)return false;
        const auto data=*reinterpret_cast<const std::uintptr_t*>(p+0x58);
        const int count=*reinterpret_cast<const int*>(p+0x5c),capacity=*reinterpret_cast<const int*>(p+0x60);
        if(!data||count<1||count>128||capacity<count||capacity>256)return false;
        for(int i=0;i<count;++i){
            const auto obj=reinterpret_cast<const std::uintptr_t*>(data)[i];
            if(!obj||*reinterpret_cast<const std::uintptr_t*>(obj+4)!=p)return false;
            const auto kind=classify(*reinterpret_cast<const std::uintptr_t*>(obj));
            // An unknown/native frontend callback makes the whole movie ineligible.
            if(!kind)return false;
            out->callbacks[out->count++]={obj,kind};out->layers|=kind->layer;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct Clip {void* movie{};char path[192]{};};
// FString arguments here are read-only, caller-owned views; native getters and
// setters inspect only their first two fields and never destroy or retain them.
struct StringView {const char* data;int count,capacity;};
using GetBool=int(__thiscall*)(void*,const StringView*);
using SetBool=void(__thiscall*)(void*,const StringView*,int);
using Visibility=int(__thiscall*)(void*);
using RenderAll=std::uintptr_t(__thiscall*)(void*);
using RenderOne=std::uintptr_t(__thiscall*)(void*,void*);
using ManagerAccessor=void*(__cdecl*)();
using EnterManager=int(__thiscall*)(void*,int);
using LeaveManager=void(__thiscall*)(void*);
GetBool getBool{};SetBool setBool{};Visibility originalVisibility{};
RenderAll originalRenderAll{};RenderOne originalRenderOne{};
ManagerAccessor managerAccessor{};EnterManager enterManager{};LeaveManager leaveManager{};
bool lockManager(void* manager) noexcept {
    __try {
        // Getter/setter/query all use this same manager. Its native lock is a
        // Windows critical section, so their nested acquisitions are valid.
        return manager&&managerAccessor&&enterManager&&leaveManager&&
            managerAccessor()==manager&&enterManager(manager,1)!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
#ifdef CE_TEST_HOST
HudTestGet testGet{};HudTestSet testSet{};
#endif
int readVisible(const Clip& clip){
#ifdef CE_TEST_HOST
    return testGet(clip.movie,clip.path);
#else
    const int n=static_cast<int>(std::strlen(clip.path))+1;StringView s{clip.path,n,n};
    return getBool(clip.movie,&s);
#endif
}
void writeVisible(const Clip& clip,int value){
#ifdef CE_TEST_HOST
    testSet(clip.movie,clip.path,value);
#else
    const int n=static_cast<int>(std::strlen(clip.path))+1;StringView s{clip.path,n,n};
    setBool(clip.movie,&s,value);
#endif
}
bool clipCount(std::uintptr_t object,std::uintptr_t* array,int* count) noexcept {
    __try {
        *array=*reinterpret_cast<const std::uintptr_t*>(object+0xc);
        *count=*reinterpret_cast<const int*>(object+0x10);
        const int capacity=*reinterpret_cast<const int*>(object+0x14);
        return *count>=0&&*count<=64&&capacity>=*count&&capacity<=128&&(!*count||*array);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool makeClip(std::uintptr_t entry,void* movie,Clip* clip) noexcept {
    __try {
        const auto str=*reinterpret_cast<const char*const*>(entry);
        const int count=*reinterpret_cast<const int*>(entry+4),capacity=*reinterpret_cast<const int*>(entry+8);
        if(!str||count<2||count>176||capacity<count||capacity>4096||str[count-1]!=0)return false;
        for(int i=0;i<count-1;++i){const char c=str[i];if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'))return false;}
        clip->movie=movie;std::memcpy(clip->path,"_root.",6);
        std::memcpy(clip->path+6,str,count-1);std::memcpy(clip->path+6+count-1,"._visible",10);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct Scope;
thread_local Scope* currentScope=nullptr;
struct Scope {
    Scope* previous;unsigned selected;bool eligible;void* lockedManager{};std::vector<Clip> hidden;
    Scope(bool gameplay,void* manager=nullptr):previous(currentScope),selected(choices.load()),eligible(gameplay&&installed.load()&&!faulted.load()){
#ifndef CE_TEST_HOST
        // Snapshot gameplay eligibility before taking the Flash lock. Hold the
        // lock until restoration so an update cannot observe temporary values.
        if(eligible&&selected){if(lockManager(manager))lockedManager=manager;else eligible=false;}
#endif
        currentScope=this;
    }
    ~Scope(){
        // Reverse order also handles parent/child clips. Nothing is retained
        // beyond this synchronous render call, including after Restore all.
        for(auto it=hidden.rbegin();it!=hidden.rend();++it)writeVisible(*it,1);
        currentScope=previous;
        if(lockedManager)leaveManager(lockedManager);
    }
    void hide(const Clip& clip){
        if(hidden.size()>=512)return;
        for(const auto& previousClip:hidden)
            if(previousClip.movie==clip.movie&&!std::strcmp(previousClip.path,clip.path))return;
        // Only change an existing boolean that was true. Missing clips and
        // game-hidden clips are untouched and never forced visible afterward.
        if(readVisible(clip)!=1)return;
        hidden.push_back(clip);writeVisible(clip,0);++suppressed;
    }
};
int filter(void* movie,std::uintptr_t caller,int natural){
    auto scope=currentScope;
    if(!natural||!scope||!scope->eligible||(caller!=0x458b08&&caller!=0x458bad))return natural;
    Movie info;if(!inspectMovie(movie,&info))return natural;
    ++observations;seen.fetch_or(info.layers);
    if(scope->selected&masterBit){++suppressed;return 0;}
    if(!(scope->selected&info.layers))return natural;
    try {
        for(unsigned i=0;i<info.count;++i){const auto& cb=info.callbacks[i];
            if(!cb.kind->paths||!(cb.kind->layer&scope->selected))continue;
            std::uintptr_t array=0;int count=0;if(!clipCount(cb.object,&array,&count))continue;
            for(int n=0;n<count;++n){Clip clip;if(makeClip(array+0x58*n,movie,&clip))scope->hide(clip);}
        }
    } catch(...) {choices=0;faulted=true;}
    return natural;
}
int __fastcall visibilityHook(void* movie,void*){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress())-base;
    const int result=originalVisibility(movie); // retains original manager locking/side effects
    return filter(movie,caller,result);
}
std::uintptr_t __fastcall renderAllHook(void* manager,void*){
    Scope scope(freeCameraStatus().available,manager);return originalRenderAll(manager);
}
std::uintptr_t __fastcall renderOneHook(void* manager,void*,void* movie){
    Scope scope(freeCameraStatus().available,manager);return originalRenderOne(manager,movie);
}
bool match(std::uintptr_t address,const unsigned char* bytes,std::size_t size) noexcept {
    __try{return std::memcmp(reinterpret_cast<void*>(address),bytes,size)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool classSignatures() noexcept {
    __try {
        for(const auto& c:classes){
            const auto locator=*reinterpret_cast<const std::uintptr_t*>(base+c.rva-4);
            if(locator<base||locator>base+0x117a000-20)return false;
            const auto type=*reinterpret_cast<const std::uintptr_t*>(locator+12);
            if(type<base||type>base+0x117a000-128)return false;
            const auto name=reinterpret_cast<const char*>(type+8);const auto len=std::strlen(c.name);
            if(std::memcmp(name,".?AV",4)||std::memcmp(name+4,c.name,len)||std::memcmp(name+4+len,"@@",3))return false;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool initializeHudControl() noexcept {
#ifdef CE_TEST_HOST
    installed=true;return true;
#else
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    unsigned char query[]={0xf6,0x05,0,0,0,0,0x01,0x56,0x57,0x8b,0xf1,0x75,0x18};
    const auto flag=base+0x1030ca0;std::memcpy(query+2,&flag,4);
    const unsigned char all[]={0x55,0x8b,0xec,0x83,0xec,0x40,0x56,0x57,0x8b,0xf1};
    const unsigned char one[]={0x55,0x8b,0xec,0x83,0xec,0x3c,0x56,0x57,0x8b,0x7d,0x08};
    const unsigned char call1[]={0xe8,0x9b,0x77,0x01,0x00},call2[]={0xe8,0xf6,0x76,0x01,0x00};
    const unsigned char getter[]={0x55,0x8b,0xec,0x83,0xec,0x14,0x83,0x65,0xfc,0x00,0x56,0x57,0x8b,0xf9};
    const unsigned char setter[]={0x55,0x8b,0xec,0x83,0xec,0x10,0x57,0x8b,0xf9,0x83,0x7f,0x28,0x00};
    const unsigned char accessor[]={0x56,0xe8,0x80,0x7e,0xf3,0xff,0x8b,0xf0,0x85,0xf6,0x74,0x2b};
    const unsigned char enter[]={0x56,0x57,0x33,0xff,0x8b,0xf1,0xff,0xb6,0x2c,0x01,0x00,0x00,0x47,0x83,0x7c,0x24,0x10,0x00};
    const unsigned char leave[]={0xff,0xb1,0x2c,0x01,0x00,0x00,0xff,0x89,0x30,0x01,0x00,0x00,0xe8,0xc5,0xf7,0xbe,0xff,0x59,0xc3};
    if(!match(base+0x4702a3,query,sizeof(query))||!match(base+0x458ad0,all,sizeof(all))||!match(base+0x458b89,one,sizeof(one))||
       !match(base+0x458b03,call1,sizeof(call1))||!match(base+0x458ba8,call2,sizeof(call2))||
       !match(base+0x470688,getter,sizeof(getter))||!match(base+0x47079f,setter,sizeof(setter))||
       !match(base+0x45acc3,accessor,sizeof(accessor))||!match(base+0x457f3c,enter,sizeof(enter))||
       !match(base+0x457f71,leave,sizeof(leave))||!classSignatures())return false;
    getBool=reinterpret_cast<GetBool>(base+0x470688);setBool=reinterpret_cast<SetBool>(base+0x47079f);
    managerAccessor=reinterpret_cast<ManagerAccessor>(base+0x45acc3);
    enterManager=reinterpret_cast<EnterManager>(base+0x457f3c);leaveManager=reinterpret_cast<LeaveManager>(base+0x457f71);
    void* targets[]={reinterpret_cast<void*>(base+0x4702a3),reinterpret_cast<void*>(base+0x458ad0),reinterpret_cast<void*>(base+0x458b89)};
    void* hooks[]={reinterpret_cast<void*>(visibilityHook),reinterpret_cast<void*>(renderAllHook),reinterpret_cast<void*>(renderOneHook)};
    void** originals[]={reinterpret_cast<void**>(&originalVisibility),reinterpret_cast<void**>(&originalRenderAll),reinterpret_cast<void**>(&originalRenderOne)};
    int created=0;
    for(;created<3;++created)if(MH_CreateHook(targets[created],hooks[created],originals[created])!=MH_OK)break;
    if(created!=3){for(int i=0;i<created;++i)MH_RemoveHook(targets[i]);return false;}
    for(auto target:targets)if(MH_QueueEnableHook(target)!=MH_OK){for(auto t:targets)MH_QueueDisableHook(t);MH_ApplyQueued();return false;}
    if(MH_ApplyQueued()!=MH_OK){for(auto t:targets)MH_DisableHook(t);return false;}
    installed=true;return true;
#endif
}
HudStatus hudStatus() noexcept {
    HudStatus result;result.installed=installed&&!faulted;const auto selected=choices.load();
    result.supportedMask=result.installed?allLayers:0;result.hiddenMask=selected&allLayers;result.allHidden=(selected&masterBit)!=0;
    result.observedMask=seen;result.observations=observations;result.suppressed=suppressed;
    result.reason=faulted?"HUD controls stopped; normal interface restored":!installed?"HUD functions differ from the supported build":
        "Visibility applies in campaign and solo Deniable Ops. Pause and debug menus stay available.";
    return result;
}
void setHudLayerHidden(HudLayer layer,bool hidden) noexcept {
    const auto bit=static_cast<unsigned>(layer);if(!installed||faulted||!bit||(bit&~allLayers)||(bit&(bit-1)))return;
    if(hidden)choices.fetch_or(bit);else choices.fetch_and(~bit);
}
void setAllHudHidden(bool hidden) noexcept {if(installed&&!faulted){if(hidden)choices.fetch_or(masterBit);else choices.fetch_and(~masterBit);}}
void restoreHud() noexcept {choices=0;}
#ifdef CE_TEST_HOST
void testHudDraw(HudTestDraw draw,bool gameplay,HudTestGet get,HudTestSet set){
    const auto oldGet=testGet;const auto oldSet=testSet;testGet=get;testSet=set;
    try {{Scope scope(gameplay);draw();}}catch(...){testGet=oldGet;testSet=oldSet;throw;}
    testGet=oldGet;testSet=oldSet;
}
int testHudQuery(void* movie,std::uintptr_t callerRva,int naturalResult){return filter(movie,callerRva,naturalResult);}
#endif
}
