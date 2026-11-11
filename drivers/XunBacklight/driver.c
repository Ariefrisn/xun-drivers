/*
 * XunBacklight - panel brightness of Redmi Pad SE (xun, SM6225) under Windows.
 *
 * Verified from ktz8866_leds.ko & dtbo_b.img:
 *   - Primary backlight controller: KTZ8866 @ I2C 0x11 on QUP0 SE1 (0x04A84000, GPIO 4/5 WEST)
 *   - HWEN pin: TLMM GPIO 102 SOUTH (0x00400000 + 0x00500000 + 102 * 0x1000)
 *   - Registers: 0x09 power (0x9C -> 5 ms -> 0x9E on, 0x9C -> 0x98 off),
 *                0x04 LSB[2:0], 0x05 MSB[10:3] (0..0x7FF)
 *   - QUP0 SE1 bus access serialized across drivers via xun_qup_lock.h (XUN_QUP_SE1).
 *   - Optional C:\xun\bl.dcs falls back to DSI0 DCS 0x51 DMA/FIFO write.
 *
 * Control: C:\xun\brightness = 0..100 (text), polled every 330 ms. Log: C:\XunBacklight.log.
 */
#include "driver.h"
#include "xun_ktz8866.h"

#define XUN_BL_VERSION       "v0.6"

#define DSI0_PA              0x05E94000ULL
#define DSI0_SIZE            0x400
#define DSI_CTRL             0x004
#define DSI_CMD_DMA_CTRL     0x03C
#define DSI_DMA_CMD_OFFSET   0x048
#define DSI_DMA_CMD_LENGTH   0x04C
#define DSI_TRIG_CTRL        0x084
#define DSI_DMA_SW_TRIGGER   0x090
#define DSI_INT_CTRL         0x110
#define DSI_TPG_CTRL         0x15C

#define CTRL_ENABLE          (1u << 0)
#define CTRL_CMD_MODE_EN     (1u << 2)
#define DMA_CTRL_LOW_POWER   (1u << 26)
#define INT_CMD_DMA_DONE     (1u << 0)
#define TRIG_DMA_SW          4u

#define BL_MAX               0x7FF
#define POLL_MS              330

typedef struct _DEVICE_CONTEXT {
    XUN_KTZ_BUS      Ktz;
    volatile UCHAR  *Dsi;
    PUCHAR           DmaVa;
    PHYSICAL_ADDRESS DmaPa;
    XUN_QUP_LOCK     QupLock;
    BOOLEAN          KtzOn;
    BOOLEAN          HwReady;
    LONG             LastPct;
    PKTHREAD         Thread;
    KEVENT           StopEvent;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY         EvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT          EvtD0Exit;

static ULONG Rd(volatile UCHAR *B, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(B + Off)); }
static VOID Wr(volatile UCHAR *B, ULONG Off, ULONG Val) { WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), Val); }

static BOOLEAN FileExists(PCWSTR Path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

static LONG ReadPercent(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\xun\\brightness");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    CHAR buf[16];
    LONG v = -1;
    ULONG i;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return -1;
    }
    RtlZeroMemory(buf, sizeof(buf));
    if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf) - 1, NULL, NULL))) {
        for (i = 0; i < iosb.Information; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                v = (v < 0 ? 0 : v * 10) + (buf[i] - '0');
                if (v > 100) {
                    v = 100;
                }
            } else if (v >= 0) {
                break;
            }
        }
    }
    ZwClose(h);
    return v;
}

