@echo off
rem Builds Litely, then build\installer\Litely-Setup.exe and Litely-portable.zip. Extra arguments go to ISCC (for example /DTest).
setlocal
call "%~dp0..\build.cmd" || exit /b 1
cd /d "%~dp0"
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
"%ISCC%" /Q %* litely.iss || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Force -Path ..\build\Litely.exe,..\LICENSE -DestinationPath ..\build\installer\Litely-portable.zip" || exit /b 1
rem The sample videos and their credits as a separate download, when build\samples has any.
if exist "..\build\samples\*.mp4" powershell -NoProfile -Command "Compress-Archive -Force -Path ..\build\samples\*.mp4,..\build\samples\README.txt -DestinationPath ..\build\installer\Litely-samples.zip" || exit /b 1
