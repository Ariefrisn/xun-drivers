/*
 * XunWifi: driver/device setup (WDK wificx sample driver.cpp + device.cpp) and the modem thread
 * (XunModem driver.c): the WLAN core boots the modem DSP through TrustZone and then runs its
 * polling loop forever on one thread. It boots on every start (Wi-Fi needs it) unless
 * C:\xun\modem.off exists; the boot guard below stops a crashing driver from starting again.
 */
#include "wpch.h"

XUN_WIFI_DEVICE *g_Wifi;
const UCHAR g_WifiMac[6] = { 0x02, 0x54, 0x4F, 0x50, 0x41, 0x5A };

extern "C" DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;

static volatile LONG s_ModemActive = 0;
static volatile LONG s_WifiDeviceAdded = 0;

/* TRUE if Path exists (and Delete: it is deleted). */
static BOOLEAN FileProbe(PCWSTR Path, BOOLEAN Delete)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, (Delete ? DELETE : FILE_READ_ATTRIBUTES) | SYNCHRONIZE, &oa, &iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                 (Delete ? FILE_DELETE_ON_CLOSE : 0) | FILE_SYNCHRONOUS_IO_NONALERT |
                                 FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

/*
 * Boot guard: a Net driver loads on every boot, so a crash in its setup would repeat forever.
 * DeviceAdd creates C:\xun\wifi.boot and refuses to start if it already exists (the previous
 * start never got 20 s in); a passive timer deletes it after 20 s of normal running.
 * install-wifi.cmd deletes it before every install.
 */
static BOOLEAN GuardEnter(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\xun\\wifi.boot");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS s;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    s = ZwCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_CREATE,
                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (s == STATUS_OBJECT_NAME_COLLISION) {
        return FALSE;
    }
    if (NT_SUCCESS(s)) {
        ZwClose(h);
    }
    return TRUE;                                       /* no C:\xun yet etc.: do not block */
}

static VOID GuardLeave(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\xun\\wifi.boot");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                                FILE_DELETE_ON_CLOSE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                NULL, 0))) {
        ZwClose(h);
    }
}

static VOID EvtGuardTimer(WDFTIMER Timer)
{
    UNREFERENCED_PARAMETER(Timer);
    GuardLeave();
    WLOG("boot guard: 20 s up, C:\\xun\\wifi.boot removed\r\n");
}

static VOID GuardTimerStart(WDFDEVICE Device)
{
    WDF_TIMER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFTIMER timer;

    WDF_TIMER_CONFIG_INIT(&config, EvtGuardTimer);
    config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Device;
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    if (NT_SUCCESS(WdfTimerCreate(&config, &attrs, &timer))) {
        WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(20000));
    }
}

static VOID ModemThread(PVOID Context)
{
    LONG_PTR s;

    UNREFERENCED_PARAMETER(Context);
    KeSetSystemAffinityThreadEx((KAFFINITY)1);         /* TZ resumes + polling stay on one core */
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY - 2);
    /* Wi-Fi needs the modem on every boot: boot it unless C:\xun\modem.off exists. A crash here is
       caught by the boot guard (C:\xun\wifi.boot). modem.arm (XunModem's one-shot) is consumed too. */
    FileProbe(L"\\??\\C:\\xun\\modem.arm", TRUE);
    if (FileProbe(L"\\??\\C:\\xun\\modem.off", FALSE)) {
        WLOG("C:\\xun\\modem.off exists: modem not started\r\n");
        PsTerminateSystemThread(STATUS_SUCCESS);
    }
    WLOG("booting the modem\r\n");
    ExSetTimerResolution(10000, TRUE);
    s = ModemPasTest();                                /* returns only on gModemStop or a failure */
    WLOG("ModemPasTest returned %llx%s\r\n", (ULONGLONG)s, gModemStop ? " (driver stop)" : "");
    LogSetLazy(FALSE);
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    WLOG("\r\n==== XunWifi " XUN_WIFI_VERSION " ====\r\n");
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    config.DriverPoolTag = XUN_WIFI_TAG;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    WLOG("WdfDriverCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static void StartModemWorker(XUN_WIFI_DEVICE *Dev)
{
    HANDLE h;
    NTSTATUS status;

    if (Dev == nullptr || Dev->ModemThread != nullptr) {
        return;
    }
    gModemStop = FALSE;
    InterlockedExchange(&s_ModemActive, 1);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, ModemThread, NULL);
    if (NT_SUCCESS(status)) {
        ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                  (PVOID *)&Dev->ModemThread, NULL);
        ZwClose(h);
    } else {
        InterlockedExchange(&s_ModemActive, 0);
        WLOG("PsCreateSystemThread: %08x\r\n", status);
    }
}