static BOOLEAN DsiDcsSend(PDEVICE_CONTEXT C, const UCHAR *Payload, ULONG Len)
{
    UCHAR pkt[16];
    ULONG size, ctrl, dmaCtrl, trig, waited;
    BOOLEAN done = FALSE;

    if (C->Dsi == NULL || C->DmaVa == NULL || Len > sizeof(pkt) - 4) {
        return FALSE;
    }
    size = (4 + Len + 3) & ~3u;
    RtlFillMemory(pkt, sizeof(pkt), 0xFF);
    pkt[0] = (UCHAR)Len;
    pkt[1] = (UCHAR)(Len >> 8);
    pkt[2] = 0x39;
    pkt[3] = 0xC0;
    RtlCopyMemory(pkt + 4, Payload, Len);
    RtlCopyMemory(C->DmaVa, pkt, size);
    KeMemoryBarrier();

    ctrl = Rd(C->Dsi, DSI_CTRL);
    trig = Rd(C->Dsi, DSI_TRIG_CTRL);
    if ((trig & 7) != TRIG_DMA_SW) {
        Wr(C->Dsi, DSI_TRIG_CTRL, (trig & ~7u) | TRIG_DMA_SW);
    }
    Wr(C->Dsi, DSI_CTRL, ctrl | CTRL_CMD_MODE_EN | CTRL_ENABLE);
    Wr(C->Dsi, DSI_INT_CTRL, Rd(C->Dsi, DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(C->Dsi, DSI_TPG_CTRL, 0);
    Wr(C->Dsi, DSI_DMA_CMD_OFFSET, C->DmaPa.LowPart);
    Wr(C->Dsi, DSI_DMA_CMD_LENGTH, size);
    dmaCtrl = Rd(C->Dsi, DSI_CMD_DMA_CTRL);
    Wr(C->Dsi, DSI_CMD_DMA_CTRL, dmaCtrl & ~DMA_CTRL_LOW_POWER);
    Wr(C->Dsi, DSI_DMA_SW_TRIGGER, 1);

    for (waited = 0; waited < 50; waited++) {
        if (Rd(C->Dsi, DSI_INT_CTRL) & INT_CMD_DMA_DONE) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(1000);
    }
    Wr(C->Dsi, DSI_INT_CTRL, Rd(C->Dsi, DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(C->Dsi, DSI_CMD_DMA_CTRL, dmaCtrl);
    Wr(C->Dsi, DSI_CTRL, ctrl);
    if (trig != Rd(C->Dsi, DSI_TRIG_CTRL)) {
        Wr(C->Dsi, DSI_TRIG_CTRL, trig);
    }
    return done;
}

static BOOLEAN SetLevel(PDEVICE_CONTEXT C, LONG Pct)
{
    ULONG v = (ULONG)Pct * BL_MAX / 100;
    BOOLEAN ok = FALSE, forceDcs = FileExists(L"\\??\\C:\\xun\\bl.dcs");
    static BOOLEAN ktzWarned;

    if (Pct > 0 && v < 16) {
        v = 16;
    }
    if (!forceDcs && C->Ktz.Se1 != NULL && XunQupLockAcquire(&C->QupLock, 500)) {
        if (Pct == 0) {
            ok = XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_LSB, 0) &&
                 XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_MSB, 0) &&
                 XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_EN, 0x9C) &&
                 XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_EN, 0x98);
            C->KtzOn = FALSE;
        } else {
            BOOLEAN chipOk = XunKtzInitChip(&C->Ktz);
            if (chipOk && !C->KtzOn) {
                chipOk = XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_EN, 0x9C);
                XunKtzDelayUs(5000);
                chipOk = chipOk && XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_EN, 0x9E);
                if (chipOk) {
                    C->KtzOn = TRUE;
                }
            }
            if (chipOk) {
                ok = XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_LSB, (UCHAR)(v & 0x7)) &&
                     XunKtzWriteReg(&C->Ktz, XUN_KTZ_REG_BL_MSB, (UCHAR)((v >> 3) & 0xFF));
            }
        }
        XunQupLockRelease(&C->QupLock);
        if (!ok && !ktzWarned) {
            ktzWarned = TRUE;
            LogPrint("KTZ8866 failed (fw=%08x cbcr=%08x irq=%08x hwen=%08x/%08x->%08x/%08x)\n",
                     C->Ktz.LastFw, C->Ktz.LastCbcr, C->Ktz.LastIrq,
                     C->Ktz.HwenBeforeCtl, C->Ktz.HwenBeforeIo,
                     C->Ktz.HwenAfterCtl, C->Ktz.HwenAfterIo);
        }
    }
    if (forceDcs && Pct > 0) {
        UCHAR dcs[3];
        ULONG lv = (Pct > 0 && v < 1) ? 1 : v;
        dcs[0] = 0x51;
        dcs[1] = (UCHAR)((lv >> 3) & 0xFF);
        dcs[2] = (UCHAR)(lv & 0x7);
        ok = DsiDcsSend(C, dcs, sizeof(dcs));
    }
    LogPrint("brightness %ld%% -> 0x%03x (%s)\n", Pct, v, ok ? "ok" : "failed");
    return ok;
}

