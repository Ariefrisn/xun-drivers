# EDK2 UEFI & ACPI Source for Xiaomi Redmi Pad SE (xun)

This directory contains the EDK2 UEFI platform definitions, Qualcomm SM6225 (Divar) silicon glue, memory map headers, and ACPI source tables (`.dsl`/`.asl`) used to build Windows on ARM compatible UEFI firmware for **Xiaomi Redmi Pad SE (`xun`)**.

## Directory Structure

```
efi/
├── ACPI/
│   ├── Common/
│   │   └── SSDT.asl
│   ├── Platforms/Xiaomi/xun/
│   │   ├── AcpiTables.inf
│   │   └── DSDT.dsl                    # Patched DSDT (Adreno 610, SMMU, MDP, Framebuffer apertures)
│   └── Silicon/Qualcomm/Divar/
│       ├── APIC.dsl
│       ├── DSDT_Minimal.asl
│       ├── FACP.dsl
│       ├── GTDT.dsl
│       └── PPTT.dsl
├── Platforms/Xiaomi/xunPkg/            # xun platform DSC/FDF and memory configuration
│   ├── DeviceBuild.py
│   ├── Include/
│   │   ├── APRIORI.inc
│   │   ├── DXE.inc
│   │   ├── RAW.inc
│   │   └── SimpleFbRotate.md
│   ├── Library/
│   │   ├── ConfigurationMapLib/
│   │   └── MemoryMapLib/
│   ├── xun.dec
│   ├── xun.dsc
│   └── xun.fdf
├── Silicon/
│   ├── Qualcomm/DivarPkg/              # Qualcomm SM6225 silicon support
│   │   ├── DivarPkg.dsc.inc
│   │   └── Library/PlatformSecLib/
│   └── Silicium/SiliciumPkg/
│       └── Include/Library/            # MemoryMapLib.h, MemoryMapHelperLib.h
└── scripts/
    ├── build_uefi.py                   # Python cross-platform UEFI builder
    ├── build_active_devices.sh
    ├── pack_v4_uefi.py                 # Repacks Mu boot-v1 into Android header-v4 for xun ABL
    └── setup_env.sh
```

## Compilation

1. **ACPI Tables:**
   ```bash
   iasl -ve efi/ACPI/Platforms/Xiaomi/xun/DSDT.dsl
   ```

2. **UEFI Firmware Build:**
   ```bash
   python efi/scripts/build_uefi.py -d xun -r RELEASE
   ```

3. **Android Boot Header v4 Repack:**
   ```bash
   python efi/scripts/pack_v4_uefi.py Mu-xun.img Mu-xun-v4.img
   ```
