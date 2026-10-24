#pragma once
#include <cstdint>

namespace ce {
enum class HudLayer : std::uint32_t {
    Crosshair=1,TargetMarkers=2,Inventory=4,Prompts=8,
    Objectives=16,Status=32,Subtitles=64
};
struct HudStatus {
    bool installed=false,allHidden=false;
    std::uint32_t supportedMask=0,hiddenMask=0,observedMask=0;
    std::uint64_t observations=0,suppressed=0;
    const char* reason="Waiting for gameplay HUD support";
};
bool initializeHudControl() noexcept;
HudStatus hudStatus() noexcept;
void setHudLayerHidden(HudLayer layer,bool hidden) noexcept;
void setAllHudHidden(bool hidden) noexcept;
void restoreHud() noexcept;
#ifdef CE_TEST_HOST
// Fake movie objects use the same bounded callback/clip layout as production.
using HudTestGet=int(*)(void*,const char*);
using HudTestSet=void(*)(void*,const char*,int);
using HudTestDraw=void(*)();
void testHudDraw(HudTestDraw draw,bool gameplay,HudTestGet get,HudTestSet set);
int testHudQuery(void* movie,std::uintptr_t callerRva,int naturalResult);
#endif
}
