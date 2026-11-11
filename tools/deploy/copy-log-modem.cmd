@echo off
rem Copies C:\XunModem.log next to this script (the flash drive).
copy /y C:\XunModem.log "%~dp0XunModem.log"
echo copied to %~dp0XunModem.log
pause
