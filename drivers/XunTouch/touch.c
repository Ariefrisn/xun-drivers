/*
 * FocalTech FT8203 SPI touch controller -> Windows multi-touch digitizer via
 * VHF.
 *
 * Verified from Redmi Pad SE (xun) dtbo_b.img & focaltech_ts_spi disassembly:
 *   - Bus: QUP0 SE0 SPI (0x04A80000), GPIO 0..3 WEST, IRQ 80 WEST, RST 86 WEST
 *   - Read from reg 0x01 (62 bytes): buf[0]=gest, buf[1]=td_status (0xFF/0xEF
 * idle, [3:0]=count), buf[2 + 6*i] = per-finger 6 bytes.
 *   - Mode (td >> 4) == 0 uses 15-bit coords (0..11999 x 0..19199 = 0x2EDF x
 * 0x4AFF); mode 2/8 uses 12-bit coords (x10 -> 0..11999 x 0..19199).
 *   - Physical mount is 180 deg inverted relative to panel top-left; inverted
 * here by default unless C:\xun\touch.noflip exists.
 *   - If reset leaves chip in ROMBOOT (0x55 -> 0x90), loads
 * C:\xun\fw\focaltech_ts_fw*.bin via PRAM.
 */
#include "driver.h"

#define REPORTID_TOUCH 0x01
#define REPORTID_MAXCNT 0x02
#define LE16(v) (UCHAR)((v) & 0xFF), (UCHAR)(((v) >> 8) & 0xFF)

/* Physical size of the 11.0" 1200x1920 panel in 0.01 cm units (14.8 x 23.7 cm)
 */
#define PHYS_X 1480
#define PHYS_Y 2370

#define FINGER_COLLECTION(maxx, maxy)                                          \
  0x05, 0x0D,     /*   Usage Page (Digitizer)            */                    \
      0x09, 0x22, /*   Usage (Finger)                    */                    \
      0xA1, 0x02, /*   Collection (Logical)              */                    \
      0x09, 0x42, /*     Usage (Tip Switch)              */                    \
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07,  \
      0x81, 0x03, 0x09, 0x51, /*     Usage (Contact Identifier)      */        \
      0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05,                    \
      0x01, /*     Usage Page (Generic Desktop)    */                          \
      0x75, 0x10, 0x55, 0x0E, 0x65, 0x11, 0x35, 0x00, 0x26, LE16(maxx), 0x46,  \
      LE16(PHYS_X), 0x09, 0x30, 0x81, 0x02, 0x26, LE16(maxy), 0x46,            \
      LE16(PHYS_Y), 0x09, 0x31, 0x81, 0x02, 0x55, 0x00, 0x65, 0x00, 0x45,      \
      0x00, 0xC0

static const UCHAR g_ReportDescriptor[] = {
    0x05,
    0x0D, /* Usage Page (Digitizer) */
    0x09,
    0x04, /* Usage (Touch Screen)   */
    0xA1,
    0x01, /* Collection (Application) */
    0x85,
    REPORTID_TOUCH,
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    FINGER_COLLECTION(TS_LOGICAL_MAX_X, TS_LOGICAL_MAX_Y),
    0x05,
    0x0D,
    0x55,
    0x0C,
    0x66,
    0x01,
    0x10, /* Unit Exponent -4, Unit (seconds) */
    0x47,
    0xFF,
    0xFF,
    0x00,
    0x00,
    0x27,
    0xFF,
    0xFF,
    0x00,
    0x00,
    0x75,
    0x10,
    0x95,
    0x01,
    0x09,
    0x56,
    0x81,
    0x02, /* Usage (Scan Time), Input */
    0x55,
    0x00,
    0x65,
    0x00,
    0x45,
    0x00,
    0x09,
    0x54, /* Usage (Contact Count) */
    0x15,
    0x00,
    0x25,
    0x7F,
    0x75,
    0x08,
    0x95,
    0x01,
    0x81,
    0x02,
    0x85,
    REPORTID_MAXCNT,
    0x09,
    0x55, /* Usage (Contact Count Maximum) */
    0x25,
    TS_MAX_CONTACTS,
    0x75,
    0x08,
    0x95,
    0x01,
    0xB1,
    0x02,
    0xC0};

