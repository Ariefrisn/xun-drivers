@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunTouch (FT8203 QUP0 SE0 SPI) installer + optional PRAM firmware staging
cd /d "%~dp0"
set L=%~dp0install-touch.log
echo [%date% %time%] install XunTouch > "%L%"
if not exist C:\xun\fw mkdir C:\xun\fw >> "%L%" 2>&1
if exist fw\focaltech_ts_fw*.bin (
  xcopy /y /q fw\focaltech_ts_fw*.bin C:\xun\fw\ >> "%L%" 2>&1
) else if exist focaltech_ts_fw*.bin (
  xcopy /y /q focaltech_ts_fw*.bin C:\xun\fw\ >> "%L%" 2>&1
)
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\XunTouch >> "%L%" 2>&1
devcon.exe install XunTouch.inf Root\XunTouch >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
devcon.exe status Root\XunTouch >> "%L%" 2>&1
type "%L%"
echo.
echo Driver log: C:\XunTouch.log
pause

