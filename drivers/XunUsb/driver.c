/*
 * XunUsb - LowerFilter for ACPI\PNP0D10 (USBXHCI.SYS) on Redmi Pad SE (xun, SM6225).
 * Runs before USBXHCI.SYS PrepareHardware to initialize:
 *   - SMMU SID 0x120 (0x0C600000) identity bypass
 *   - GCC gcc_usb30_prim_gdsc (0x0141A004) & USB clocks
 *   - SNPS Femto HS PHY (0x01613000, EUD 0x01612000, override 6C=A5, 70=8C, 74=16)
 *   - QSCRATCH (0x04EF8800) HS-only + DWC3 (0x04E00000) Host Mode (GCTL.PRTCAPDIR = 1)
 */
#include "driver.h"

#define GCC_PA          0x01400000ULL
#define GCC_SIZE        0x20000
#define GCC_USB_GDSCR   0x1A004
#define GCC_USB_M_CBCR  0x1A008
#define GCC_USB_S_CBCR  0x1A014
#define GCC_USB_U_CBCR  0x1A018

#define EUD_PA          0x01612000ULL
#define HSPHY_PA        0x01613000ULL

#define DWC3_PA         0x04E00000ULL
#define DWC3_SIZE       0x100000
#define DWC3_GCTL       0xC110
#define DWC3_GUSB2PHY   0xC200
#define DWC3_GUSB3PIPE  0xC2C0
#define QSCRATCH_OFF    0xF8800
#define QSCRATCH_GEN    (QSCRATCH_OFF + 0x08)
#define QSCRATCH_HSPHY  (QSCRATCH_OFF + 0x10)

#define SMMU_PA         0x0C600000ULL
#define SMMU_SIZE       0x80000
#define USB_SMMU_SID    0x120u