#pragma pack(push, 1)
typedef struct _TOUCH_FINGER {
  UCHAR Tip;
  UCHAR Id;
  USHORT X;
  USHORT Y;
} TOUCH_FINGER;

typedef struct _TOUCH_REPORT {
  UCHAR ReportId;
  TOUCH_FINGER Finger[TS_MAX_CONTACTS];
  USHORT ScanTime;
  UCHAR Count;
} TOUCH_REPORT;
#pragma pack(pop)

/* ---- VHF ------------------------------------------------------------------
 */

static VOID EvtVhfGetFeature(PVOID ClientContext, VHFOPERATIONHANDLE Op,
                             PVOID OpContext, PHID_XFER_PACKET Packet) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;

  UNREFERENCED_PARAMETER(ClientContext);
  UNREFERENCED_PARAMETER(OpContext);

  if (Packet->reportId == REPORTID_MAXCNT && Packet->reportBufferLen >= 2) {
    Packet->reportBuffer[0] = REPORTID_MAXCNT;
    Packet->reportBuffer[1] = TS_MAX_CONTACTS;
    status = STATUS_SUCCESS;
  }
  VhfAsyncOperationComplete(Op, status);
}

NTSTATUS TouchVhfCreate(PDEVICE_CONTEXT Ctx) {
  VHF_CONFIG cfg;
  NTSTATUS status;

  VHF_CONFIG_INIT(&cfg, WdfDeviceWdmGetDeviceObject(Ctx->Device),
                  (USHORT)sizeof(g_ReportDescriptor),
                  (PUCHAR)g_ReportDescriptor);
  cfg.VhfClientContext = Ctx;
  cfg.VendorID = 0x2808;  /* FocalTech */
  cfg.ProductID = 0x8203; /* FT8203 */
  cfg.VersionNumber = 0x0001;
  cfg.EvtVhfAsyncOperationGetFeature = EvtVhfGetFeature;

  status = VhfCreate(&cfg, &Ctx->Vhf);
  LogPrint("VhfCreate: %08x\n", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  status = VhfStart(Ctx->Vhf);
  LogPrint("VhfStart: %08x\n", status);
  Ctx->VhfStarted = NT_SUCCESS(status);
  return status;
}

VOID TouchVhfDelete(PDEVICE_CONTEXT Ctx) {
  if (Ctx->Vhf != NULL) {
    VhfDelete(Ctx->Vhf, TRUE);
    Ctx->Vhf = NULL;
    Ctx->VhfStarted = FALSE;
  }
}

/* ---- Helpers & Optional PRAM Loader -------------------------------------- */

static VOID SleepMs(ULONG Ms) {
  LARGE_INTEGER t;
  t.QuadPart = -10000LL * Ms;
  KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static BOOLEAN FileExists(PCWSTR Path) {
  UNICODE_STRING n;
  OBJECT_ATTRIBUTES oa;
  IO_STATUS_BLOCK iosb;
  HANDLE h;

  RtlInitUnicodeString(&n, Path);
  InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                             NULL, NULL);
  if (!NT_SUCCESS(ZwCreateFile(
          &h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL,
          FILE_ATTRIBUTE_NORMAL,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
    return FALSE;
  }
  ZwClose(h);
  return TRUE;
}

static PUCHAR ReadWholeFile(PCWSTR Path, PULONG SizeOut) {
  UNICODE_STRING n;
  OBJECT_ATTRIBUTES oa;
  IO_STATUS_BLOCK iosb;
  FILE_STANDARD_INFORMATION fsi;
  HANDLE h;
  PUCHAR buf = NULL;

  *SizeOut = 0;
  RtlInitUnicodeString(&n, Path);
  InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                             NULL, NULL);
  if (!NT_SUCCESS(ZwCreateFile(
          &h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
          FILE_ATTRIBUTE_NORMAL,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
    return NULL;
  }
  if (NT_SUCCESS(ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi),
                                        FileStandardInformation)) &&
      fsi.EndOfFile.QuadPart > 0 && fsi.EndOfFile.QuadPart <= 512 * 1024) {
    ULONG sz = fsi.EndOfFile.LowPart;
    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, XUN_POOL_TAG);
    if (buf != NULL) {
      if (NT_SUCCESS(
              ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sz, NULL, NULL)) &&
          iosb.Information == sz) {
        *SizeOut = sz;
      } else {
        ExFreePoolWithTag(buf, XUN_POOL_TAG);
        buf = NULL;
      }
    }
  }
  ZwClose(h);
  return buf;
}

