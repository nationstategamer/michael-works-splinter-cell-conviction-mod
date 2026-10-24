#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <Xinput.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include "free_camera_math.h"
#include "exploration_control.h"
namespace fs=std::filesystem;
int keyDowns=0,keyUps=0,mouseMoves=0;
LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)++keyDowns;if(msg==WM_KEYUP||msg==WM_SYSKEYUP)++keyUps;if(msg==WM_MOUSEMOVE)++mouseMoves;
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class T>T api(HMODULE module,const char* name){auto p=(T)GetProcAddress(module,name);require(p!=nullptr,name);return p;}
void screenshot(IDirect3DDevice9* device,const fs::path& file) {
    IDirect3DSurface9* back{};IDirect3DSurface9* system{};
    require(SUCCEEDED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back)),"GetBackBuffer");
    D3DSURFACE_DESC desc{};back->GetDesc(&desc);
    require(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&system,nullptr)),"screenshot surface");
    require(SUCCEEDED(device->GetRenderTargetData(back,system)),"GetRenderTargetData");
    D3DLOCKED_RECT locked{};require(SUCCEEDED(system->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"LockRect");
    BITMAPFILEHEADER header{};header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+desc.Width*desc.Height*4;
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=desc.Width;info.biHeight=-(LONG)desc.Height;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream out(file,std::ios::binary);out.write((char*)&header,sizeof(header));out.write((char*)&info,sizeof(info));
    for(UINT y=0;y<desc.Height;++y)out.write((char*)locked.pBits+y*locked.Pitch,desc.Width*4);
    system->UnlockRect();system->Release();back->Release();
}
int wmain(int argc,wchar_t** argv) {
 try {
    require(argc==3||(argc==4&&std::wstring(argv[3])==L"--verify-preferences"),"Usage: harness.exe test-dll fixture-game-root [--verify-preferences]");
    const bool verifyPreferences=argc==4;
    fs::path root=fs::absolute(argv[2]).lexically_normal();
    require(!fs::exists(root/L"src/system/conviction_game.exe"),"Harness root must be an isolated fixture, never a game installation");
    fs::create_directories(root/L"ConvictionEnhanced-state");
    const auto fixtureState=root/L"ConvictionEnhanced-state";
    const auto fixtureStatus=fixtureState/L"overlay-status.ini";
    const auto fixtureQueue=fixtureState/L"overlay-pending.ini";
    // Settings queuing binds to the exact active-state bytes. Other tests
    // validate the manager's full schema; this native host needs no real game.
    {
        const char active[]="{\"schema\":1,\"fixture\":true}\n";
        std::ofstream file(fixtureState/L"active.json",std::ios::binary|std::ios::trunc);
        file.write(active,sizeof(active)-1);require(static_cast<bool>(file),"Write synthetic active state");
    }
    auto writeStatus=[&](const wchar_t* section,const wchar_t* key,const wchar_t* value){
        require(WritePrivateProfileStringW(section,key,value,fixtureStatus.c_str())!=FALSE,"Write synthetic overlay status");
    };
    writeStatus(L"Status",L"Schema",L"1");
    writeStatus(L"Status",L"Installed",L"1");
    writeStatus(L"Status",L"Integrity",L"ok");
    writeStatus(L"Status",L"Compatible",L"1");
    writeStatus(L"Status",L"CanQueue",L"1");
    writeStatus(L"Status",L"Revision",L"153793411ee968c4200b5e5ebaab30c42d1a302156d1b00e4167140c1036082f");
    writeStatus(L"Status",L"Message",L"Synthetic isolated test configuration");
    writeStatus(L"Selection",L"FOV",L"95");
    writeStatus(L"Selection",L"Deadzone",L"0.15");
    writeStatus(L"Selection",L"MouseInput",L"standard");
    writeStatus(L"Selection",L"Controls",L"blacklist");
    writeStatus(L"Selection",L"Tactical",L"0");
    writeStatus(L"Selection",L"Deniable",L"0");
    if(!verifyPreferences)fs::remove(fixtureQueue);
    SetEnvironmentVariableW(L"CE_TEST_GAME_ROOT",root.c_str());
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"CEOverlayHarness";RegisterClassW(&wc);
    // Hidden test window: no focus or input is taken from the user's desktop.
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"Michael Works Splinter Cell Conviction Mod test harness",WS_OVERLAPPEDWINDOW,0,0,1000,780,nullptr,nullptr,instance,nullptr);
    auto module=LoadLibraryW(argv[1]);require(module!=nullptr,"Load test plugin");
    api<void(*)()>(module,"InitializeASI")();
    auto ready=api<BOOL(*)()>(module,"CETestReady");
    for(int i=0;i<200&&!ready();++i)Sleep(50);require(ready(),"Renderer hooks not ready");
    auto show=api<void(*)(BOOL)>(module,"CETestShow");auto visible=api<BOOL(*)()>(module,"CETestVisible");
    auto countFrames=api<unsigned long long(*)()>(module,"CETestFrames");auto countResets=api<unsigned long long(*)()>(module,"CETestResets");
    auto queue=api<BOOL(*)()>(module,"CETestQueue");
    auto queuedEvents=api<int(*)()>(module,"CETestQueuedEvents");
    auto page=api<int(*)()>(module,"CETestPage");
    auto padSet=api<void(*)(unsigned,const XINPUT_STATE*,BOOL)>(module,"CETestPad");
    auto padTime=api<void(*)(unsigned long long)>(module,"CETestPadTime");
    auto padFocus=api<void(*)(BOOL)>(module,"CETestPadFocus");
    auto gamePad=api<DWORD(*)(unsigned,DWORD,XINPUT_STATE*)>(module,"CETestGamePad");
    auto navId=api<unsigned(*)()>(module,"CETestNavId");
    auto firstItemId=api<unsigned(*)(int)>(module,"CETestPageFirstId");
    auto popup=api<BOOL(*)()>(module,"CETestPopup");
    auto fov=api<int(*)()>(module,"CETestFov");
    auto padReady=api<BOOL(*)()>(module,"CETestPadReady");
    auto checkpointView=api<void(*)(unsigned,int,const int*,unsigned,BOOL)>(module,"CETestCheckpointView");
    auto checkpointDispatches=api<unsigned(*)()>(module,"CETestCheckpointDispatches");
    auto checkpointLast=api<int(*)()>(module,"CETestCheckpointLast");
    auto checkpointPending=api<BOOL(*)()>(module,"CETestCheckpointPending");
    auto checkpointChoice=api<int(*)()>(module,"CETestCheckpointChoice");
    auto practiceActive=api<BOOL(*)()>(module,"CETestPracticeActive");
    auto partConfigure=api<void(*)(int)>(module,"CETestPartConfigure");
    auto partDispatches=api<unsigned(*)()>(module,"CETestPartDispatches");
    auto partLast=api<int(*)()>(module,"CETestPartLast");
    auto partPending=api<BOOL(*)()>(module,"CETestPartPending");
    auto selectedPart=api<int(*)()>(module,"CETestSelectedPart");
    auto exploreView=api<void(*)(unsigned,float,float,float,BOOL,BOOL)>(module,"CETestExploreView");
    auto exploreContextView=api<void(*)(unsigned,float,float,float,const char*,const char*,BOOL,BOOL)>(module,"CETestExploreContextView");
    auto checkpointAvailable=api<BOOL(*)()>(module,"CETestCheckpointAvailable");
    auto exploreTick=api<void(*)()>(module,"CETestExploreTick");
    auto exploreFailure=api<void(*)(unsigned)>(module,"CETestExploreFailure");
    auto exploreMoves=api<unsigned(*)()>(module,"CETestExploreNativeMoves");
    auto exploreHasReturn=api<BOOL(*)()>(module,"CETestExploreHasReturn");
    auto exploreTarget=api<BOOL(*)()>(module,"CETestExploreTarget");
    auto explorePending=api<BOOL(*)()>(module,"CETestExplorePending");
    auto exploreMessageEmpty=api<BOOL(*)()>(module,"CETestExploreMessageEmpty");
    auto exploreCurrent=api<void(*)(float*)>(module,"CETestExploreCurrent");
    auto exploreAnchor=api<void(*)(float*)>(module,"CETestExploreAnchor");
    auto exploreBody=api<void(*)(float,float,float)>(module,"CETestExploreBody");
    auto exploreSupport=api<BOOL(*)(float*)>(module,"CETestExploreSupport");
    auto explorePreviewState=api<unsigned(*)()>(module,"CETestExplorePreviewState");
    auto explorePreviewPoint=api<void(*)(float*)>(module,"CETestExplorePreviewPoint");
    auto exploreMarkerDrawn=api<BOOL(*)()>(module,"CETestExploreMarkerDrawn");
    auto exploreMarkerOnScreen=api<BOOL(*)()>(module,"CETestExploreMarkerOnScreen");
    auto cameraFov=api<void(*)(float)>(module,"CETestCameraFov");
    auto padError=api<void(*)(unsigned,DWORD)>(module,"CETestPadError");
    auto pauseRead=api<void(*)()>(module,"CETestPauseRead");
    auto readPaused=api<BOOL(*)()>(module,"CETestReadPaused");
    auto releaseRead=api<void(*)()>(module,"CETestReleaseRead");
    auto cameraObserve=api<void(*)(unsigned,ce::CameraPose*,BOOL)>(module,"CETestCameraObserve");
    auto cameraContextObserve=api<void(*)(unsigned,ce::CameraPose*,const char*,const char*,BOOL,BOOL)>(module,"CETestCameraContextObserve");
    auto cameraClock=api<void(*)(unsigned long long)>(module,"CETestCameraClock");
    auto cameraStart=api<void(*)()>(module,"CETestCameraStart");
    auto cameraActive=api<BOOL(*)()>(module,"CETestCameraActive");
    auto hudMask=api<unsigned(*)()>(module,"CETestHudMask");
    auto hudAllHidden=api<BOOL(*)()>(module,"CETestHudAllHidden");
    auto hudHint=api<BOOL(*)()>(module,"CETestHudHint");
    auto hudRestore=api<void(*)()>(module,"CETestHudRestore");
    if(verifyPreferences){
        require(hudMask()==5&&hudAllHidden()&&!hudHint(),"Fresh process did not load the saved individual choices, master and hint");
        hudRestore();
        require(hudMask()==0&&!hudAllHidden()&&hudHint(),"Restore all failed after loading persisted choices");
        std::ifstream file(root/L"ConvictionEnhanced-state/interface-preferences.ini");
        const std::string contents((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        require(contents.find("HiddenMask=0")!=std::string::npos&&contents.find("AllHidden=0")!=std::string::npos&&contents.find("CameraHint=1")!=std::string::npos,"Restore all did not persist visible defaults");
        std::cout<<"PASS: separate process loaded saved HUD mask/master/hint, then Restore all saved defaults.\n";
        return 0;
    }
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);require(d3d!=nullptr,"Direct3DCreate9");
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_COPY;pp.hDeviceWindow=hwnd;
    pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=960;pp.BackBufferHeight=720;
    IDirect3DDevice9* device{};require(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,hwnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)),"Create test D3D9 device");
    bool checkpointFixture=false;unsigned checkpointIdentity=31;int checkpointCurrent=0;const int checkpointIds[]={0,2,7};
    bool explorationFixture=false;unsigned explorationIdentity=41;
    bool soloOpsFixture=false,soloOpsSplit=false;const char* soloOpsMap="C01_MafiaClub";const char* soloOpsConnection="SOLO";
    ce::CameraPose soloOpsNatural{10,20,30,0,0,0};
    auto frame=[&]{
        if(checkpointFixture)checkpointView(checkpointIdentity,checkpointCurrent,checkpointIds,3,TRUE);
        if(explorationFixture){float xyz[3]{};exploreCurrent(xyz);
            if(soloOpsFixture){
                checkpointView(checkpointIdentity,-1,nullptr,0,FALSE);
                exploreContextView(explorationIdentity,xyz[0],xyz[1],xyz[2],soloOpsMap,soloOpsConnection,soloOpsSplit,TRUE);
                auto view=soloOpsNatural;cameraContextObserve(explorationIdentity,&view,soloOpsMap,soloOpsConnection,soloOpsSplit,TRUE);
            }else exploreView(explorationIdentity,xyz[0],xyz[1],xyz[2],TRUE,TRUE);
        }
        device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(12,20,25),1,0);
        const auto presented=device->Present(nullptr,nullptr,nullptr,nullptr);
        if(FAILED(presented))std::cerr<<"Presentation error="<<std::hex<<presented<<" device="<<device->TestCooperativeLevel()<<std::dec<<" at frame="<<countFrames()<<"\n";
        require(SUCCEEDED(presented),"Present failed");
        if(explorationFixture)exploreTick();
    };
    frame();require(countFrames()>0,"Present hook did not run");
    // Real entry detours + scripted raw states: no physical controller or game is used.
    XINPUT_STATE pad{},returned{};pad.dwPacketNumber=7;
    padSet(2,&pad,TRUE);frame();
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;pad.Gamepad.sThumbLX=14000;pad.Gamepad.bRightTrigger=180;padSet(2,&pad,TRUE);
    require(gamePad(0,2,&returned)==ERROR_SUCCESS&&memcmp(&returned,&pad,sizeof(pad))==0,"Closed controller path changed raw input");
    require(gamePad(1,2,&returned)==ERROR_SUCCESS&&returned.dwPacketNumber==107&&memcmp(&returned.Gamepad,&pad.Gamepad,sizeof(pad.Gamepad))==0,"Second XInput path changed raw input");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();
    padTime(2000);pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);
    require(gamePad(0,2,&returned)==ERROR_SUCCESS&&returned.Gamepad.wButtons==0,"Opening chord leaked to game");
    const DWORD capturedPacket=returned.dwPacketNumber;
    padTime(2599);frame();require(!visible(),"Controller menu opened before hold threshold");
    padTime(2600);frame();require(visible(),"Controller chord failed to open menu");
    require(gamePad(1,2,&returned)==ERROR_SUCCESS&&returned.Gamepad.wButtons==0,"Xidi/game alternate path not captured");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();frame();require(navId()!=0,"Controller opening did not focus an item");
    auto padPress=[&](WORD buttons){pad.Gamepad.wButtons=buttons;padSet(2,&pad,TRUE);frame();frame();pad.Gamepad.wButtons=0;padSet(2,&pad,TRUE);frame();frame();};
    auto verifyGameplayTriggerRelease=[&](const char* context){
        require(!visible()&&!cameraActive(),"Trigger passthrough fixture still owns controller input");
        pad.Gamepad={};padSet(2,&pad,TRUE);frame();
        DWORD before[2]{},pressed[2]{};
        for(unsigned entry=0;entry<2;++entry){
            require(gamePad(entry,2,&returned)==ERROR_SUCCESS,"Trigger neutral poll failed");before[entry]=returned.dwPacketNumber;
        }
        // Keep the physical packet fixed and every other field unchanged while
        // LT presses/releases. Both detours must publish the real release and a
        // new virtual packet after relinquishing their earlier menu/camera capture.
        pad.Gamepad.wButtons=XINPUT_GAMEPAD_X;pad.Gamepad.bRightTrigger=91;
        pad.Gamepad.sThumbLX=12345;pad.Gamepad.sThumbLY=-2345;
        pad.Gamepad.sThumbRX=-1234;pad.Gamepad.sThumbRY=23456;
        pad.Gamepad.bLeftTrigger=255;padSet(2,&pad,TRUE);
        for(unsigned entry=0;entry<2;++entry){
            require(gamePad(entry,2,&returned)==ERROR_SUCCESS&&memcmp(&returned.Gamepad,&pad.Gamepad,sizeof(pad.Gamepad))==0,"Gameplay LT press changed trigger or unrelated input fields");
            require(returned.dwPacketNumber!=before[entry],"Gameplay LT press did not advance its packet");pressed[entry]=returned.dwPacketNumber;
        }
        pad.Gamepad.bLeftTrigger=0;padSet(2,&pad,TRUE);
        for(unsigned entry=0;entry<2;++entry){
            require(gamePad(entry,2,&returned)==ERROR_SUCCESS&&memcmp(&returned.Gamepad,&pad.Gamepad,sizeof(pad.Gamepad))==0,"Gameplay LT release was lost or changed unrelated input fields");
            require(returned.dwPacketNumber!=pressed[entry],"Gameplay LT release did not advance its packet");
            const DWORD releasedPacket=returned.dwPacketNumber;
            require(gamePad(entry,2,&returned)==ERROR_SUCCESS&&returned.dwPacketNumber==releasedPacket&&memcmp(&returned.Gamepad,&pad.Gamepad,sizeof(pad.Gamepad))==0,"Repeated released LT sample changed its packet or input fields");
        }
        pad.Gamepad={};padSet(2,&pad,TRUE);frame();
        std::cout<<"PASS: gameplay LT press/release on both XInput paths after "<<context<<"; unrelated input fields preserved.\n";
    };
    require(padReady(),"Controller did not become ready after release");
    screenshot(device,root/L"controller-before-cross.bmp");
    padPress(XINPUT_GAMEPAD_A);screenshot(device,root/L"controller-after-cross.bmp");require(popup(),"Cross failed to open FOV choice");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(!popup()&&fov()==100,"D-pad/Cross failed to select FOV");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Cross failed to reopen choice");
    padPress(XINPUT_GAMEPAD_B);require(!popup()&&visible(),"Circle did not return from popup without closing menu");
    padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);require(page()==1,"R1 failed to switch to diagnostics");
    screenshot(device,root/L"controller-diagnostics.bmp");
    padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);require(page()==2,"R1 failed to switch to Help");
    padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);require(page()==1,"L1 failed to switch back");
    padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);require(page()==0,"L1 failed to return to settings");
    require(firstItemId(0)!=0&&navId()==firstItemId(0),"Returning from Diagnostics did not focus the Settings FOV control");
    // The HUD test backend supports all seven groups. Exercise real UI actions,
    // not setter seams, and keep the previous settings/controller checks intact.
    require(hudMask()==0&&!hudAllHidden()&&hudHint(),"Interface did not start with all elements visible");
    // Opening and cancelling exploration must leave ordinary saves active.
    const ce::CameraPose earlyCamera{10,20,30,0,0,0};auto earlyView=earlyCamera;
    exploreContextView(explorationIdentity,10,20,0,"C01_MafiaClub","SOLO",FALSE,TRUE);explorationFixture=true;soloOpsFixture=true;
    cameraClock(1000);cameraContextObserve(explorationIdentity,&earlyView,soloOpsMap,soloOpsConnection,FALSE,TRUE);frame();
    padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);require(page()==6,"L1 did not wrap Settings to Exploration");
    require(firstItemId(6)!=0&&navId()==firstItemId(6),"Exploration page did not focus its camera destination control");
    require(!checkpointAvailable(),"Solo Ops preview requires no campaign checkpoint support");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup()&&!practiceActive()&&!explorePending(),"Saving a return point skipped protected confirmation");
    padPress(XINPUT_GAMEPAD_B);
    require(!popup()&&visible()&&!practiceActive()&&!exploreHasReturn()&&exploreMoves()==0,"Cancelling exploration changed saves, player position or return point");
    // Destination guidance remains available when ordinary HUD and the optional
    // camera controls hint are hidden, without activating protected practice.
    padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);
    require(page()==4&&navId()==firstItemId(4),"Marker HUD fixture did not reach Interface");
    padPress(XINPUT_GAMEPAD_A);padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudAllHidden()&&!hudHint(),"Marker HUD fixture failed to hide HUD and camera hint");
    padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);
    require(page()==6&&navId()==firstItemId(6),"Marker fixture did not refocus Exploration");
    padPress(XINPUT_GAMEPAD_A);require(cameraActive()&&!visible(),"Marker fixture did not start Exploration flight");
    auto earlyFlight=earlyCamera;cameraContextObserve(explorationIdentity,&earlyFlight,soloOpsMap,soloOpsConnection,FALSE,TRUE);
    padTime(2700);frame();frame();
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::Valid)&&exploreMarkerDrawn(),
        "Live landing preview disappeared with HUD and camera hint hidden");
    require(!exploreMarkerOnScreen(),"A directly-below landing was projected through a level camera");
    require(!practiceActive()&&!exploreHasReturn()&&!exploreTarget()&&!explorePending()&&exploreMoves()==0,
        "Read-only landing preview activated practice, selected a destination or moved Sam");
    screenshot(device,root/L"exploration-marker-nohud.bmp");
    padPress(XINPUT_GAMEPAD_B);frame();frame();
    require(!cameraActive()&&!visible()&&!exploreMarkerDrawn()&&explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::None),
        "Cancelling camera flight retained a live destination marker");
    hudRestore();show(TRUE);frame();frame();
    require(!practiceActive()&&!exploreHasReturn()&&exploreMoves()==0,"Cancelled preview changed protected-practice state");
    soloOpsFixture=false;explorationFixture=false;checkpointFixture=true;
    padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);require(page()==5,"L1 did not switch Exploration to Mission");
    require(firstItemId(5)!=0&&navId()==firstItemId(5),"Mission page did not focus checkpoint choice");
    require(!practiceActive()&&checkpointDispatches()==0,"Practice or load started without confirmation");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Cross did not open checkpoint choice");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(checkpointChoice()==2&&!popup(),"D-pad/Cross did not select the next recorded checkpoint");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup()&&!practiceActive(),"Checkpoint action did not ask before practice activation");
    screenshot(device,root/L"mission-confirm.bmp");
    const auto checkpointKeyDowns=keyDowns,checkpointKeyUps=keyUps;
    SendMessageW(hwnd,WM_KEYDOWN,VK_F1,0);SendMessageW(hwnd,WM_KEYUP,VK_F1,1L<<30);frame();frame();
    SendMessageW(hwnd,WM_KEYDOWN,VK_F7,0);SendMessageW(hwnd,WM_KEYUP,VK_F7,1L<<30);frame();frame();
    require(page()==5&&popup()&&keyDowns==checkpointKeyDowns&&keyUps==checkpointKeyUps,"Page shortcut escaped the checkpoint confirmation modal");
    padPress(XINPUT_GAMEPAD_B);
    require(!popup()&&visible()&&!practiceActive()&&!checkpointPending(),"Circle did not cancel checkpoint confirmation cleanly");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Confirmation did not reopen");
    SendMessageW(hwnd,WM_KEYDOWN,VK_ESCAPE,0);frame();SendMessageW(hwnd,WM_KEYUP,VK_ESCAPE,1L<<30);frame();frame();
    require(!popup()&&visible()&&!practiceActive(),"Escape did not cancel confirmation without closing the menu");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Practice confirmation did not reopen after Escape");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;padSet(2,&pad,TRUE);frame();frame();
    require(practiceActive()&&!visible()&&checkpointPending(),"Confirmed checkpoint did not isolate saves and queue loading");
    require(checkpointDispatches()==0,"Held confirmation Cross reached the loader");
    frame();require(checkpointDispatches()==0,"Repeated held Cross executed pending checkpoint");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();frame();frame();
    require(checkpointDispatches()==1&&checkpointLast()==2,"Released controls did not execute exactly the chosen checkpoint");
    frame();frame();require(checkpointDispatches()==1,"Checkpoint request executed more than once");
    checkpointCurrent=2;++checkpointIdentity;frame();show(TRUE);frame();frame();
    require(visible()&&navId()==firstItemId(5),"Checkpoint return did not preserve menu navigation");
    screenshot(device,root/L"mission-practice.bmp");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);require(popup(),"Stale confirmation fixture failed");
    ++checkpointIdentity;frame();frame();padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);
    require(visible()&&checkpointDispatches()==1&&!checkpointPending(),"A stale mission confirmation queued a load");
    // The disabled Start action can leave Cancel as the only focusable item;
    // Cross may therefore have cancelled already. Circle closes a remaining modal.
    if(popup())padPress(XINPUT_GAMEPAD_B);
    require(!popup()&&visible(),"Stale mission confirmation could not be cancelled");
    std::cout<<"PASS: Mission PS5 choice, confirmation/Circle/Escape, protected practice activation, held-Cross drain, one load and stale-mission refusal.\n";
    partConfigure(7);frame();show(FALSE);frame();show(TRUE);frame();frame();
    require(page()==5&&selectedPart()==7&&navId()==firstItemId(5),"Mission part selector did not focus current Part 1");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Cross did not open mission-part choice");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(selectedPart()==8&&!popup(),"D-pad/Cross did not choose Part 2");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup()&&partDispatches()==0,"Part switch did not require confirmation");
    screenshot(device,root/L"mission-part-confirm.bmp");
    padPress(XINPUT_GAMEPAD_B);require(!popup()&&visible()&&partDispatches()==0,"Circle did not cancel part switching");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Part switch confirmation did not reopen");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;padSet(2,&pad,TRUE);frame();frame();
    require(!visible()&&partPending()&&partDispatches()==0,"Held Cross executed a part switch");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();frame();frame();
    require(partDispatches()==1&&partLast()==8&&checkpointDispatches()==1,"Part 2 switch did not queue exactly once independently of checkpoint restore");
    partConfigure(8);checkpointCurrent=-1;frame();frame();show(TRUE);frame();frame();
    require(!partPending()&&selectedPart()==8&&navId()==firstItemId(5),"Part 2 arrival did not restore menu controls");
    screenshot(device,root/L"mission-part-loaded.bmp");
    padPress(XINPUT_GAMEPAD_A);padPress(XINPUT_GAMEPAD_DPAD_UP);padPress(XINPUT_GAMEPAD_A);
    require(selectedPart()==7,"Part 1 return choice failed");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);require(popup(),"Part 1 return confirmation missing");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    require(partDispatches()==2&&partLast()==7,"Part 1 return did not queue once");
    partConfigure(7);frame();frame();show(TRUE);frame();frame();require(!partPending()&&selectedPart()==7,"Part 1 arrival not recognized");
    show(FALSE);verifyGameplayTriggerRelease("mission-part return");
    partConfigure(0);checkpointCurrent=2;frame();frame();show(FALSE);frame();show(TRUE);frame();frame();
    std::cout<<"PASS: PS5 Part 1/2 choice, cancellation, protected confirmation, held-button drain, native travel/arrival and return.\n";

    padPress(XINPUT_GAMEPAD_LEFT_SHOULDER);require(page()==4,"L1 did not switch Mission to Interface");
    require(firstItemId(4)!=0&&navId()==firstItemId(4),"Interface page did not focus its master checkbox");
    screenshot(device,root/L"interface-before-cross.bmp");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudMask()==1&&!hudAllHidden(),"Positive Show crosshair checkbox did not hide only the crosshair");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudMask()==3,"Target/world marker visibility did not remain independent");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudMask()==7,"Weapon/gadget visibility did not remain independent");
    padPress(XINPUT_GAMEPAD_DPAD_UP);padPress(XINPUT_GAMEPAD_DPAD_UP);padPress(XINPUT_GAMEPAD_DPAD_UP);
    padPress(XINPUT_GAMEPAD_A);
    require(hudAllHidden()&&hudMask()==7,"Master HUD hide discarded individual visibility choices");
    padPress(XINPUT_GAMEPAD_A);
    require(!hudAllHidden()&&hudMask()==7,"Disabling master HUD hide failed to restore individual choices");
    padPress(XINPUT_GAMEPAD_A);
    require(hudAllHidden()&&hudMask()==7,"Master HUD hide did not reactivate");
    // With individual controls disabled, navigation must skip them and retain a
    // reachable hint toggle and restore button. The debug menu must stay usable.
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(!hudHint()&&hudAllHidden()&&hudMask()==7&&visible(),"Master hide blocked hint control or closed the debug menu");
    screenshot(device,root/L"interface-hidden.bmp");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudMask()==0&&!hudAllHidden()&&hudHint()&&visible(),"Restore all failed to restore HUD choices and camera hint");
    screenshot(device,root/L"interface-restored.bmp");
    // The direct restoration export is idempotent and available for emergency
    // cleanup in tests; user-facing mutations above all went through the UI.
    hudRestore();require(hudMask()==0&&!hudAllHidden()&&hudHint(),"Repeated interface restoration changed defaults");
    padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);require(page()==5,"R1 did not switch Interface to Mission");
    padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);require(page()==6,"R1 did not switch Mission to Exploration");
    padPress(XINPUT_GAMEPAD_RIGHT_SHOULDER);require(page()==0,"R1 did not wrap Exploration to Settings");
    require(navId()==firstItemId(0),"Interface-to-Settings wrap did not focus the FOV control");
    std::cout<<"PASS: Interface fifth-page PS5 navigation, independent visibility choices, master preservation, disabled-control navigation, hint toggle and restore-all.\n";
    unsigned previousNav=navId();pad.Gamepad.sThumbLY=-20000;padSet(2,&pad,TRUE);frame();frame();
    pad.Gamepad.sThumbLY=0;padSet(2,&pad,TRUE);frame();frame();require(navId()!=previousNav,"Left stick failed to navigate");
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_B;padSet(2,&pad,TRUE);frame();require(!visible(),"Circle failed to close menu");
    require(gamePad(0,2,&returned)==ERROR_SUCCESS&&returned.Gamepad.wButtons==0,"Held close button leaked to game");
    pad.Gamepad={};padSet(2,&pad,TRUE);gamePad(0,2,&returned);frame();
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;padSet(2,&pad,TRUE);gamePad(0,2,&returned);
    require(returned.Gamepad.wButtons==XINPUT_GAMEPAD_A&&returned.dwPacketNumber!=capturedPacket,"Input/packet transition did not restore after release");
    verifyGameplayTriggerRelease("menu close");
    // Focus loss and reconnect must close and must not reopen a held chord.
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();show(TRUE);frame();
    padFocus(FALSE);frame();require(!visible(),"Controller focus loss did not close menu");
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);padFocus(TRUE);padTime(4000);frame();padTime(5000);frame();
    require(!visible(),"Held chord reopened after focus regain");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();show(TRUE);frame();
    padSet(2,nullptr,FALSE);require(gamePad(0,2,&returned)==ERROR_DEVICE_NOT_CONNECTED,"Disconnect return code changed");frame();require(!visible(),"Disconnect did not close menu");
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);padTime(6000);frame();padTime(7000);frame();require(!visible(),"Held reconnect chord reopened menu");
    pad.Gamepad={};padSet(2,&pad,TRUE);padTime(8000);frame();
    show(TRUE);frame();padError(2,ERROR_GEN_FAILURE);
    returned.dwPacketNumber=900;returned.Gamepad.wButtons=0x1234;XINPUT_STATE errorSentinel=returned;
    require(gamePad(0,2,&returned)==ERROR_GEN_FAILURE&&memcmp(&returned,&errorSentinel,sizeof(returned))==0,"Failed XInput call was altered");
    frame();require(!visible(),"Failed poll retained menu/stale controller input");
    padSet(2,&pad,TRUE);padTime(9000);frame();show(TRUE);frame();
    std::atomic<bool> reading{true};std::atomic<unsigned> reads{0},readFailures{0};
    std::thread reader([&]{while(reading){XINPUT_STATE background{};if(gamePad(reads.load()%2,2,&background)!=ERROR_SUCCESS)++readFailures;++reads;Sleep(1);}});
    try{for(int i=0;i<90;++i){if(i%10==0)show(i%20==0);frame();}}
    catch(...){reading=false;reader.join();throw;}
    reading=false;reader.join();require(reads>10&&readFailures==0,"Concurrent Xidi-style polling failed");show(FALSE);
    padFocus(FALSE);frame();pauseRead();
    std::thread staleReader([&]{XINPUT_STATE stale{};gamePad(0,2,&stale);});
    for(int i=0;i<1000&&!readPaused();++i)Sleep(1);
    const bool didPause=readPaused()!=FALSE;
    if(didPause){pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);padFocus(TRUE);padTime(10000);frame();}
    releaseRead();staleReader.join();require(didPause,"Stale-read test did not reach its barrier");
    padTime(11000);frame();padTime(12000);frame();require(!visible(),"Stale worker neutral reopened a held chord after focus gain");
    padSet(2,nullptr,FALSE);gamePad(0,2,&returned);frame();
    // Focus notifications also need to reach ImGui while its menu is hidden.
    SendMessageW(hwnd,WM_KILLFOCUS,0,0);padFocus(FALSE);frame();
    SendMessageW(hwnd,WM_SETFOCUS,0,0);padFocus(TRUE);pad.Gamepad={};padSet(2,&pad,TRUE);padTime(12500);frame();
    show(TRUE);frame();SendMessageW(hwnd,WM_KEYDOWN,VK_F1,0);SendMessageW(hwnd,WM_KEYUP,VK_F1,1L<<30);frame();frame();frame();
    require(padReady()&&navId()==firstItemId(0),"Hidden focus return did not restore controller navigation");
    padPress(XINPUT_GAMEPAD_A);require(popup(),"Cross did not select after hidden focus return");
    padPress(XINPUT_GAMEPAD_B);require(visible()&&!popup(),"Circle did not cancel after focus return");
    padPress(XINPUT_GAMEPAD_B);require(!visible(),"Circle did not close after focus return");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();padSet(2,nullptr,FALSE);gamePad(0,2,&returned);frame();
    std::cout<<"PASS: both XInput entry detours, immediate chord capture, slot 3 ownership, FOV selection, popup cancel, shoulder pages, stick navigation, close-button drain, packet restore, focus/reconnect/error recovery; concurrent reads="<<reads<<".\n";
    // Free camera uses only a simulated render-view output. No game process or
    // physical controller is read or altered by these integration checks.
    const ce::CameraPose naturalCamera{10,20,30,0,0,0};
    auto samePose=[](const ce::CameraPose& a,const ce::CameraPose& b){return a.x==b.x&&a.y==b.y&&a.z==b.z&&a.pitch==b.pitch&&a.yaw==b.yaw&&a.roll==b.roll;};
    auto viewCamera=[&]{auto view=naturalCamera;cameraObserve(41,&view,TRUE);return view;};
    XINPUT_GAMEPAD neutralPad{},pauseOnly{};pauseOnly.wButtons=XINPUT_GAMEPAD_START;
    auto gameIs=[&](unsigned entry,const XINPUT_GAMEPAD& expected){return gamePad(entry,2,&returned)==ERROR_SUCCESS&&memcmp(&returned.Gamepad,&expected,sizeof(expected))==0;};
    cameraClock(1000);padTime(13000);pad.Gamepad={};padSet(2,&pad,TRUE);frame();viewCamera();show(TRUE);frame();
    // Starting with Cross/Options held cannot activate motion or resume the game.
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_START;padSet(2,&pad,TRUE);cameraStart();
    require(cameraActive()&&!visible(),"Free-camera start did not retain capture while hiding menu");
    require(gameIs(0,neutralPad),"Camera activation leaked held Cross/Options");frame();require(!padReady(),"Camera entry skipped its neutral barrier");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();require(padReady(),"Camera input did not become ready after release");
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_DPAD_UP;
    pad.Gamepad.sThumbLX=20000;pad.Gamepad.sThumbLY=-15000;pad.Gamepad.sThumbRX=16000;pad.Gamepad.sThumbRY=-19000;
    pad.Gamepad.bLeftTrigger=150;pad.Gamepad.bRightTrigger=220;padSet(2,&pad,TRUE);
    const DWORD beforePausePacket=returned.dwPacketNumber;
    require(gameIs(0,pauseOnly),"Camera did not pass only Options while suppressing axes/triggers/other buttons");
    const DWORD pausePacket=returned.dwPacketNumber;require(pausePacket!=beforePausePacket,"Camera Options press did not advance virtual packet");
    require(gameIs(0,pauseOnly)&&returned.dwPacketNumber==pausePacket,"Held Options changed its emitted packet without changing payload");
    require(gameIs(1,pauseOnly),"Alternate XInput entry did not pass only camera Options");
    // A newer raw observation during a blocked hook call must not synthesize an
    // Options release, which would turn the next held sample into another press.
    pad.Gamepad={};pad.Gamepad.wButtons=XINPUT_GAMEPAD_START;padSet(2,&pad,TRUE);gamePad(0,2,&returned);
    const DWORD heldPausePacket=returned.dwPacketNumber;XINPUT_STATE staleCameraPad{};DWORD staleCameraResult=ERROR_GEN_FAILURE;
    pauseRead();std::thread cameraReader([&]{staleCameraResult=gamePad(0,2,&staleCameraPad);});
    bool cameraReadPaused=false;
    try {
        for(int i=0;i<1000&&!readPaused();++i)Sleep(1);
        cameraReadPaused=readPaused()!=FALSE;if(cameraReadPaused)frame();
    } catch(...){releaseRead();cameraReader.join();throw;}
    releaseRead();cameraReader.join();
    require(cameraReadPaused&&staleCameraResult==ERROR_SUCCESS,"Camera pause race did not reach its barrier");
    require(memcmp(&staleCameraPad.Gamepad,&pauseOnly,sizeof(pauseOnly))==0&&staleCameraPad.dwPacketNumber==heldPausePacket,
        "Concurrent camera poll synthesized an Options release or packet change");
    pad.Gamepad={};padSet(2,&pad,TRUE);require(gameIs(0,neutralPad)&&returned.dwPacketNumber!=heldPausePacket,"Camera Options release was not emitted with a new packet");
    require(gameIs(1,neutralPad),"Alternate XInput path retained Options after release");frame();
    pad.Gamepad.sThumbLY=32767;pad.Gamepad.sThumbRX=20000;pad.Gamepad.sThumbRY=18000;pad.Gamepad.bRightTrigger=255;padSet(2,&pad,TRUE);
    Sleep(20);frame();const auto flying=viewCamera();
    require(flying.x>naturalCamera.x&&flying.z>naturalCamera.z&&flying.yaw>0&&flying.pitch>0,"PS5 sticks/R2 did not move and turn the free camera");
    require(gameIs(0,neutralPad),"Camera movement leaked gameplay axes or triggers");
    pad.Gamepad={};pad.Gamepad.bLeftTrigger=255;padSet(2,&pad,TRUE);Sleep(20);frame();const auto descending=viewCamera();
    require(descending.z<flying.z,"L2 did not lower the free camera");screenshot(device,root/L"free-camera-hud.bmp");
    // Circle requests stop immediately; Options must release before the held
    // exit controls can be returned to the game after the backend is disabled.
    pad.Gamepad={};pad.Gamepad.wButtons=XINPUT_GAMEPAD_START;padSet(2,&pad,TRUE);require(gameIs(0,pauseOnly),"Options was not available before camera exit");
    const DWORD exitPausePacket=returned.dwPacketNumber;
    pad.Gamepad.wButtons|=XINPUT_GAMEPAD_B;padSet(2,&pad,TRUE);frame();
    require(gameIs(0,neutralPad)&&returned.dwPacketNumber!=exitPausePacket,"Camera stop did not release held Options immediately");
    frame();require(!cameraActive()&&!visible(),"Circle failed to stop free camera and return to gameplay");
    require(samePose(viewCamera(),naturalCamera),"Stopped camera still overrode render output");
    require(gameIs(0,neutralPad),"Held camera-exit buttons leaked through the release drain");
    pad.Gamepad={};padSet(2,&pad,TRUE);gamePad(0,2,&returned);frame();
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;padSet(2,&pad,TRUE);require(gameIs(0,pad.Gamepad),"Gameplay controller did not restore after camera exit release");
    verifyGameplayTriggerRelease("free-camera return");
    auto startReadyCamera=[&]{pad.Gamepad={};padSet(2,&pad,TRUE);frame();viewCamera();show(TRUE);frame();cameraStart();frame();require(cameraActive()&&!visible()&&padReady(),"Camera failed to restart with a neutral controller");};
    startReadyCamera();
    padTime(14000);pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);frame();
    padTime(14599);frame();require(cameraActive()&&!visible(),"Camera chord returned to menu before its hold threshold");
    padTime(14600);frame();require(!cameraActive()&&visible(),"Held L3/R3 did not stop camera and return to menu");
    require(gameIs(0,neutralPad),"Return-to-menu chord leaked to game");
    require(samePose(viewCamera(),naturalCamera),"Return-to-menu kept overriding the camera");
    show(FALSE);startReadyCamera();
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_START;padSet(2,&pad,TRUE);require(gameIs(0,pauseOnly),"Options did not pass before camera focus loss");
    padFocus(FALSE);frame();require(!cameraActive()&&!visible()&&gameIs(0,neutralPad),"Focus loss retained camera or held Options");
    require(samePose(viewCamera(),naturalCamera),"Focus loss retained render override");
    padFocus(TRUE);frame();require(gameIs(0,neutralPad),"Focus regain replayed held Options before neutral");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();startReadyCamera();
    padSet(2,nullptr,FALSE);require(gamePad(0,2,&returned)==ERROR_DEVICE_NOT_CONNECTED,"Camera disconnect return code changed");
    frame();require(!cameraActive()&&!visible()&&samePose(viewCamera(),naturalCamera),"Controller disconnect did not restore the natural camera");
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);padTime(15000);frame();padTime(16000);frame();
    require(!cameraActive()&&!visible(),"Held reconnect chord reopened camera/menu");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();padSet(2,nullptr,FALSE);gamePad(0,2,&returned);frame();
    std::cout<<"PASS: free-camera entry barrier, Options-only payload/packet filtering on both XInput paths, concurrent held-Options polling, PS5 sticks/triggers, Circle drain, L3/R3 return, focus/disconnect recovery, natural render restoration.\n";
    // A real camera flight chooses the destination; native-host fixtures model
    // floor/capsule checks and relocation, never a game process or pawn write.
    exploreView(explorationIdentity,10,20,0,TRUE,TRUE);explorationFixture=true;
    exploreFailure(0);padTime(17000);padFocus(TRUE);pad.Gamepad={};padSet(2,&pad,TRUE);frame();
    auto openExploration=[&]{
        pad.Gamepad={};padSet(2,&pad,TRUE);auto view=naturalCamera;cameraObserve(explorationIdentity,&view,TRUE);show(TRUE);frame();
        SendMessageW(hwnd,WM_KEYDOWN,VK_F7,0);SendMessageW(hwnd,WM_KEYUP,VK_F7,1L<<30);frame();frame();frame();
        require(page()==6&&firstItemId(6)!=0&&navId()==firstItemId(6),"F7 did not focus the exploration destination control");
    };
    auto exploreButton=[&](unsigned index){openExploration();for(unsigned i=0;i<index;++i)padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);};
    auto sameXYZ=[](const float* a,float x,float y,float z){return std::fabs(a[0]-x)<.05f&&std::fabs(a[1]-y)<.05f&&std::fabs(a[2]-z)<.05f;};
    float explorationPosition[3]{},returnPosition[3]{},liveLanding[3]{},selectedLanding[3]{};
    openExploration();screenshot(device,root/L"exploration-ready.bmp");
    padPress(XINPUT_GAMEPAD_A);require(cameraActive()&&!visible(),"Exploration destination action did not start the free camera");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();
    pad.Gamepad.sThumbLY=32767;pad.Gamepad.bRightTrigger=180;padSet(2,&pad,TRUE);Sleep(20);frame();
    require(gameIs(0,neutralPad)&&gameIs(1,neutralPad),"Exploration flight leaked camera input through an XInput path");
    const auto explorationFlight=viewCamera();
    require(explorationFlight.x>naturalCamera.x&&explorationFlight.z>naturalCamera.z,"Exploration flight did not leave Sam's current position");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();
    padTime(17100);frame();frame();
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::Valid)&&exploreMarkerDrawn(),
        "Exploration flight did not render a checked live landing preview");
    explorePreviewPoint(liveLanding);
    require(std::fabs(liveLanding[0]-explorationFlight.x)<.05f&&std::fabs(liveLanding[1]-explorationFlight.y)<.05f,
        "Live landing marker did not follow the free-camera destination");
    padTime(17200);pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);frame();
    padTime(17799);frame();require(cameraActive()&&!visible(),"Exploration chord stopped the camera before its hold threshold");
    padTime(17800);frame();require(!cameraActive()&&visible()&&exploreTarget(),"Camera-to-menu handoff lost the exploration destination");
    require(samePose(viewCamera(),naturalCamera)&&exploreTarget(),"Restoring the natural camera replaced the selected destination");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();frame();
    // The selected destination is a new preview revision. Advance the native
    // query clock past its throttle before expecting the checked landing.
    padTime(17900);frame();frame();
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::Valid)&&exploreMarkerDrawn(),
        "Selected landing preview disappeared after returning to the menu");
    explorePreviewPoint(selectedLanding);
    require(sameXYZ(selectedLanding,liveLanding[0],liveLanding[1],liveLanding[2]),
        "Restoring the normal camera changed the selected ground marker");
    // A known view looks straight down at the selected floor. This exercises
    // actual projected overlay geometry instead of merely checking status text.
    ce::CameraPose landingView{selectedLanding[0],selectedLanding[1],selectedLanding[2]+200,-16384,0,0};
    cameraFov(90);cameraObserve(explorationIdentity,&landingView,TRUE);frame();frame();
    require(exploreMarkerDrawn()&&exploreMarkerOnScreen(),"Valid on-screen landing did not render its ground ring");
    screenshot(device,root/L"exploration-marker-ground.bmp");
    cameraFov(0);cameraObserve(explorationIdentity,&landingView,TRUE);frame();frame();
    require(exploreMarkerDrawn()&&!exploreMarkerOnScreen(),"Unknown FOV projected a misleading ground marker");
    cameraFov(90);viewCamera();frame();frame();
    exploreCurrent(explorationPosition);require(sameXYZ(explorationPosition,10,20,0)&&exploreMoves()==0&&!exploreHasReturn(),"Camera flight moved Sam or invented a return point");
    exploreButton(2);require(popup(),"Teleport action did not require confirmation");
    screenshot(device,root/L"exploration-confirm.bmp");
    padPress(XINPUT_GAMEPAD_B);require(visible()&&!popup()&&exploreMoves()==0&&!explorePending(),"Circle did not cancel teleport without moving Sam");
    exploreButton(2);require(popup(),"Teleport confirmation did not reopen");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);
    pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;padSet(2,&pad,TRUE);frame();frame();
    require(practiceActive()&&!visible()&&explorePending()&&exploreMoves()==0,"Teleport failed to protect saves or wait for held Cross");
    frame();require(exploreMoves()==0,"Repeated held Cross reached native movement");
    pad.Gamepad={};padSet(2,&pad,TRUE);frame();frame();
    require(!explorePending()&&exploreMoves()==1&&exploreHasReturn(),"Neutral release did not execute exactly one protected teleport");
    exploreCurrent(explorationPosition);exploreAnchor(returnPosition);
    require(sameXYZ(explorationPosition,explorationFlight.x,explorationFlight.y,0),"Sam did not move to validated ground beneath the chosen camera destination");
    require(sameXYZ(returnPosition,10,20,0),"Automatic return point used the camera position instead of Sam's original position");
    frame();frame();require(exploreMoves()==1,"Teleport executed more than once");
    openExploration();require(exploreMessageEmpty(),"Successful teleport retained its queued progress instruction");
    screenshot(device,root/L"exploration-moved.bmp");
    exploreButton(3);require(popup(),"Return action did not require confirmation");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    exploreCurrent(explorationPosition);exploreAnchor(returnPosition);
    require(exploreMoves()==2&&sameXYZ(explorationPosition,10,20,0)&&sameXYZ(returnPosition,10,20,0),"Return failed to restore the original position and preserve its anchor");
    // The same destination becoming occupied must be refused before any native
    // relocation, and the escape anchor must survive the refusal.
    exploreFailure(4);exploreButton(2);require(popup(),"Blocked landing fixture could not request its retained destination");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    exploreCurrent(explorationPosition);exploreAnchor(returnPosition);
    require(!explorePending()&&exploreMoves()==2&&sameXYZ(explorationPosition,10,20,0)&&exploreHasReturn()&&sameXYZ(returnPosition,10,20,0),"Blocked landing moved Sam or discarded the safe return point");
    openExploration();require(exploreMessageEmpty(),"Blocked landing retained its queued progress instruction");
    padTime(18000);frame();frame();
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::Blocked)&&exploreMarkerDrawn()&&!exploreMarkerOnScreen(),
        "Blocked landing still displayed a safe ground ring or lost its warning");
    screenshot(device,root/L"exploration-blocked.bmp");exploreFailure(0);
    padTime(18100);frame();frame();
    exploreButton(2);require(popup(),"Stale exploration fixture failed to open confirmation");
    ++explorationIdentity;exploreView(explorationIdentity,10,20,0,TRUE,TRUE);frame();frame();
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);
    require(exploreMoves()==2&&!explorePending()&&!exploreHasReturn()&&!exploreTarget(),"A changed scene retained coordinates or accepted stale teleport confirmation");
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::None)&&!exploreMarkerDrawn(),
        "A changed scene retained the old landing marker");
    if(popup())padPress(XINPUT_GAMEPAD_B);
    // Save-return confirmation remains input-gated too. Losing focus while Cross
    // is held must cancel it, without leaving a newly saved anchor behind.
    auto changedView=naturalCamera;cameraObserve(explorationIdentity,&changedView,TRUE);
    exploreButton(1);require(popup(),"Focus-loss save-return fixture failed to open confirmation");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);pad.Gamepad.wButtons=XINPUT_GAMEPAD_A;padSet(2,&pad,TRUE);frame();
    require(explorePending()&&!exploreHasReturn(),"Held save confirmation bypassed its release barrier");
    padFocus(FALSE);frame();require(!visible()&&!explorePending()&&!exploreHasReturn()&&exploreMoves()==2,"Focus loss retained an exploration request or changed its anchor");
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::None)&&!exploreMarkerDrawn(),
        "Focus loss retained a destination marker");
    pad.Gamepad={};padSet(2,&pad,TRUE);padFocus(TRUE);frame();frame();
    verifyGameplayTriggerRelease("exploration return");
    explorationFixture=false;padSet(2,nullptr,FALSE);gamePad(0,2,&returned);frame();
    std::cout<<"PASS: Exploration PS5 camera destination handoff, live/selected landing markers independent of HUD/hint, projected ground ring, unknown-FOV suppression, cancellation, protected release-gated teleport, original-position return, blocked landing, stale-scene and focus cancellation.\n";
    // Real map/session predicates feed both backends. Campaign checkpoints are
    // deliberately unavailable throughout this controller-driven solo Ops trip.
    checkpointFixture=false;soloOpsFixture=explorationFixture=true;
    soloOpsMap="C02_Embassy";soloOpsConnection="SOLO";soloOpsSplit=false;++explorationIdentity;
    soloOpsNatural={10,20,150,-16384,0,0};
    padTime(18500);pad.Gamepad={};padSet(2,&pad,TRUE);padFocus(TRUE);exploreFailure(0);
    require(gamePad(0,2,&returned)==ERROR_SUCCESS&&gamePad(1,2,&returned)==ERROR_SUCCESS,
        "Solo Ops reconnect fixture did not publish the game's fresh neutral controller reads");
    frame();frame();
    const auto beforeSoloMoves=exploreMoves();exploreCurrent(returnPosition);
    require(!checkpointAvailable()&&!exploreHasReturn()&&!exploreTarget(),"Solo Ops transition inherited campaign coordinates or checkpoint eligibility");
    auto openSoloExploration=[&]{
        pad.Gamepad={};padSet(2,&pad,TRUE);show(TRUE);frame();
        SendMessageW(hwnd,WM_KEYDOWN,VK_F7,0);SendMessageW(hwnd,WM_KEYUP,VK_F7,1L<<30);frame();frame();frame();
        if(page()!=6||navId()!=firstItemId(6)){
            std::cerr<<"Solo Ops focus page="<<page()<<" nav="<<navId()<<" first="<<firstItemId(6)<<" ready="<<padReady()<<" camera="<<cameraActive()<<"\n";
            screenshot(device,root/L"solo-ops-focus-failure.bmp");
        }
        require(page()==6&&navId()==firstItemId(6),"Solo Ops menu did not focus the camera destination action");
    };
    openSoloExploration();padPress(XINPUT_GAMEPAD_A);require(cameraActive()&&!visible(),"PS5 failed to start solo Ops Exploration camera");
    require(gameIs(0,neutralPad)&&gameIs(1,neutralPad),
        "Solo Ops flight did not capture the game's reads through both XInput entries");
    pad.Gamepad.sThumbLX=32767;pad.Gamepad.bRightTrigger=180;padSet(2,&pad,TRUE);Sleep(20);frame();
    pad.Gamepad={};padSet(2,&pad,TRUE);padTime(18600);frame();frame();
    require(explorePreviewState()==static_cast<unsigned>(ce::ExplorationPreviewState::Valid)&&exploreMarkerDrawn()
        &&exploreMarkerOnScreen()&&!checkpointAvailable(),"Solo Ops flight did not show its checked landing ring without campaign support");
    padTime(18700);pad.Gamepad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB;padSet(2,&pad,TRUE);frame();
    padTime(19300);frame();pad.Gamepad={};padSet(2,&pad,TRUE);padTime(19400);frame();frame();
    require(!cameraActive()&&visible()&&exploreTarget()&&exploreMarkerDrawn(),"Solo Ops PS5 handoff lost the selected marker");
    screenshot(device,root/L"solo-ops-exploration-marker.bmp");
    openSoloExploration();padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup(),"Solo Ops teleport did not require confirmation");padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    require(practiceActive()&&exploreMoves()==beforeSoloMoves+1&&exploreHasReturn()&&!checkpointAvailable(),
        "Solo Ops protected PS5 teleport required campaign checkpoints or failed to preserve an anchor");
    float savedFloor=0;
    require(exploreSupport(&savedFloor)&&std::fabs(savedFloor+92)<.05f,
        "Solo Ops automatic return point did not retain its validated supporting floor");
    // The real game can crouch between opening the menu and a later return.
    // This fixture changes the collision body, not the controller mapping or
    // original anchor, and retains a single known stationary floor at Z=-92.
    exploreBody(48,-92,-92);frame();frame();
    openSoloExploration();for(int index=0;index<3;++index)padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup(),"Solo Ops return did not require confirmation");padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    exploreCurrent(explorationPosition);
    require(exploreMoves()==beforeSoloMoves+2&&sameXYZ(explorationPosition,returnPosition[0],returnPosition[1],-92+48+2),
        "Solo Ops standing-to-crouched return did not reach its original floor with the current body height");
    float standingAnchor[3]{};exploreAnchor(standingAnchor);
    require(sameXYZ(standingAnchor,returnPosition[0],returnPosition[1],returnPosition[2])
        &&exploreSupport(&savedFloor)&&std::fabs(savedFloor+92)<.05f,
        "Crouched return changed the original standing anchor or its supporting floor");
    screenshot(device,root/L"solo-ops-return-crouched.bmp");
    // Explicitly replace the anchor while crouched, then stand and repeat the
    // trip. The saved body centre stays unchanged; the landing follows its
    // verified floor, using the character's current collision body.
    openSoloExploration();padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup(),"Crouched return-point replacement skipped confirmation");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    float crouchedAnchor[3]{};exploreAnchor(crouchedAnchor);
    require(sameXYZ(crouchedAnchor,returnPosition[0],returnPosition[1],-92+48+2)
        &&exploreSupport(&savedFloor)&&std::fabs(savedFloor+92)<.05f,
        "PS5 replacement did not capture the crouched position and its actual supporting floor");
    exploreBody(90,-92,-92);frame();frame();
    openSoloExploration();padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup(),"Standing repeat teleport skipped confirmation");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    require(exploreMoves()==beforeSoloMoves+3,"Standing repeat trip did not execute one protected outbound move");
    openSoloExploration();for(int index=0;index<3;++index)padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(popup(),"Solo Ops crouched-to-standing return skipped confirmation");
    padPress(XINPUT_GAMEPAD_DPAD_RIGHT);padPress(XINPUT_GAMEPAD_A);frame();frame();
    exploreCurrent(explorationPosition);float retainedCrouchedAnchor[3]{};exploreAnchor(retainedCrouchedAnchor);
    require(exploreMoves()==beforeSoloMoves+4&&sameXYZ(explorationPosition,returnPosition[0],returnPosition[1],-92+90+2)
        &&sameXYZ(retainedCrouchedAnchor,crouchedAnchor[0],crouchedAnchor[1],crouchedAnchor[2]),
        "Solo Ops crouched-to-standing return used the old body centre or replaced its anchor");
    screenshot(device,root/L"solo-ops-return-standing.bmp");
    verifyGameplayTriggerRelease("solo Deniable Ops return");
    soloOpsConnection="LINK";frame();frame();
    require(!exploreTarget()&&!exploreHasReturn()&&!exploreSupport(&savedFloor)&&!exploreMarkerDrawn()&&!cameraActive(),
        "Network transition retained solo Ops coordinates, supporting floor or camera marker");
    soloOpsConnection="SOLO";soloOpsSplit=true;frame();frame();
    require(!exploreTarget()&&!exploreHasReturn()&&!exploreSupport(&savedFloor)&&!exploreMarkerDrawn(),
        "Split context retained a solo Ops landing marker or return-support floor");
    soloOpsFixture=explorationFixture=false;padSet(2,nullptr,FALSE);gamePad(0,2,&returned);frame();
    std::cout<<"PASS: solo Deniable Ops real map/session predicates, campaign-checkpoint independence, PS5 marker/teleport, floor-relative standing/crouched return both ways, replacement, input release and network/split invalidation.\n";
    auto kd=keyDowns,ku=keyUps,mm=mouseMoves;
    SendMessageW(hwnd,WM_KEYDOWN,VK_TAB,0);SendMessageW(hwnd,WM_KEYUP,VK_TAB,0);SendMessageW(hwnd,WM_MOUSEMOVE,0,MAKELPARAM(4,5));
    require(keyDowns==kd+1&&keyUps==ku+1&&mouseMoves==mm+1,"Closed menu did not pass input through");
    // A navigation key held before the menu opens remains the game's key.
    kd=keyDowns;ku=keyUps;
    SendMessageW(hwnd,WM_KEYDOWN,VK_UP,0);show(TRUE);
    SendMessageW(hwnd,WM_KEYDOWN,VK_UP,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_UP,1L<<30);
    require(keyDowns==kd+2&&keyUps==ku+1,"Held game key was stolen on autorepeat");show(FALSE);
    // Local thread keyboard state only: no input is injected into other applications.
    BYTE originalKeys[256]{},chordKeys[256]{};GetKeyboardState(originalKeys);memcpy(chordKeys,originalKeys,256);
    chordKeys[VK_CONTROL]=chordKeys[VK_SHIFT]=0;SetKeyboardState(chordKeys);
    kd=keyDowns;ku=keyUps;SendMessageW(hwnd,WM_KEYDOWN,VK_F10,0);
    chordKeys[VK_CONTROL]=chordKeys[VK_SHIFT]=0x80;SetKeyboardState(chordKeys);
    SendMessageW(hwnd,WM_KEYDOWN,VK_F10,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_F10,1L<<30);
    require(!visible()&&keyDowns==kd+2&&keyUps==ku+1,"Held F10 was stolen when modifiers changed");
    kd=keyDowns;ku=keyUps;SendMessageW(hwnd,WM_KEYDOWN,VK_F10,0);
    chordKeys[VK_CONTROL]=chordKeys[VK_SHIFT]=0;SetKeyboardState(chordKeys);
    SendMessageW(hwnd,WM_KEYDOWN,VK_F10,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_F10,1L<<30);
    require(visible()&&keyDowns==kd&&keyUps==ku,"Fresh toggle chord failed");SetKeyboardState(originalKeys);
    // Windows can deliver F10 as a system key even without Alt.
    show(FALSE);chordKeys[VK_CONTROL]=chordKeys[VK_SHIFT]=0x80;chordKeys[VK_MENU]=0;SetKeyboardState(chordKeys);
    kd=keyDowns;ku=keyUps;
    SendMessageW(hwnd,WM_SYSKEYDOWN,VK_F10,0);SendMessageW(hwnd,WM_SYSKEYDOWN,VK_F10,1L<<30);SendMessageW(hwnd,WM_SYSKEYUP,VK_F10,1L<<30);
    require(visible()&&keyDowns==kd&&keyUps==ku,"System F10 toggle or ownership failed");SetKeyboardState(originalKeys);
    SendMessageW(hwnd,WM_KEYDOWN,VK_TAB,0);show(FALSE);
    SendMessageW(hwnd,WM_SYSKEYDOWN,VK_TAB,1L<<30);SendMessageW(hwnd,WM_SYSKEYUP,VK_TAB,1L<<30);
    require(keyDowns==kd&&keyUps==ku,"Owned key release through system messages leaked");show(TRUE);
    SendMessageW(hwnd,WM_KEYDOWN,VK_TAB,0);require(queuedEvents()>0,"Expected navigation event");
    show(FALSE);require(queuedEvents()==0,"Closing retained queued ImGui input");SendMessageW(hwnd,WM_KEYUP,VK_TAB,1L<<30);
    for(int i=0;i<30;++i){SendMessageW(hwnd,WM_KEYDOWN,VK_SHIFT,0);SendMessageW(hwnd,WM_KEYUP,VK_SHIFT,1L<<30);}
    require(queuedEvents()==0,"Hidden menu accumulated modifier events");show(TRUE);require(queuedEvents()==0,"Reopening replayed stale input");show(FALSE);
    for(int i=0;i<10;++i){show(TRUE);frame();show(FALSE);frame();}
    show(TRUE);frame();kd=keyDowns;ku=keyUps;mm=mouseMoves;
    SendMessageW(hwnd,WM_KEYDOWN,VK_TAB,0);frame();show(FALSE);SendMessageW(hwnd,WM_KEYDOWN,VK_TAB,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_TAB,0);
    require(keyDowns==kd&&keyUps==ku,"Owned keyup leaked when closing menu");
    show(TRUE);SendMessageW(hwnd,WM_MOUSEMOVE,0,MAKELPARAM(6,7));
    require(mouseMoves==mm+1,"Mouse messages were intercepted");
    SendMessageW(hwnd,WM_KEYDOWN,'A',0);SendMessageW(hwnd,WM_KEYUP,'A',0);
    require(keyDowns==kd+1&&keyUps==ku+1,"Unowned keys were intercepted");
    for(int i=0;i<5;++i)frame();
    screenshot(device,root/L"overlay-render.bmp");
    SendMessageW(hwnd,WM_KEYDOWN,VK_F2,0);SendMessageW(hwnd,WM_KEYUP,VK_F2,1L<<30);frame();frame();
    require(page()==1,"Diagnostics shortcut failed");screenshot(device,root/L"overlay-diagnostics.bmp");
    SendMessageW(hwnd,WM_KEYDOWN,VK_F3,0);SendMessageW(hwnd,WM_KEYUP,VK_F3,1L<<30);frame();frame();
    require(page()==2,"Help shortcut failed");screenshot(device,root/L"overlay-help.bmp");
    SendMessageW(hwnd,WM_KEYDOWN,VK_F4,0);SendMessageW(hwnd,WM_KEYUP,VK_F4,1L<<30);frame();frame();
    require(page()==3,"Camera shortcut failed");screenshot(device,root/L"overlay-camera.bmp");
    kd=keyDowns;ku=keyUps;
    SendMessageW(hwnd,WM_KEYDOWN,VK_F5,0);SendMessageW(hwnd,WM_KEYDOWN,VK_F5,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_F5,1L<<30);frame();frame();
    require(page()==4&&keyDowns==kd&&keyUps==ku,"Interface F5 shortcut or complete held-key ownership failed");
    screenshot(device,root/L"overlay-interface.bmp");
    kd=keyDowns;ku=keyUps;
    SendMessageW(hwnd,WM_KEYDOWN,VK_F6,0);SendMessageW(hwnd,WM_KEYDOWN,VK_F6,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_F6,1L<<30);frame();frame();frame();
    require(page()==5&&keyDowns==kd&&keyUps==ku,"Mission F6 shortcut or complete held-key ownership failed");
    screenshot(device,root/L"overlay-mission.bmp");
    kd=keyDowns;ku=keyUps;
    SendMessageW(hwnd,WM_KEYDOWN,VK_F7,0);SendMessageW(hwnd,WM_KEYDOWN,VK_F7,1L<<30);SendMessageW(hwnd,WM_KEYUP,VK_F7,1L<<30);frame();frame();frame();
    require(page()==6&&keyDowns==kd&&keyUps==ku,"Exploration F7 shortcut or complete held-key ownership failed");
    screenshot(device,root/L"overlay-exploration.bmp");
    SendMessageW(hwnd,WM_KEYDOWN,VK_F1,0);SendMessageW(hwnd,WM_KEYUP,VK_F1,1L<<30);frame();frame();
    require(page()==0,"Settings shortcut failed");
    // Verify the overlay doesn't leave changed render state or viewport behind.
    D3DVIEWPORT9 vp{4,8,800,600,.1f,.9f};device->SetViewport(&vp);device->SetRenderState(D3DRS_ZENABLE,TRUE);
    frame();D3DVIEWPORT9 after{};device->GetViewport(&after);DWORD z{};device->GetRenderState(D3DRS_ZENABLE,&z);
    require(memcmp(&vp,&after,sizeof(vp))==0&&z==TRUE,"Render state leaked from overlay");
    IDirect3DSurface9 *originalTarget{},*offscreen{},*depth{},*extra{},*observed{};
    require(SUCCEEDED(device->GetRenderTarget(0,&originalTarget)),"Capture original target");
    require(SUCCEEDED(device->CreateRenderTarget(960,720,D3DFMT_X8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&offscreen,nullptr)),"Offscreen target");
    require(SUCCEEDED(device->CreateDepthStencilSurface(960,720,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr)),"Depth surface");
    require(SUCCEEDED(device->SetRenderTarget(0,offscreen))&&SUCCEEDED(device->SetDepthStencilSurface(depth)),"Set offscreen targets");
    D3DCAPS9 caps{};device->GetDeviceCaps(&caps);
    if(caps.NumSimultaneousRTs>1){require(SUCCEEDED(device->CreateRenderTarget(960,720,D3DFMT_X8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&extra,nullptr)),"Extra target");require(SUCCEEDED(device->SetRenderTarget(1,extra)),"Set MRT");}
    frame();device->GetRenderTarget(0,&observed);require(observed==offscreen,"Offscreen target not restored");observed->Release();
    device->GetDepthStencilSurface(&observed);require(observed==depth,"Depth target not restored");observed->Release();
    if(extra){device->GetRenderTarget(1,&observed);require(observed==extra,"MRT not restored");observed->Release();device->SetRenderTarget(1,nullptr);extra->Release();}
    device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,originalTarget);depth->Release();offscreen->Release();originalTarget->Release();
    SendMessageW(hwnd,WM_KILLFOCUS,0,0);require(!visible(),"Focus loss did not close overlay");
    auto oldResets=countResets();
    for(int i=0;i<3;++i){require(SUCCEEDED(device->Reset(&pp)),"Reset failed");show(TRUE);frame();}
    require(countResets()==oldResets+3,"Reset hook not observed");
    writeStatus(L"Status",L"CanQueue",L"0");
    frame();screenshot(device,root/L"core-only-settings.bmp");
    require(!queue(),"Core-only install accepted a preset settings queue");
    require(!fs::exists(fixtureQueue),"Core-only queue refusal created pending settings");
    writeStatus(L"Status",L"CanQueue",L"1");frame();
    require(queue(),"Settings queue failed after enabling verified local presets");
    require(fs::exists(root/L"ConvictionEnhanced-state/overlay-pending.ini"),"Queue file missing");
    std::cout<<"PASS: self-contained synthetic settings revision, core-only queue refusal and restored preset queuing.\n";
    SendMessageW(hwnd,WM_KEYDOWN,VK_ESCAPE,0);require(!visible(),"Escape did not close menu");SendMessageW(hwnd,WM_KEYUP,VK_ESCAPE,0);
    // Finish with deliberately nondefault preferences, selected through real UI
    // events. A second harness process must load these before any GUI action.
    pad.Gamepad={};padSet(2,&pad,TRUE);padFocus(TRUE);padTime(20000);frame();show(TRUE);
    SendMessageW(hwnd,WM_SETFOCUS,0,0);frame();frame();
    SendMessageW(hwnd,WM_KEYDOWN,VK_F5,0);SendMessageW(hwnd,WM_KEYUP,VK_F5,1L<<30);frame();frame();
    frame();frame(); // tab selection and keyboard focus settle on separate frames
    screenshot(device,root/L"interface-persistence.bmp");
    if(page()!=4||navId()!=firstItemId(4))std::cerr<<"Persistence focus: page="<<page()<<" nav="<<navId()<<" first="<<firstItemId(4)<<" padReady="<<padReady()<<" visible="<<visible()<<"\n";
    require(page()==4&&navId()==firstItemId(4),"Persistence setup did not focus Interface master");
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudMask()==5,"Persistence setup failed to choose crosshair and inventory independently");
    padPress(XINPUT_GAMEPAD_DPAD_UP);padPress(XINPUT_GAMEPAD_DPAD_UP);padPress(XINPUT_GAMEPAD_DPAD_UP);padPress(XINPUT_GAMEPAD_A);
    padPress(XINPUT_GAMEPAD_DPAD_DOWN);padPress(XINPUT_GAMEPAD_A);
    require(hudMask()==5&&hudAllHidden()&&!hudHint(),"Persistence setup failed to preserve individual choices under master or hide hint");
    require(fs::is_regular_file(root/L"ConvictionEnhanced-state/interface-preferences.ini"),"Interface changes did not create the preferences file");
    std::cout<<"PASS: real Interface actions saved nondefault choices for a separate-launch check.\n";
    std::cout<<"PASS: native D3D9 render, ten toggles, held-key ownership, hotkey, input queue clearing, mouse pass-through, viewport/Z/MRT/depth restoration, focus loss, three Resets, settings queue. Frames="<<countFrames()<<"\n";
    // Process-lifetime plugin: no live unhook/unload is offered or exercised.
    return 0;
 } catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
