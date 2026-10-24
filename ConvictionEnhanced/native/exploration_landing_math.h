#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <cmath>
#include "exploration_policy.h"

namespace ce {
inline constexpr float ExplorationSupportTolerance=8;
inline constexpr float ExplorationLandingClearance=2;
struct ExplorationReturnTrace {ExplorationPosition start{};float distance=0;};
inline bool validExplorationFloor(float value) noexcept {
    return std::isfinite(value)&&std::fabs(value)<=ExplorationPositionLimit;
}
inline bool validExplorationHalfHeight(float value) noexcept {
    return std::isfinite(value)&&value>0&&value<=256;
}
// The saved reference is the actual verified supporting floor, never an old
// body center. Changing stance must not change which floor a return selects.
inline bool explorationReturnTrace(const ExplorationPosition& anchor,float savedFloorZ,
    float currentHalfHeight,ExplorationReturnTrace* output) noexcept {
    if(output)*output={};
    if(!output||!validExplorationPosition(anchor)||!validExplorationFloor(savedFloorZ)
        ||!validExplorationHalfHeight(currentHalfHeight))return false;
    ExplorationReturnTrace value{{anchor.x,anchor.y,savedFloorZ+currentHalfHeight+ExplorationLandingClearance},
        currentHalfHeight+16};
    auto end=value.start;end.z-=value.distance;
    if(!validExplorationPosition(value.start)||!validExplorationPosition(end))return false;
    *output=value;return true;
}
// Both production placement and its native-host fixture use this exact math.
// Native callers still independently verify walkability and current-body space.
inline bool explorationLandingCenter(const ExplorationPosition& target,float floorZ,
    float currentHalfHeight,bool returning,float savedFloorZ,ExplorationPosition* output) noexcept {
    if(output)*output={};
    if(!output||!validExplorationPosition(target)||!validExplorationFloor(floorZ)
        ||!validExplorationHalfHeight(currentHalfHeight))return false;
    if(returning&&(!validExplorationFloor(savedFloorZ)
        ||std::fabs(floorZ-savedFloorZ)>ExplorationSupportTolerance))return false;
    const ExplorationPosition value{target.x,target.y,floorZ+currentHalfHeight+ExplorationLandingClearance};
    if(!validExplorationPosition(value))return false;
    *output=value;return true;
}
}