static BOOLEAN FtsFwValid(UCHAR Id) {
  return (Id == 0x82 || Id == 0x80 || Id == 0x54 || Id == 0x58);
}

static NTSTATUS FtsTryUploadPramPath(PDEVICE_CONTEXT Ctx, PCWSTR ForcePath) {
  static const PCWSTR paths[] = {
      L"\\??\\C:\\xun\\fw\\focaltech_ts_fw.bin",
      L"\\??\\C:\\xun\\fw\\focaltech_ts_fw_huaxing_ft8203.bin",
      L"\\??\\C:\\xun\\fw\\focaltech_ts_fw_boe_ft8203.bin"};
  PUCHAR fw = NULL;
  ULONG fwLen = 0, i, off = 0;
  UCHAR pkt[4 + 256], cmd, romId[2] = {0};
  NTSTATUS status;

  if (ForcePath != NULL) {
    fw = ReadWholeFile(ForcePath, &fwLen);
    if (fw != NULL) {
      LogPrint("pram: loaded %ws (%u bytes)\n", ForcePath, fwLen);
    }
  } else {
    for (i = 0; i < ARRAYSIZE(paths) && fw == NULL; i++) {
      fw = ReadWholeFile(paths[i], &fwLen);
      if (fw != NULL) {
        LogPrint("pram: loaded %ws (%u bytes)\n", paths[i], fwLen);
      }
    }
  }
  if (fw == NULL) {
    return STATUS_OBJECT_NAME_NOT_FOUND;
  }

  TlmmSetOutput(&Ctx->PinReset, FALSE);
  SleepMs(20);
  TlmmSetOutput(&Ctx->PinReset, TRUE);
  SleepMs(12);

  cmd = 0x55;
  FtsSpiWrite(&Ctx->Bus, &cmd, 1);
  SleepMs(2);
  status = FtsSpiRead(&Ctx->Bus, 0x90, romId, 2);
  LogPrint("pram: romboot id=%02x %02x status=%08x\n", romId[0], romId[1],
           status);
  if (!NT_SUCCESS(status)) {
    ExFreePoolWithTag(fw, XUN_POOL_TAG);
    return status;
  }

  if (fwLen >= 8) {
    LogPrint("pram: hdr=%02x %02x %02x %02x %02x %02x %02x %02x\n",
             fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], fw[6], fw[7]);
  }

  while (off < fwLen) {
    ULONG chunk = min(256u, fwLen - off);
    pkt[0] = 0xAE;
    pkt[1] = (UCHAR)((off >> 16) & 0xFF);
    pkt[2] = (UCHAR)((off >> 8) & 0xFF);
    pkt[3] = (UCHAR)(off & 0xFF);
    RtlCopyMemory(pkt + 4, fw + off, chunk);
    status = FtsSpiWrite(&Ctx->Bus, pkt, 4 + chunk);
    if (!NT_SUCCESS(status)) {
      LogPrint("pram: write err off=%lu status=%08x\n", off, status);
      ExFreePoolWithTag(fw, XUN_POOL_TAG);
      return status;
    }
    KeStallExecutionProcessor(500);
    off += chunk;
  }

  if (fwLen >= 256u) {
    USHORT ecc256Host = 0;
    ULONG b256;
    UCHAR ecc256Cmd[7] = {0xCC, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    UCHAR ecc256Done = 0xFF, ecc256Val[2] = {0};
    USHORT ecc256Tp;

    for (i = 0; i + 1 < 256u; i += 2) {
      ecc256Host ^= (USHORT)((fw[i] << 8) | fw[i + 1]);
      for (b256 = 0; b256 < 16; b256++) {
        ecc256Host = (ecc256Host & 1) ? (USHORT)((ecc256Host >> 1) ^ 0x8408)
                                      : (USHORT)(ecc256Host >> 1);
      }
    }

    (void)FtsSpiWrite(&Ctx->Bus, ecc256Cmd, 7);
    for (i = 0; i < 100; i++) {
      SleepMs(1);
      if (NT_SUCCESS(FtsSpiRead(&Ctx->Bus, 0xCE, &ecc256Done, 1)) &&
          (ecc256Done == 0 || ecc256Done == 0xA5)) {
        break;
      }
    }
    (void)FtsSpiRead(&Ctx->Bus, 0xCD, ecc256Val, 2);
    ecc256Tp = (USHORT)((ecc256Val[0] << 8) | ecc256Val[1]);
    LogPrint("pram: ecc256_host=%04x ecc256_tp=%04x\n", ecc256Host, ecc256Tp);
  }

  {
    USHORT eccHost = 0;
    ULONG b;
    UCHAR eccCmd[7] = {
        0xCC, 0x00, 0x00, 0x00,
        (UCHAR)((fwLen >> 16) & 0xFF),
        (UCHAR)((fwLen >> 8) & 0xFF),
        (UCHAR)(fwLen & 0xFF)
    };
    NTSTATUS eccSt;
    UCHAR eccDone = 0xFF, eccVal[2] = {0};
    NTSTATUS eccRdSt;
    USHORT eccTp;

    for (i = 0; i + 1 < fwLen; i += 2) {
      eccHost ^= (USHORT)((fw[i] << 8) | fw[i + 1]);
      for (b = 0; b < 16; b++) {
        eccHost = (eccHost & 1) ? (USHORT)((eccHost >> 1) ^ 0x8408)
                                : (USHORT)(eccHost >> 1);
      }
    }

    eccSt = FtsSpiWrite(&Ctx->Bus, eccCmd, 7);
    for (i = 0; i < 100; i++) {
      SleepMs(1);
      if (NT_SUCCESS(FtsSpiRead(&Ctx->Bus, 0xCE, &eccDone, 1)) &&
          eccDone == 0) {
        break;
      }
    }
    eccRdSt = FtsSpiRead(&Ctx->Bus, 0xCD, eccVal, 2);
    eccTp = (USHORT)((eccVal[0] << 8) | eccVal[1]);
    LogPrint("pram: ecc_wr=%08x done=%02x ecc_rd=%08x ecc_host=%04x ecc_tp=%04x\n",
             eccSt, eccDone, eccRdSt, eccHost, eccTp);
  }

  ExFreePoolWithTag(fw, XUN_POOL_TAG);

  cmd = 0x08; /* start PRAM app */
  status = FtsSpiWrite(&Ctx->Bus, &cmd, 1);
  LogPrint("pram: start app status=%08x\n", status);
  SleepMs(300);
  return STATUS_SUCCESS;
}

