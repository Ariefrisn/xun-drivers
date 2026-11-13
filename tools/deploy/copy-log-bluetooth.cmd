@echo off
rem Copies C:\XunBluetooth.log next to this script.
copy /y C:\XunBluetooth.log "%~dp0XunBluetooth.log"
echo copied to %~dp0XunBluetooth.log
pause
