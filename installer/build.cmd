@echo off
rem Builds Litely, then build\installer\Litely-Setup.exe and Litely-portable.zip. Extra arguments go to ISCC (for example /DTest).
setlocal
call "%~dp0..\build.cmd" || exit /b 1
cd /d "%~dp0"
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
"%ISCC%" /Q %* litely.iss || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Force -Path ..\build\Litely.exe -DestinationPath ..\build\installer\Litely-portable.zip" || exit /b 1
