@echo off
setlocal enabledelayedexpansion
set L=C:\xun\install.log
if exist C:\xun\installed.flag exit /b 0
if not exist C:\xun mkdir C:\xun
echo [%date% %time%] xun auto-install start >> "%L%"

set DRV=C:\xun\drivers
set DEVCON=
for %%p in ("%DRV%\XunTouch\devcon.exe" "%DRV%\XunGpuW\devcon.exe" "%DRV%\XunDisplay\devcon.exe" "C:\xun\devcon.exe") do (
  if exist "%%~p" if not defined DEVCON set DEVCON=%%~p
)
if not defined DEVCON (
  echo devcon.exe not found under %DRV% >> "%L%"
  exit /b 1
)

for %%c in ("%DRV%\XunTouch\xun-woa-test.cer" "%DRV%\XunGpuW\xun-woa-test.cer" "%DRV%\XunDisplay\xun-woa-test.cer" "C:\xun\xun-woa-test.cer") do (
  if exist "%%~c" (
    certutil -addstore -f Root "%%~c" >> "%L%" 2>&1
    certutil -addstore -f TrustedPublisher "%%~c" >> "%L%" 2>&1
    goto :cert_done
  )
)
:cert_done

if exist "C:\xun\umd\xungpu_d3d10.dll" (
  copy /y "C:\xun\umd\xungpu_d3d10.dll" "%SystemRoot%\System32\xungpu_d3d10.dll" >> "%L%" 2>&1
)
if not exist C:\xun\umd.disable (
  echo 1 > C:\xun\umd.enable
)

rem Ensure Root\XunTouch is installed first (OOBE/Setup touch blocker fix)
if exist "%DRV%\XunTouch\XunTouch.inf" (
  "%DEVCON%" status Root\XunTouch >nul 2>&1
  if errorlevel 1 (
    "%DEVCON%" install "%DRV%\XunTouch\XunTouch.inf" Root\XunTouch >> "%L%" 2>&1
    echo XunTouch install exit !errorlevel! >> "%L%"
  )
)

rem Check whether UEFI exposes ACPI\TPZG0610 (GPU0 + display)
set HAS_GPU0=0
"%DEVCON%" find ACPI\TPZG0610 2>nul | findstr /i "TPZG0610" >nul && set HAS_GPU0=1
if "!HAS_GPU0!"=="1" (
  if exist "%DRV%\XunGpuW\XunGpuW.inf" (
    pnputil /add-driver "%DRV%\XunGpuW\XunGpuW.inf" /install >> "%L%" 2>&1
    echo XunGpuW pnputil exit !errorlevel! >> "%L%"
  )
) else (
  if exist "%DRV%\XunDisplay\XunDisplay.inf" (
    "%DEVCON%" status Root\XunDisplay >nul 2>&1 || "%DEVCON%" install "%DRV%\XunDisplay\XunDisplay.inf" Root\XunDisplay >> "%L%" 2>&1
    "%DEVCON%" status Root\XunDisplay 2>&1 | findstr /c:"Driver is running" >nul && (
      reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000" /v ConfigFlags /t REG_DWORD /d 1 /f >> "%L%" 2>&1
    )
  )
)

rem Remaining root-enumerated core drivers (never install XunModem together with XunWifi)
for %%d in (XunButtons XunBacklight XunBattery XunCpu XunRpm) do (
  if exist "%DRV%\%%d\%%d.inf" (
    "%DEVCON%" status Root\%%d >nul 2>&1
    if errorlevel 1 (
      "%DEVCON%" install "%DRV%\%%d\%%d.inf" Root\%%d >> "%L%" 2>&1
      echo %%d install exit !errorlevel! >> "%L%"
    )
  )
)

if exist "C:\xun\fw\image\modem.mdt" if exist "%DRV%\XunWifi\XunWifi.inf" (
  "%DEVCON%" status Root\XunWifi >nul 2>&1 || "%DEVCON%" install "%DRV%\XunWifi\XunWifi.inf" Root\XunWifi >> "%L%" 2>&1
)

for %%d in (XunAudio XunSpeaker) do (
  if exist "%DRV%\%%d\%%d.inf" (
    "%DEVCON%" status Root\%%d >nul 2>&1 || "%DEVCON%" install "%DRV%\%%d\%%d.inf" Root\%%d >> "%L%" 2>&1
  )
)

echo ok > C:\xun\installed.flag
echo [%date% %time%] xun auto-install done >> "%L%"
exit /b 0
