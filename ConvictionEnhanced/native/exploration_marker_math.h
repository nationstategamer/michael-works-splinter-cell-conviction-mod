#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <array>
#include <cmath>
#include <cstddef>
#include "free_camera_math.h"

namespace ce {
struct MarkerWorldPoint {float x=0,y=0,z=0;};
struct MarkerScreenPoint {float x=0,y=0;};
struct MarkerViewport {float x=0,y=0,width=0,height=0;};
// Scales map camera right/up divided by forward depth to normalized device
// coordinates. Offsets are explicit so a caller never has to guess a crop.
struct MarkerProjection {double scaleX=0,scaleY=0,offsetX=0,offsetY=0;};
struct MarkerBearing {float x=0,y=0;bool behind=false;};
inline constexpr std::size_t ExplorationMarkerRingSegments=24;
using ExplorationMarkerRing=std::array<MarkerWorldPoint,ExplorationMarkerRingSegments>;

inline bool validMarkerViewport(const MarkerViewport& viewport) noexcept {
    return std::isfinite(viewport.x)&&std::isfinite(viewport.y)
        &&std::isfinite(viewport.width)&&std::isfinite(viewport.height)
        &&std::fabs(viewport.x)<=65536&&std::fabs(viewport.y)<=65536
        &&viewport.width>=1&&viewport.width<=32768
        &&viewport.height>=1&&viewport.height<=32768;
}
inline bool validMarkerProjection(const MarkerProjection& projection) noexcept {
    return std::isfinite(projection.scaleX)&&std::isfinite(projection.scaleY)
        &&std::isfinite(projection.offsetX)&&std::isfinite(projection.offsetY)
        &&projection.scaleX>.001&&projection.scaleX<10000
        &&projection.scaleY>.001&&projection.scaleY<10000
        &&std::fabs(projection.offsetX)<=2&&std::fabs(projection.offsetY)<=2;
}
inline bool validMarkerPoint(const MarkerWorldPoint& point) noexcept {
    return std::isfinite(point.x)&&std::isfinite(point.y)&&std::isfinite(point.z)
        &&std::fabs(point.x)<=CameraPositionLimit
        &&std::fabs(point.y)<=CameraPositionLimit
        &&std::fabs(point.z)<=CameraPositionLimit;
}

// PlayerCalcView's controller FOV is horizontal. The retail scene constructor
// conditionally extends it above 16:9 (viewport+0x2B0), then builds its matrix
// with width*pixelAspect and unscaled height. These inputs must come from the
// current validated view; configured FOV alone is not a renderer observation.
inline bool makeMarkerProjection(double horizontalFovDegrees,const MarkerViewport& viewport,
    bool correctUltrawide,double pixelAspect,MarkerProjection* output) noexcept {
    if(output)*output={};
    if(!output||!validMarkerViewport(viewport)||!std::isfinite(horizontalFovDegrees)
        ||horizontalFovDegrees<=1||horizontalFovDegrees>=179
        ||!std::isfinite(pixelAspect)||pixelAspect<.1||pixelAspect>10)return false;
    constexpr double Pi=3.14159265358979323846;
    const double aspect=static_cast<double>(viewport.width)/viewport.height;
    double tangent=std::tan(horizontalFovDegrees*Pi/360.0);
    if(correctUltrawide&&aspect>16.0/9.0)tangent*=aspect*(9.0/16.0);
    const MarkerProjection candidate{1.0/tangent,aspect*pixelAspect/tangent,0,0};
    if(!validMarkerProjection(candidate))return false;
    *output=candidate;return true;
}
inline bool makeMarkerProjection(double horizontalFovDegrees,const MarkerViewport& viewport,
    MarkerProjection* output) noexcept {
    return makeMarkerProjection(horizontalFovDegrees,viewport,false,1.0,output);
}
inline bool makeMarkerProjection(double horizontalFovDegrees,const MarkerViewport& viewport,
    bool correctUltrawide,MarkerProjection* output) noexcept {
    return makeMarkerProjection(horizontalFovDegrees,viewport,correctUltrawide,1.0,output);
}

namespace marker_detail {
struct CameraPoint {double right=0,up=0,depth=0;};
inline bool cameraPoint(const CameraPose& camera,const MarkerWorldPoint& point,CameraPoint* output) noexcept {
    if(!output||!validCameraPose(camera)||!validMarkerPoint(point))return false;
    constexpr double RadiansPerUnit=6.28318530717958647692/65536.0;
    const double pitch=signedCameraAngle(camera.pitch)*RadiansPerUnit;
    const double yaw=signedCameraAngle(camera.yaw)*RadiansPerUnit;
    const double roll=signedCameraAngle(camera.roll)*RadiansPerUnit;
    const double cp=std::cos(pitch),sp=std::sin(pitch),cy=std::cos(yaw),sy=std::sin(yaw);
    const double cr=std::cos(roll),sr=std::sin(roll);
    const double dx=static_cast<double>(point.x)-camera.x;
    const double dy=static_cast<double>(point.y)-camera.y;
    const double dz=static_cast<double>(point.z)-camera.z;
    // Unreal FRotationMatrix axes, including roll. Positive pitch looks up;
    // positive yaw turns +X toward +Y. Screen Y grows downward.
    output->depth=dx*cp*cy+dy*cp*sy+dz*sp;
    output->right=dx*(sr*sp*cy-cr*sy)+dy*(sr*sp*sy+cr*cy)-dz*sr*cp;
    output->up=dx*(-cr*sp*cy-sr*sy)+dy*(-cr*sp*sy+sr*cy)+dz*cr*cp;
    return std::isfinite(output->depth)&&std::isfinite(output->right)&&std::isfinite(output->up);
}
}

// A failure clears output, preventing a caller from drawing stale coordinates.
// Offscreen and behind-camera points are deliberately not returned as pixels.
inline bool projectExplorationMarker(const CameraPose& camera,const MarkerProjection& projection,
    const MarkerViewport& viewport,const MarkerWorldPoint& world,MarkerScreenPoint* output) noexcept {
    if(output)*output={};
    marker_detail::CameraPoint point;
    if(!output||!validMarkerProjection(projection)||!validMarkerViewport(viewport)
        ||!marker_detail::cameraPoint(camera,world,&point)||point.depth<=.1f)return false;
    const double x=point.right*projection.scaleX/point.depth+projection.offsetX;
    const double y=point.up*projection.scaleY/point.depth+projection.offsetY;
    if(!std::isfinite(x)||!std::isfinite(y)||x< -1||x>1||y< -1||y>1)return false;
    output->x=static_cast<float>(viewport.x+(x+1)*viewport.width*.5);
    output->y=static_cast<float>(viewport.y+(1-y)*viewport.height*.5);
    return true;
}

// A direction cue is intentionally distinct from projected world pixels.
// It points toward the target even when below the camera or behind it; callers
// should label the cue as a direction, never draw it as the physical landing.
inline bool explorationMarkerBearing(const CameraPose& camera,const MarkerProjection& projection,
    const MarkerWorldPoint& world,MarkerBearing* output) noexcept {
    if(output)*output={};
    marker_detail::CameraPoint point;
    if(!output||!validMarkerProjection(projection)||!marker_detail::cameraPoint(camera,world,&point))return false;
    double x=point.right*projection.scaleX,y=-point.up*projection.scaleY;
    const double length=std::hypot(x,y);
    if(length<=.000001){
        if(point.depth>=-.1f)return false;
        *output={0,1,true};return true;
    }
    x/=length;y/=length;
    *output={static_cast<float>(x),static_cast<float>(y),point.depth<-.1f};return true;
}

inline bool makeExplorationMarkerRing(const MarkerWorldPoint& center,float radius,
    ExplorationMarkerRing* output) noexcept {
    if(output)*output={};
    if(!output||!validMarkerPoint(center)||!std::isfinite(radius)||radius<=0||radius>1024)return false;
    ExplorationMarkerRing candidate{};
    constexpr double Turn=6.28318530717958647692;
    for(std::size_t index=0;index<candidate.size();++index){
        const double angle=Turn*static_cast<double>(index)/candidate.size();
        candidate[index]={static_cast<float>(center.x+radius*std::cos(angle)),
            static_cast<float>(center.y+radius*std::sin(angle)),center.z};
        if(!validMarkerPoint(candidate[index]))return false;
    }
    *output=candidate;return true;
}
} // namespace ce
