# xun-drivers

Windows 11 on ARM64 driver stack and Mesa UMD for **Xiaomi Redmi Pad SE** (`xun`, Qualcomm SM6225 *khaje*, Wi-Fi only `apq`).

> **Important:** Build the `.sys` drivers and `xungpu_d3d10.dll` via GitHub Actions (`.github/workflows/build.yml` and `.github/workflows/mesa.yml`), unless WDK `10.0.26100.0` + MSBuild ARM64 + Clang-CL/Meson are installed locally.

## Verified Hardware Mapping (`xun`)

| Subsystem | Driver | Bus / Hardware | Key Details |
|---|---|---|---|
| **GPU + Display (WDDM 2.x)** | `XunGpuW` + `mesa-overlay` (`xungpu_d3d10.dll`) | `ACPI\TPZG0610` (Adreno 610 + MDSS) | `1200x1920@90Hz` (`m84_42_03_0c`: h `50/20/36`, v `410/6/10`), HW 180° flip (`SSPP_SRC_FORMAT +0x30 \|= 0xC000`), `C:\xun\disp.noflip` override, FW `C:\xun\fw\gpu\a610_zap.{mdt,b02}` + `a630_sqe.fw`, UMD opt-in `C:\xun\umd.enable` |
| **Fallback Display (KMDOD)** | `XunDisplay` | `Root\XunDisplay` (yields to `ACPI\TPZG0610`) | `1200x1920@90Hz` + `60Hz` EDID (`"XUN"`), SW 180° compose (`C:\xun\disp.noflip` override), `0°/90°/180°/270°` rotation unlocked |
| **Backlight** | `XunBacklight` (+ integrated in `XunDisplay`/`XunGpuW`) | `KTZ8866@0x11` on `QUP0 SE1` I2C | `HWEN GPIO 102 SOUTH`, `C:\xun\bl.dcs` DSI0 DCS fallback, serialized via `xun_qup_lock.h` |
| **Touchscreen** | `XunTouch` | FocalTech `FT8203` on `QUP0 SE0` SPI (`0x04A80000`) | `GPIO 0..3 WEST func 1`, `6.4 MHz`, `GCC_QUP0_S0_CBCR=0x1F144` / `S0_RCG=0x1F148` `BIT(10)`, reg `0x01` 62-byte SPI v2 + CRC16 (`0x8408`), `0..11999 x 0..19199` (15-bit) + 12-bit fallback, 180° flip (`C:\xun\touch.noflip`), optional PRAM upload from `C:\xun\fw\focaltech_ts_fw*.bin` |
| **Battery / PD / Gauge** | `XunBattery` | `SC89890H@0x6A` + `SC2150@0x4E` (`SE1`) & `SH366101@0x55` (`SE2`) | `SE2 GPIO 6/7 WEST` (`0x1F3A4`/`0x1F3A8` `BIT(12)`), `8000 mAh`, per-transaction `xun_qup_lock.h` locking |
| **Buttons & Cover** | `XunButtons` | `TLMM GPIO 66 WEST` (`0x00542000`) + key events | Active-low Hall lid/sleep switch (`System Sleep 0x82`), `C:\xun\hall.off` override |
| **Audio / Speaker Amps** | `XunAudio` + `XunSpeaker` | `QUP0 SE1` & `SE2` I2C (`0x34, 0x35, 0x28, 0x29, 0x2B, 0x59`) | Reset `GPIO 106 SOUTH` + `GPIO 70 WEST`, runtime `sipa`/`fs16xx` detection, serialized via `xun_qup_lock.h` |
| **Other Platform** | `XunCpu`, `XunRpm`, `XunWifi`, `XunModem`, `XunGpu` | SoC clocks / RPM GLINK / WCN3990 | `XunWifi` and `XunModem` are mutually exclusive (never install both) |

