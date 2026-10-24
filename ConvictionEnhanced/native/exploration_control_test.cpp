#include "exploration_control.h"
#include "checkpoint_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>

// The standalone host has no game checkpoint system. The integrated graphics
// host links the real checkpoint test backend instead of this executable.
namespace ce {CheckpointStatus checkpointStatus() noexcept{return {};}}
namespace {
unsigned checks=0;
void require(bool result,const char* message){++checks;if(!result){std::cerr<<"FAIL: "<<message<<"\n";std::exit(1);}}
void input(bool menu=true,bool neutral=true,bool protectedSaves=true,unsigned mask=1,bool draining=false,bool camera=false) {
    ce::pollExplorationInput(nullptr,mask,menu,neutral,draining,camera,protectedSaves);
}
void view(unsigned id=41,const ce::ExplorationPosition& point={10,20,0},bool valid=true,bool safe=true) {
    ce::testExplorationView(id,point,valid,safe);
}
std::uint64_t generation(){return ce::explorationStatus().generation;}
ce::FreeCameraStatus camera(unsigned id=41,const ce::ExplorationPosition& point={100,200,30}) {
    ce::FreeCameraStatus value;value.installed=value.active=value.available=true;value.controller=id;value.actor=id+3;
    value.session=9;
    value.pose={point.x,point.y,point.z,0,0,0};return value;
}
void setup(bool protectedSaves=true) {
    ce::testExplorationReset();input(true,true,protectedSaves);view();
    require(ce::explorationStatus().available&&ce::explorationStatus().currentSafe,"Current safe context available");
    require(ce::rememberExplorationCamera(camera()),"Deliberate matching camera target captured");
}
void releaseAndTick(){input(false,true);ce::testExplorationTick();}
void save() {
    input();require(ce::requestExplorationSaveReturn(generation()),"Save return request accepted");ce::testExplorationTick();
    require(ce::explorationStatus().hasReturnPoint,"Return point saved");
}
void livePreview(bool safe=true) {
    ce::testExplorationReset();input(false,false,false,1,false,true);view(41,{10,20,0},true,safe);
    ce::publishExplorationPreview(camera(),true);
}
void soloDeniableOpsExploration() {
    using Preview=ce::ExplorationPreviewState;
    ce::testExplorationReset();input(false,false,false,1,false,true);
    auto contextView=[](const char* map,const char* connection="SOLO",bool split=false){
        ce::testExplorationContextView(41,{10,20,0},map,connection,split,true);
    };
    require(!ce::checkpointStatus().available,"Solo Ops host has no campaign checkpoint eligibility");
    contextView("C01_MafiaClub");
    require(ce::explorationStatus().available,"Real solo Hunter map predicate enables Exploration without campaign checkpoints");
    auto flight=camera();flight.gameplay=ce::GameplayKind::SoloDeniableOps;
    ce::publishExplorationPreview(flight,true);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::Valid&&ce::explorationStatus().preview.fromCamera,
        "Solo Ops live flight validates its landing marker");
    require(!ce::explorationStatus().hasReturnPoint&&ce::testExplorationNativeMoves()==0,
        "Solo Ops preview does not save an anchor or move the player");
    require(ce::rememberExplorationCamera(flight),"Solo Ops deliberate camera handoff selects the destination");
    ce::testExplorationClock(1100);input(true,true,false);ce::publishExplorationPreview({},false);ce::testExplorationTick();
    auto selected=ce::explorationStatus().preview;
    require(selected.state==Preview::Valid&&!selected.fromCamera&&selected.target==ce::ExplorationPosition{100,200,30},
        "Solo Ops selected marker retains and rechecks the exact destination");
    require(!ce::requestExplorationTeleport(generation()),"Solo Ops marker cannot bypass protected practice");
    save();const auto anchor=ce::explorationStatus().returnPoint;
    require(ce::testExplorationNativeMoves()==0&&anchor==ce::ExplorationPosition{10,20,0},
        "Solo Ops manual return-point save is stationary and player based");
    input(true,false);require(ce::requestExplorationTeleport(generation()),"Protected solo Ops teleport queues");
    ce::testExplorationTick();require(ce::testExplorationNativeMoves()==0,"Solo Ops held confirmation cannot move");
    releaseAndTick();require(ce::explorationStatus().moves==1&&ce::explorationStatus().current==ce::ExplorationPosition{100,200,0},
        "Solo Ops teleport uses the checked floor");
    require(ce::explorationStatus().returnPoint==anchor,"Solo Ops outbound preserves the original anchor");
    input();require(ce::requestExplorationReturn(generation()),"Protected solo Ops return queues");releaseAndTick();
    require(ce::explorationStatus().returns==1&&ce::explorationStatus().current==anchor&&ce::explorationStatus().returnPoint==anchor,
        "Solo Ops return reaches and retains its original anchor");
    const auto previous=generation();input();contextView("C02_Embassy");
    require(ce::explorationStatus().available&&generation()!=previous&&!ce::explorationStatus().hasReturnPoint
        &&!ce::explorationStatus().hasCameraTarget&&ce::explorationStatus().preview.state==Preview::None,
        "Solo Ops map change at reused object addresses clears every old coordinate");
    for(const char* session:{"LINK","XBOX","SPLIT","Party","Invited"}){
        contextView("C01_MafiaClub");save();require(ce::rememberExplorationCamera(flight),"Solo fixture destination selected before unsupported transition");
        contextView("C01_MafiaClub",session);ce::publishExplorationPreview(flight,true);ce::testExplorationTick();
        require(!ce::explorationStatus().available&&!ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().hasCameraTarget
            &&ce::explorationStatus().preview.state==Preview::None,"Non-solo connection clears Exploration coordinates and marker");
        require(!ce::requestExplorationTeleport(generation())&&!ce::requestExplorationReturn(generation()),
            "Non-solo session cannot queue relocation");
    }
    contextView("C01_MafiaClub");save();contextView("C01_MafiaClub","SOLO",true);
    require(!ce::explorationStatus().available&&!ce::explorationStatus().hasReturnPoint,"Split-screen refuses even a SOLO-labelled context");
    for(const char* map:{"C05_Antonov","T02_Unknown","Unknown.unr"}){
        contextView(map);require(!ce::explorationStatus().available,"Unsupported or co-op-story map cannot enable Exploration");
    }
    contextView("_Single\\08_ThirdEchelon_A.unr");
    require(ce::explorationStatus().available,"Campaign real-predicate fixture remains supported after solo Ops checks");
}
void floorRelativeReturns() {
    setup();save();
    require(ce::explorationStatus().hasReturnSupport&&ce::explorationStatus().returnFloorZ==-92
        &&ce::explorationStatus().currentHalfHeight==90,"Manual save binds actual native support and body diagnostics");
    require(ce::requestExplorationTeleport(generation()),"Standing outbound queued");releaseAndTick();
    ce::testExplorationBody(48,-92,-92);input();
    require(ce::explorationStatus().current.z==-42&&ce::explorationStatus().currentHalfHeight==48,"Crouch fixture changes body center without changing floor");
    require(ce::requestExplorationReturn(generation()),"Standing anchor queues crouched return");releaseAndTick();
    auto status=ce::explorationStatus();
    require(status.returns==1&&status.current==ce::ExplorationPosition{10,20,-42},"Standing-to-crouched return reaches original XY and floor");
    require(status.returnPoint==ce::ExplorationPosition{10,20,0}&&status.hasReturnSupport&&status.returnFloorZ==-92,
        "Crouched return retains original anchor and actual supporting floor");

    setup();ce::testExplorationBody(48,-92,-92);save();
    require(ce::explorationStatus().returnPoint.z==-42,"Crouched anchor records its actual original center");
    require(ce::requestExplorationTeleport(generation()),"Crouched outbound queued");releaseAndTick();
    ce::testExplorationBody(90,-92,-92);input();
    require(ce::requestExplorationReturn(generation()),"Crouched anchor queues standing return");releaseAndTick();status=ce::explorationStatus();
    require(status.returns==1&&status.current==ce::ExplorationPosition{10,20,0},"Crouched-to-standing return reaches original floor with full body clearance");
    require(status.returnPoint.z==-42&&status.returnFloorZ==-92,"Standing return does not rewrite the crouched anchor reference");

    for(float changedFloor:{-101.f,-83.f}){
        setup();save();require(ce::requestExplorationTeleport(generation()),"Outbound before changed support queued");releaseAndTick();
        ce::testExplorationBody(48,-92,changedFloor);input();require(ce::requestExplorationReturn(generation()),"Changed-floor return queues for fresh validation");
        const auto attempts=ce::testExplorationNativeMoves();releaseAndTick();status=ce::explorationStatus();
        require(ce::testExplorationNativeMoves()==attempts&&status.returns==0,"Floor changed beyond eight units refuses native movement");
        require(status.hasReturnSupport&&status.returnFloorZ==-92&&status.hasReturnPoint,"Changed-floor refusal preserves the original recovery support");
    }
    for(float changedFloor:{-100.f,-84.f}){
        setup();save();require(ce::requestExplorationTeleport(generation()),"Outbound before tolerated support shift queued");releaseAndTick();
        ce::testExplorationBody(48,-92,changedFloor);input();require(ce::requestExplorationReturn(generation()),"Near-floor return queued");releaseAndTick();status=ce::explorationStatus();
        require(status.returns==1&&status.current==ce::ExplorationPosition{10,20,changedFloor+50},"Fresh floor within eight units supports current body");
        require(status.returnFloorZ==-92,"Tolerated floor shift does not silently reanchor the reference");
    }
    for(unsigned failureFlag:{1u,4u,64u}){
        setup();ce::testExplorationBody(48,-92,-92);save();require(ce::requestExplorationTeleport(generation()),"Low-stance outbound before query failure queued");releaseAndTick();
        ce::testExplorationBody(90,-92,-92);input();require(ce::requestExplorationReturn(generation()),"Standing return checks obstruction and native query health");
        ce::testExplorationFailure(failureFlag);const auto attempts=ce::testExplorationNativeMoves();releaseAndTick();status=ce::explorationStatus();
        require(ce::testExplorationNativeMoves()==attempts&&status.returns==0,"Tall body obstruction or failed ground query refuses return");
        require(status.hasReturnSupport&&status.returnFloorZ==-92,"Failed query cannot replace saved support with unverified ground");
    }

    setup();ce::testExplorationBody(48,-92,-92);require(ce::requestExplorationTeleport(generation()),"Automatic crouched anchor request queued");
    ce::testExplorationFailure(8);releaseAndTick();status=ce::explorationStatus();
    require(status.moves==0&&status.hasReturnPoint&&status.hasReturnSupport&&status.returnFloorZ==-92,
        "Failed native outbound retains the automatically captured actual floor");
    ce::testExplorationFailure(0);ce::testExplorationBody(90,-92,-92);input();
    require(ce::requestExplorationReturn(generation()),"Failed outbound anchor can be used from another stance");releaseAndTick();
    require(ce::explorationStatus().returns==1&&ce::explorationStatus().current.z==0,"Recovery after failed outbound uses the saved floor");

    setup();save();ce::testExplorationBody(48,-300,-300);save();status=ce::explorationStatus();
    require(status.returnPoint.z==-250&&status.returnFloorZ==-300&&status.hasReturnSupport,"Explicit replacement refreshes actual support with the new anchor");
    require(ce::requestExplorationTeleport(generation()),"Outbound from replaced floor queued");releaseAndTick();ce::testExplorationBody(90,-300,-300);input();
    require(ce::requestExplorationReturn(generation()),"Replacement return queues with changed stance");releaseAndTick();
    require(ce::explorationStatus().returns==1&&ce::explorationStatus().current.z==-208,"Replacement cannot reuse stale support from the previous floor");

    setup();save();ce::testExplorationClearReturnSupport();
    require(ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().hasReturnSupport,"Missing support cannot be inferred from saved center");
    require(!ce::requestExplorationReturn(generation())&&ce::testExplorationNativeMoves()==0,"Missing support fails closed before queueing native return");
    save();require(ce::explorationStatus().hasReturnSupport,"A fresh explicit replacement repairs support by real validation");
    ce::testExplorationFocused(false);input();
    require(ce::explorationStatus().hasReturnSupport&&ce::explorationStatus().returnFloorZ==-92,"Same-scene focus loss preserves verified support");
    ce::testExplorationFocused(true);input();view(51);
    require(!ce::explorationStatus().hasReturnSupport&&!ce::explorationStatus().hasReturnPoint,"New scene clears anchor and support together");

    setup();save();require(ce::requestExplorationTeleport(generation()),"Outbound before reentrant return queued");releaseAndTick();
    input();require(ce::requestExplorationReturn(generation()),"Reentrant return queued");ce::testExplorationFailure(16);releaseAndTick();
    require(!ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().hasReturnSupport&&ce::explorationStatus().returns==0,
        "Reentrant scene change cannot revive old support on return completion");
}
}
int main() {
    // Exact passive retail observation: stationary, unattached Third Echelon
    // AStaticMeshActor floor, vtable0xFFA30C, flags0xA and physics10.
    require(ce::testExplorationStaticFloor(0xffa30c,0xa,10,true,0,{}),"Observed retail static-mesh floor is safe");
    require(!ce::testExplorationStaticFloor(0xffa30c,0x8,10,true,0,{}),"Dynamic mesh lacks immovable flag");
    require(!ce::testExplorationStaticFloor(0xffa310,0xa,10,true,0,{}),"Other physics10 classes are not admitted");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,10,false,0,{}),"Other-level floor refused");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,10,true,0x123456,{}),"Attached static mesh refused");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,10,true,0,{.01f,0,0}),"Moving mesh X refused");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,10,true,0,{0,.01f,0}),"Moving mesh Y refused");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,10,true,0,{0,0,.01f}),"Moving mesh Z refused");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,1,true,0,{}),"Walking actor cannot be a floor");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,11,true,0,{}),"Unverified physics mode refused");
    require(!ce::testExplorationStaticFloor(0xffa30c,0xa,10,true,0,{std::numeric_limits<float>::quiet_NaN(),0,0}),"Invalid mesh velocity refused");
    require(ce::testExplorationStaticFloor(0x123456,0x2,0,true,0,{}),"Existing immovable physics0 floor remains supported");
    setup(false);
    require(!ce::requestExplorationTeleport(generation()),"Unprotected teleport refused");
    require(!ce::requestExplorationSaveReturn(generation()),"Unprotected anchor request refused");
    require(ce::testExplorationNativeMoves()==0,"Unprotected requests never move");
    input();require(!ce::requestExplorationTeleport(generation()+1),"Stale generation refused");
    auto wrong=camera(99);require(!ce::rememberExplorationCamera(wrong),"Foreign pawn camera refused");
    wrong=camera();wrong.active=false;require(!ce::rememberExplorationCamera(wrong),"Inactive camera refused");

    setup();input(true,false);require(ce::requestExplorationSaveReturn(generation()),"Held Cross can only queue save");
    ce::testExplorationTick();require(ce::explorationStatus().pending&&!ce::explorationStatus().hasReturnPoint,"Held save waits for release");
    input();ce::testExplorationTick();require(ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().pending,"Neutral save completes in menu");
    require(ce::testExplorationNativeMoves()==0,"Save anchor never moves the world");
    require(ce::explorationStatus().returnPoint==ce::ExplorationPosition{10,20,0},"Anchor is real pawn XYZ, not camera");

    require(ce::requestExplorationTeleport(generation()),"Protected teleport queued");
    require(!ce::requestExplorationReturn(generation()),"Busy request refused");
    ce::testExplorationTick();require(ce::testExplorationNativeMoves()==0,"Open menu blocks movement");
    input(false,false,true,1,true);ce::testExplorationTick();require(ce::testExplorationNativeMoves()==0,"Drain blocks movement");
    input(false,true,true,1,false,true);ce::testExplorationTick();require(ce::testExplorationNativeMoves()==0,"Owned free camera blocks movement");
    releaseAndTick();require(ce::testExplorationNativeMoves()==1,"One neutral dispatch");
    require(ce::explorationStatus().current==ce::ExplorationPosition{100,200,0},"Camera target grounded before moving");
    require(ce::explorationStatus().moves==1&&ce::explorationStatus().returns==0,"Successful outbound counted");
    require(ce::explorationStatus().returnPoint==ce::ExplorationPosition{10,20,0},"Outbound preserves original anchor");
    ce::testExplorationTick();require(ce::testExplorationNativeMoves()==1,"Consumed request does not replay");
    input();require(ce::requestExplorationReturn(generation()),"Return request queued");releaseAndTick();
    require(ce::explorationStatus().current==ce::ExplorationPosition{10,20,0}&&ce::explorationStatus().returns==1,"Native return reaches original anchor");
    require(ce::explorationStatus().hasReturnPoint,"Return keeps anchor available");

    for(unsigned flags:{1u,2u,4u,64u,128u,256u,512u,1024u}) {
        setup();save();const auto anchor=ce::explorationStatus().returnPoint;
        require(ce::requestExplorationTeleport(generation()),"Failure scenario queued");
        ce::testExplorationFailure(flags);releaseAndTick();
        require(ce::testExplorationNativeMoves()==0,"Rejected landing/context never invokes movement");
        require(!ce::explorationStatus().pending&&!ce::explorationStatus().executing,"Failure clears pending/busy");
        require(ce::explorationStatus().current==ce::ExplorationPosition{10,20,0},"Rejected request preserves pawn position");
        require(ce::explorationStatus().hasReturnPoint&&ce::explorationStatus().returnPoint==anchor,"Rejected request keeps recovery anchor");
    }
    for(unsigned flags:{8u,32u}) {
        setup();require(ce::requestExplorationTeleport(generation()),"Native failure queued");
        ce::testExplorationFailure(flags);releaseAndTick();
        require(ce::testExplorationNativeMoves()==1,"One native attempt");
        require(ce::explorationStatus().moves==0&&ce::explorationStatus().hasReturnPoint,"Native failure keeps anchor but is not success");
        require(!ce::explorationStatus().executing,"Native failure releases busy");
    }
    setup();require(ce::requestExplorationTeleport(generation()),"Reentrant identity scenario queued");
    ce::testExplorationFailure(16);releaseAndTick();
    require(ce::testExplorationNativeMoves()==1&&!ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().hasCameraTarget,"Reentrant scene change cannot resurrect old coordinates");
    require(ce::explorationStatus().moves==0&&!ce::explorationStatus().executing,"Stale completion not counted");

    setup();require(ce::requestExplorationTeleport(generation()),"Nested tick scenario queued");input(false,true);
    require(ce::testExplorationNestedTick(),"Nested original simulation never executes relocation");
    require(ce::testExplorationNativeMoves()==1,"Only outer post-original tick dispatches");

    setup();require(ce::requestExplorationTeleport(generation()),"Disconnect scenario queued");input(false,true,true,0);ce::testExplorationTick();
    require(!ce::explorationStatus().pending&&ce::testExplorationNativeMoves()==0,"Disconnect cancels request");
    setup();require(ce::requestExplorationTeleport(generation()),"Changed controller scenario queued");input(false,true,true,2);ce::testExplorationTick();
    require(!ce::explorationStatus().pending&&ce::testExplorationNativeMoves()==0&&!ce::explorationStatus().hasReturnPoint,"Controller replacement cannot inherit request or capture anchor");
    setup();input(true,false);require(ce::requestExplorationSaveReturn(generation()),"Save for controller change queued");input(true,true,true,2);ce::testExplorationTick();
    require(!ce::explorationStatus().pending&&!ce::explorationStatus().hasReturnPoint,"Changed controller cancels save");
    setup();input(true,false);require(ce::requestExplorationSaveReturn(generation()),"Save for focus loss queued");ce::testExplorationFocused(false);ce::testExplorationTick();
    require(!ce::explorationStatus().pending&&!ce::explorationStatus().hasReturnPoint,"Focus loss cancels deferred save");
    setup();save();require(ce::requestExplorationTeleport(generation()),"Transient disconnect scenario queued");
    input(false,true,true,0);require(!ce::explorationStatus().pending&&ce::explorationStatus().hasReturnPoint,"Disconnect publication immediately cancels and retains anchor");
    input(false,true,true,1);ce::testExplorationTick();
    require(ce::testExplorationNativeMoves()==0&&ce::explorationStatus().hasReturnPoint,"Reconnect before tick cannot revive teleport");
    setup();save();require(ce::requestExplorationTeleport(generation()),"Transient focus loss scenario queued");
    ce::testExplorationFocused(false);input(false,true);require(!ce::explorationStatus().pending&&ce::explorationStatus().hasReturnPoint,"Focus-loss publication immediately cancels and retains anchor");
    ce::testExplorationFocused(true);input(false,true);ce::testExplorationTick();
    require(ce::testExplorationNativeMoves()==0&&ce::explorationStatus().hasReturnPoint,"Focus regain before tick cannot revive teleport");
    setup();input(true,false);require(ce::requestExplorationSaveReturn(generation()),"Transient disconnect deferred save queued");
    input(true,true,true,0);input(true,true,true,1);ce::testExplorationTick();
    require(!ce::explorationStatus().pending&&!ce::explorationStatus().hasReturnPoint,"Transient disconnect cannot revive deferred save");
    setup();input(true,false);require(ce::requestExplorationSaveReturn(generation()),"Transient focus deferred save queued");
    ce::testExplorationFocused(false);input();ce::testExplorationFocused(true);input();ce::testExplorationTick();
    require(!ce::explorationStatus().pending&&!ce::explorationStatus().hasReturnPoint,"Transient focus loss cannot revive deferred save");
    setup();require(ce::requestExplorationTeleport(generation()),"Failed native save guard queued");ce::testExplorationNativeProtection(false);releaseAndTick();
    require(ce::testExplorationNativeMoves()==0&&!ce::explorationStatus().pending,"Fresh native save health vetoes cached permission");

    setup();save();auto oldGeneration=generation();view(51);
    require(generation()!=oldGeneration&&!ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().hasCameraTarget,"New pawn/level clears target and anchor");
    setup();save();oldGeneration=generation();ce::testExplorationClock(2500);input(true,true);ce::testExplorationFocused(false);ce::testExplorationTick();
    require(ce::explorationStatus().hasReturnPoint&&generation()==oldGeneration&&!ce::explorationStatus().available,"Stale background view retains same-scene anchor");
    ce::testExplorationFocused(true);view();input();ce::testExplorationTick();
    require(ce::explorationStatus().hasReturnPoint&&ce::explorationStatus().available,"Fresh foreground view recovers original anchor");
    ce::explorationNativeView(reinterpret_cast<void*>(99),false);ce::testExplorationTick();
    require(ce::explorationStatus().hasReturnPoint,"Foreign viewport cannot invalidate primary anchor");
    ce::explorationNativeView(reinterpret_cast<void*>(41),false);ce::testExplorationTick();
    require(!ce::explorationStatus().hasReturnPoint&&!ce::explorationStatus().available,"Explicit invalid primary view invalidates old anchor");
    setup();require(ce::requestExplorationTeleport(generation()),"Expiry scenario queued");ce::testExplorationClock(7001);input(false,true);ce::testExplorationTick();
    require(ce::testExplorationNativeMoves()==0&&!ce::explorationStatus().pending,"Expired request cannot execute");
    setup();require(ce::requestExplorationTeleport(generation()),"Cancel scenario queued");ce::cancelExplorationRequest();releaseAndTick();
    require(ce::testExplorationNativeMoves()==0&&!ce::explorationStatus().pending,"Explicit cancel never moves");
    setup();view(41,{10,20,0},true,false);
    require(!ce::requestExplorationTeleport(generation())&&!ce::requestExplorationSaveReturn(generation()),"Unsafe stance cannot queue mutation or anchor");

    using Preview=ce::ExplorationPreviewState;
    livePreview();
    require(ce::explorationStatus().preview.state==Preview::Pending,"Live publication waits for native tick validation");
    require(ce::testExplorationLandingQueries()==0,"Present publication invokes no collision query");
    ce::testExplorationTick();auto preview=ce::explorationStatus().preview;
    require(preview.state==Preview::Valid&&preview.fromCamera&&preview.session==9,"Native tick validates the owned live camera");
    require(preview.hasFloor&&preview.landingCenter==ce::ExplorationPosition{100,200,0}
        &&preview.floorPoint==ce::ExplorationPosition{100,200,-92}&&preview.radius==35,"Preview reports exact landing body and floor point");
    require(preview.generation==generation()&&preview.revision,"Preview carries scene and candidate provenance");
    require(preview.controller==41&&preview.pawn==44,"Checked preview carries its exact owning controller and pawn");
    require(ce::testExplorationNativeMoves()==0&&!ce::explorationStatus().hasReturnPoint
        &&!ce::explorationStatus().hasCameraTarget,"Preview cannot relocate, capture an anchor or select a destination");
    require(!ce::requestExplorationTeleport(generation()),"Unprotected live preview does not grant relocation permission");
    require(ce::testExplorationLandingQueries()==1,"One query on the first outer tick");
    ce::testExplorationTick();require(ce::testExplorationLandingQueries()==1,"Repeated ticks respect the ten hertz query bound");
    auto movedCamera=camera(41,{105,200,30});ce::testExplorationClock(1050);input(false,false,false,1,false,true);
    ce::publishExplorationPreview(movedCamera,true);ce::testExplorationTick();
    require(ce::testExplorationLandingQueries()==1,"Changed live candidate still respects query frequency");
    auto retained=ce::explorationStatus().preview;
    require(retained.state==Preview::Valid&&retained.revision==preview.revision&&retained.target==preview.target
        &&retained.floorPoint==preview.floorPoint,"Brief live lag preserves one coherent checked target instead of relabelling old geometry");
    ce::testExplorationClock(1100);input(false,false,false,1,false,true);ce::publishExplorationPreview(movedCamera,true);ce::testExplorationTick();
    preview=ce::explorationStatus().preview;
    require(ce::testExplorationLandingQueries()==2&&preview.state==Preview::Valid&&preview.target.x==105
        &&preview.floorPoint.x==105&&preview.revision!=retained.revision,"Latest candidate resolves on the next bounded query");
    ce::testExplorationClock(1400);input(false,false,false,1,false,true);ce::publishExplorationPreview(movedCamera,true);
    require(ce::explorationStatus().preview.state==Preview::Pending&&!ce::explorationStatus().preview.hasFloor,"Checked live geometry expires after 250 milliseconds");
    ce::testExplorationTick();require(ce::explorationStatus().preview.state==Preview::Valid,"Expired preview can be checked again");
    movedCamera.session=10;ce::publishExplorationPreview(movedCamera,true);
    require(ce::explorationStatus().preview.state==Preview::Pending&&ce::explorationStatus().preview.session==10,"A new camera session never inherits a checked marker");
    require(ce::rememberExplorationCamera(movedCamera),"Deliberate handoff selects an exact destination");
    input(true,true,false);ce::publishExplorationPreview({},false);preview=ce::explorationStatus().preview;
    require(preview.state==Preview::Pending&&!preview.fromCamera&&preview.session==0,"Handoff requires an exact selected-target check");
    ce::testExplorationClock(1500);input(true,true,false);ce::publishExplorationPreview({},false);ce::testExplorationTick();
    preview=ce::explorationStatus().preview;
    require(preview.state==Preview::Valid&&!preview.fromCamera&&preview.target.x==105,"Selected target remains visible with the natural gameplay camera");
    const auto selectedRevision=preview.revision;input(false,true,false);ce::publishExplorationPreview({},false);
    require(ce::explorationStatus().preview.revision==selectedRevision,"Closing the menu retains the selected preview");
    require(!ce::explorationStatus().hasReturnPoint&&ce::testExplorationNativeMoves()==0,"Live and selected preview need neither practice nor world mutation");

    livePreview(false);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::Valid,"Destination geometry preview does not require Sam's current movement eligibility");
    require(!ce::explorationStatus().currentSafe,"Preview does not weaken the current stance safeguard");
    for(unsigned flags:{1u,2u,4u,64u}) {
        livePreview();ce::testExplorationFailure(flags);ce::testExplorationTick();preview=ce::explorationStatus().preview;
        require(preview.state==Preview::Blocked,"Unsafe destination receives a blocked preview");
        require(preview.hasFloor==(flags==4),"Only a confirmed floor is retained for a blocked capsule marker");
        require(ce::testExplorationNativeMoves()==0&&!ce::explorationStatus().hasReturnPoint,"Blocked preview has no relocation or anchor side effect");
    }
    livePreview();ce::testExplorationFailure(128);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::Valid,"A stance change during read-only preview does not invalidate target geometry");
    livePreview();ce::testExplorationFailure(256);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::None,"A collision-shape change during queries discards the old result");
    livePreview();ce::testExplorationFailure(512);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::None,"A controller change during queries prevents result publication");
    livePreview();ce::testExplorationFailure(1024);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::Valid,"Read-only preview is independent of protected-save availability");
    require(ce::testExplorationNativeMoves()==0,"Preview remains read-only when native practice is unavailable");

    setup();save();ce::testExplorationTick();const auto anchor=ce::explorationStatus().returnPoint;
    ce::testExplorationFocused(false);input();
    require(ce::explorationStatus().preview.state==Preview::None&&ce::explorationStatus().returnPoint==anchor,"Focus loss hides markers but preserves the same-scene anchor");
    ce::testExplorationFocused(true);ce::testExplorationClock(1100);input();view();ce::publishExplorationPreview({},false);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::Valid&&ce::explorationStatus().returnPoint==anchor,"Focus return revalidates the retained selected destination");
    view(51);require(ce::explorationStatus().preview.state==Preview::None&&!ce::explorationStatus().hasCameraTarget,"A new scene clears every preview coordinate");
    require(ce::explorationStatus().preview.controller==0&&ce::explorationStatus().preview.pawn==0,"A cleared preview cannot retain old view provenance");
    livePreview();ce::testExplorationTick();input(false,false,false,0,false,true);
    require(ce::explorationStatus().preview.state==Preview::None,"Disconnect immediately hides the preview");
    input(false,false,false,1,false,true);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::None,"Reconnect alone cannot revive a discarded live preview");
    livePreview();ce::testExplorationTick();ce::testExplorationClock(2101);
    require(ce::explorationStatus().preview.state==Preview::None,"Stale view/input cannot display a landing marker");
    livePreview();auto foreign=camera(99);ce::publishExplorationPreview(foreign,true);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::None,"Foreign camera identity cannot supply preview geometry");
    livePreview();auto invalid=camera();invalid.pose.x=std::numeric_limits<float>::infinity();ce::publishExplorationPreview(invalid,true);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::None,"Invalid camera coordinates never reach native queries");
    livePreview();auto inactive=camera();inactive.active=false;ce::publishExplorationPreview(inactive,true);
    require(ce::explorationStatus().preview.state==Preview::None,"Inactive camera cannot keep a live marker");
    livePreview();ce::publishExplorationPreview({},false);ce::testExplorationTick();
    require(ce::explorationStatus().preview.state==Preview::None,"Cancelling an unselected flight discards its preview");
    soloDeniableOpsExploration();
    floorRelativeReturns();
    std::cout<<"PASS: "<<checks<<" exploration backend checks; native-tick dispatch, grounded targets, anchor preservation, fresh safety, input ownership, read-only landing previews and reentrant refusal.\n";
}
