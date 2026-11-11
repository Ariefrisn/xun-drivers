@echo off
rem Copies C:\XunWifi.log next to this script (the flash drive).
copy /y C:\XunWifi.log "%~dp0XunWifi.log"
echo copied to %~dp0XunWifi.log
pause
