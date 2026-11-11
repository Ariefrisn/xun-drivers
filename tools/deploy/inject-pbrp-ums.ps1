<#
.SYNOPSIS
Offline injection of Xun Windows drivers, Mesa UMD (xungpu_d3d10.dll), firmware, and auto-install
hooks onto the tablet's Windows partition mounted via PBRP USB Mass Storage (or disk image/VHDX).

.EXAMPLE
.\inject-pbrp-ums.ps1 -WinDrive W: -BundleDir d:\path\to\XunBundle-arm64 -UmdDll d:\path\to\xungpu_d3d10.dll -EspDrive E:
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]:?$')]
    [string]$WinDrive,

    [Parameter(Mandatory = $true)]
    [string]$BundleDir,

    [string]$UmdDll,
    [string]$FwDir = 'd:\Project\XUN\backup\xun_20261110_205416\vendor_firmware\vendor_firmware',
    [string]$EspDrive,
    [switch]$CopyUnattend,
    [switch]$SkipDism
)

$ErrorActionPreference = 'Stop'
$W = $WinDrive.TrimEnd('\', ':') + ':'

if (-not (Test-Path "$W\Windows\System32\ntoskrnl.exe")) {
    throw "'$W\' does not look like a mounted offline Windows partition (missing Windows\System32\ntoskrnl.exe)."
}

$bundle = (Resolve-Path $BundleDir).Path
$driversSrc = if (Test-Path (Join-Path $bundle 'drivers')) { Join-Path $bundle 'drivers' } else { $bundle }
$xunDirs = Get-ChildItem $driversSrc -Directory -Filter 'Xun*' -ErrorAction SilentlyContinue
if (-not $xunDirs) {
    throw "No Xun* driver directories found under '$driversSrc'."
}

$targetXun = "$W\xun"
$targetDrv = "$targetXun\drivers"
$targetFwGpu = "$targetXun\fw\gpu"
$targetUmd = "$targetXun\umd"
New-Item -ItemType Directory -Force $targetDrv, $targetFwGpu, $targetUmd | Out-Null

Write-Host "[1/6] Staging $($xunDirs.Count) driver packages to $targetDrv ..."
foreach ($d in $xunDirs) {
    $dst = Join-Path $targetDrv $d.Name
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item "$($d.FullName)\*" $dst -Recurse -Force
}

Write-Host "[2/6] Staging Mesa UMD & umd.enable ..."
if (-not $UmdDll) {
    $candidates = @(
        (Join-Path $bundle 'umd\xungpu_d3d10.dll'),
        (Join-Path $bundle 'xungpu_d3d10.dll'),
        (Join-Path $driversSrc '..\xungpu-umd-arm64\xungpu_d3d10.dll')
    )
    $UmdDll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ($UmdDll -and (Test-Path $UmdDll)) {
    Copy-Item $UmdDll "$W\Windows\System32\xungpu_d3d10.dll" -Force
    Copy-Item $UmdDll "$targetUmd\xungpu_d3d10.dll" -Force
    Write-Host "  Copied xungpu_d3d10.dll -> $W\Windows\System32\ and $targetUmd\"
} else {
    Write-Warning "xungpu_d3d10.dll not found (-UmdDll not specified). WDDM display requires xungpu_d3d10.dll + C:\xun\umd.enable."
}
Set-Content -Path "$targetXun\umd.enable" -Value '1' -Encoding ascii
Remove-Item "$targetXun\installed.flag" -ErrorAction SilentlyContinue

Write-Host "[3/6] Staging GPU + Touch firmware from '$FwDir' ..."
if (Test-Path $FwDir) {
    foreach ($pat in @('a610_zap.*', 'a630_sqe.fw')) {
        Get-ChildItem $FwDir -File -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $targetFwGpu -Force
        }
    }
    foreach ($pat in @('focaltech_ts_fw*.bin', 'fs1815.fsm', 'sipa.bin')) {
        Get-ChildItem $FwDir -File -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName "$targetXun\fw\" -Force
        }
    }
} else {
    Write-Warning "Firmware directory '$FwDir' not found; copy a610_zap.* and a630_sqe.fw to $targetFwGpu manually."
}

Write-Host "[4/6] Staging auto-install script, GroupPolicy startup script, and SetupComplete ..."
$deployDir = $PSScriptRoot
Copy-Item (Join-Path $deployDir 'install.cmd') "$targetXun\install.cmd" -Force
New-Item -ItemType Directory -Force "$W\Windows\Setup\Scripts" | Out-Null
Copy-Item (Join-Path $deployDir 'SetupComplete.cmd') "$W\Windows\Setup\Scripts\SetupComplete.cmd" -Force
$gpDst = "$W\Windows\System32\GroupPolicy"
New-Item -ItemType Directory -Force $gpDst | Out-Null
Copy-Item (Join-Path $deployDir 'gp\*') $gpDst -Recurse -Force

if ($CopyUnattend) {
    $unattend = Join-Path (Split-Path -Parent $deployDir) 'unattend.xml'
    if (Test-Path $unattend) {
        New-Item -ItemType Directory -Force "$W\Windows\Panther\Unattend" | Out-Null
        Copy-Item $unattend "$W\Windows\Panther\Unattend\unattend.xml" -Force
        Write-Host "  Copied unattend.xml -> $W\Windows\Panther\Unattend\unattend.xml"
    }
}

if (-not $SkipDism) {
    Write-Host "[5/6] Injecting all INFs into offline Driver Store via DISM ..."
    & dism.exe /Image:"$W\" /Add-Driver /Driver:"$targetDrv" /Recurse /ForceUnsigned
    if ($LASTEXITCODE) { throw "DISM Add-Driver failed with exit code $LASTEXITCODE" }
} else {
    Write-Host "[5/6] Skipped DISM (-SkipDism)."
}

Write-Host "[6/6] Checking ESP BCD testsigning ..."
if ($EspDrive) {
    $E = $EspDrive.TrimEnd('\', ':') + ':'
    $bcd = "$E\EFI\Microsoft\Boot\BCD"
    if (Test-Path $bcd) {
        & bcdedit.exe /store $bcd /set '{default}' testsigning on
        if ($LASTEXITCODE) { Write-Warning "bcdedit testsigning returned $LASTEXITCODE" }
    } else {
        Write-Warning "BCD not found at $bcd"
    }
} else {
    Write-Host "  Pass -EspDrive <drive>: if you also want this script to enforce 'testsigning on' in ESP BCD."
}

Write-Host ""
Write-Host "Offline injection complete on $W\. Next:"
Write-Host "  1. Safely eject UMS drive(s) on Windows host and reboot tablet to Windows (Slot A)."
Write-Host "  2. XunGpuW (ACPI\TPZG0610) loads on first boot via DISM Driver Store (1200x1920@90Hz, upright + rotation)."
Write-Host "  3. C:\xun\install.cmd installs Root\XunTouch + Root\Xun* on Startup/SetupComplete (or press Shift+F10 in OOBE and run C:\xun\install.cmd)."