typedef struct _USB_CTX {
    volatile UCHAR *Gcc;
    volatile UCHAR *Eud;
    volatile UCHAR *HsPhy;
    volatile UCHAR *Dwc3;
    volatile UCHAR *Smmu;
} USB_CTX, *PUSB_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_CTX, GetUsbCtx)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHw;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHw;

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static ULONG Rd32(volatile UCHAR *Base, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

static VOID Wr32(volatile UCHAR *Base, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
}

static VOID SmmuBypassUsb(volatile UCHAR *Smmu)
{
    ULONG id1 = Rd32(Smmu, 0x24);
    ULONG nsmr = Rd32(Smmu, 0x20) & 0xFFu;
    ULONG ncb = id1 & 0xFFu;
    ULONG pg = 1u << (((id1 >> 28) & 1u) ? 16u : 12u);
    ULONG i, smr, cb;

    for (i = 0; i < nsmr; i++) {
        smr = Rd32(Smmu, 0x800 + i * 4);
        if ((smr & 0x80000000u) && (smr & 0x7FFFu) == USB_SMMU_SID) {
            cb = Rd32(Smmu, 0xC00 + i * 4) & 0xFFu;
            LogPrint("smmu: sid %x already SMR%u CB%u\r\n", USB_SMMU_SID, i, cb);
            return;
        }
    }
    for (i = 0; i < nsmr; i++) {
        if ((Rd32(Smmu, 0x800 + i * 4) & 0x80000000u) == 0) {
            break;
        }
    }
    cb = ncb ? (ncb - 1) : 0;
    while (cb > 0 && (Rd32(Smmu, 0x1000 + cb * 4) & 0x30000u) != 0) {
        cb--;
    }
    if (i >= nsmr) {
        LogPrint("smmu: no free SMR\r\n");
        return;
    }
    Wr32(Smmu, 0x800 + i * 4, 0x80000000u | USB_SMMU_SID);
    Wr32(Smmu, 0xC00 + i * 4, cb);
    Wr32(Smmu, 0x1000 + cb * 4, 0x00010000u);           /* bypass */
    Wr32(Smmu, (ncb + 1 + cb) * pg + 0x0, 0x000000E0u); /* SCTLR */
    LogPrint("smmu: sid %x -> SMR%u CB%u\r\n", USB_SMMU_SID, i, cb);
}

static VOID EnableGdscAndClks(volatile UCHAR *Gcc)
{
    ULONG gdsc = Rd32(Gcc, GCC_USB_GDSCR), k;
    ULONG offs[3] = { GCC_USB_M_CBCR, GCC_USB_S_CBCR, GCC_USB_U_CBCR };

    if (gdsc & 1u) {
        Wr32(Gcc, GCC_USB_GDSCR, gdsc & ~1u);
        for (k = 0; k < 50; k++) {
            if (Rd32(Gcc, GCC_USB_GDSCR) & 0x80000000u) {
                break;
            }
            SleepMs(1);
        }
    }
    LogPrint("gcc gdsc=%08x -> %08x\r\n", gdsc, Rd32(Gcc, GCC_USB_GDSCR));
    for (k = 0; k < 3; k++) {
        ULONG v = Rd32(Gcc, offs[k]);
        if (v & 0x80000000u) {
            Wr32(Gcc, offs[k], v | 1u);
            SleepMs(1);
        }
        LogPrint("gcc cbcr[%05x]=%08x -> %08x\r\n", offs[k], v, Rd32(Gcc, offs[k]));
    }
}

static VOID InitHsPhyAndDwc3Host(PUSB_CTX Ctx)
{
    ULONG c0, gctl, g2, g3, qgen, qhs;

    LogPrint("eud=%08x\r\n", Rd32(Ctx->Eud, 0));

    /* SNPS Femto HS PHY reset & param override from live_fdt.dts */
    Wr32(Ctx->HsPhy, 0x50, Rd32(Ctx->HsPhy, 0x50) | 0x02u); /* POR = 1 */
    c0 = Rd32(Ctx->HsPhy, 0x54);
    c0 = (c0 & ~0x04u) | 0x08u;                             /* clear SIDDQ, set RETENABLEN */
    Wr32(Ctx->HsPhy, 0x54, c0);
    Wr32(Ctx->HsPhy, 0x6C, 0xA5u);
    Wr32(Ctx->HsPhy, 0x70, 0x8Cu);
    Wr32(Ctx->HsPhy, 0x74, 0x16u);
    SleepMs(1);
    Wr32(Ctx->HsPhy, 0x50, Rd32(Ctx->HsPhy, 0x50) & ~0x02u); /* POR = 0 */
    SleepMs(1);
    LogPrint("hsphy: ctrl5=%08x common0=%08x 6c=%02x 70=%02x 74=%02x\r\n",
             Rd32(Ctx->HsPhy, 0x50), Rd32(Ctx->HsPhy, 0x54),
             Rd32(Ctx->HsPhy, 0x6C), Rd32(Ctx->HsPhy, 0x70), Rd32(Ctx->HsPhy, 0x74));

    /* QSCRATCH: HS-only mode (no SS QMP PHY on xun) + Host VBUS/SESSVLD clear */
    qgen = Rd32(Ctx->Dwc3, QSCRATCH_GEN);
    qgen = (qgen | 0x09u) & ~0x100u;                        /* PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW */
    Wr32(Ctx->Dwc3, QSCRATCH_GEN, qgen);
    qhs = Rd32(Ctx->Dwc3, QSCRATCH_HSPHY);
    qhs &= ~((1u << 20) | (1u << 28));                      /* clear UTMI_OTG_VBUS_VALID & SW_SESSVLD_SEL */
    Wr32(Ctx->Dwc3, QSCRATCH_HSPHY, qhs);

    /* DWC3 PHY soft reset & Host Mode (PRTCAPDIR = 1) */
    g2 = Rd32(Ctx->Dwc3, DWC3_GUSB2PHY);
    Wr32(Ctx->Dwc3, DWC3_GUSB2PHY, g2 | 0x80000000u);
    SleepMs(10);
    Wr32(Ctx->Dwc3, DWC3_GUSB2PHY, (g2 & ~0x80000040u));    /* clear PHYSOFTRST & SUSPHY */
    g3 = Rd32(Ctx->Dwc3, DWC3_GUSB3PIPE);
    Wr32(Ctx->Dwc3, DWC3_GUSB3PIPE, g3 & ~(1u << 17));      /* clear SUSPHY */

    gctl = Rd32(Ctx->Dwc3, DWC3_GCTL);
    gctl = (gctl & ~0x3000u) | 0x1000u | 0x10001u;          /* PRTCAPDIR=1 (Host), U2RSTECN, DSBLCLKGTNG */
    Wr32(Ctx->Dwc3, DWC3_GCTL, gctl);

    LogPrint("dwc3: gctl=%08x xhci_cap=%08x hcs1=%08x usbcmd=%08x usbsts=%08x portsc=%08x\r\n",
             Rd32(Ctx->Dwc3, DWC3_GCTL), Rd32(Ctx->Dwc3, 0x00), Rd32(Ctx->Dwc3, 0x04),
             Rd32(Ctx->Dwc3, 0x20), Rd32(Ctx->Dwc3, 0x24), Rd32(Ctx->Dwc3, 0x420));
}

static NTSTATUS EvtPrepareHw(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PUSB_CTX ctx = GetUsbCtx(Device);
    PHYSICAL_ADDRESS pa;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    LogOpen();
    LogPrint("==== XunUsb %s (LowerFilter) ====\r\n", XUN_USB_VERSION);

    pa.QuadPart = (LONGLONG)GCC_PA;   ctx->Gcc   = (volatile UCHAR *)MmMapIoSpace(pa, GCC_SIZE, MmNonCached);
    pa.QuadPart = (LONGLONG)EUD_PA;   ctx->Eud   = (volatile UCHAR *)MmMapIoSpace(pa, 0x1000, MmNonCached);
    pa.QuadPart = (LONGLONG)HSPHY_PA; ctx->HsPhy = (volatile UCHAR *)MmMapIoSpace(pa, 0x1000, MmNonCached);
    pa.QuadPart = (LONGLONG)DWC3_PA;  ctx->Dwc3  = (volatile UCHAR *)MmMapIoSpace(pa, DWC3_SIZE, MmNonCached);
    pa.QuadPart = (LONGLONG)SMMU_PA;  ctx->Smmu  = (volatile UCHAR *)MmMapIoSpace(pa, SMMU_SIZE, MmNonCached);

    if (!ctx->Gcc || !ctx->Eud || !ctx->HsPhy || !ctx->Dwc3 || !ctx->Smmu) {
        LogPrint("MmMapIoSpace failed\r\n");
        return STATUS_SUCCESS; /* do not block USBXHCI if map fails */
    }

    SmmuBypassUsb(ctx->Smmu);
    EnableGdscAndClks(ctx->Gcc);
    InitHsPhyAndDwc3Host(ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PUSB_CTX ctx = GetUsbCtx(Device);

    UNREFERENCED_PARAMETER(PreviousState);
    if (ctx->Dwc3) {
        LogPrint("d0: gctl=%08x usbcmd=%08x usbsts=%08x portsc=%08x\r\n",
                 Rd32(ctx->Dwc3, DWC3_GCTL), Rd32(ctx->Dwc3, 0x20),
                 Rd32(ctx->Dwc3, 0x24), Rd32(ctx->Dwc3, 0x420));
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHw(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PUSB_CTX ctx = GetUsbCtx(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    if (ctx->Gcc)   { MmUnmapIoSpace((PVOID)ctx->Gcc, GCC_SIZE);   ctx->Gcc = NULL; }
    if (ctx->Eud)   { MmUnmapIoSpace((PVOID)ctx->Eud, 0x1000);     ctx->Eud = NULL; }
    if (ctx->HsPhy) { MmUnmapIoSpace((PVOID)ctx->HsPhy, 0x1000);   ctx->HsPhy = NULL; }
    if (ctx->Dwc3)  { MmUnmapIoSpace((PVOID)ctx->Dwc3, DWC3_SIZE); ctx->Dwc3 = NULL; }
    if (ctx->Smmu)  { MmUnmapIoSpace((PVOID)ctx->Smmu, SMMU_SIZE); ctx->Smmu = NULL; }
    LogClose();
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;

    UNREFERENCED_PARAMETER(Driver);
    WdfFdoInitSetFilter(DeviceInit);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHw;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHw;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, USB_CTX);
    return WdfDeviceCreate(&DeviceInit, &attr, &dev);
}

static VOID EvtUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogClose();
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    cfg.EvtDriverUnload = EvtUnload;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
