@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunWifi: Wi-Fi adapter for Windows (WiFiCx). It boots the modem on every Windows start, so the
rem XunModem device is removed first (never both). Run it right after a reboot (cold modem).
rem To keep the modem off: create C:\xun\modem.off. Log: C:\XunWifi.log (copy-log-wifi.cmd).
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install XunWifi >> "%L%"
if exist fw\image\modem.mdt (
  echo copying firmware to C:\xun\fw ...
  xcopy /e /i /y /q fw C:\xun\fw >> "%L%" 2>&1
)
if not exist C:\xun\fw\image\modem.mdt (
  echo C:\xun\fw\image\modem.mdt missing: run XunModem\install.cmd once first >> "%L%"
  type "%L%"
  pause
  exit /b 1
)
devcon.exe remove Root\XunModem >> "%L%" 2>&1
del /f /q C:\xun\wifi.boot >nul 2>&1
del /f /q C:\xun\modem.off >nul 2>&1
echo armed > C:\xun\modem.arm
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\XunWifi >> "%L%" 2>&1
devcon.exe install XunWifi.inf Root\XunWifi >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Wait 30 s before rebooting (boot guard). After a reboot Wi-Fi comes up by itself in ~30 s.
echo Then: Settings - Network - Wi-Fi, connect; afterwards run copy-log-wifi.cmd.
pause
