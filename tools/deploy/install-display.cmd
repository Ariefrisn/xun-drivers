@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunDisplay installer: install the adapter, and only if it is running disable BasicDisplay
rem (both would scan out of the same framebuffer). Undo: uninstall-display.cmd
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install XunDisplay >> "%L%"
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\XunDisplay >> "%L%" 2>&1
devcon.exe install XunDisplay.inf Root\XunDisplay >> "%L%" 2>&1
echo devcon install exit %errorlevel% >> "%L%"
timeout /t 3 >nul
devcon.exe status Root\XunDisplay > "%TEMP%\tdstat.txt" 2>&1
type "%TEMP%\tdstat.txt" >> "%L%"
findstr /c:"Driver is running" "%TEMP%\tdstat.txt" >nul
if errorlevel 1 (
  echo XunDisplay is NOT running - BasicDisplay left enabled >> "%L%"
) else (
  rem hot "devcon disable" of the adapter DWM holds can hang PnP: mark it disabled in the registry, takes effect after a reboot
  reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000" /v ConfigFlags /t REG_DWORD /d 1 /f >> "%L%" 2>&1
  echo BasicDisplay marked disabled (ConfigFlags=1), reboot to apply >> "%L%"
  if exist "%~dp0xun-brightness.ps1" powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0xun-brightness.ps1" >> "%L%" 2>&1
)
copy /y C:\XunDisplay.log "%~dp0XunDisplay.log" >nul 2>&1
type "%L%"
echo.
echo Reboot to switch to XunDisplay. If the screen stays on the logo: ssh in and run  reg add ...Enum\ROOT\BASICDISPLAY\0000 /v ConfigFlags /t REG_DWORD /d 0 /f  + devcon enable ROOT\BasicDisplay, or Win+R %~dp0uninstall-display.cmd
pause
