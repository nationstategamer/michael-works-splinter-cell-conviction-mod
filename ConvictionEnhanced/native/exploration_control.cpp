#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstring>
#include "MinHook.h"
#include "exploration_control.h"
#include "exploration_landing_math.h"
#include "checkpoint_control.h"
#include "gameplay_context.h"
#include "practice_storage.h"

namespace ce { namespace {
SRWLOCK explorationLock=SRWLOCK_INIT;
struct Lock {Lock(){AcquireSRWLockExclusive(&explorationLock);}~Lock(){ReleaseSRWLockExclusive(&explorationLock);}};
struct Scene {
    std::uintptr_t engine=0,controller=0,pawn=0,level=0,profile=0,connection=0,connectionBlock=0,camera=0,impl=0;
    std::uint64_t checkpointGeneration=0;
    char map[260]{};
    bool operator==(const Scene& b)const noexcept {
        return engine==b.engine&&controller==b.controller&&pawn==b.pawn&&level==b.level
            &&profile==b.profile&&connection==b.connection&&connectionBlock==b.connectionBlock
            &&camera==b.camera&&impl==b.impl&&checkpointGeneration==b.checkpointGeneration
            &&std::strcmp(map,b.map)==0;
    }
};
struct Sample {Scene scene{};ExplorationPosition position{},velocity{},extent{};unsigned placementFlags=0;
    bool valid=false,transition=false,currentSafe=false,hasSupport=false;float supportFloorZ=0;};
struct ReturnSupport {ExplorationIdentity identity{};ExplorationPosition anchor{};float floorZ=0;bool valid=false;};
struct Input {HWND window=nullptr;unsigned mask=0;std::uint64_t time=0;bool menu=false,neutral=false,draining=false,camera=false,protectedSaves=false;};
struct PreviewRequest {
    ExplorationIdentity identity{};ExplorationPosition target{};
    std::uint64_t revision=0,session=0,publishedAt=0;
    unsigned mask=0;bool valid=false,fromCamera=false;
};
struct Hit {std::uint32_t next=0,actor=0,primitive=0;ExplorationPosition location{},unknown{},normal{};
    std::uint32_t unknown30=0;float time=1;std::int32_t item=-1;std::uint32_t a=0,b=0,c=0,d=0;};
static_assert(sizeof(Hit)==0x4c,"Exact-build native trace result layout");
ExplorationStatus status;
ExplorationPolicy policy;
ReturnSupport returnSupport;
Input input;
Sample observed;
Scene boundScene;
std::uint64_t generation=0,observedAt=0,lastViewAt=0,saveQueuedAt=0;
std::uint64_t inputCancellation=0;
std::uintptr_t lastViewController=0,base=0;
bool bound=false,saveQueued=false;
bool viewInvalid=false;
unsigned queuedMask=0,saveMask=0;
ExplorationIdentity cameraIdentity{},saveIdentity{};
ExplorationPosition cameraTarget{};
bool cameraValid=false;
PreviewRequest previewRequest;
std::uint64_t previewRevision=0,lastPreviewQueryAt=0,previewCheckedAt=0;
const char* detail="Exploration tools ready";
// Preserve the native EAX result regardless of whether a caller consumes it.
using TickFn=std::uintptr_t(__thiscall*)(void*,float);
TickFn originalTick=nullptr;
thread_local bool insideTick=false;
#ifdef CE_TEST_HOST
std::uint64_t testNow=1000;
bool testFocused=true;
bool testProtection=true,testNest=false;
unsigned movesInsideNested=0;
Sample fixture;
float testHalfHeight=90,testCurrentFloorZ=-92,testDestinationFloorZ=-92;
unsigned failure=0,nativeMoves=0,landingQueries=0;
ExplorationPosition lastDestination{};
std::uint64_t now() noexcept{return testNow;}
bool focused(const Input&) noexcept{return testFocused;}
bool protectedNow(bool published) noexcept{return published&&testProtection;}
#else
std::uint64_t now() noexcept{return GetTickCount64();}
bool focused(const Input& value) noexcept{return value.window&&GetForegroundWindow()==value.window;}
bool protectedNow(bool published) noexcept {
    const auto actual=practiceStorageStatus();return published&&actual.active&&actual.healthy;
}
#endif
bool fresh(std::uint64_t timestamp,std::uint64_t time) noexcept{return timestamp&&time>=timestamp&&time-timestamp<=1000;}
bool freshView(const Sample& sample,std::uint64_t time) noexcept {
    return !viewInvalid&&lastViewController==sample.scene.controller&&fresh(lastViewAt,time);
}
float squared(const ExplorationPosition& a,const ExplorationPosition& b) noexcept {
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return x*x+y*y+z*z;
}
bool closeTo(const ExplorationPosition& a,const ExplorationPosition& b,float distance=.5f) noexcept {
    return validExplorationPosition(a)&&validExplorationPosition(b)&&squared(a,b)<=distance*distance;
}
bool staticFloorProperties(bool sameLevel,unsigned flags,unsigned physics,std::uintptr_t table,
    std::uintptr_t ownBase,const ExplorationPosition& velocity,std::uintptr_t meshTable) noexcept {
    if(!sameLevel||!(flags&2))return false;
    if(physics==0)return true;
    // Retail static geometry uses both physics0 and physics10. The latter is
    // admitted only for the observed exact AStaticMeshActor class, still marked
    // immovable, unattached and stationary. Movers and dynamic actors remain out.
    return physics==10&&table==meshTable&&!ownBase&&validExplorationPosition(velocity)
        &&velocity.x==0&&velocity.y==0&&velocity.z==0;
}
bool bytes(std::uintptr_t address,const unsigned char* value,std::size_t size) noexcept {
    __try{return std::memcmp(reinterpret_cast<const void*>(address),value,size)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
#ifndef CE_TEST_HOST
bool signatures() noexcept {
    const unsigned char tick[]={0x55,0x8d,0x6c,0x24,0x8c,0x81,0xec,0x0c,1,0,0,0x53,0x56,0x57,0x33,0xff};
    const unsigned char tickTail[]={0x5e,0x5b,0x83,0xc5,0x74,0xc9,0xc2,4,0};
    const unsigned char location[]={0x53,0x56,0x8b,0x74,0x24,0x0c,0x57,0x6a,1,0xff,0x74,0x24,0x18,0x8b,0xd9,0x8b,0x8b,0x34,1,0,0};
    const unsigned char trace[]={0x55,0x8b,0xec,0x83,0xec,0x0c,0x53,0x56,0x57,0x8b,0xf1};
    const unsigned char point[]={0x55,0x8b,0xec,0x83,0xec,0x0c,0x53,0x56,0x57,0x8b,0xf1};
    const unsigned char traceTail[]={0x5f,0x33,0xc0,0x85,0xdb,0x5e,0x0f,0x94,0xc0,0x5b,0xc9,0xc2,0x1c,0};
    const unsigned char pointTail[]={0x5f,0x5e,0x8b,0xc3,0x5b,0xc9,0xc2,0x14,0};
    return bytes(base+0x39efc5,tick,sizeof(tick))&&bytes(base+0x39fc63,tickTail,sizeof(tickTail))
        &&bytes(base+0x3e123e,location,sizeof(location))&&bytes(base+0x3d7572,trace,sizeof(trace))
        &&bytes(base+0x3d7632,traceTail,sizeof(traceTail))&&bytes(base+0x3d551a,point,sizeof(point))
        &&bytes(base+0x3d5592,pointTail,sizeof(pointTail));
}
bool copyMap(std::uintptr_t address,char* output) noexcept {
    const auto pointer=*reinterpret_cast<const std::uintptr_t*>(address);
    const int size=*reinterpret_cast<const int*>(address+4),capacity=*reinterpret_cast<const int*>(address+8);
    if(!pointer||size<2||size>260||capacity<size||capacity>4096)return false;
    const auto data=reinterpret_cast<const unsigned char*>(pointer);if(data[size-1])return false;
    for(int i=0;i<size-1;++i)if(data[i]<32||data[i]>126)return false;
    std::memcpy(output,data,size);return true;
}
bool staticFloorActor(std::uintptr_t actor,std::uintptr_t level) noexcept {
    __try {
        if(!actor)return false;
        const bool sameLevel=*reinterpret_cast<const std::uintptr_t*>(actor+0x134)==level;
        const unsigned flags=*reinterpret_cast<const unsigned*>(actor+0x68);
        const unsigned physics=*reinterpret_cast<const unsigned char*>(actor+0x53);
        if(physics==0)return staticFloorProperties(sameLevel,flags,physics,0,0,{},base+0xbfa30c);
        if(physics!=10)return false;
        return staticFloorProperties(sameLevel,flags,physics,*reinterpret_cast<const std::uintptr_t*>(actor),
            *reinterpret_cast<const std::uintptr_t*>(actor+0x16c),
            *reinterpret_cast<const ExplorationPosition*>(actor+0x1c0),base+0xbfa30c);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool primaryView(void* controller) noexcept {
    __try {
        const auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};
        auto engine=!at(base+0xfa3178)?at(base+0xfa3370):0;if(!engine)engine=at(base+0xfa336c);
        if(!engine)return false;const auto client=at(engine+0x44);if(!client||!at(client+0x2c))return false;
        const auto viewport=at(at(client+0x2c));return viewport&&at(viewport+0x30)==reinterpret_cast<std::uintptr_t>(controller);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool inspect(void* expectedEngine,const CheckpointStatus& checkpoint,Sample* result,const char** why) noexcept {
    *result={};
    __try {
        auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};
        auto integer=[](std::uintptr_t p){return *reinterpret_cast<const int*>(p);};
        auto vector=[](std::uintptr_t p){return *reinterpret_cast<const ExplorationPosition*>(p);};
        if(at(base+0xfa3200)||at(base+0xfa321c)||checkpoint.pending||checkpoint.loading||checkpoint.partPending||checkpoint.partLoading) {
            result->transition=true;*why="Waiting for mission or checkpoint loading";return false;
        }
        auto& scene=result->scene;scene.engine=!at(base+0xfa3178)?at(base+0xfa3370):0;if(!scene.engine)scene.engine=at(base+0xfa336c);
        if(!scene.engine||(expectedEngine&&scene.engine!=reinterpret_cast<std::uintptr_t>(expectedEngine)))return false;
        const auto engineTable=at(scene.engine);
        if(at(engineTable+0x5c)!=base+0x39efc5||at(scene.engine+0x1fc)&0x100)return false;
        scene.connection=at(base+0x101f8c0);if(!scene.connection)return false;
        scene.connectionBlock=at(scene.connection+0x284);
        if(!scene.connectionBlock||integer(scene.connectionBlock+4)!=4||integer(scene.connectionBlock+8)<4||integer(scene.connectionBlock+8)>64)return false;
        const auto connectionData=at(scene.connectionBlock+12);if(!connectionData||std::memcmp(reinterpret_cast<const void*>(connectionData),"SOLO",4))return false;
        const auto client=at(scene.engine+0x44);if(!client||integer(client+0x30)<1||integer(client+0x30)>8||!at(client+0x2c))return false;
        const auto viewport=at(at(client+0x2c));if(!viewport||at(viewport+0x18)!=client)return false;
        scene.controller=at(viewport+0x30);if(!scene.controller||at(scene.controller+0x430)!=viewport)return false;
        if(!(at(scene.controller+0x604)&2)||integer(viewport+0x388)!=1)return false;
        const auto manager=at(scene.engine+0x264);if(!manager)return false;
        const int profileId=integer(manager+0x230);if(profileId<0||profileId>3||integer(manager+0x234)!=-1)return false;
        scene.profile=at(manager+0xa8+profileId*4);if(!scene.profile||integer(scene.profile+0x24)!=1)return false;
        scene.level=at(scene.engine+0x19c);if(!scene.level||at(scene.controller+0x134)!=scene.level||!copyMap(scene.level+0x6c,scene.map))return false;
        const auto kind=gameplayKind(scene.map,static_cast<int>(std::strlen(scene.map))+1,static_cast<int>(std::strlen(scene.map))+1,"SOLO",4,false);
        if(kind==GameplayKind::Unavailable)return false;
        scene.checkpointGeneration=kind==GameplayKind::Campaign?checkpoint.generation:0;
        scene.camera=at(scene.controller+0x704);if(!scene.camera)return false;scene.impl=at(scene.camera+0x580);if(!scene.impl)return false;
        scene.pawn=at(scene.impl+0x24);
        if(!scene.pawn||at(scene.impl+0x1c)!=scene.camera||at(scene.impl+0x20)!=scene.controller||at(scene.pawn+0x134)!=scene.level)return false;
        const auto levelTable=at(scene.level);
        if(at(levelTable+0x88)!=base+0x3d5c5e||at(levelTable+0xd4)!=base+0x3d551a)return false;
        result->position=vector(scene.pawn+0x94);result->velocity=vector(scene.pawn+0x1c0);
        const float radius=*reinterpret_cast<const float*>(scene.pawn+0x274),height=*reinterpret_cast<const float*>(scene.pawn+0x278);
        if(!validExplorationPosition(result->position)||!validExplorationPosition(result->velocity)
            ||!std::isfinite(radius)||!std::isfinite(height)||radius<5||radius>128||height<radius||height>256)return false;
        result->extent={radius,radius,height};
        const auto flags=at(scene.pawn+0x64);
        result->placementFlags=flags&0x100000?1:(at(scene.pawn+0x68)&0x4000)?2:(flags&0x80)?0xffff:0;
        result->valid=true;*why="Stand still on clear ground, outside cover or interactions";
        if(*reinterpret_cast<const unsigned char*>(scene.pawn+0x53)!=1||squared(result->velocity,{})>100
            ||(at(scene.pawn+0xa3c)&0x01100000)||(at(scene.pawn+0xa40)&0x00200000)||(at(scene.pawn+0xa44)&0x100))return true;
        const auto floor=at(scene.pawn+0x16c);
        if(floor&&!staticFloorActor(floor,scene.level))return true;
        if(((at(scene.pawn+0x60)|at(scene.pawn+0x6c))&0x400)==0)return true;
        result->currentSafe=true;*why="Checking clear, supported ground";return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){*why="Waiting for stable player and level objects";return false;}
}
using TraceFn=int(__thiscall*)(void*,Hit*,void*,const ExplorationPosition*,const ExplorationPosition*,unsigned,const char*,const ExplorationPosition*);
using PointFn=int(__thiscall*)(void*,void*,const ExplorationPosition*,const ExplorationPosition*,unsigned,Hit*);
using LocationFn=int(__thiscall*)(void*,const ExplorationPosition*,int);
bool staticFloor(const Sample& sample,const Hit& hit) noexcept {
    const auto actor=static_cast<std::uintptr_t>(hit.actor);
    return actor!=sample.scene.pawn&&staticFloorActor(actor,sample.scene.level);
}
bool ground(const Sample& sample,const ExplorationPosition& point,float distance,Hit* hit) noexcept {
    __try {
        ExplorationPosition end=point;end.z-=distance;const ExplorationPosition zero{};*hit={};
        const int clear=reinterpret_cast<TraceFn>(base+0x3d7572)(reinterpret_cast<void*>(sample.scene.level),hit,
            reinterpret_cast<void*>(sample.scene.pawn),&end,&point,0x08000004,reinterpret_cast<const char*>(base+0xc01dc0),&zero);
        const float norm=hit->normal.x*hit->normal.x+hit->normal.y*hit->normal.y+hit->normal.z*hit->normal.z;
        return clear==0&&hit->time>0&&hit->time<=1&&validExplorationPosition(hit->location)
            &&validExplorationPosition(hit->normal)&&norm>.75f&&norm<1.25f&&hit->normal.z>=.7f&&staticFloor(sample,*hit);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool clearCapsule(const Sample& sample,const ExplorationPosition& point) noexcept {
    __try {return reinterpret_cast<PointFn>(base+0x3d551a)(reinterpret_cast<void*>(sample.scene.level),reinterpret_cast<void*>(sample.scene.pawn),
        &point,&sample.extent,sample.placementFlags,nullptr)==1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool validateCurrent(Sample* sample,const char** why=nullptr) noexcept {
    sample->hasSupport=false;sample->supportFloorZ=0;
    if(!sample->currentSafe)return false;
    Hit hit{};
    if(!ground(*sample,sample->position,sample->extent.z+12,&hit)){if(why)*why="No verified walkable static floor beneath Sam";return false;}
    if(std::fabs((sample->position.z-hit.location.z)-sample->extent.z)>8){if(why)*why="Sam must be standing on supported ground";return false;}
    if(!clearCapsule(*sample,sample->position)){if(why)*why="Sam's current space is blocked";return false;}
    sample->supportFloorZ=hit.location.z;sample->hasSupport=true;
    if(why)*why="Exploration tools ready";return true;
}
bool landing(const Sample& sample,const ExplorationPosition& target,bool returning,ExplorationPosition* result,const char** why,
    ExplorationPosition* floorPoint=nullptr,bool* hasFloor=nullptr,const ReturnSupport* support=nullptr) noexcept {
    if(hasFloor)*hasFloor=false;
    if(!validExplorationPosition(target)){*why="Camera target contains invalid coordinates";return false;}
    Hit hit{};ExplorationPosition start=target;start.z+=2;float distance=1024;
    if(returning){
        ExplorationReturnTrace trace{};
        if(!support||!support->valid||!(support->anchor==target)
            ||!explorationReturnTrace(target,support->floorZ,sample.extent.z,&trace)){
            *why="The saved return point has no verified supporting floor";return false;
        }
        start=trace.start;distance=trace.distance;
    }
    if(!ground(sample,start,distance,&hit)){*why="No nearby walkable static floor below the target";return false;}
    if(floorPoint)*floorPoint=hit.location;if(hasFloor)*hasFloor=true;
    if(!explorationLandingCenter(target,hit.location.z,sample.extent.z,returning,support?support->floorZ:0,result)){
        *why=returning?"The saved return floor is no longer supported":"The landing position is invalid";return false;
    }
    if(!validExplorationPosition(*result)||!clearCapsule(sample,*result)){*why="The target has insufficient room for Sam";return false;}
    return true;
}
bool move(const Sample& sample,const ExplorationPosition& target) noexcept {
    __try {return reinterpret_cast<LocationFn>(base+0x3e123e)(reinterpret_cast<void*>(sample.scene.pawn),&target,0)==1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
#else
bool inspect(void*,const CheckpointStatus&,Sample* sample,const char** why) noexcept {*sample=fixture;*why="Exploration tools ready";return fixture.valid;}
bool validateCurrent(Sample* sample,const char** why=nullptr) noexcept{
    (void)why;sample->hasSupport=false;sample->supportFloorZ=0;
    if(!sample->currentSafe||(failure&64)||!validExplorationFloor(testCurrentFloorZ)
        ||!validExplorationHalfHeight(sample->extent.z)
        ||std::fabs((sample->position.z-testCurrentFloorZ)-sample->extent.z)>ExplorationSupportTolerance)return false;
    sample->supportFloorZ=testCurrentFloorZ;sample->hasSupport=true;return true;
}
bool landing(const Sample& sample,const ExplorationPosition& target,bool returning,ExplorationPosition* result,const char** why,
    ExplorationPosition* floorPoint=nullptr,bool* hasFloor=nullptr,const ReturnSupport* support=nullptr) noexcept {
    ++landingQueries;if(hasFloor)*hasFloor=false;
    if(failure&(1|2|64)){*why="No nearby walkable static floor below the target";return false;}
    ExplorationReturnTrace trace{{target.x,target.y,target.z+2},1024};
    if(returning&&(!support||!support->valid||!(support->anchor==target)
        ||!explorationReturnTrace(target,support->floorZ,sample.extent.z,&trace))){
        *why="The saved return point has no verified supporting floor";return false;
    }
    if(!validExplorationFloor(testDestinationFloorZ)||testDestinationFloorZ>=trace.start.z
        ||testDestinationFloorZ<trace.start.z-trace.distance){*why="No nearby walkable static floor below the target";return false;}
    if(floorPoint)*floorPoint={target.x,target.y,testDestinationFloorZ};if(hasFloor)*hasFloor=true;
    if(!explorationLandingCenter(target,testDestinationFloorZ,sample.extent.z,returning,support?support->floorZ:0,result)){
        *why="The saved return floor is no longer supported";return false;
    }
    if(failure&4){*why="The target has insufficient room for Sam";return false;}
    if(failure&128)fixture.currentSafe=false;
    if(failure&256)fixture.extent.z+=10;
    if(failure&512){Lock lock;input.mask=input.mask==1?2:1;}
    if(failure&1024)testProtection=false;
    return validExplorationPosition(*result);
}
bool move(const Sample&,const ExplorationPosition& target) noexcept {
    ++nativeMoves;lastDestination=target;
    if(failure&8)return false;
    fixture.position=target;
    testCurrentFloorZ=testDestinationFloorZ;
    if(failure&32)fixture.position.x+=100;
    if(failure&16){++fixture.scene.controller;++fixture.scene.pawn;++fixture.scene.level;}
    return true;
}
#endif
ExplorationContext context(const Sample& sample,const Input& controls,std::uint64_t time) noexcept {
    ExplorationContext value{};value.identity={generation,sample.scene.controller,sample.scene.pawn,sample.scene.level};value.nowMs=time;
    value.supported=status.installed&&sample.valid&&!viewInvalid&&fresh(observedAt,time);value.focused=focused(controls)&&fresh(controls.time,time)&&freshView(sample,time)
        &&controls.mask&&(controls.mask&(controls.mask-1))==0;
    if((policy.pending()||policy.executing())&&controls.mask!=queuedMask)value.focused=false;
    value.menuOpen=controls.menu;value.neutral=controls.neutral;value.inputDraining=controls.draining||controls.camera;
    value.saveProtected=controls.protectedSaves;value.transition=sample.transition;
    value.currentSafe=sample.currentSafe;value.currentPosition=sample.position;return value;
}
void clearPreview() noexcept {
    previewRequest={};status.preview={};previewCheckedAt=0;
}
bool freshPreview(std::uint64_t time) noexcept {
    return previewCheckedAt&&time>=previewCheckedAt&&time-previewCheckedAt<=250;
}
void selectPreview(const ExplorationIdentity& identity,const ExplorationPosition& target,bool fromCamera,std::uint64_t session) noexcept {
    if(!previewRequest.valid||!(previewRequest.identity==identity)||!(previewRequest.target==target)
        ||previewRequest.fromCamera!=fromCamera||previewRequest.session!=session||previewRequest.mask!=input.mask) {
        const bool keepCheckedLive=fromCamera&&previewRequest.valid&&previewRequest.fromCamera
            &&previewRequest.identity==identity&&previewRequest.session==session&&previewRequest.mask==input.mask
            &&freshPreview(now())&&(status.preview.state==ExplorationPreviewState::Valid||status.preview.state==ExplorationPreviewState::Blocked);
        ++previewRevision;if(!previewRevision)++previewRevision;
        previewRequest={identity,target,previewRevision,session,now(),input.mask,true,fromCamera};
        if(!keepCheckedLive) {
            status.preview={};status.preview.state=ExplorationPreviewState::Pending;
            status.preview.revision=previewRevision;status.preview.generation=identity.generation;
            status.preview.controller=identity.controller;status.preview.pawn=identity.pawn;
            status.preview.session=session;status.preview.fromCamera=fromCamera;status.preview.target=target;
            status.preview.reason="Checking the landing position";previewCheckedAt=0;
        }
    } else previewRequest.publishedAt=now();
}
bool previewEligible(const ExplorationContext& value,const Input& controls,std::uint64_t time) noexcept {
    return previewRequest.valid&&value.supported&&value.focused&&previewRequest.identity==value.identity
        &&controls.mask==previewRequest.mask&&(!previewRequest.fromCamera||(controls.camera&&fresh(previewRequest.publishedAt,time)));
}
const char* policyReason(ExplorationPolicyReason reason) noexcept {
    switch(reason) {
    case ExplorationPolicyReason::Queued:case ExplorationPolicyReason::WaitingForInput:return "Release the controls to complete the request";
    case ExplorationPolicyReason::SaveProtection:return "Protected practice must be active before relocation";
    case ExplorationPolicyReason::Unfocused:return "Return to the game before using exploration tools";
    case ExplorationPolicyReason::InvalidLanding:case ExplorationPolicyReason::NativeFailure:return detail;
    case ExplorationPolicyReason::ReturnSaved:return "Safe return point saved for this scene";
    case ExplorationPolicyReason::Completed:return "Move completed; your safe return point is retained";
    case ExplorationPolicyReason::Expired:case ExplorationPolicyReason::ClockChanged:return "Request expired; choose it again";
    case ExplorationPolicyReason::NoReturnPoint:return "Save a return point or complete a first teleport";
    case ExplorationPolicyReason::StaleGeneration:case ExplorationPolicyReason::IdentityChanged:case ExplorationPolicyReason::Transition:
    case ExplorationPolicyReason::InFlightInvalidated:return "Scene changed; choose a new target and return point";
    case ExplorationPolicyReason::UnsafeCurrent:return "Stand still on clear ground, outside cover or interactions";
    default:return nullptr;
    }
}
bool supportMatches(const ExplorationIdentity& identity,const ExplorationPosition& anchor) noexcept {
    return returnSupport.valid&&returnSupport.identity==identity&&returnSupport.anchor==anchor
        &&validExplorationFloor(returnSupport.floorZ);
}
void synchronizeReturnSupport(const ExplorationIdentity& identity) noexcept {
    ExplorationPosition anchor{};
    if(!policy.returnPoint(&anchor)||!supportMatches(identity,anchor))returnSupport={};
}
void captureReturnSupport(const Sample& sample,const ExplorationIdentity& identity) noexcept {
    ExplorationPosition anchor{};
    if(sample.hasSupport&&validExplorationFloor(sample.supportFloorZ)&&policy.returnPoint(&anchor)
        &&anchor==sample.position)returnSupport={identity,anchor,sample.supportFloorZ,true};
    else returnSupport={};
}
void updateStatus(const ExplorationContext& value,const char* why) noexcept {
    status.available=value.supported&&freshView(observed,now());status.currentSafe=status.available&&value.currentSafe;status.generation=generation;status.current=value.currentPosition;
    status.pending=policy.pending()||saveQueued;status.executing=policy.executing();status.hasReturnPoint=policy.returnPoint(&status.returnPoint);
    synchronizeReturnSupport(value.identity);
    status.hasReturnSupport=status.hasReturnPoint&&supportMatches(value.identity,status.returnPoint);
    status.returnFloorZ=status.hasReturnSupport?returnSupport.floorZ:0;
    status.currentHalfHeight=observed.valid?observed.extent.z:0;
    status.hasCameraTarget=cameraValid&&cameraIdentity==value.identity;status.cameraTarget=status.hasCameraTarget?cameraTarget:ExplorationPosition{};
    const auto reason=policyReason(policy.lastReason());status.reason=saveQueued?"Release the controls to save the return point":
        !value.supported?why:!freshView(observed,now())?"Return to the game; waiting for a valid player view":
        !value.currentSafe?why:reason?reason:why;
}
void publish(const Sample& sample,const Input& controls,const char* why) noexcept {
    const auto time=now();
    if(!sample.valid||sample.transition) {
        observed=sample;observedAt=0;bound=false;cameraValid=false;saveQueued=false;clearPreview();
        ExplorationContext missing{};missing.transition=sample.transition;policy.update(missing);updateStatus(missing,why);return;
    }
    if(!bound||!(sample.scene==boundScene)) {
        ++generation;if(!generation)++generation;boundScene=sample.scene;bound=true;cameraValid=false;saveQueued=false;clearPreview();
    }
    observed=sample;observedAt=time;const auto value=context(sample,controls,time);policy.update(value);updateStatus(value,why);
}
void processPreview(void* engine,const Sample& sample,const PreviewRequest& candidate,std::uint64_t cancellation) noexcept {
    ExplorationPreview result{};result.state=ExplorationPreviewState::Blocked;
    result.revision=candidate.revision;result.generation=candidate.identity.generation;
    result.controller=candidate.identity.controller;result.pawn=candidate.identity.pawn;
    result.session=candidate.session;result.fromCamera=candidate.fromCamera;result.target=candidate.target;
    result.radius=sample.extent.x;result.reason="Landing position is unavailable";
    // Native geometry calls are read-only and run only after the original outer
    // simulation tick, with no exploration/render/input mutex held. Preview does
    // not activate practice, touch the relocation policy or capture an anchor.
    const bool valid=landing(sample,candidate.target,false,&result.landingCenter,&result.reason,&result.floorPoint,&result.hasFloor);
    Sample after{};const char* why=nullptr;
    const bool same=inspect(engine,checkpointStatus(),&after,&why)&&after.valid&&after.scene==sample.scene
        &&after.extent==sample.extent&&after.placementFlags==sample.placementFlags;
    Lock lock;const auto value=context(observed,input,now());
    if(previewRequest.revision!=candidate.revision)return;
    if(!same||inputCancellation!=cancellation||!previewEligible(value,input,now())){clearPreview();return;}
    if(valid){result.state=ExplorationPreviewState::Valid;result.reason="Clear landing position";}
    status.preview=result;previewCheckedAt=now();
}
void processTick(void* engine) noexcept {
    Input controls;std::uintptr_t viewController;std::uint64_t viewTime;bool invalidView=false;
    {Lock lock;if(!status.installed)return;controls=input;viewController=lastViewController;viewTime=lastViewAt;invalidView=viewInvalid;}
    bool saveHealthy=protectedNow(controls.protectedSaves);
    const auto checkpoint=checkpointStatus();Sample sample{};const char* why="Load supported solo gameplay to use exploration tools";
    const bool ready=inspect(engine,checkpoint,&sample,&why);
    if(ready&&invalidView)sample.valid=false;
    if(ready&&sample.valid&&fresh(viewTime,now())&&sample.scene.controller==viewController&&focused(controls)&&fresh(controls.time,now()))sample.currentSafe=validateCurrent(&sample,&why);
    else sample.currentSafe=false;
    ExplorationOperation pending{};ExplorationContext value{};bool needLanding=false,needPreview=false;
    PreviewRequest candidate{};std::uint64_t previewCancellation=0;ReturnSupport pendingSupport{};
    {Lock lock;controls=input;controls.protectedSaves=controls.protectedSaves&&saveHealthy;publish(sample,controls,why);value=context(observed,controls,now());
        if(saveQueued) {
            if(!(saveIdentity==value.identity)||controls.mask!=saveMask||!value.focused||!value.saveProtected||now()<saveQueuedAt||now()-saveQueuedAt>5000)saveQueued=false;
            else if(value.menuOpen&&value.neutral&&!value.inputDraining){
                if(observed.hasSupport&&policy.replaceReturnPoint(value))captureReturnSupport(observed,value.identity);
                saveQueued=false;
            }
            updateStatus(value,why);
        }
        if(policy.pendingOperation(&pending)&&value.supported&&value.focused&&!value.menuOpen&&value.neutral&&!value.inputDraining){
            needLanding=true;
            if(pending.action==ExplorationAction::Return&&supportMatches(pending.identity,pending.target))pendingSupport=returnSupport;
        }
        const auto time=now();
        if(!policy.pending()&&!policy.executing()&&!saveQueued&&previewEligible(value,controls,time)
            &&(!lastPreviewQueryAt||time<lastPreviewQueryAt||time-lastPreviewQueryAt>=100)) {
            candidate=previewRequest;previewCancellation=inputCancellation;lastPreviewQueryAt=time;needPreview=true;
        }
    }
    if(needPreview)processPreview(engine,sample,candidate,previewCancellation);
    if(!needLanding)return;
    ExplorationPosition destination{};const char* landingReason="Native collision-checked move was refused; return point retained";
    const bool validLanding=sample.currentSafe&&landing(sample,pending.target,pending.action==ExplorationAction::Return,
        &destination,&landingReason,nullptr,nullptr,&pendingSupport);
    Sample freshSample{};const char* freshWhy=why;
    const bool stillReady=inspect(engine,checkpointStatus(),&freshSample,&freshWhy)&&freshSample.valid&&freshSample.scene==sample.scene
        &&closeTo(freshSample.position,sample.position);
    if(stillReady)freshSample.currentSafe=freshSample.currentSafe&&sample.currentSafe&&freshSample.extent==sample.extent
        &&freshSample.placementFlags==sample.placementFlags&&validateCurrent(&freshSample);
    saveHealthy=protectedNow(controls.protectedSaves);
    ExplorationOperation accepted{};bool taken=false;std::uint64_t acceptedInput=0;
    {Lock lock;controls=input;controls.protectedSaves=controls.protectedSaves&&saveHealthy;
        if(!stillReady){freshSample.valid=false;publish(freshSample,controls,"Player or scene changed during validation");return;}
        publish(freshSample,controls,why);value=context(observed,controls,now());detail=landingReason;
        const bool hadAnchor=policy.hasReturnPoint();
        taken=policy.take(value,pending.token,validLanding&&freshSample.hasSupport,&accepted);
        if(taken&&!hadAnchor&&accepted.action==ExplorationAction::Teleport)captureReturnSupport(freshSample,value.identity);
        acceptedInput=inputCancellation;updateStatus(value,why);
    }
    if(!taken)return;
    // No exploration lock is held across native collision/movement callbacks.
    // The standard SetLocation entry invokes checked FindSpot/encroachment and
    // native collision membership/volume/attachment updates; no actor fields are
    // written by this mod, and FarMoveActor's mutating test mode is never used.
    bool moved=false;
    {Input latest;bool uninterrupted=false;{Lock lock;latest=input;uninterrupted=inputCancellation==acceptedInput;}
        if(uninterrupted&&focused(latest)&&latest.mask==queuedMask&&fresh(latest.time,now())&&protectedNow(latest.protectedSaves)
            &&!latest.menu&&latest.neutral&&!latest.draining&&!latest.camera)moved=move(freshSample,destination);
    }
    Sample after{};const char* afterWhy=why;const bool afterReady=inspect(engine,checkpointStatus(),&after,&afterWhy)&&after.valid;
    if(afterReady&&focused(controls))after.currentSafe=validateCurrent(&after);
    const bool success=moved&&afterReady&&after.currentSafe&&after.scene==freshSample.scene&&closeTo(after.position,destination);
    saveHealthy=protectedNow(controls.protectedSaves);
    {Lock lock;controls=input;controls.protectedSaves=controls.protectedSaves&&saveHealthy;publish(after,controls,afterWhy);detail=success?"Move completed":"Move could not be verified; return point retained";
        const bool current=policy.complete(accepted.token,success);
        if(current&&success){if(accepted.action==ExplorationAction::Return)++status.returns;else ++status.moves;}
        updateStatus(context(observed,controls,now()),afterWhy);
    }
}
std::uintptr_t dispatchTick(void* engine,float delta) {
    struct Scope {bool previous;Scope():previous(insideTick){insideTick=true;}~Scope(){insideTick=previous;}} scope;
    const auto result=originalTick?originalTick(engine,delta):0;
    if(!scope.previous)processTick(engine);
    return result;
}
#ifndef CE_TEST_HOST
std::uintptr_t __fastcall tickHook(void* engine,void*,float delta) {return dispatchTick(engine,delta);}
#else
std::uintptr_t __fastcall nestedFixtureTick(void* engine,void*,float delta) {
    if(testNest){testNest=false;dispatchTick(engine,delta);movesInsideNested=nativeMoves;}
    return 0x13572468;
}
#endif
bool queue(ExplorationAction action,std::uint64_t requestedGeneration) noexcept {
    Lock lock;const auto value=context(observed,input,now());
    if(saveQueued)return false;
    bool result=false;
    if(action==ExplorationAction::Teleport)result=cameraValid&&cameraIdentity==value.identity&&policy.queueTeleport(cameraTarget,requestedGeneration,value);
    else {
        ExplorationPosition anchor{};
        if(!policy.returnPoint(&anchor)||!supportMatches(value.identity,anchor)){
            status.reason="The saved return point has no verified supporting floor";return false;
        }
        result=policy.queueReturn(requestedGeneration,value);
    }
    if(result)queuedMask=input.mask;
    updateStatus(value,"Exploration tools ready");return result;
}
}
bool initializeExploration(bool twoInstanceMode) noexcept {
    Lock lock;
    if(twoInstanceMode){status.reason="Exploration inactive in Fusion Fix two-instance mode";return false;}
#ifdef CE_TEST_HOST
    status.installed=true;status.reason="Waiting for a supported gameplay view";return true;
#else
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(!signatures()){status.reason="Native exploration functions differ from supported build";return false;}
    auto target=reinterpret_cast<void*>(base+0x39efc5);
    if(MH_CreateHook(target,tickHook,reinterpret_cast<void**>(&originalTick))!=MH_OK){status.reason="Native simulation tick hook unavailable";return false;}
    if(MH_EnableHook(target)!=MH_OK){MH_DisableHook(target);status.reason="Native simulation tick hook could not be enabled";return false;}
    status.installed=true;status.reason="Load solo gameplay to use exploration tools";return true;
#endif
}
ExplorationStatus explorationStatus() noexcept {
    Lock lock;auto copy=status;const auto time=now();
    if(!fresh(observedAt,time)){copy.available=false;copy.currentSafe=false;copy.hasCameraTarget=false;copy.reason="Waiting for a supported gameplay view";}
    if(!previewEligible(context(observed,input,time),input,time))copy.preview={};
    else if(copy.preview.state!=ExplorationPreviewState::Pending&&!freshPreview(time)) {
        copy.preview.state=ExplorationPreviewState::Pending;copy.preview.hasFloor=false;copy.preview.reason="Checking the landing position";
    }
    return copy;
}
void pollExplorationInput(HWND window,unsigned connectedMask,bool menuOpen,bool neutral,bool inputDraining,bool cameraOwned,bool practiceHealthy) noexcept {
    Lock lock;input={window,connectedMask,now(),menuOpen,neutral,inputDraining,cameraOwned,practiceHealthy};
    const bool invalidMask=!connectedMask||(connectedMask&(connectedMask-1));
    if(invalidMask){input.draining=true;input.neutral=false;}
    const bool changedOwner=((policy.pending()||policy.executing())&&connectedMask!=queuedMask)
        ||(saveQueued&&connectedMask!=saveMask);
    if(invalidMask||changedOwner||!focused(input)) {
        ++inputCancellation;if(!inputCancellation)++inputCancellation;
        policy.cancel();saveQueued=false;clearPreview();
        // Input publication can happen between simulation ticks, including a
        // disconnect/reconnect or focus loss/regain pair. Latch cancellation
        // immediately without discarding this scene's established return point.
        ExplorationContext cancelled{};cancelled.identity=policy.identity();
        cancelled.supported=cancelled.identity.valid();cancelled.nowMs=now();
        cancelled.focused=false;cancelled.saveProtected=practiceHealthy;
        policy.update(cancelled);updateStatus(context(observed,input,now()),"Exploration request cancelled by input change");
    }
}
bool rememberExplorationCamera(const FreeCameraStatus& camera) noexcept {
    Lock lock;const auto value=context(observed,input,now());
    if(!value.supported||!camera.active||!camera.available||camera.controller!=observed.scene.controller||camera.actor!=observed.scene.pawn
        ||!validExplorationPosition({camera.pose.x,camera.pose.y,camera.pose.z}))return false;
    cameraTarget={camera.pose.x,camera.pose.y,camera.pose.z};cameraIdentity=value.identity;cameraValid=true;
    selectPreview(cameraIdentity,cameraTarget,false,0);updateStatus(value,"Camera destination selected");return true;
}
void publishExplorationPreview(const FreeCameraStatus& camera,bool explorationFlight) noexcept {
    Lock lock;const auto value=context(observed,input,now());
    if(!value.supported||!value.focused){clearPreview();return;}
    if(explorationFlight) {
        const ExplorationPosition point{camera.pose.x,camera.pose.y,camera.pose.z};
        if(!camera.active||!camera.available||!camera.session||!input.camera||camera.controller!=observed.scene.controller
            ||camera.actor!=observed.scene.pawn||!validExplorationPosition(point)){clearPreview();return;}
        selectPreview(value.identity,point,true,camera.session);
    } else if(cameraValid&&cameraIdentity==value.identity)selectPreview(cameraIdentity,cameraTarget,false,0);
    else clearPreview();
}
bool requestExplorationTeleport(std::uint64_t requestedGeneration) noexcept{return queue(ExplorationAction::Teleport,requestedGeneration);}
bool requestExplorationReturn(std::uint64_t requestedGeneration) noexcept{return queue(ExplorationAction::Return,requestedGeneration);}
bool requestExplorationSaveReturn(std::uint64_t requestedGeneration) noexcept {
    Lock lock;auto value=context(observed,input,now());policy.update(value);
    if(saveQueued||policy.pending()||policy.executing()||!value.supported||!value.focused||!value.saveProtected||!value.menuOpen
        ||!requestedGeneration||requestedGeneration!=value.identity.generation||!value.currentSafe)return false;
    saveQueued=true;saveIdentity=value.identity;saveQueuedAt=now();saveMask=input.mask;updateStatus(value,"Release the controls to save the return point");return true;
}
void cancelExplorationRequest() noexcept {Lock lock;policy.cancel();saveQueued=false;updateStatus(context(observed,input,now()),"Request cancelled");}
void explorationNativeView(void* controller,bool validPawnView) noexcept {
#ifndef CE_TEST_HOST
    if(!primaryView(controller))return;
#else
    {Lock lock;if(fixture.scene.controller!=reinterpret_cast<std::uintptr_t>(controller))return;}
#endif
    Lock lock;viewInvalid=!validPawnView;lastViewController=validPawnView?reinterpret_cast<std::uintptr_t>(controller):0;lastViewAt=validPawnView?now():0;
}
#ifdef CE_TEST_HOST
void testExplorationReset() noexcept {Lock lock;status={};policy={};returnSupport={};input={};observed={};boundScene={};fixture={};generation=observedAt=lastViewAt=saveQueuedAt=inputCancellation=previewRevision=lastPreviewQueryAt=previewCheckedAt=0;lastViewController=0;bound=saveQueued=cameraValid=viewInvalid=false;cameraIdentity={};saveIdentity={};cameraTarget={};previewRequest={};detail="Exploration tools ready";testNow=1000;testFocused=testProtection=true;testNest=false;queuedMask=saveMask=movesInsideNested=0;failure=nativeMoves=landingQueries=0;testHalfHeight=90;testCurrentFloorZ=testDestinationFloorZ=-92;lastDestination={};status.installed=true;originalTick=nullptr;insideTick=false;}
void testExplorationClock(std::uint64_t value) noexcept {Lock lock;testNow=value;}
void testExplorationFocused(bool value) noexcept {Lock lock;testFocused=value;}
void testExplorationBody(float halfHeight,float currentFloorZ,float destinationFloorZ) noexcept {
    Lock lock;testHalfHeight=halfHeight;testCurrentFloorZ=currentFloorZ;testDestinationFloorZ=destinationFloorZ;
    fixture.extent.z=halfHeight;fixture.position.z=currentFloorZ+halfHeight+ExplorationLandingClearance;
    fixture.currentSafe=validateCurrent(&fixture);publish(fixture,input,"Exploration body fixture updated");
}
void testExplorationClearReturnSupport() noexcept {
    Lock lock;returnSupport={};updateStatus(context(observed,input,now()),"Return support fixture cleared");
}
void testExplorationView(std::uint32_t identity,const ExplorationPosition& position,bool valid,bool currentSafe) noexcept {
    Lock lock;fixture={};fixture.scene.engine=100;fixture.scene.controller=identity;fixture.scene.pawn=identity+3;fixture.scene.level=identity+4;fixture.scene.camera=identity+1;fixture.scene.impl=identity+2;fixture.scene.profile=200;fixture.scene.connection=300;fixture.scene.connectionBlock=301;std::memcpy(fixture.scene.map,"_Single\\08_ThirdEchelon_A.unr",sizeof("_Single\\08_ThirdEchelon_A.unr"));fixture.position=position;fixture.valid=valid;fixture.currentSafe=currentSafe;fixture.extent={35,35,testHalfHeight};lastViewController=identity;lastViewAt=now();viewInvalid=false;publish(fixture,input,"Exploration tools ready");
}
void testExplorationContextView(std::uint32_t identity,const ExplorationPosition& position,
    const char* map,const char* connection,bool splitScreen,bool currentSafe) noexcept {
    Lock lock;fixture={};fixture.scene.engine=100;fixture.scene.controller=identity;
    fixture.scene.pawn=identity+3;fixture.scene.level=identity+4;fixture.scene.camera=identity+1;
    fixture.scene.impl=identity+2;fixture.scene.profile=200;fixture.scene.connection=300;fixture.scene.connectionBlock=301;
    char session[64]{};std::size_t mapSize=0,sessionSize=0;
    if(map)while(mapSize<sizeof(fixture.scene.map)&&map[mapSize])++mapSize;
    if(connection)while(sessionSize<sizeof(session)&&connection[sessionSize])++sessionSize;
    const bool bounded=map&&connection&&mapSize<sizeof(fixture.scene.map)&&sessionSize<sizeof(session);
    if(bounded){std::memcpy(fixture.scene.map,map,mapSize);std::memcpy(session,connection,sessionSize);}
    const auto kind=bounded?gameplayKind(fixture.scene.map,static_cast<int>(mapSize+1),
        static_cast<int>(mapSize+1),session,static_cast<int>(sessionSize),splitScreen):GameplayKind::Unavailable;
    fixture.position=position;fixture.extent={35,35,testHalfHeight};fixture.valid=kind!=GameplayKind::Unavailable;
    fixture.currentSafe=fixture.valid&&currentSafe;
    lastViewController=fixture.valid?identity:0;lastViewAt=fixture.valid?now():0;viewInvalid=!fixture.valid;
    publish(fixture,input,fixture.valid?"Exploration tools ready":"Unsupported map or session");
}
void testExplorationFailure(unsigned value) noexcept {Lock lock;failure=value;}
void testExplorationTick() noexcept {dispatchTick(nullptr,0);}
bool testExplorationNestedTick() noexcept {const auto before=nativeMoves;movesInsideNested=~0u;testNest=true;originalTick=reinterpret_cast<TickFn>(nestedFixtureTick);const auto result=dispatchTick(nullptr,0);originalTick=nullptr;return movesInsideNested==before&&result==0x13572468;}
void testExplorationNativeProtection(bool value) noexcept {Lock lock;testProtection=value;}
bool testExplorationStaticFloor(std::uintptr_t table,unsigned flags,unsigned physics,bool sameLevel,
    std::uintptr_t ownBase,const ExplorationPosition& velocity) noexcept {
    return staticFloorProperties(sameLevel,flags,physics,table,ownBase,velocity,0x00ffa30c);
}
unsigned testExplorationNativeMoves() noexcept {Lock lock;return nativeMoves;}
unsigned testExplorationLandingQueries() noexcept {Lock lock;return landingQueries;}
ExplorationPosition testExplorationLastDestination() noexcept {Lock lock;return lastDestination;}
#endif
}