static void StopModemAndShutdown(XUN_WIFI_DEVICE *Dev, PCSTR Reason)
{
    WLOG("%s\r\n", Reason);
    if (Dev != nullptr && Dev->ModemThread != nullptr) {
        gModemStop = TRUE;
        KeWaitForSingleObject(Dev->ModemThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Dev->ModemThread);
        Dev->ModemThread = nullptr;
    }
    if (InterlockedExchange(&s_ModemActive, 0) != 0) {
        ModemShutdown();
    }
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    StopModemAndShutdown(nullptr, "unload");
    g_Wifi = nullptr;
    LogClose();
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WIFI_DEVICE_CONFIG wifiConfig;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    if (InterlockedCompareExchange(&s_WifiDeviceAdded, 1, 0) != 0) {
        WLOG("duplicate wifi device node detected, rejecting\r\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    GuardLeave();
    status = NetDeviceInitConfig(DeviceInit);          /* data path: NetAdapterCx */
    WLOG("NetDeviceInitConfig: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WifiDeviceInitConfig(DeviceInit);         /* control path: WiFiCx */
    WLOG("WifiDeviceInitConfig: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, XUN_WIFI_DEVICE);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    WLOG("WdfDeviceCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WIFI_DEVICE_CONFIG_INIT(&wifiConfig, WDI_VERSION_LATEST, EvtWifiSendCommand, EvtWifiCreateAdapter,
                            EvtWifiCreateWifiDirectDevice);
    status = WifiDeviceInitialize(device, &wifiConfig);
    WLOG("WifiDeviceInitialize: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    g_Wifi = WifiDev(device);
    g_Wifi->WdfTriageInfoPtr = WdfGetTriageInfo();
    g_Wifi->Device = device;
    g_Wifi->Tlv.AllocationContext = 0;
    g_Wifi->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(device);
    WLOG("OS WDI version %08x (driver %08x)\r\n", g_Wifi->Tlv.PeerVersion, (ULONG)WDI_VERSION_LATEST);
    return STATUS_SUCCESS;
}

/* WiFiCx creates the station adapter (EvtWifiCreateAdapter) after this succeeds, so the Wi-Fi
   capabilities must be set here. They are static (WCN3950: 1x1, 2.4 + 5 GHz); the firmware boots
   in the background and scans wait for it (WlanOnReady). */
static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    NTSTATUS status;
    XUN_WIFI_DEVICE *dev = WifiDev(Device);

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    status = WifiSetCapabilities(Device);
    WLOG("WifiSetCapabilities: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    dev->Prepared = TRUE;
    GuardTimerStart(Device);
    StartModemWorker(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    XUN_WIFI_DEVICE *dev = WifiDev(Device);

    WLOG("D0Entry prev=%u\r\n", (ULONG)PreviousState);
    if (dev->Prepared && dev->ModemThread == nullptr) {
        StartModemWorker(dev);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    XUN_WIFI_DEVICE *dev = WifiDev(Device);

    WLOG("D0Exit target=%u\r\n", (ULONG)TargetState);
    if (TargetState != WdfPowerDeviceD0) {
        StopModemAndShutdown(dev, "D0Exit shutdown");
    }
    return STATUS_SUCCESS;
}

/* Device removal: stop CE/GLINK/SMP2P and shut down MPSS + IPA cleanly so reload never collides. */
static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    XUN_WIFI_DEVICE *dev = WifiDev(Device);

    UNREFERENCED_PARAMETER(Translated);
    dev->Prepared = FALSE;
    StopModemAndShutdown(dev, "ReleaseHardware");
    return STATUS_SUCCESS;
}
