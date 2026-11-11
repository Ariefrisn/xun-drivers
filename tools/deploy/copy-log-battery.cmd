@echo off
rem Copies C:\XunBattery.log (+ the install log) next to this script. The driver keeps the log
rem open for writing, so Notepad/copy get a sharing violation; .NET with ReadWrite sharing works.
powershell -NoProfile -Command "$s=[IO.File]::Open('C:\XunBattery.log','Open','Read','ReadWrite'); $d=[IO.File]::Create('%~dp0XunBattery.log'); $s.CopyTo($d); $d.Close(); $s.Close()" && echo copied to %~dp0XunBattery.log
copy /y C:\xun\stage\install-battery.log "%~dp0install-battery.log" >nul 2>&1
pause
