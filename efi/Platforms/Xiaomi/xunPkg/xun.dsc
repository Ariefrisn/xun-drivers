##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = xun
  PLATFORM_GUID                  = 7C1E4B22-6D3A-4F58-9E21-2B8A5D0C4F63
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/xunPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = xunPkg/xun.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM6225
  # 1 = SM6225-AD
  #
  SOC_TYPE                       = 0  # SM6225, not SM6225-AD

!include DivarPkg/DivarPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x5FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi Pad SE"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"xun"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"23043RP34G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"xun"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1920

  #
  # The panel on this unit is mounted upside down, so every blt has to be
  # mirrored. Mu's GOP reports the native geometry and rotates in software.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferRotate|180
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

!if $(TARGET) == DEBUG
  # 30 ms per newline in DEBUG builds: readable without triggering WDT timeout
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|30000
!endif

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|xunPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|xunPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  xun/AcpiTables.inf
