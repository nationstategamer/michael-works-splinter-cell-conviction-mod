// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include "exploration_marker_math.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned checks=0;
void check(bool okay,const char* message){++checks;if(!okay){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
bool close(double a,double b,double tolerance=.001){return std::fabs(a-b)<=tolerance;}
}
int main(){
    using namespace ce;
    const MarkerViewport viewport{0,0,1920,1080};MarkerProjection projection;
    check(makeMarkerProjection(90,viewport,&projection),"horizontal FOV projection");
    check(close(projection.scaleX,1)&&close(projection.scaleY,16.0/9.0),"horizontal and vertical scales");
    MarkerScreenPoint pixel;CameraPose camera{};
    check(projectExplorationMarker(camera,projection,viewport,{100,0,0},&pixel),"front center projects");
    check(close(pixel.x,960)&&close(pixel.y,540),"front center pixel");
    check(projectExplorationMarker(camera,projection,viewport,{100,50,0},&pixel),"world +Y is screen right");
    check(close(pixel.x,1440)&&close(pixel.y,540),"right position");
    check(projectExplorationMarker(camera,projection,viewport,{100,-50,0},&pixel),"world -Y is screen left");
    check(close(pixel.x,480),"left position");
    check(projectExplorationMarker(camera,projection,viewport,{100,0,25},&pixel),"world +Z is screen up");
    check(close(pixel.y,300),"up position");
    check(projectExplorationMarker(camera,projection,viewport,{100,0,-25},&pixel),"world -Z is screen down");
    check(close(pixel.y,780),"down position");
    check(!projectExplorationMarker(camera,projection,viewport,{-100,0,0},&pixel),"behind rejected");
    check(close(pixel.x,0)&&close(pixel.y,0),"failure clears stale output");
    check(!projectExplorationMarker(camera,projection,viewport,{0,0,0},&pixel),"zero depth rejected");
    check(!projectExplorationMarker(camera,projection,viewport,{.1f,0,0},&pixel),"near plane rejected");
    check(!projectExplorationMarker(camera,projection,viewport,{100,101,0},&pixel),"offscreen right rejected");
    check(!projectExplorationMarker(camera,projection,viewport,{100,0,100},&pixel),"offscreen up rejected");
    check(!projectExplorationMarker(camera,projection,viewport,{100,0,0},nullptr),"null pixel rejected");
    camera.yaw=16384;
    check(projectExplorationMarker(camera,projection,viewport,{0,100,0},&pixel),"quarter-turn yaw");
    check(close(pixel.x,960)&&close(pixel.y,540),"yaw center");
    check(projectExplorationMarker(camera,projection,viewport,{-25,100,0},&pixel),"yaw right axis");
    check(close(pixel.x,1200),"yaw right pixel");
    camera={};camera.pitch=8192;
    check(projectExplorationMarker(camera,projection,viewport,{100,0,100},&pixel),"positive pitch raises forward");
    check(close(pixel.x,960)&&close(pixel.y,540),"pitch center");
    camera={};camera.roll=16384;
    check(projectExplorationMarker(camera,projection,viewport,{100,25,0},&pixel),"roll maps right world axis upward");
    check(close(pixel.x,960)&&close(pixel.y,300),"roll pixel");
    camera={100,200,300,0,0,0};
    check(projectExplorationMarker(camera,projection,viewport,{200,200,300},&pixel),"translated camera");
    check(close(pixel.x,960)&&close(pixel.y,540),"translation center");
    camera={};camera.yaw=65536;
    check(projectExplorationMarker(camera,projection,viewport,{100,0,0},&pixel),"rotator wrap");
    check(close(pixel.x,960),"wrapped center");
    const MarkerViewport offsetViewport{100,50,800,600};MarkerProjection fourThree;
    check(makeMarkerProjection(90,offsetViewport,&fourThree),"4:3 viewport projection");
    check(projectExplorationMarker({},fourThree,offsetViewport,{100,0,0},&pixel),"offset viewport projection");
    check(close(pixel.x,500)&&close(pixel.y,350),"offset viewport center");
    MarkerProjection ultra;
    check(makeMarkerProjection(90,{0,0,2560,1080},true,&ultra),"retail ultrawide correction");
    check(close(ultra.scaleX,.75)&&close(ultra.scaleY,16.0/9.0),"ultrawide keeps vertical FOV");
    check(makeMarkerProjection(90,viewport,true,&ultra),"16:9 correction flag");
    check(close(ultra.scaleX,1)&&close(ultra.scaleY,16.0/9.0),"16:9 unchanged");
    check(makeMarkerProjection(90,offsetViewport,true,&ultra),"4:3 correction flag");
    check(close(ultra.scaleX,1)&&close(ultra.scaleY,4.0/3.0),"narrow aspect unchanged");
    check(makeMarkerProjection(90,viewport,false,1.2,&ultra),"pixel aspect supported");
    check(close(ultra.scaleX,1)&&close(ultra.scaleY,(16.0/9.0)*1.2),"pixel aspect affects vertical scale");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    const float infinity=std::numeric_limits<float>::infinity();
    for(double fov:{0.0,1.0,179.0,180.0,-1.0,static_cast<double>(nan),static_cast<double>(infinity)})
        check(!makeMarkerProjection(fov,viewport,&ultra),"invalid FOV rejected");
    for(double aspect:{0.0,-1.0,.09,10.1,static_cast<double>(nan),static_cast<double>(infinity)})
        check(!makeMarkerProjection(90,viewport,false,aspect,&ultra),"invalid pixel aspect rejected");
    check(!makeMarkerProjection(90,viewport,nullptr),"null projection rejected");
    for(MarkerViewport invalid:std::array<MarkerViewport,6>{{{0,0,0,1080},{0,0,1920,0},{0,0,-1,1080},{0,0,32769,1080},{nan,0,1920,1080},{0,0,1920,infinity}}})
        check(!makeMarkerProjection(90,invalid,&ultra),"invalid viewport rejected");
    for(MarkerWorldPoint invalid:std::array<MarkerWorldPoint,4>{{{nan,0,0},{0,infinity,0},{0,0,-infinity},{CameraPositionLimit+1,0,0}}})
        check(!projectExplorationMarker({},projection,viewport,invalid,&pixel),"invalid world rejected");
    for(MarkerProjection invalid:std::array<MarkerProjection,5>{{{0,1,0,0},{1,-1,0,0},{nan,1,0,0},{1,1,infinity,0},{1,1,0,3}}})
        check(!projectExplorationMarker({},invalid,viewport,{100,0,0},&pixel),"invalid matrix rejected");
    camera={};camera.x=nan;
    check(!projectExplorationMarker(camera,projection,viewport,{100,0,0},&pixel),"invalid camera rejected");
    const MarkerProjection offsetProjection{1,16.0/9.0,.25,-.25};
    check(projectExplorationMarker({},offsetProjection,viewport,{100,0,0},&pixel),"explicit projection offset");
    check(close(pixel.x,1200)&&close(pixel.y,675),"projection offset pixel");
    MarkerBearing bearing;
    check(explorationMarkerBearing({},projection,{0,0,-100},&bearing),"directly below direction");
    check(close(bearing.x,0)&&close(bearing.y,1)&&!bearing.behind,"below points down");
    check(explorationMarkerBearing({},projection,{100,200,0},&bearing),"offscreen right direction");
    check(close(bearing.x,1)&&close(bearing.y,0)&&!bearing.behind,"right direction");
    check(explorationMarkerBearing({},projection,{-100,200,0},&bearing),"behind right direction");
    check(close(bearing.x,1)&&close(bearing.y,0)&&bearing.behind,"behind does not falsely mirror");
    check(explorationMarkerBearing({},projection,{-100,0,0},&bearing),"directly behind fallback");
    check(close(bearing.x,0)&&close(bearing.y,1)&&bearing.behind,"behind fallback down");
    check(!explorationMarkerBearing({},projection,{100,0,0},&bearing),"center needs no direction cue");
    check(!explorationMarkerBearing({},projection,{0,0,0},&bearing),"coincident has no direction cue");
    check(!explorationMarkerBearing({},projection,{nan,0,0},&bearing),"invalid bearing rejected");
    ExplorationMarkerRing ring;
    check(makeExplorationMarkerRing({100,200,300},25,&ring),"floor ring generation");
    check(ring.size()==24,"modest ring segment count");
    for(const auto& point:ring){
        check(close(std::hypot(point.x-100,point.y-200),25),"ring radius");
        check(close(point.z,300),"ring lies on floor plane");
    }
    for(float radius:{0.0f,-1.0f,1025.0f,nan,infinity})
        check(!makeExplorationMarkerRing({0,0,0},radius,&ring),"invalid ring radius rejected");
    check(!makeExplorationMarkerRing({CameraPositionLimit,0,0},25,&ring),"ring cannot exceed coordinate bound");
    check(!makeExplorationMarkerRing({nan,0,0},25,&ring),"invalid ring center rejected");
    check(!makeExplorationMarkerRing({0,0,0},25,nullptr),"null ring rejected");
    std::printf("%u exploration marker math checks passed\n",checks);
}
