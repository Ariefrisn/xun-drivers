@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunBluetooth: WCN3990/WCN3950 Bluetooth LowerFilter under inbox BthMini.sys.
rem Requires BT firmware blobs (CRBTFW21.TLV / CRNV21.BIN etc.) in C:\xun\fw\bt\ and XunRpm v0.10+.
rem Log: C:\XunBluetooth.log (copy-log-bluetooth.cmd).
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install XunBluetooth >> "%L%"
if exist bt_fw (
  echo copying BT firmware to C:\xun\fw\bt ...
  xcopy /e /i /y /q bt_fw C:\xun\fw\bt >> "%L%" 2>&1
)
if exist fw\bt (
  echo copying BT firmware to C:\xun\fw\bt ...
  xcopy /e /i /y /q fw\bt C:\xun\fw\bt >> "%L%" 2>&1
)
if not exist C:\xun\fw\bt\CRBTFW21.TLV if not exist C:\xun\fw\bt\CMBTFW13.TLV (
  echo WARN: C:\xun\fw\bt\ firmware blobs missing! Copy bt_fw\* to C:\xun\fw\bt\ >> "%L%"
)
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
if exist ..\XunRpm\XunRpm.inf (
  echo updating XunRpm v0.10+ for PM6125 BT L9/L16/L17/L23 rails...
  devcon.exe update ..\XunRpm\XunRpm.inf Root\XunRpm >> "%L%" 2>&1
)
devcon.exe remove Root\XunBluetooth >> "%L%" 2>&1
devcon.exe install XunBluetooth.inf Root\XunBluetooth >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Check Device Manager - Bluetooth, and C:\XunBluetooth.log (or run copy-log-bluetooth.cmd).
echo If BT LDO rails weren't on yet before XunRpm update, reboot once and check C:\XunBluetooth.log.
pause
