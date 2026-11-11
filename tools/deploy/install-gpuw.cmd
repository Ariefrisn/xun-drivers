@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem XunGpuW (Adreno 610 WDDM KMD on ACPI\TPZG0610 + Mesa xungpu_d3d10.dll UMD) installer
cd /d "%~dp0"
set L=%~dp0install-gpuw.log
echo [%date% %time%] install XunGpuW > "%L%"
if not exist C:\xun\fw\gpu mkdir C:\xun\fw\gpu >> "%L%" 2>&1
if exist fw\gpu\* xcopy /y /q fw\gpu\* C:\xun\fw\gpu\ >> "%L%" 2>&1
if exist xungpu_d3d10.dll copy /y xungpu_d3d10.dll "%SystemRoot%\System32\xungpu_d3d10.dll" >> "%L%" 2>&1
if exist ..\xungpu-umd-arm64\xungpu_d3d10.dll copy /y ..\xungpu-umd-arm64\xungpu_d3d10.dll "%SystemRoot%\System32\xungpu_d3d10.dll" >> "%L%" 2>&1
if not exist C:\xun\umd.disable echo 1 > C:\xun\umd.enable
certutil -addstore -f Root xun-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher xun-woa-test.cer >> "%L%" 2>&1
pnputil /add-driver "%~dp0XunGpuW.inf" /install >> "%L%" 2>&1
echo pnputil exit %errorlevel% >> "%L%"
devcon.exe rescan >> "%L%" 2>&1
type "%L%"
echo.
echo Reboot to let DWM pick up XunGpuW + xungpu_d3d10.dll. Driver log: C:\XunGpuW.log
pause

