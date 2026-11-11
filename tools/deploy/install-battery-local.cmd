@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunBattery, step 2: installs ONLY from C:\xun\stage\XunBattery (copied there by the
rem flash drive's install.cmd). Safe to run again any time, also from the desktop copy.
rem Optional %1 = flash drive folder to copy the logs back to.
set STAGE=C:\xun\stage\XunBattery
set L=C:\xun\stage\install-battery.log
set "BACK=%~1"
cd /d "%STAGE%" || (echo %STAGE% missing & pause & exit /b 1)
if not exist devcon.exe (echo devcon.exe missing in %STAGE% & pause & exit /b 1)
if not exist XunBattery.inf (echo XunBattery.inf missing in %STAGE% & pause & exit /b 1)
echo [%date% %time%] install XunBattery from %STAGE% >> "%L%"
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\XunBattery >> "%L%" 2>&1
devcon.exe install XunBattery.inf Root\XunBattery >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
if "%BACK%"=="" goto :done
echo Waiting 30 s for the hub/flash drive, then copying logs to %BACK% ...
timeout /t 30 /nobreak >nul
rem the driver keeps its log open for writing: plain copy/Notepad get a sharing violation
powershell -NoProfile -Command "$s=[IO.File]::Open('C:\XunBattery.log','Open','Read','ReadWrite'); $d=[IO.File]::Create('%BACK%\XunBattery.log'); $s.CopyTo($d); $d.Close(); $s.Close()" && echo copied XunBattery.log || echo flash drive not back: logs stay in C:\XunBattery.log and %L%
copy /y "%L%" "%BACK%\install-battery.log" >nul 2>&1
:done
echo Driver log: C:\XunBattery.log (copy-log-battery.cmd on the flash drive copies it)
pause
