<#
.SYNOPSIS
Dual-OS slot switcher, PBRP RAM boot, and Option 1 partition installer for Redmi Pad SE (xun).

.DESCRIPTION
Option 1 Dual-OS slot layout:
  slot_a : Windows 11 ARM64 UEFI (boot_a = Mu-xun-v4.img)
  slot_b : LineageOS 16 + PBRP 4.0 (boot_b = Android kernel, recovery_b = pbrp-4.0-xun-recovery.img)

Usage:
  .\xun-boot.ps1 status
  .\xun-boot.ps1 windows
  .\xun-boot.ps1 android
  .\xun-boot.ps1 recovery
  .\xun-boot.ps1 pbrp
  .\xun-boot.ps1 flash-option1 -ConfirmFlash [-FlashFullSlotB] [-BackupDir ...] [-UefiImg ...] [-PbrpRecoveryImg ...]
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('status', 'windows', 'android', 'recovery', 'pbrp', 'flash-option1')]
    [string]$Action,

    [switch]$ConfirmFlash,
    [switch]$FlashFullSlotB,
    [string]$FastbootPath,
    [string]$XunRoot,
    [string]$BackupDir,
    [string]$UefiImg,
    [string]$PbrpRecoveryImg,
    [string]$PbrpBootImg
)

$ErrorActionPreference = 'Stop'
if (-not $XunRoot) {
    $XunRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}

function Resolve-Fastboot {
    if ($FastbootPath -and (Test-Path $FastbootPath)) { return (Resolve-Path $FastbootPath).Path }
    $cmd = Get-Command fastboot.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $local = Join-Path $XunRoot 'tools\platform-tools\fastboot.exe'
    if (Test-Path $local) { return $local }
    throw "fastboot.exe not found in PATH or '$local'"
}

function Invoke-Fastboot {
    param([string]$Fb, [string[]]$FbArgs, [switch]$Capture)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Fb @FbArgs 2>&1
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    $text = ($out | ForEach-Object { $_.ToString() }) -join "`n"
    if (-not $Capture -and $text) { Write-Host $text }
    if ($rc -ne 0) {
        throw "fastboot $($FbArgs -join ' ') failed (exit $rc):`n$text"
    }
    return $text
}

function Assert-BootloaderFastboot([string]$Fb) {
    $devs = Invoke-Fastboot -Fb $Fb -FbArgs @('devices') -Capture
    if ($devs -notmatch 'fastboot') {
        throw "No fastboot device detected. Boot tablet into fastboot (Vol- + Power)."
    }
    $us = Invoke-Fastboot -Fb $Fb -FbArgs @('getvar', 'is-userspace') -Capture
    if ($us -notmatch 'is-userspace:\s*no') {
        throw "Device is not in bootloader fastboot (fastbootd is unsupported for slot/UEFI ops)."
    }
}

$fb = Resolve-Fastboot

switch ($Action) {
    'status' {
        Invoke-Fastboot -Fb $fb -FbArgs @('devices') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('getvar', 'current-slot') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('getvar', 'slot-successful:a') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('getvar', 'slot-successful:b') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('getvar', 'is-userspace') | Out-Null
    }
    'windows' {
        Assert-BootloaderFastboot $fb
        Invoke-Fastboot -Fb $fb -FbArgs @('set_active', 'a') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('reboot') | Out-Null
    }
    'android' {
        Assert-BootloaderFastboot $fb
        Invoke-Fastboot -Fb $fb -FbArgs @('set_active', 'b') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('reboot') | Out-Null
    }
    'recovery' {
        Assert-BootloaderFastboot $fb
        Invoke-Fastboot -Fb $fb -FbArgs @('set_active', 'b') | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('reboot', 'recovery') | Out-Null
    }
    'pbrp' {
        Assert-BootloaderFastboot $fb
        if (-not $PbrpBootImg) {
            $PbrpBootImg = Join-Path $XunRoot 'recovery\pbrp_2025\pbrp_4.0_xun_FASTBOOT_BOOT.img'
        }
        if (-not (Test-Path $PbrpBootImg)) { throw "PBRP boot image not found: $PbrpBootImg" }
        Invoke-Fastboot -Fb $fb -FbArgs @('boot', $PbrpBootImg) | Out-Null
    }
    'flash-option1' {
        if (-not $ConfirmFlash) {
            throw "Refusing to flash without -ConfirmFlash. Review files first, then re-run with -ConfirmFlash."
        }
        Assert-BootloaderFastboot $fb
        if (-not $UefiImg) { $UefiImg = Join-Path $XunRoot 'Mu-xun-v4.img' }
        if (-not $BackupDir) { $BackupDir = Join-Path $XunRoot 'backup\xun_20261110_205416' }
        if (-not $PbrpRecoveryImg) {
            $PbrpRecoveryImg = Join-Path $XunRoot 'recovery\pbrp_2025\pbrp-4.0-xun-recovery.img'
        }
        $bootB = Join-Path $BackupDir 'boot_b.img'
        foreach ($f in @($UefiImg, $bootB, $PbrpRecoveryImg)) {
            if (-not (Test-Path $f)) { throw "Required image missing: $f" }
        }
        Invoke-Fastboot -Fb $fb -FbArgs @('flash', 'boot_a', $UefiImg) | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('flash', 'boot_b', $bootB) | Out-Null
        Invoke-Fastboot -Fb $fb -FbArgs @('flash', 'recovery_b', $PbrpRecoveryImg) | Out-Null
        if ($FlashFullSlotB) {
            foreach ($p in @('init_boot_b', 'vendor_boot_b', 'dtbo_b')) {
                $img = Join-Path $BackupDir "$p.img"
                if (-not (Test-Path $img)) { throw "Missing backup image: $img" }
                Invoke-Fastboot -Fb $fb -FbArgs @('flash', $p, $img) | Out-Null
            }
        }
        Write-Host "Option 1 images flashed: slot_a=Windows UEFI (Mu-xun-v4.img), slot_b=LineageOS 16 + PBRP 4.0."
        Write-Host "Next: '.\xun-boot.ps1 windows', '.\xun-boot.ps1 android', or '.\xun-boot.ps1 recovery'."
    }
}

