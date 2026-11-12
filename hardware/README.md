# Hardware Reference & Device Tree Sources for Xiaomi Redmi Pad SE (xun)

This directory contains the decompiled Device Tree Sources (`.dts`) extracted from the stock Xiaomi firmware for **Redmi Pad SE (`xun`, SM6225)**, as well as a technical reference list of proprietary firmware blobs.

## Directory Structure

```
hardware/
├── dts/
│   ├── xun.dts             # Full resolved Flattened Device Tree (/sys/firmware/fdt)
│   ├── xun_dtbo_45.dts     # DTBO overlay table entry 45 (khaje-idp-nopmi-xun)
│   └── xun_dtbo_46.dts     # DTBO overlay table entry 46 (khaje-idp-nopmi-xun)
└── firmware-list.txt       # Technical audit of vendor blobs & C:\xun staging paths
```

## Key Hardware Topology

- **SoC:** Qualcomm Snapdragon 680 (SM6225 / Khaje / Bengal derivative)
- **CPU:** 4x Cortex-A73 @ 2.4 GHz (Gold) + 4x Cortex-A53 @ 1.9 GHz (Silver)
- **GPU:** Adreno 610 (A610)
- **Display Controller:** MDP5 (0x05E00000), Framebuffer base: `0x5C000000` (15 MB, 1200x1920 60Hz)
- **Backlight:** Kinetic KTZ8866 I2C (`0x11`) via QUP0 SE1 (`0x04A84000`) + HWEN GPIO 102
- **Touchscreen:** FocalTech FT8203 SPI via QUP0 SE0 (`0x04A80000`), IRQ GPIO 80, RST GPIO 86
- **Power / Buttons:** PM6125 PON (Power & Vol-), TLMM GPIO 5 (Vol+)
- **Audio DSP:** Hexagon v66 LPASS ADSP PIL boot via `adsp.mdt`

