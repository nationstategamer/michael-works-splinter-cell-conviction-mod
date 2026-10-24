#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <intrin.h>
#include "MinHook.h"
#include "free_camera.h"
#include "controller_startup.h"
#include "checkpoint_control.h"
#include "exploration_control.h"

namespace ce { namespace {
SRWLOCK cameraLock=SRWLOCK_INIT;
struct Lock {Lock(){AcquireSRWLockExclusive(&cameraLock);}~Lock(){ReleaseSRWLockExclusive(&cameraLock);}};
struct Identity {std::uintptr_t controller{},camera{},impl{},pawn{},level{};GameplayKind gameplay=GameplayKind::Unavailable;
    bool correctUltrawideFov=false;float pixelAspect=1;bool projectionUncropped=true;
    bool operator==(const Identity& b)const{return controller==b.controller&&camera==b.camera&&impl==b.impl&&pawn==b.pawn&&level==b.level&&gameplay==b.gameplay;}};
Identity observedIdentity{},ownedIdentity{};
FreeCameraStatus status;
CameraPose naturalPose{};
std::uint64_t lastObservation=0;
std::uintptr_t base=0;
using ViewFn=std::uintptr_t(__thiscall*)(void*,std::uintptr_t*,float*,std::int32_t*,std::uint32_t);
ViewFn originalView=nullptr;
#ifdef CE_TEST_HOST
std::uint64_t testNow=1000;
float testFov=90;
std::uint64_t now(){return testNow;}
#else
std::uint64_t now(){return GetTickCount64();}
#endif
bool fresh(std::uint64_t t){return lastObservation&&t>=lastObservation&&t-lastObservation<=1000;}
// All offsets below come from this exact build's runtime code and reciprocal
// pointers observed in a live single-player checkpoint. Reads alone may fail
// during a level change; no stale pointer is retained for later dereferencing.
enum class Inspection { OtherViewport, Unavailable, Ready };
Inspection inspectIdentity(void* self,Identity* out) noexcept {
    __try {
        auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};
        auto engine=!at(base+0xfa3178)?at(base+0xfa3370):0;
        if(!engine)engine=at(base+0xfa336c);
        if(!engine)return Inspection::Unavailable;
        auto client=at(engine+0x44);
        if(!client||*reinterpret_cast<const int*>(client+0x30)<1)return Inspection::Unavailable;
        auto array=at(client+0x2c);if(!array)return Inspection::Unavailable;
        auto viewport=at(array);if(!viewport)return Inspection::Unavailable;
        auto controller=at(viewport+0x30);
        if(controller!=reinterpret_cast<std::uintptr_t>(self))return Inspection::OtherViewport;
        const bool splitScreen=(*reinterpret_cast<const std::uint16_t*>(engine+0x1fc)&0x100)!=0;
        if(splitScreen)return Inspection::Unavailable;
        auto camera=at(controller+0x704);if(!camera)return Inspection::Unavailable;
        auto impl=at(camera+0x580);if(!impl)return Inspection::Unavailable;
        auto pawn=at(impl+0x24);
        if(at(impl+0x1c)!=camera||at(impl+0x20)!=controller||!pawn)return Inspection::Unavailable;
        auto level=at(controller+0x134);if(!level)return Inspection::Unavailable;
        // ULevel.FURL.Map is a bounded ASCII FString. Menu and multiplayer
        // also own camera/pawn objects, so those pointers alone are insufficient.
        const auto map=reinterpret_cast<const char*>(at(level+0x6c));
        const int length=*reinterpret_cast<const int*>(level+0x70),capacity=*reinterpret_cast<const int*>(level+0x74);
        if(!map||length<2||length>260||capacity<length||capacity>4096)return Inspection::Unavailable;
        char mapCopy[260]{};std::memcpy(mapCopy,map,length);
        // Exact-build connection singleton (accessor 0x4C0C10). Its +0x280
        // Ubisoft string has a ref block at +4: length+4, capacity+8, data+12.
        // InitMPConnection sets SOLO only for Single; SPLIT/LINK/XBOX and
        // Party/Invited are rejected. Never call accessors that allocate state.
        const auto connection=at(base+0x101f8c0);if(!connection)return Inspection::Unavailable;
        const auto block=at(connection+0x284);if(!block)return Inspection::Unavailable;
        const int connectionLength=*reinterpret_cast<const int*>(block+4),connectionCapacity=*reinterpret_cast<const int*>(block+8);
        const auto connectionData=reinterpret_cast<const char*>(at(block+12));
        if(!connectionData||connectionLength!=4||connectionCapacity<4||connectionCapacity>64)return Inspection::Unavailable;
        char connectionCopy[4]{};std::memcpy(connectionCopy,connectionData,4);
        if(at(base+0x101f8c0)!=connection||at(connection+0x284)!=block)return Inspection::Unavailable;
        const auto gameplay=gameplayKind(mapCopy,length,capacity,connectionCopy,connectionLength,splitScreen);
        if(gameplay==GameplayKind::Unavailable)return Inspection::Unavailable;
        *out={controller,camera,impl,pawn,level,gameplay};
        // Read-only inputs used by the native scene projection constructor.
        out->correctUltrawideFov=at(viewport+0x2b0)!=0;
        out->pixelAspect=at(viewport+0x150)?*reinterpret_cast<const float*>(viewport+0x100):1;
        // Alternate native crop transforms need their own verified projection.
        // Hide only the marker's world projection rather than guess its offset.
        out->projectionUncropped=!at(viewport+0x12c)&&!at(viewport+0x130)&&!at(viewport+0x134);
        return Inspection::Ready;
    } __except(EXCEPTION_EXECUTE_HANDLER){return Inspection::Unavailable;}
}
void unavailablePrimaryView() noexcept {
    Lock lock;status.available=false;status.active=false;status.gameplay=GameplayKind::Unavailable;
    status.renderViewValid=false;
    status.reason="Load a campaign or solo Deniable Ops match";lastObservation=0;
}
bool readPose(const float* xyz,const std::int32_t* angles,CameraPose* pose) noexcept {
    __try {*pose={xyz[0],xyz[1],xyz[2],angles[0],angles[1],angles[2]};return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool writePose(float* xyz,std::int32_t* angles,const CameraPose& pose) noexcept {
    // These are the function's caller-owned output buffers, already written by
    // the original. No actor, controller, CameraImpl, FOV or input data is written.
    __try {xyz[0]=pose.x;xyz[1]=pose.y;xyz[2]=pose.z;angles[0]=pose.pitch;angles[1]=pose.yaw;angles[2]=pose.roll;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
float readViewFov(const Identity& identity) noexcept {
#ifdef CE_TEST_HOST
    (void)identity;return testFov;
#else
    // Verified renderer scene construction consumes this controller FOV after
    // PlayerCalcView. Never write it or derive it from the queued preset.
    __try {return *reinterpret_cast<const float*>(identity.controller+0x38c);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
#endif
}
void viewResult(const Identity& identity,std::uintptr_t actor,float* xyz,std::int32_t* angles,bool valid) noexcept {
    CameraPose pose{};
    // The campaign's PlayerCalcView returns the owning pawn as ViewActor,
    // while its XYZ/rotator describe the detached third-person camera.
    valid=valid&&actor==identity.pawn&&readPose(xyz,angles,&pose)&&validCameraPose(pose);
    Lock lock;
    ++status.observations;
    status.controller=static_cast<std::uint32_t>(identity.controller);
    status.actor=static_cast<std::uint32_t>(actor);status.camera=static_cast<std::uint32_t>(identity.camera);
    status.gameplay=valid?identity.gameplay:GameplayKind::Unavailable;
    if(!valid){status.available=false;status.active=false;status.renderViewValid=false;status.reason="Waiting for a campaign or solo Deniable Ops camera";lastObservation=0;return;}
    if(status.active&&!fresh(now())){status.active=false;status.reason="Camera updates interrupted; free camera stopped";}
    observedIdentity=identity;naturalPose=pose;lastObservation=now();status.available=true;
    if(status.active&&!(identity==ownedIdentity)){status.active=false;status.reason="Camera changed; free camera stopped";}
    if(status.active) {
        if(writePose(xyz,angles,status.pose)){++status.overrides;status.reason="Free camera active";}
        else {status.active=false;status.available=false;status.reason="Camera output unavailable";}
    } else {status.pose=pose;status.reason="Gameplay camera ready";}
    status.horizontalFov=readViewFov(identity);
    status.correctUltrawideFov=identity.correctUltrawideFov;status.pixelAspect=identity.pixelAspect;
    status.renderViewValid=readPose(xyz,angles,&status.renderPose)&&validCameraPose(status.renderPose)
        &&std::isfinite(status.horizontalFov)&&status.horizontalFov>=15&&status.horizontalFov<=170
        &&std::isfinite(status.pixelAspect)&&status.pixelAspect>=.25f&&status.pixelAspect<=4
        &&identity.projectionUncropped;
}
std::uintptr_t __fastcall viewHook(void* self,void*,std::uintptr_t* actor,float* xyz,std::int32_t* angles,std::uint32_t extra) {
    const bool renderView=reinterpret_cast<std::uintptr_t>(_ReturnAddress())==base+0x388505;
    const auto result=originalView(self,actor,xyz,angles,extra);
    if(!renderView)return result;
#ifndef CE_TEST_HOST
    if(extra==0){ce::controllerStartupNativeView(self);ce::checkpointNativeView(self);}
#endif
    Identity identity{};
    // Other viewports never publish primary camera state. Unsupported primary
    // views disarm immediately, including a solo-to-network transition, so HUD
    // eligibility also cannot linger for the normal observation freshness limit.
    const auto inspection=inspectIdentity(self,&identity);
    if(inspection==Inspection::Ready)viewResult(identity,*actor,xyz,angles,true);
    else if(inspection==Inspection::Unavailable)unavailablePrimaryView();
#ifndef CE_TEST_HOST
    if(inspection!=Inspection::OtherViewport)
        ce::explorationNativeView(self,extra==0&&ce::freeCameraStatus().available);
#endif
    return result;
}
bool bytesMatch(std::uintptr_t address,const unsigned char* bytes,std::size_t size) noexcept {
    __try {return std::memcmp(reinterpret_cast<void*>(address),bytes,size)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool initializeFreeCamera() noexcept {
#ifdef CE_TEST_HOST
    Lock lock;status.installed=true;status.reason="Waiting for a gameplay camera";return true;
#else
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // PlayerCalcView event wrapper, verified 0x78837B at preferred base0x400000.
    // It returns actor in EAX and copies XYZ/rotator to args2/3, ret16.
    const unsigned char signature[]={0x55,0x8b,0xec,0x83,0xec,0x20,0x8b,0x45,0x08,0x8b,0x00,0x53,0x56,0x8b,0x75,0x0c,0x57,0x89,0x45,0xe0,0x8b,0x45,0x14};
    unsigned char engineSignature[]={0x83,0x3d,0,0,0,0,0,0x75,0x09,0xa1,0,0,0,0,0x85,0xc0,0x75,0x05,0xa1,0,0,0,0,0xc3};
    auto flag=base+0xfa3178,first=base+0xfa3370,second=base+0xfa336c;
    std::memcpy(engineSignature+2,&flag,4);std::memcpy(engineSignature+10,&first,4);std::memcpy(engineSignature+19,&second,4);
    auto target=reinterpret_cast<void*>(base+0x38837b);
    const unsigned char renderCall[]={0xe8,0x76,0xfe,0xff,0xff};
    // Native Single-context writer at 0x81B87D assigns literal SOLO to +0x280.
    unsigned char soloWriter[]={0x68,0,0,0,0,0x8d,0x8f,0x80,0x02,0,0,0xe8,0x03,0xda,0xbe,0xff};
    auto soloLiteral=base+0xcd905c;std::memcpy(soloWriter+1,&soloLiteral,4);
    if(!bytesMatch(reinterpret_cast<std::uintptr_t>(target),signature,sizeof(signature))||!bytesMatch(base+0x392b49,engineSignature,sizeof(engineSignature))||!bytesMatch(base+0x388500,renderCall,sizeof(renderCall))||!bytesMatch(base+0x41b87d,soloWriter,sizeof(soloWriter))) {
        Lock lock;status.reason="Camera function differs from the supported build";return false;
    }
    if(MH_CreateHook(target,viewHook,reinterpret_cast<void**>(&originalView))!=MH_OK) {
        Lock lock;status.reason="Camera hook unavailable";return false;
    }
    if(MH_EnableHook(target)!=MH_OK) {
        // Keep the trampoline alive if an enable operation partially succeeded.
        MH_DisableHook(target);Lock lock;status.reason="Camera hook could not be enabled";return false;
    }
    Lock lock;status.installed=true;status.reason="Waiting for a gameplay camera";return true;
#endif
}
FreeCameraStatus freeCameraStatus() noexcept {
    Lock lock;auto result=status;
    if(!fresh(now())){status.active=false;result.active=false;result.available=false;result.renderViewValid=false;result.gameplay=GameplayKind::Unavailable;result.reason="Load a campaign or solo Deniable Ops match";}
    return result;
}
bool beginFreeCamera(std::uint64_t session) noexcept {
    Lock lock;
    if(!session||!status.installed||status.active||!status.available||!fresh(now()))return false;
    ownedIdentity=observedIdentity;status.pose=naturalPose;status.session=session;status.active=true;status.reason="Free camera active";return true;
}
bool moveFreeCamera(std::uint64_t session,const CameraMotion& motion,double dt,float speed) noexcept {
    Lock lock;
    if(!status.active||status.session!=session||!fresh(now())){if(status.session==session)status.active=false;return false;}
    if(!applyCameraMotion(status.pose,motion,dt,speed)){status.active=false;status.reason="Invalid camera movement stopped";return false;}
    return true;
}
void stopFreeCamera(std::uint64_t session) noexcept {
    Lock lock;
    if(session&&session!=status.session)return;
    status.active=false;status.session=0;status.reason="Returned to the game camera";
}
#ifdef CE_TEST_HOST
void testCameraView(std::uint32_t id,CameraPose* pose,bool valid,std::uint32_t viewActor) noexcept {
    float xyz[]={pose->x,pose->y,pose->z};std::int32_t angles[]={pose->pitch,pose->yaw,pose->roll};
    Identity identity{id,id+1,id+2,id+3,id+4,GameplayKind::Campaign};
    viewResult(identity,viewActor?viewActor:id+3,xyz,angles,valid);
    *pose={xyz[0],xyz[1],xyz[2],angles[0],angles[1],angles[2]};
}
void testCameraContextView(std::uint32_t id,CameraPose* pose,const char* map,const char* connection,bool splitScreen,bool primary) noexcept {
    if(!primary)return;
    const int count=map?static_cast<int>(std::strlen(map))+1:0;
    const auto gameplay=gameplayKind(map,count,count,connection,connection?static_cast<int>(std::strlen(connection)):0,splitScreen);
    if(gameplay==GameplayKind::Unavailable){unavailablePrimaryView();return;}
    float xyz[]={pose->x,pose->y,pose->z};std::int32_t angles[]={pose->pitch,pose->yaw,pose->roll};
    Identity identity{id,id+1,id+2,id+3,id+4,gameplay};
    viewResult(identity,id+3,xyz,angles,true);
    *pose={xyz[0],xyz[1],xyz[2],angles[0],angles[1],angles[2]};
}
void testCameraClock(std::uint64_t t) noexcept {Lock lock;testNow=t;}
void testCameraFov(float value) noexcept {Lock lock;testFov=value;}
#endif
}