static NTSTATUS FtsTryUploadPram(PDEVICE_CONTEXT Ctx) {
  return FtsTryUploadPramPath(Ctx, NULL);
}

/* ---- Hardware -------------------------------------------------------------
 */

NTSTATUS TouchHwInit(PDEVICE_CONTEXT Ctx) {
  static const ULONG spiPins[4] = {TS_PIN_SPI_MISO, TS_PIN_SPI_MOSI,
                                   TS_PIN_SPI_CLK, TS_PIN_SPI_CS};
  NTSTATUS status = STATUS_SUCCESS;
  UCHAR id1 = 0, id2 = 0, fw = 0, vendor = 0;
  ULONG i;

  for (i = 0; i < 4 && NT_SUCCESS(status); i++) {
    status = TlmmPinMap(&Ctx->PinSpi[i], TLMM_TILE_WEST, spiPins[i]);
  }
  if (NT_SUCCESS(status))
    status = TlmmPinMap(&Ctx->PinIrq, TLMM_TILE_WEST, TS_PIN_IRQ);
  if (NT_SUCCESS(status))
    status = TlmmPinMap(&Ctx->PinReset, TLMM_TILE_WEST, TS_PIN_RESET);
  LogPrint("TLMM map: %08x\n", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  for (i = 0; i < 4; i++) {
    TlmmConfig(&Ctx->PinSpi[i], TS_PIN_SPI_FUNC, TLMM_PULL_UP, 6, FALSE);
  }
  TlmmConfig(&Ctx->PinIrq, 0, TLMM_PULL_UP, 8, FALSE);
  TlmmConfig(&Ctx->PinReset, 0, TLMM_PULL_UP, 8, TRUE);

  /* Precise initial hardware reset pulse on TLMM GPIO 86 (0x00556000) */
  TlmmSetOutput(&Ctx->PinReset, FALSE);
  SleepMs(20);
  TlmmSetOutput(&Ctx->PinReset, TRUE);
  SleepMs(200);

  status = GccEnableQup0Se0();
  if (!NT_SUCCESS(status)) {
    LogPrint("GCC SE0 enable failed: %08x (continuing)\n", status);
  }
  status = GeniSpiInit(&Ctx->Bus, QUP0_SE0_BASE);
  LogPrint("GeniSpiInit: %08x\n", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = FtsReadReg(&Ctx->Bus, FTS_REG_CHIP_ID, &id1);
  LogPrint("probe: chip_id=%02x status=%08x\n", id1, status);
  if (!NT_SUCCESS(status) || !FtsFwValid(id1)) {
    TlmmSetOutput(&Ctx->PinReset, FALSE);
    SleepMs(20);
    TlmmSetOutput(&Ctx->PinReset, TRUE);
    SleepMs(250);
    status = FtsReadReg(&Ctx->Bus, FTS_REG_CHIP_ID, &id1);
    LogPrint("probe (after retry reset): chip_id=%02x status=%08x\n", id1, status);
    if (!NT_SUCCESS(status) || !FtsFwValid(id1)) {
      if (NT_SUCCESS(FtsTryUploadPram(Ctx))) {
        status = FtsReadReg(&Ctx->Bus, FTS_REG_CHIP_ID, &id1);
        LogPrint("probe (after PRAM): chip_id=%02x status=%08x\n", id1, status);
        if (!NT_SUCCESS(status) || !FtsFwValid(id1)) {
          if (NT_SUCCESS(FtsTryUploadPramPath(
                  Ctx, L"\\??\\C:\\xun\\fw\\focaltech_ts_fw_boe_ft8203.bin"))) {
            status = FtsReadReg(&Ctx->Bus, FTS_REG_CHIP_ID, &id1);
            LogPrint("probe (after PRAM BOE): chip_id=%02x status=%08x\n", id1,
                     status);
          }
        }
      }
    }
  }

  FtsReadReg(&Ctx->Bus, FTS_REG_CHIP_ID2, &id2);
  FtsReadReg(&Ctx->Bus, FTS_REG_FW_VER, &fw);
  FtsReadReg(&Ctx->Bus, FTS_REG_VENDOR_ID, &vendor);
  LogPrint("chip_id2=%02x fw_ver=%02x vendor=%02x irq_line=%u\n", id2, fw,
           vendor, TlmmGetInput(&Ctx->PinIrq));

  if (FileExists(L"\\??\\C:\\xun\\touch.fast")) {
    UCHAR back = 0;
    FtsWriteReg(&Ctx->Bus, 0x88, 12);
    FtsReadReg(&Ctx->Bus, 0x88, &back);
    LogPrint("touch.fast: reg 0x88=%02x\n", back);
  }
  Ctx->Flip180 = !FileExists(L"\\??\\C:\\xun\\touch.noflip");
  LogPrint("touch 180-deg flip: %s\n",
           Ctx->Flip180 ? "enabled" : "disabled (touch.noflip)");

  Ctx->HwReady = TRUE;
  return STATUS_SUCCESS;
}

VOID TouchHwDeinit(PDEVICE_CONTEXT Ctx) {
  ULONG i;
  Ctx->HwReady = FALSE;
  GeniSpiDeinit(&Ctx->Bus);
  for (i = 0; i < 4; i++) {
    TlmmPinUnmap(&Ctx->PinSpi[i]);
  }
  TlmmPinUnmap(&Ctx->PinIrq);
  TlmmPinUnmap(&Ctx->PinReset);
}

/* ---- Polling thread --------------------------------------------------------
 */

static VOID TouchProcess(PDEVICE_CONTEXT Ctx, const UCHAR *Buf,
                         ULONGLONG ReadTime) {
  TOUCH_REPORT rpt;
  HID_XFER_PACKET pkt;
  ULONG i, n = 0, slots;
  USHORT mask = 0;
  UCHAR statusByte = Buf[1];
  UCHAR mode = statusByte >> 4;
  ULONG td = statusByte & 0x0F;

  if (statusByte == 0xEF && Buf[2] == 0xEF && Buf[3] == 0xEF) {
    return;
  }

  RtlZeroMemory(&rpt, sizeof(rpt));
  rpt.ReportId = REPORTID_TOUCH;

  slots = (statusByte == 0xFF || td == 0 || td > TS_MAX_CONTACTS)
              ? 0
              : TS_MAX_CONTACTS;
  for (i = 0; i < slots; i++) {
    const UCHAR *p = Buf + 2 + 6 * i;
    UCHAR event = p[0] >> 6;
    UCHAR id = p[2] >> 4;
    ULONG x, y;
    BOOLEAN down = (event != 1);

    if (id >= TS_MAX_CONTACTS || event == 3) {
      continue;
    }
    if (mode == 2 || mode == 8) {
      /* 12-bit legacy mode -> scale x10 into 0..11999 x 0..19199 */
      x = ((((ULONG)(p[0] & 0x0F) << 8) | p[1])) * 10;
      y = ((((ULONG)(p[2] & 0x0F) << 8) | p[3])) * 10;
    } else {
      /* 15-bit high-res mode matching focaltech_ts_spi (0x2EDF x 0x4AFF) */
      x = ((ULONG)(p[0] & 0x0F) << 11) | ((ULONG)p[1] << 3) |
          ((p[4] >> 4) & 0x07);
      y = ((ULONG)(p[2] & 0x0F) << 11) | ((ULONG)p[3] << 3) | (p[4] & 0x07);
    }
    if (!down && !(Ctx->ActiveMask & (1u << id))) {
      continue;
    }
    if (Ctx->Flip180) {
      x = (x <= TS_LOGICAL_MAX_X) ? (TS_LOGICAL_MAX_X - x) : 0;
      y = (y <= TS_LOGICAL_MAX_Y) ? (TS_LOGICAL_MAX_Y - y) : 0;
    } else {
      x = min(x, (ULONG)TS_LOGICAL_MAX_X);
      y = min(y, (ULONG)TS_LOGICAL_MAX_Y);
    }

    if (x > Ctx->MaxRawX || y > Ctx->MaxRawY) {
      Ctx->MaxRawX = max(Ctx->MaxRawX, x);
      Ctx->MaxRawY = max(Ctx->MaxRawY, y);
    }
    if ((!down || !(Ctx->ActiveMask & (1u << id))) && Ctx->RawLogged < 200) {
      Ctx->RawLogged++;
      LogPrint("%s id=%u x=%u y=%u mode=%u raw=%02x %02x %02x %02x %02x %02x "
               "td=%02x\n",
               event == 1 ? "up  " : "down", id, x, y, mode, p[0], p[1], p[2],
               p[3], p[4], p[5], statusByte);
    }
    rpt.Finger[n].Tip = down ? 1 : 0;
    rpt.Finger[n].Id = id;
    rpt.Finger[n].X = (USHORT)x;
    rpt.Finger[n].Y = (USHORT)y;
    Ctx->LastX[id] = rpt.Finger[n].X;
    Ctx->LastY[id] = rpt.Finger[n].Y;
    if (down) {
      mask |= (USHORT)(1u << id);
    }
    n++;
  }

  for (i = 0; i < TS_MAX_CONTACTS && n < TS_MAX_CONTACTS; i++) {
    BOOLEAN inReport = FALSE;
    ULONG j;

    if (!(Ctx->ActiveMask & (1u << i)) || (mask & (1u << i))) {
      continue;
    }
    for (j = 0; j < n; j++) {
      inReport |= (rpt.Finger[j].Id == i);
    }
    if (!inReport) {
      rpt.Finger[n].Tip = 0;
      rpt.Finger[n].Id = (UCHAR)i;
      rpt.Finger[n].X = Ctx->LastX[i];
      rpt.Finger[n].Y = Ctx->LastY[i];
      n++;
    }
  }
  Ctx->ActiveMask = mask;
  rpt.Count = (UCHAR)n;
  rpt.ScanTime = (USHORT)(ReadTime / 1000);

  if (n == 0 || !Ctx->VhfStarted) {
    return;
  }
  pkt.reportBuffer = (PUCHAR)&rpt;
  pkt.reportBufferLen = sizeof(rpt);
  pkt.reportId = REPORTID_TOUCH;
  VhfReadReportSubmit(Ctx->Vhf, &pkt);
}

static KSTART_ROUTINE TouchThread;

static VOID TouchThread(PVOID Context) {
  PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
  UCHAR buf[FTS_TOUCH_DATA_LEN];
  LARGE_INTEGER period;
  ULONG errors = 0, polls = 0;
  BOOLEAN wasTouching = FALSE;
  ULONGLONG lastChange = KeQueryInterruptTime();
  UCHAR prev[FTS_TOUCH_DATA_LEN];

  period.QuadPart = -10000LL * 5;
  ExSetTimerResolution(10000, TRUE);
  RtlZeroMemory(prev, sizeof(prev));
  LogPrint("thread started\n");

  while (KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE,
                               &period) == STATUS_TIMEOUT) {
    NTSTATUS status;
    BOOLEAN irqLow = !TlmmGetInput(&Ctx->PinIrq);
    ULONGLONG readTime;

    polls++;
    if (!irqLow && !wasTouching && (polls % 20) != 0) {
      continue;
    }
    if (wasTouching && KeQueryInterruptTime() - lastChange > 5000000ULL) {
      UCHAR none[FTS_TOUCH_DATA_LEN];
      UCHAR id1 = 0;
      LogPrint("watchdog: contact frame unchanged 500 ms -> release & reset\n");
      RtlFillMemory(none, sizeof(none), 0xFF);
      none[1] = 0;
      TouchProcess(Ctx, none, KeQueryInterruptTime());
      wasTouching = FALSE;
      TlmmSetOutput(&Ctx->PinReset, FALSE);
      SleepMs(20);
      TlmmSetOutput(&Ctx->PinReset, TRUE);
      SleepMs(250);
      if (!NT_SUCCESS(FtsReadReg(&Ctx->Bus, FTS_REG_CHIP_ID, &id1)) ||
          !FtsFwValid(id1)) {
        FtsTryUploadPram(Ctx);
      }
      lastChange = KeQueryInterruptTime();
      RtlZeroMemory(prev, sizeof(prev));
      continue;
    }

    readTime = KeQueryInterruptTime();
    status = FtsSpiRead(&Ctx->Bus, FTS_REG_TOUCH_DATA, buf, sizeof(buf));
    if (!NT_SUCCESS(status)) {
      if (errors++ < 20) {
        LogPrint("SPI read err %08x irq=%08x\n", status,
                 Ctx->Bus.LastIrqStatus);
      }
      /* Prevent CPU-hogging busy-wait loop / race-condition with DWM memory allocation (Fix BSOD 0x50) */
      SleepMs(errors > 20 ? 1000 : 50);
      continue;
    }
    if (RtlCompareMemory(buf, prev, sizeof(buf)) != sizeof(buf)) {
      lastChange = KeQueryInterruptTime();
    }
    RtlCopyMemory(prev, buf, sizeof(buf));
    if (buf[1] == 0xEF && buf[2] == 0xEF && buf[3] == 0xEF) {
      FtsTryUploadPram(Ctx);
      continue;
    }
    wasTouching = ((buf[1] != 0xFF) && ((buf[1] & 0x0F) != 0) &&
                   ((buf[1] & 0x0F) <= TS_MAX_CONTACTS)) ||
                  (Ctx->ActiveMask != 0);
    TouchProcess(Ctx, buf, readTime);
  }
  ExSetTimerResolution(0, FALSE);
  LogPrint("thread exit (polls=%u errors=%u)\n", polls, errors);
  PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS TouchThreadStart(PDEVICE_CONTEXT Ctx) {
  NTSTATUS status;

  KeInitializeEvent(&Ctx->StopEvent, NotificationEvent, FALSE);
  status = PsCreateSystemThread(&Ctx->ThreadHandle, THREAD_ALL_ACCESS, NULL,
                                NULL, NULL, TouchThread, Ctx);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  status = ObReferenceObjectByHandle(Ctx->ThreadHandle, THREAD_ALL_ACCESS,
                                     *PsThreadType, KernelMode,
                                     (PVOID *)&Ctx->Thread, NULL);
  ZwClose(Ctx->ThreadHandle);
  Ctx->ThreadHandle = NULL;
  return status;
}

VOID TouchThreadStop(PDEVICE_CONTEXT Ctx) {
  if (Ctx->Thread != NULL) {
    KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Ctx->Thread);
    Ctx->Thread = NULL;
  }
}
