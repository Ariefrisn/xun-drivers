@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem Stages built drivers, Mesa UMD, and firmware from this bundle to C:\xun and runs C:\xun\install.cmd.
cd /d "%~dp0"
if not exist C:\xun\drivers mkdir C:\xun\drivers
if not exist C:\xun\fw\gpu mkdir C:\xun\fw\gpu
if not exist C:\xun\umd mkdir C:\xun\umd

if exist drivers (
  xcopy /e /i /y /q drivers C:\xun\drivers >nul
) else (
  for /d %%d in (Xun*) do xcopy /e /i /y /q "%%d" "C:\xun\drivers\%%d" >nul
)
if exist fw xcopy /e /i /y /q fw C:\xun\fw >nul
if exist umd\xungpu_d3d10.dll copy /y umd\xungpu_d3d10.dll C:\xun\umd\xungpu_d3d10.dll >nul
if exist xungpu_d3d10.dll copy /y xungpu_d3d10.dll C:\xun\umd\xungpu_d3d10.dll >nul
copy /y "%~dp0install.cmd" C:\xun\install.cmd >nul 2>&1
del /f /q C:\xun\installed.flag >nul 2>&1
call C:\xun\install.cmd
type C:\xun\install.log
echo.
echo Done. Reboot for display/GPU changes to take effect.
pause

