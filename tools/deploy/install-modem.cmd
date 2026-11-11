@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunModem: copies the modem firmware (fw\ next to this script) to C:\xun\fw, arms one
rem modem boot (C:\xun\modem.arm, the driver deletes it when it starts) and installs the driver.
rem The modem must be cold: boot Windows WITHOUT running "Modem test" in the UEFI menu.
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install XunModem >> "%L%"
if exist fw\image\modem.mdt (
  echo copying firmware to C:\xun\fw ...
  xcopy /e /i /y /q fw C:\xun\fw >> "%L%" 2>&1
) else (
  echo no fw\ next to the script, keeping C:\xun\fw >> "%L%"
)
if not exist C:\xun\fw\image\modem.mdt (
  echo C:\xun\fw\image\modem.mdt missing! >> "%L%"
  type "%L%"
  pause
  exit /b 1
)
echo armed > C:\xun\modem.arm
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\XunModem >> "%L%" 2>&1
devcon.exe install XunModem.inf Root\XunModem >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Wait ~2 minutes, then copy C:\XunModem.log to the flash drive (copy-log-modem.cmd).
pause