static NTSTATUS HwInit(PDEVICE_CONTEXT C)
{
    PHYSICAL_ADDRESS pa, lo, hi, skip;

    XunQupLockInit(&C->QupLock, XUN_QUP_SE1);
    XunKtzInit(&C->Ktz);
    pa.QuadPart = (LONGLONG)DSI0_PA;
    C->Dsi = (volatile UCHAR *)MmMapIoSpaceEx(pa, DSI0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    C->DmaVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, skip, MmNonCached);
    if (C->DmaVa != NULL) {
        C->DmaPa = MmGetPhysicalAddress(C->DmaVa);
    }
    C->KtzOn = FALSE;
    LogPrint("KTZ SE1=%p HWEN=%p, DSI0=%p dma_pa=%llx\n", C->Ktz.Se1, C->Ktz.Hwen, C->Dsi, C->DmaPa.QuadPart);
    if (C->Ktz.Se1 == NULL && C->Dsi == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    C->HwReady = TRUE;
    return STATUS_SUCCESS;
}

static VOID HwDeinit(PDEVICE_CONTEXT C)
{
    C->HwReady = FALSE;
    XunQupLockClose(&C->QupLock);
    if (C->DmaVa != NULL) {
        MmFreeContiguousMemorySpecifyCache(C->DmaVa, PAGE_SIZE, MmNonCached);
        C->DmaVa = NULL;
    }
    if (C->Dsi != NULL) {
        MmUnmapIoSpace((PVOID)C->Dsi, DSI0_SIZE);
        C->Dsi = NULL;
    }
    XunKtzDeinit(&C->Ktz);
}

static KSTART_ROUTINE PollThread;

static VOID PollThread(PVOID Context)
{
    PDEVICE_CONTEXT C = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;

    period.QuadPart = -10000LL * POLL_MS;
    LogPrint("poll thread started (C:\\xun\\brightness)\n");

    while (KeWaitForSingleObject(&C->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        LONG pct = ReadPercent();
        if (pct >= 0 && pct != C->LastPct) {
            if (SetLevel(C, pct)) {
                C->LastPct = pct;
            }
        }
    }
    LogPrint("poll thread exit\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- WDF -------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== XunBacklight " XUN_BL_VERSION " (KTZ8866 QUP0 SE1 + DCS fallback) ====\n");
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogClose();
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(DeviceGetContext(device), sizeof(DEVICE_CONTEXT));
        DeviceGetContext(device)->LastPct = -1;
    }
    return status;
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    LogPrint("HwInit: %08x\n", HwInit(DeviceGetContext(Device)));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    HwDeinit(DeviceGetContext(Device));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    HANDLE h;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);
    if (!ctx->HwReady) {
        return STATUS_SUCCESS;
    }
    ctx->LastPct = -1;
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&ctx->Thread, NULL);
    ZwClose(h);
    return status;
}

static NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);
    if (ctx->Thread != NULL) {
        KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ctx->Thread);
        ctx->Thread = NULL;
    }
    return STATUS_SUCCESS;
}
