@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunBattery, step 1 (runs from the flash drive): ONLY copies the package to C: and puts
rem XunBattery-install.cmd on the desktop. Nothing is installed or removed here, so losing the
rem flash drive (the old driver drops the OTG boost that powers the hub) cannot break anything.
set STAGE=C:\xun\stage\XunBattery
if not exist "%STAGE%" mkdir "%STAGE%"
xcopy /y /q "%~dp0*" "%STAGE%\" >nul
if errorlevel 1 (
  echo COPY TO %STAGE% FAILED - nothing was changed.
  pause
  exit /b 1
)
if exist "%STAGE%\install-battery-local.cmd" (
  copy /y "%STAGE%\install-battery-local.cmd" "C:\Users\Public\Desktop\XunBattery-install.cmd" >nul
  set LOCAL_SCRIPT=%STAGE%\install-battery-local.cmd
) else (
  copy /y "%STAGE%\install-local.cmd" "C:\Users\Public\Desktop\XunBattery-install.cmd" >nul
  set LOCAL_SCRIPT=%STAGE%\install-local.cmd
)
echo Copied to %STAGE%:
dir /b "%STAGE%"
echo.
echo Desktop: XunBattery-install.cmd (installs from C:, works without the flash drive).
echo Starting it now...
start "XunBattery install" cmd /c ""%LOCAL_SCRIPT%" "%~dp0.""
exit /b 0