## IPC / Shared Constants
- `XunGpuW/xungpu_escape.h` and `mesa-overlay/files/src/freedreno/drm/wddm/xungpu_escape.h` share `XUNGPU_ESC_MAGIC = 0x55504758u` (`'XGPU'`) and `XUNGPU_ALLOC_MAGIC = 0x41475058u` (`'XPGA'`).
- Multi-driver `QUP0 SE1` / `SE2` I2C transactions synchronize via named kernel mutants (`\BaseNamedObjects\XunQup0Se1Lock` and `\BaseNamedObjects\XunQup0Se2Lock` in `drivers/common/xun_qup_lock.h`).

## Option 1 Dual-OS Slot Layout

| Slot | OS | `boot_*` | `recovery_*` |
|---|---|---|---|
| **Slot A (`a`)** | **Windows 11 ARM64** | `Mu-xun-v4.img` (`boot_a`) | — |
| **Slot B (`b`)** | **LineageOS 16 + PBRP 4.0** | `boot_b.img` (`OS=16.0.0`) | `pbrp-4.0-xun-recovery.img` (`ksz=0`, ramdisk-only) |

> **Why `recovery_b` needs `boot_b` to be Android:** `pbrp-4.0-xun-recovery.img` has `kernel_size = 0`, so ABL loads the kernel from `boot_<active_slot>`. If `boot_b` contains `Mu-xun-v4.img` while `current-slot = b`, both normal boot and `Vol+ + Power` launch Windows UEFI instead of PBRP.

Use `tools/xun-boot.ps1` (Windows host) or `tools/xun-boot.sh` (Linux/macOS):
```powershell
.\tools\xun-boot.ps1 status
.\tools\xun-boot.ps1 pbrp       # RAM-boot PBRP from bootloader fastboot (no flash)
.\tools\xun-boot.ps1 windows    # Switch active slot -> a (Windows) and reboot
.\tools\xun-boot.ps1 android    # Switch active slot -> b (LineageOS / Vol+ PBRP) and reboot
```

## Build (GitHub Actions)

1. Push `xun-drivers/` to a GitHub repository.
2. Run **Build drivers** (`.github/workflows/build.yml`) -> download `XunBundle-arm64` (includes all 13 signed `out/Xun*` packages, `devcon.exe`, `xun-woa-test.cer`, `tools/deploy/*`, `tgputest`, `taudio`).
3. Run **Build Mesa UMD** (`.github/workflows/mesa.yml`) -> download `xungpu-umd-arm64` (`xungpu_d3d10.dll`).

## Offline Driver Injection via PBRP UMS (`tools/deploy/inject-pbrp-ums.ps1`)

1. Boot PBRP transiently from bootloader fastboot:
   ```powershell
   D:\Project\XUN\tools\platform-tools\fastboot.exe boot D:\Project\XUN\recovery\pbrp_2025\pbrp_4.0_xun_FASTBOOT_BOOT.img
   ```
2. Expose the `win` partition (64 GiB NTFS) as USB Mass Storage:
   ```powershell
   adb push tools\deploy\pbrp_ums_enable.sh /tmp/
   adb shell sh /tmp/pbrp_ums_enable.sh win
   ```
3. Run offline injection on the mounted Windows drive (e.g. `W:`):
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\deploy\inject-pbrp-ums.ps1 `
     -WinDrive W: `
     -BundleDir C:\path\to\XunBundle-arm64 `
     -UmdDll C:\path\to\xungpu-umd-arm64\xungpu_d3d10.dll
   ```
   Optionally expose `esp` (`sh /tmp/pbrp_ums_enable.sh esp`) and pass `-EspDrive E:` to ensure `testsigning on` in BCD.
4. Safely eject the UMS drive(s), run `adb shell sh /tmp/pbrp_ums_enable.sh off`, and reboot to Slot A (`Windows`).
   - `XunGpuW` (`ACPI\TPZG0610`) + `xungpu_d3d10.dll` are active on boot (landscape + 180° upright + Adreno 610 DWM).
   - `C:\xun\install.cmd` is staged in `SetupComplete.cmd` and local `GroupPolicy\Machine\Scripts\Startup\scripts.ini` (and `tools\unattend.xml` `specialize`) to install `Root\XunTouch` and remaining `Root\Xun*` drivers automatically (or run `C:\xun\install.cmd` directly from `Shift+F10` in OOBE).
