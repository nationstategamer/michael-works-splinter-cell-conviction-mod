#include "free_camera.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
std::uint64_t clockValue = 1000;
void require(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
bool same(const ce::CameraPose& a, const ce::CameraPose& b) {
    return a.x==b.x && a.y==b.y && a.z==b.z
        && a.pitch==b.pitch && a.yaw==b.yaw && a.roll==b.roll;
}
bool near(float a, float b) { return std::fabs(a-b)<.0005f; }
void advance(std::uint64_t milliseconds) {
    clockValue += milliseconds;
    ce::testCameraClock(clockValue);
}
void observe(std::uint32_t identity, ce::CameraPose& output, bool valid=true) {
    advance(16);
    ce::testCameraView(identity,&output,valid);
}

void installationAndNaturalOutput() {
    require(!ce::freeCameraStatus().installed, "Camera started installed without initialization");
    require(!ce::beginFreeCamera(1), "Camera activated without installation or observation");
    ce::CameraPose natural{10,20,30,0,0,123};
    const auto original=natural;
    observe(100,natural);
    require(same(natural,original), "Inactive observer changed the renderer output");
    require(!ce::beginFreeCamera(1), "An observed pose bypassed installation gating");
    require(ce::initializeFreeCamera(), "Test camera initialization failed");
    auto state=ce::freeCameraStatus();
    require(state.installed && state.available && !state.active, "Installed camera observation was not available");
    require(state.controller==100 && state.camera==101 && state.actor==103, "Camera identity diagnostic mismatch");
    require(same(state.pose,original), "Observation did not preserve the complete natural pose");
    require(!ce::beginFreeCamera(0), "Zero session token activated the camera");
    ce::testCameraView(100,&natural,true,101); // AECamera is not this build's ViewActor.
    require(!ce::freeCameraStatus().available, "Non-owning render target enabled the campaign camera");
    require(!ce::beginFreeCamera(2), "Non-owning render target activated an override");
    require(same(natural,original), "Rejected render target changed output buffers");
    observe(100,natural);
}

void activationMovementAndOutputIsolation() {
    const auto observationsBefore=ce::freeCameraStatus().observations;
    const auto overridesBefore=ce::freeCameraStatus().overrides;
    require(ce::beginFreeCamera(11), "Fresh camera could not activate");
    const auto captured=ce::freeCameraStatus().pose;
    require(captured.x==10 && captured.y==20 && captured.z==30 && captured.roll==123,
        "Activation failed to capture the latest natural view");
    require(!ce::beginFreeCamera(12), "A second activation replaced an owned session");
    ce::CameraMotion motion{}; motion.forward=1;
    require(!ce::moveFreeCamera(12,motion,.05,100), "Wrong session moved the camera");
    require(ce::freeCameraStatus().active && same(ce::freeCameraStatus().pose,captured),
        "Wrong movement token stopped or changed the current session");
    require(ce::moveFreeCamera(11,motion,.05,100), "Valid camera motion was rejected");
    auto moved=ce::freeCameraStatus().pose;
    require(same(ce::freeCameraStatus().renderPose,captured),
        "Unrendered movement advanced the marker's renderer pose");
    require(near(moved.x,15) && moved.y==20 && moved.z==30 && moved.roll==0,
        "Camera motion did not update the detached pose");
    require(ce::freeCameraStatus().overrides==overridesBefore,
        "Movement counted as a renderer override before a view was rendered");

    // This stand-in for the engine's own camera remains separate. Only the
    // caller's output copy goes through the renderer hook test seam.
    const ce::CameraPose enginePose{500,600,700,800,900,1000};
    ce::CameraPose rendererOutput=enginePose;
    observe(100,rendererOutput);
    require(same(rendererOutput,moved), "Active view did not override the rendered output");
    require(enginePose.x==500 && enginePose.roll==1000, "Detached movement modified the source engine pose");
    auto state=ce::freeCameraStatus();
    require(state.renderViewValid&&same(state.renderPose,rendererOutput),
        "Marker projection did not publish the actual overridden renderer output");
    require(state.overrides==overridesBefore+1 && state.observations==observationsBefore+1,
        "Renderer observation/override diagnostics did not count the view correctly");
    ce::stopFreeCamera(12);
    require(ce::freeCameraStatus().active && ce::freeCameraStatus().session==11,
        "Stale stop token stopped the current session");
    ce::stopFreeCamera(11);
    require(!ce::freeCameraStatus().active && ce::freeCameraStatus().session==0,
        "Matching stop did not release the backend session");
    rendererOutput=enginePose;
    observe(100,rendererOutput);
    require(same(rendererOutput,enginePose), "Stopping did not immediately restore ordinary rendered output");
    require(ce::beginFreeCamera(12), "Fresh activation after stop failed");
    require(same(ce::freeCameraStatus().pose,enginePose), "Reactivation used the old detached pose");
    ce::stopFreeCamera();
}

void identityAndInvalidOutputStop() {
    ce::CameraPose initial{1,2,3,0,0,0};
    observe(200,initial);
    require(ce::beginFreeCamera(21), "Identity test activation failed");
    ce::CameraPose nextLevel{40,50,60,70,80,90};
    const auto nextNatural=nextLevel;
    const auto overrides=ce::freeCameraStatus().overrides;
    observe(300,nextLevel);
    require(!ce::freeCameraStatus().active, "A changed camera identity retained the previous free camera");
    require(same(nextLevel,nextNatural), "A changed camera received stale free-camera output");
    require(ce::freeCameraStatus().overrides==overrides, "Identity change counted an override");
    require(ce::beginFreeCamera(22), "A new valid camera could not activate deliberately");
    ce::stopFreeCamera(21);
    require(ce::freeCameraStatus().active, "Old identity stop token stopped a newer camera session");
    ce::CameraPose untrusted{70,80,90,100,110,120};
    const auto untrustedOriginal=untrusted;
    observe(300,untrusted,false);
    auto state=ce::freeCameraStatus();
    require(!state.active && !state.available, "Invalid view failed to stop and disarm camera activation");
    require(same(untrusted,untrustedOriginal), "Invalid view was overridden");
    require(!ce::beginFreeCamera(23), "Invalid observation allowed reactivation");

    ce::CameraPose valid{2,3,4,0,0,0};
    observe(300,valid);
    require(ce::beginFreeCamera(24), "Valid observation did not recover availability");
    ce::CameraPose badPose{std::numeric_limits<float>::quiet_NaN(),3,4,0,0,0};
    observe(300,badPose);
    require(!ce::freeCameraStatus().active && !ce::freeCameraStatus().available,
        "NaN renderer position did not stop free camera");
    require(std::isnan(badPose.x), "Invalid renderer position was silently replaced");
    ce::stopFreeCamera();
}

void freshnessAndInvalidMovement() {
    ce::CameraPose natural{10,10,10,0,0,0};
    observe(400,natural);
    advance(1001);
    require(!ce::freeCameraStatus().available, "Expired observation remained available");
    require(!ce::beginFreeCamera(31), "Expired observation activated the camera");
    observe(400,natural);
    require(ce::beginFreeCamera(32), "Fresh observation did not permit activation");
    advance(1001);
    ce::CameraMotion motion{}; motion.forward=1;
    require(!ce::moveFreeCamera(32,motion,.016,300), "Movement used an expired renderer observation");
    require(!ce::freeCameraStatus().active, "Expired camera movement left the session active");
    ce::CameraPose afterStall{30,40,50,0,0,0};
    const auto afterStallNatural=afterStall;
    observe(400,afterStall);
    require(same(afterStall,afterStallNatural) && !ce::freeCameraStatus().active,
        "Returning renderer resumed an expired camera session");
    require(ce::beginFreeCamera(33), "Explicit restart after a stall failed");
    const auto before=ce::freeCameraStatus().pose;
    motion.yaw=std::numeric_limits<float>::infinity();
    require(!ce::moveFreeCamera(33,motion,.016,300), "Nonfinite input was accepted");
    require(!ce::freeCameraStatus().active && same(ce::freeCameraStatus().pose,before),
        "Rejected movement partially changed the pose or retained control");
    afterStall=afterStallNatural;
    observe(400,afterStall);
    require(same(afterStall,afterStallNatural), "Invalid movement left a renderer override behind");
    ce::stopFreeCamera();

    require(ce::beginFreeCamera(34), "Status-expiry test activation failed");
    advance(1001);
    const auto expired=ce::freeCameraStatus();
    require(!expired.active && !expired.available, "Status polling failed to stop an expired active session");
    afterStall=afterStallNatural;
    observe(400,afterStall);
    require(same(afterStall,afterStallNatural) && !ce::freeCameraStatus().active,
        "A session expired by status polling resumed when the view returned");

    require(ce::beginFreeCamera(35), "Unobserved-expiry test activation failed");
    advance(1001);
    // No status/movement call occurs during this stall. The arriving camera
    // frame itself must expire ownership before publishing its fresh timestamp.
    afterStall={80,90,100,0,0,0};
    const auto resumedNatural=afterStall;
    observe(400,afterStall);
    require(same(afterStall,resumedNatural) && !ce::freeCameraStatus().active,
        "An unobserved render stall resumed the previous camera override");
    ce::stopFreeCamera();
}

void soloDeniableOpsAndModeTransitions() {
    ce::CameraPose natural{90,100,110,0,0,0};
    advance(16);
    ce::testCameraContextView(500,&natural,"C01_MafiaClub","SOLO");
    require(ce::freeCameraStatus().available&&ce::freeCameraStatus().gameplay==ce::GameplayKind::SoloDeniableOps,
        "Verified solo Deniable Ops camera unavailable");
    require(ce::beginFreeCamera(41),"Solo Deniable Ops camera could not activate");
    ce::CameraMotion motion{};motion.strafe=1;
    require(ce::moveFreeCamera(41,motion,.05,100),"Solo Deniable Ops camera could not move");
    const auto moved=ce::freeCameraStatus().pose;
    ce::CameraPose output=natural;
    ce::testCameraContextView(500,&output,"C01_MafiaClub","SOLO");
    require(same(output,moved),"Solo Deniable Ops renderer output was not overridden");

    // The game's second viewport must not disarm the primary camera or publish
    // an unrelated pose, regardless of what context its preview would report.
    output=natural;
    ce::testCameraContextView(600,&output,"menu.unr","LINK",false,false);
    require(ce::freeCameraStatus().active&&same(output,natural),"Secondary viewport disturbed the primary session");

    const char* rejectedConnections[]={"LINK","XBOX","SPLIT","Party","Invited","UNKNOWN",""};
    for(const auto connection:rejectedConnections) {
        output=natural;
        const auto overrides=ce::freeCameraStatus().overrides;
        ce::testCameraContextView(500,&output,"C01_MafiaClub",connection);
        const auto state=ce::freeCameraStatus();
        require(!state.active&&!state.available&&state.gameplay==ce::GameplayKind::Unavailable,
            "A changed connection mode did not immediately stop and disarm free camera");
        require(same(output,natural)&&state.overrides==overrides,"Unsupported session received a renderer override");
        require(!ce::beginFreeCamera(42),"Rejected session reactivated within freshness window");
        ce::testCameraContextView(500,&output,"C01_MafiaClub","SOLO");
        require(!ce::freeCameraStatus().active,"Returning solo session automatically resumed a stale override");
        require(ce::beginFreeCamera(43),"Explicit solo activation after a rejected connection failed");
    }
    output=natural;
    ce::testCameraContextView(500,&output,"C01_MafiaClub","SOLO",true);
    require(!ce::freeCameraStatus().active&&!ce::freeCameraStatus().available&&same(output,natural),
        "Split-screen solo lobby retained free camera");
    ce::testCameraContextView(500,&output,"C01_MafiaClub","SOLO");
    require(ce::beginFreeCamera(44),"Fresh solo activation for unknown map test failed");
    ce::testCameraContextView(500,&output,"C05_Antonov","SOLO");
    require(!ce::freeCameraStatus().active&&!ce::freeCameraStatus().available,
        "Unverified co-op story map retained free camera");

    ce::testCameraContextView(500,&output,"C01_MafiaClub","SOLO");
    require(ce::beginFreeCamera(45),"Solo activation before campaign transition failed");
    ce::testCameraContextView(500,&output,"_Single\\08_ThirdEchelon_A.unr","SOLO");
    require(!ce::freeCameraStatus().active&&ce::freeCameraStatus().available
        &&ce::freeCameraStatus().gameplay==ce::GameplayKind::Campaign,
        "Gameplay mode transition at the same object identity retained an override");
    require(same(output,natural),"Gameplay mode transition received stale output");
    ce::stopFreeCamera();
}
void markerProjectionObservation() {
    ce::CameraPose natural{100,200,300,-8192,16384,0};
    ce::testCameraFov(95);observe(600,natural);
    auto state=ce::freeCameraStatus();
    require(state.renderViewValid&&state.horizontalFov==95&&same(state.renderPose,natural),
        "Fresh renderer projection observation missing");
    ce::testCameraFov(0);observe(600,natural);state=ce::freeCameraStatus();
    require(!state.renderViewValid&&state.available,"Invalid FOV either drew stale projection or disabled camera gameplay");
    ce::testCameraFov(std::numeric_limits<float>::quiet_NaN());observe(600,natural);
    require(!ce::freeCameraStatus().renderViewValid,"NaN FOV enabled the landing marker");
    ce::testCameraFov(90);observe(600,natural);
    require(ce::freeCameraStatus().renderViewValid,"Marker projection did not recover after valid FOV");
    advance(1001);
    require(!ce::freeCameraStatus().renderViewValid,"Expired view retained a drawable marker projection");
}
}

int main() {
    try {
        ce::testCameraClock(clockValue);
        installationAndNaturalOutput(); activationMovementAndOutputIsolation();
        identityAndInvalidOutputStop(); freshnessAndInvalidMovement();
        soloDeniableOpsAndModeTransitions();
        markerProjectionObservation();
        std::cout<<"Free camera backend: "<<checks<<" checks passed.\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<"\n";
        return 1;
    }
}
