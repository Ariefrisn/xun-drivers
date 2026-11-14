@echo off
setlocal enabledelayedexpansion
set L=C:\xun\install.log
if exist C:\xun\installed.flag exit /b 0
if not exist C:\xun mkdir C:\xun
set _GOT_LOCK=0
for /L %%w in (1,1,45) do (
    if exist C:\xun\installed.flag exit /b 0
    mkdir C:\xun\install.lock 2>nul && (set _GOT_LOCK=1 & goto :xun_got_lock)
    ping -n 2 127.0.0.1 >nul
)
rmdir /s /q C:\xun\install.lock 2>nul
mkdir C:\xun\install.lock 2>nul && set _GOT_LOCK=1
:xun_got_lock
if exist C:\xun\installed.flag (
    if !_GOT_LOCK! equ 1 rmdir /s /q C:\xun\install.lock 2>nul
    exit /b 0
)
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
  "%DEVCON%" find Root\XunTouch 2>nul | findstr /i "ROOT\\XunTouch" >nul
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
    "%DEVCON%" find Root\XunDisplay 2>nul | findstr /i "ROOT\\XunDisplay" >nul
    if errorlevel 1 (
      "%DEVCON%" install "%DRV%\XunDisplay\XunDisplay.inf" Root\XunDisplay >> "%L%" 2>&1
      echo XunDisplay install exit !errorlevel! >> "%L%"
    )
    "%DEVCON%" status Root\XunDisplay 2>&1 | findstr /c:"Driver is running" >nul && (
      reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000" /v ConfigFlags /t REG_DWORD /d 1 /f >> "%L%" 2>&1
    )
  )
)

rem Remaining root-enumerated core drivers (never install XunModem together with XunWifi)
for %%d in (XunButtons XunBacklight XunBattery XunCpu XunRpm) do (
  if exist "%DRV%\%%d\%%d.inf" (
    "%DEVCON%" find Root\%%d 2>nul | findstr /i "ROOT\\%%d" >nul
    if errorlevel 1 (
      "%DEVCON%" install "%DRV%\%%d\%%d.inf" Root\%%d >> "%L%" 2>&1
      echo %%d install exit !errorlevel! >> "%L%"
    ) else (
      if /i "%%d"=="XunRpm" (
        "%DEVCON%" update "%DRV%\%%d\%%d.inf" Root\%%d >> "%L%" 2>&1
        echo %%d update exit !errorlevel! >> "%L%"
      )
    )
  )
)

if exist "C:\xun\fw\image\modem.mdt" if exist "%DRV%\XunWifi\XunWifi.inf" (
  "%DEVCON%" find Root\XunWifi 2>nul | findstr /i "ROOT\\XunWifi" >nul
  if errorlevel 1 (
    "%DEVCON%" install "%DRV%\XunWifi\XunWifi.inf" Root\XunWifi >> "%L%" 2>&1
    echo XunWifi install exit !errorlevel! >> "%L%"
  )
)

if exist "%~dp0bt_fw" xcopy /e /i /y /q "%~dp0bt_fw" C:\xun\fw\bt >> "%L%" 2>&1
if exist "%DRV%\XunBluetooth\bt_fw" xcopy /e /i /y /q "%DRV%\XunBluetooth\bt_fw" C:\xun\fw\bt >> "%L%" 2>&1
if exist "%~dp0fw\bt" xcopy /e /i /y /q "%~dp0fw\bt" C:\xun\fw\bt >> "%L%" 2>&1

for %%d in (XunAudio XunSpeaker XunBluetooth) do (
  if exist "%DRV%\%%d\%%d.inf" (
    "%DEVCON%" find Root\%%d 2>nul | findstr /i "ROOT\\%%d" >nul
    if errorlevel 1 (
      "%DEVCON%" install "%DRV%\%%d\%%d.inf" Root\%%d >> "%L%" 2>&1
      echo %%d install exit !errorlevel! >> "%L%"
    ) else (
      "%DEVCON%" update "%DRV%\%%d\%%d.inf" Root\%%d >> "%L%" 2>&1
      echo %%d update exit !errorlevel! >> "%L%"
    )
  )
)

reg add "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler" /v EnablePreemption /t REG_DWORD /d 0 /f >> "%L%" 2>&1

powershell -NoProfile -Command "Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\SYSTEM' -ErrorAction SilentlyContinue | ForEach-Object { $h = (Get-ItemProperty $_.PSPath -Name HardwareID -ErrorAction SilentlyContinue).HardwareID; if ($h -and $h[0] -like 'ROOT\Xun*') { [pscustomobject]@{ Name=$_.PSChildName; Hw=$h[0] } } } | Group-Object Hw | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Group | Sort-Object Name | Select-Object -Skip 1 | ForEach-Object { & pnputil /remove-device ('ROOT\SYSTEM\' + $_.Name) } }" >> "%L%" 2>&1

powercfg /change monitor-timeout-ac 0 >> "%L%" 2>&1
powercfg /change monitor-timeout-dc 0 >> "%L%" 2>&1
powercfg /change standby-timeout-ac 0 >> "%L%" 2>&1
powercfg /change standby-timeout-dc 0 >> "%L%" 2>&1

echo ok > C:\xun\installed.flag
if !_GOT_LOCK! equ 1 rmdir /s /q C:\xun\install.lock 2>nul
echo [%date% %time%] xun auto-install done >> "%L%"
exit /b 0
