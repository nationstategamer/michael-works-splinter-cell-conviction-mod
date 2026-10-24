#pragma once
#include <cstdint>
#include "free_camera_math.h"
#include "gameplay_context.h"

namespace ce {
struct FreeCameraStatus {
    bool installed=false, available=false, active=false;
    std::uint64_t session=0, observations=0, overrides=0;
    std::uint32_t controller=0, actor=0, camera=0;
    GameplayKind gameplay=GameplayKind::Unavailable;
    CameraPose pose{};
    // Last actual primary renderer output, separate from the next requested
    // detached pose. Projection data is read-only and only used by the marker.
    CameraPose renderPose{};
    float horizontalFov=0;
    float pixelAspect=1;
    bool correctUltrawideFov=false;
    bool renderViewValid=false;
    const char* reason="Waiting for the camera hook";
};
bool initializeFreeCamera() noexcept;
FreeCameraStatus freeCameraStatus() noexcept;
bool beginFreeCamera(std::uint64_t session) noexcept;
bool moveFreeCamera(std::uint64_t session,const CameraMotion&,double dt,float speed) noexcept;
// Only renderer output buffers are overridden. Stopping under the same lock
// prevents future overrides; no game object or saved pose needs restoration.
void stopFreeCamera(std::uint64_t session=0) noexcept;
#ifdef CE_TEST_HOST
void testCameraView(std::uint32_t identity,CameraPose* pose,bool valid=true,std::uint32_t viewActor=0) noexcept;
void testCameraContextView(std::uint32_t identity,CameraPose* pose,const char* map,const char* connection,bool splitScreen=false,bool primary=true) noexcept;
void testCameraClock(std::uint64_t now) noexcept;
void testCameraFov(float horizontalFov) noexcept;
#endif
}
