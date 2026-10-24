@echo off
setlocal EnableExtensions
rem Use an existing x86 developer shell, or discover Visual Studio/Build Tools.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto tools_ready
set "MW_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MW_VSWHERE%" set "MW_VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MW_VSWHERE%" (
    echo Install Visual Studio Build Tools with Desktop development with C++ and a Windows SDK.
    exit /b 1
)
set "MW_VS_INSTALL="
for /f "usebackq delims=" %%I in (`"%MW_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MW_VS_INSTALL=%%I"
if not defined MW_VS_INSTALL (
    echo No Visual C++ x86/x64 tools installation was found.
    exit /b 1
)
call "%MW_VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
:tools_ready
where cl >nul 2>nul
if errorlevel 1 exit /b 1
where rc >nul 2>nul
if errorlevel 1 exit /b 1
set "MW_PRODUCTION_BUILT="
cd /d "%~dp0"
if not exist build mkdir build
pushd build
if /i "%~1"=="harness" goto harness
if /i "%~1"=="harness-only" goto harness
if /i "%~1"=="tests" goto tests
rc /nologo /fo version.res ..\version.rc
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /c /O2 /MT /W3 /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_DEFLATE_APIS /DMINIZ_NO_ZLIB_APIS /Fominiz_tinfl.obj ..\vendor\miniz\miniz_tinfl.c
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /c /O2 /MT /W3 /I"..\vendor\minhook\include" /I"..\vendor\minhook\src" ..\vendor\minhook\src\buffer.c ..\vendor\minhook\src\hook.c ..\vendor\minhook\src\trampoline.c ..\vendor\minhook\src\hde\hde32.c
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /I"..\vendor\imgui" /I"..\vendor\minhook\include" /c ..\overlay.cpp ..\controller_input.cpp ..\free_camera.cpp ..\hud_control.cpp ..\interface_preferences.cpp ..\controller_startup.cpp ..\practice_storage.cpp ..\checkpoint_control.cpp ..\checkpoint_archive.cpp ..\checkpoint_library.cpp ..\exploration_control.cpp ..\vendor\imgui\imgui.cpp ..\vendor\imgui\imgui_draw.cpp ..\vendor\imgui\imgui_tables.cpp ..\vendor\imgui\imgui_widgets.cpp ..\vendor\imgui\backends\imgui_impl_win32.cpp ..\vendor\imgui\backends\imgui_impl_dx9.cpp
if errorlevel 1 exit /b 1
link /nologo /DLL /MACHINE:X86 /DEF:..\overlay.def /OUT:ConvictionEnhanced.asi version.res overlay.obj controller_input.obj free_camera.obj hud_control.obj interface_preferences.obj controller_startup.obj practice_storage.obj checkpoint_control.obj checkpoint_archive.obj checkpoint_library.obj exploration_control.obj miniz_tinfl.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_win32.obj imgui_impl_dx9.obj buffer.obj hook.obj trampoline.obj hde32.obj d3d9.lib bcrypt.lib user32.lib gdi32.lib imm32.lib dwmapi.lib
if errorlevel 1 exit /b 1
set "MW_PRODUCTION_BUILT=1"
:tests
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /I"..\vendor\imgui" /I"..\vendor\minhook\include" /Fooverlay_test.obj /c ..\overlay.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /I"..\vendor\minhook\include" /Focontroller_input_test.obj /c ..\controller_input.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /I"..\vendor\minhook\include" /Fofree_camera_backend_test.obj /c ..\free_camera.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /I"..\vendor\minhook\include" /Fohud_control_backend_test.obj /c ..\hud_control.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /Fointerface_preferences_test_backend.obj /c ..\interface_preferences.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /Focontroller_startup_test_backend.obj /c ..\controller_startup.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /Fopractice_storage_test_backend.obj /c ..\practice_storage.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /I"..\vendor\minhook\include" /Focheckpoint_control_test_backend.obj /c ..\checkpoint_control.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /Focheckpoint_library_test_backend.obj /c ..\checkpoint_library.cpp
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 /DCE_TEST_HOST /I"..\vendor\minhook\include" /Foexploration_control_test_backend.obj /c ..\exploration_control.cpp
if errorlevel 1 exit /b 1
link /nologo /DLL /MACHINE:X86 /OUT:overlay_test.dll overlay_test.obj controller_input_test.obj free_camera_backend_test.obj hud_control_backend_test.obj interface_preferences_test_backend.obj controller_startup_test_backend.obj practice_storage_test_backend.obj checkpoint_control_test_backend.obj checkpoint_archive.obj checkpoint_library_test_backend.obj exploration_control_test_backend.obj miniz_tinfl.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_win32.obj imgui_impl_dx9.obj buffer.obj hook.obj trampoline.obj hde32.obj d3d9.lib bcrypt.lib user32.lib gdi32.lib imm32.lib dwmapi.lib
if errorlevel 1 exit /b 1
:harness
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W3 ..\harness.cpp /Fe:harness.exe /link d3d9.lib user32.lib
if errorlevel 1 exit /b 1
if /i "%~1"=="harness-only" goto complete
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\controller_state_test.cpp /Fe:controller_state_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\free_camera_math_test.cpp /Fe:free_camera_math_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\exploration_marker_math_test.cpp /Fe:exploration_marker_math_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\exploration_landing_math_test.cpp /Fe:exploration_landing_math_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\free_camera_test.cpp free_camera_backend_test.obj /Fe:free_camera_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\gameplay_context_test.cpp /Fe:gameplay_context_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\hud_control_test.cpp hud_control_backend_test.obj free_camera_backend_test.obj /Fe:hud_control_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\interface_preferences_test.cpp interface_preferences_test_backend.obj /Fe:interface_preferences_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\controller_startup_policy_test.cpp /Fe:controller_startup_policy_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\checkpoint_request_test.cpp /Fe:checkpoint_request_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\practice_storage_test.cpp practice_storage_test_backend.obj /Fe:practice_storage_test.exe /link bcrypt.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\checkpoint_archive_test.cpp checkpoint_archive.obj miniz_tinfl.obj /Fe:checkpoint_archive_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\checkpoint_library_test.cpp checkpoint_library_test_backend.obj checkpoint_archive.obj miniz_tinfl.obj /Fe:checkpoint_library_test.exe /link bcrypt.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\checkpoint_control_test.cpp checkpoint_control_test_backend.obj checkpoint_library_test_backend.obj checkpoint_archive.obj miniz_tinfl.obj practice_storage_test_backend.obj buffer.obj hook.obj trampoline.obj hde32.obj /Fe:checkpoint_control_test.exe /link bcrypt.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX ..\exploration_policy_test.cpp /Fe:exploration_policy_test.exe
if errorlevel 1 exit /b 1
cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX /DCE_TEST_HOST ..\exploration_control_test.cpp exploration_control_test_backend.obj /Fe:exploration_control_test.exe /link user32.lib
if errorlevel 1 exit /b 1
:complete
popd
if defined MW_PRODUCTION_BUILT (
    call :manifest
    if errorlevel 1 exit /b 1
)
echo Built requested native artifacts.
endlocal
exit /b 0

:manifest
if defined MW_MOD_PYTHON (
    "%MW_MOD_PYTHON%" ..\package.py --manifest-only
    if errorlevel 1 exit /b 1
    exit /b 0
)
where py >nul 2>nul
if not errorlevel 1 (
    py -3 ..\package.py --manifest-only
    if errorlevel 1 exit /b 1
    exit /b 0
)
where python >nul 2>nul
if not errorlevel 1 (
    python ..\package.py --manifest-only
    if errorlevel 1 exit /b 1
    exit /b 0
)
echo Python 3.10+ is required to write BUILD.json. Install Python or set MW_MOD_PYTHON to its executable.
exit /b 1
