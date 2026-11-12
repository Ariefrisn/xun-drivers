/*
 * XunBattery: battery miniclass for Redmi Pad SE (xun, sh366101@55 SE2 + sc89890h@6A SE1).
 * Root-enumerated KMDF driver, structure follows the WDK "simbatt" sample.
 */
#include <initguid.h>
#include "driver.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD XunEvtDriverUnload;
static volatile LONG g_DeviceCount;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== XunBattery v0.13 (xun: SH366101 SE2 + SC89890H + SC2150 SE1) ====\n");

    WDF_DRIVER_CONFIG_INIT(&config, XunEvtDeviceAdd);
    config.EvtDriverUnload = XunEvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    LogPrint("WdfDriverCreate: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID XunEvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogPrint("unload\n");
    LogClose();
}

NTSTATUS XunEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    if (InterlockedIncrement(&g_DeviceCount) > 1) {
        InterlockedDecrement(&g_DeviceCount);
        LogPrint("XunBattery: duplicate device node detected, rejecting duplicate instance\n");
        return STATUS_DEVICE_ALREADY_ATTACHED;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = XunEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = XunEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = XunEvtD0Entry;
    pnp.EvtDeviceD0Exit = XunEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    /* The battery class driver handles the battery IOCTLs */
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, XunWdmPreprocessDeviceControl,
                                                         IRP_MJ_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS(status)) {
        LogPrint("AssignWdmIrpPreprocessCallback: %08x\n", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    LogPrint("WdfDeviceCreate: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        InterlockedDecrement(&g_DeviceCount);
        return status;
    }
    ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device = device;
    KeInitializeSpinLock(&ctx->SnapLock);

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->ClassInitLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVICE_BATTERY, NULL);
    LogPrint("CreateDeviceInterface(GUID_DEVICE_BATTERY): %08x\n", status);
    return status;
}

NTSTATUS XunEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    status = BattHwInit(ctx);
    LogPrint("BattHwInit: %08x\n", status);
    if (NT_SUCCESS(status)) {
        BattPoll(ctx);
    }
    status = BattClassInit(ctx);
    LogPrint("BattClassInit: %08x\n", status);
    return status;
}

NTSTATUS XunEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    BattClassUnload(ctx);
    BattHwDeinit(ctx);
    InterlockedDecrement(&g_DeviceCount);
    return STATUS_SUCCESS;
}

NTSTATUS XunEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);
    if (!ctx->HwReady) {
        return STATUS_SUCCESS;
    }
    return BattThreadStart(ctx);
}

NTSTATUS XunEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    POWER_ACTION action = WdfDeviceGetSystemPowerAction(Device);

    LogPrint("D0Exit target=%u action=%u\n", (ULONG)TargetState, (ULONG)action);
    BattThreadStop(ctx);
    BattTcSinkOnly(ctx);
    /*
     * Only on shutdown/restart/sleep: on a plain driver removal (reinstall, PowerActionNone) the
     * boost also powers the hub with the flash drive the installer runs from.
     */
    if (action != PowerActionNone) {
        BattOtgOff(ctx);
    }
    return STATUS_SUCCESS;
}

NTSTATUS XunWdmPreprocessDeviceControl(WDFDEVICE Device, PIRP Irp)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    WdfWaitLockAcquire(ctx->ClassInitLock, NULL);
    if (ctx->ClassHandle != NULL) {
        status = BatteryClassIoctl(ctx->ClassHandle, Irp);
    }
    WdfWaitLockRelease(ctx->ClassInitLock);

    if (status == STATUS_NOT_SUPPORTED) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
    }
    return status;
}
