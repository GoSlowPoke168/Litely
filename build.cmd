@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
if not exist build mkdir build
rc /nologo /fo build\litely.res litely.rc
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /O2 /MT /W4 /DUNICODE /D_UNICODE main.cpp /Fo:build\main.obj /Fe:build\Litely.exe /link build\litely.res /MANIFEST:EMBED /MANIFESTINPUT:app.manifest /SUBSYSTEM:WINDOWS user32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib comdlg32.lib gdi32.lib gdiplus.lib dwmapi.lib d3d11.lib dxgi.lib dcomp.lib mfplat.lib mfuuid.lib uuid.lib wtsapi32.lib advapi32.lib uxtheme.lib


