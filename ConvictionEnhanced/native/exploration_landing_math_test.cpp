#include "exploration_landing_math.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value){std::cerr<<"FAIL: "<<message<<"\n";std::exit(1);}}
}
int main(){
    ce::ExplorationReturnTrace trace{};ce::ExplorationPosition result{};
    const ce::ExplorationPosition standing{10,20,0},crouched{10,20,-42};
    require(ce::explorationReturnTrace(standing,-92,48,&trace),"Standing anchor permits crouched return trace");
    require(trace.start==crouched&&trace.distance==64,"Crouched trace starts above the original supporting floor");
    require(trace.start.z-trace.distance<=-92&&trace.start.z>-92,"Crouched trace reaches its saved floor");
    require(ce::explorationLandingCenter(standing,-92,48,true,-92,&result)&&result==crouched,
        "Standing-to-crouched return keeps XY and uses current capsule height");
    require(ce::explorationReturnTrace(crouched,-92,90,&trace)&&trace.start==standing&&trace.distance==106,
        "Crouched anchor permits standing return trace");
    require(ce::explorationLandingCenter(crouched,-92,90,true,-92,&result)&&result==standing,
        "Crouched-to-standing return does not compare new center with old center");
    // The previously used current-height ray from the old center misses this
    // floor; the supporting-floor reference is the essential regression.
    require(standing.z+2-(48+16)>-92,"Old center-based crouched ray misses the real saved floor");
    require(std::fabs(standing.z-crouched.z)>ce::ExplorationSupportTolerance,
        "Legitimate stance change exceeds old body-center tolerance");
    for(float floor:{-100.f,-99.f,-92.f,-85.f,-84.f}){
        require(ce::explorationLandingCenter(standing,floor,48,true,-92,&result),"Floor movement within eight units accepted");
        require(result.x==10&&result.y==20&&result.z==floor+50,"Accepted result derives current center from fresh floor");
    }
    for(float floor:{-101.f,-83.f}){
        result={1,2,3};require(!ce::explorationLandingCenter(standing,floor,90,true,-92,&result),"Different floor beyond tolerance refused");
        require(result==ce::ExplorationPosition{},"Refused floor cannot expose a stale destination");
    }
    require(ce::explorationLandingCenter({100,200,30},-300,48,false,0,&result)
        &&result==ce::ExplorationPosition{100,200,-250},"Outbound landing uses current capsule and independent fresh floor");
    require(ce::explorationLandingCenter({10,20,123},-92,48,true,-92,&result)&&result==crouched,
        "Saved floor is not inferred from the old body center");
    const float nan=std::numeric_limits<float>::quiet_NaN(),infinity=std::numeric_limits<float>::infinity();
    for(float height:{0.f,-1.f,257.f,nan,infinity}){
        trace={{1,2,3},4};require(!ce::explorationReturnTrace(standing,-92,height,&trace),"Invalid current height cannot form a native trace");
        require(trace.start==ce::ExplorationPosition{}&&trace.distance==0,"Invalid height clears trace output");
        result={1,2,3};require(!ce::explorationLandingCenter(standing,-92,height,true,-92,&result),"Invalid current height cannot produce a landing");
        require(result==ce::ExplorationPosition{},"Invalid height clears landing output");
    }
    for(float floor:{nan,infinity,ce::ExplorationPositionLimit+1}){
        require(!ce::explorationReturnTrace(standing,floor,90,&trace),"Invalid saved floor refused");
        require(!ce::explorationLandingCenter(standing,floor,90,false,0,&result),"Invalid fresh floor refused");
        require(!ce::explorationLandingCenter(standing,-92,90,true,floor,&result),"Invalid reference floor refused");
    }
    require(!ce::explorationReturnTrace({nan,0,0},-92,90,&trace),"Invalid anchor coordinates refused");
    require(!ce::explorationLandingCenter({0,infinity,0},-92,90,true,-92,&result),"Invalid landing XY refused");
    require(!ce::explorationReturnTrace(standing,ce::ExplorationPositionLimit,90,&trace),"Return trace cannot exceed upper world bound");
    require(!ce::explorationReturnTrace(standing,-ce::ExplorationPositionLimit,90,&trace),"Return trace cannot exceed lower world bound");
    require(!ce::explorationLandingCenter(standing,ce::ExplorationPositionLimit,90,false,0,&result),"Landing center cannot exceed world bound");
    require(!ce::explorationReturnTrace(standing,-92,90,nullptr),"Missing trace output refused");
    require(!ce::explorationLandingCenter(standing,-92,90,true,-92,nullptr),"Missing landing output refused");
    std::cout<<"PASS: "<<checks<<" floor-relative landing checks; stance changes, saved-floor tolerance, finite bounds and output clearing.\n";
}
