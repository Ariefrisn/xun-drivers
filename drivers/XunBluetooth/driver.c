/*
 * XunBluetooth - KMDF LowerFilter under BthMini.sys for Redmi Pad SE (xun).
 */
#include "driver.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD BtEvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD BtEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE BtEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE BtEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY BtEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT BtEvtD0Exit;
static KSTART_ROUTINE BtWorkerThread;

static VOID BtWorkerThread(PVOID Context) {
  PXUN_BT_CONTEXT ctx = (PXUN_BT_CONTEXT)Context;
  NTSTATUS st;
  PVOID events[2];
  UCHAR rxBuf[512];

  LogPrint("worker: initializing WCN3990/WCN3950 over QUP0 SE3...\n");
  st = WcnInitializeRadio(ctx);
  if (!NT_SUCCESS(st)) {
    LogPrint("worker: radio init FAILED: %08x\n", st);
    ctx->InitFailed = TRUE;
    BthxDrainQueuesOnStop(ctx);
    PsTerminateSystemThread(st);
    return;
  }

  ctx->FwReady = TRUE;
  LogPrint("worker: radio UP at %u baud; entering H4/IBS loop\n",
           ctx->CurrentBaud);

  events[0] = &ctx->StopEvent;
  events[1] = &ctx->WorkEvent;

  while (!ctx->StopRequested) {
    ULONG totalRead = 0;
    ULONG n;

    BthxProcessTxAndSleep(ctx);
    BthxFlushCompletedReads(ctx);

    /* Drain all immediately available RX FIFO bytes before sleeping */
    do {
      n = UartReadPoll(ctx, rxBuf, sizeof(rxBuf));
      if (n > 0) {
        BthxFeedRxBytes(ctx, rxBuf, n);
        totalRead += n;
      }
    } while (n > 0 && !ctx->StopRequested && totalRead < 4096);

    BthxFlushCompletedReads(ctx);

    if (ctx->StopRequested) {
      break;
    }

    if (ctx->AwaitingEvt && !ctx->RadioDead && !ctx->CmdRetried &&
        (GetTicksMs() - ctx->LastTxCmdMs) > 300ULL) {
      ULONG nextBaud = (ctx->CurrentBaud == 115200u)
                           ? (ctx->OperBaudRate ? ctx->OperBaudRate : 3000000u)
                           : 115200u;
      LogPrint("bthx: no EVT after 300ms at %u baud, trying %u baud (ios=%08x st=%08x)\n",
               ctx->CurrentBaud, nextBaud,
               MmioRead32(&ctx->Se, SE_GENI_IOS),
               MmioRead32(&ctx->Se, SE_GENI_STATUS));
      (VOID)UartSetBaud(ctx, nextBaud);
      UartFlushRx(ctx);
      if (ctx->LastTxCmdLen > 0 && ctx->LastTxCmdLen <= sizeof(ctx->LastTxCmdBuf)) {
        UCHAR resendBuf[sizeof(ctx->LastTxCmdBuf)];
        ULONG resendLen = ctx->LastTxCmdLen;
        RtlCopyMemory(resendBuf, ctx->LastTxCmdBuf, resendLen);
        (VOID)UartWriteSync(ctx, resendBuf, resendLen, 500);
      }
      ctx->CmdRetried = TRUE;
    }

    if (ctx->AwaitingEvt && !ctx->RadioDead &&
        (GetTicksMs() - ctx->LastTxCmdMs) > 8000ULL) {
      ctx->RadioDead = TRUE;
      LogPrint("bthx: radio unresponsive >8s; failing device cleanly so "
               "OS/WiFi/Shutdown do not hang\n");
      BthxDrainQueuesOnStop(ctx);
      WdfDeviceSetFailed(ctx->Device, WdfDeviceFailedNoRestart);
      break;
    }

    if (totalRead == 0) {
      LARGE_INTEGER timeout;
      timeout.QuadPart = -10000LL; /* 1 ms poll tick */
      KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode, FALSE,
                               &timeout, NULL);
    }
  }

  LogPrint("worker: stopping (tx=%u rx_evt=%u rx_acl=%u)\n", ctx->TxPackets,
           ctx->RxEvents, ctx->RxAcls);
  PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG config;
  NTSTATUS status;

  LogOpen();
  LogPrint("==== XunBluetooth %s (SM6225 QUP0 SE3 WCN3990/WCN3950 BTHX) ====\n",
           XUN_BT_VERSION);

  WDF_DRIVER_CONFIG_INIT(&config, BtEvtDeviceAdd);
  config.EvtDriverUnload = BtEvtDriverUnload;
  status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
  LogPrint("WdfDriverCreate: %08x\n", status);
  if (!NT_SUCCESS(status)) {
    LogClose();
  }
  return status;
}

static VOID BtEvtDriverUnload(WDFDRIVER Driver) {
  UNREFERENCED_PARAMETER(Driver);
  LogPrint("unload\n");
  LogClose();
}

static NTSTATUS BtEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit) {
  WDF_PNPPOWER_EVENT_CALLBACKS pnp;
  WDF_OBJECT_ATTRIBUTES attrs;
  WDFDEVICE device;
  PXUN_BT_CONTEXT ctx;
  NTSTATUS status;

  UNREFERENCED_PARAMETER(Driver);

  /* LowerFilter directly below BthMini.sys */
  WdfFdoInitSetFilter(DeviceInit);

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
  pnp.EvtDevicePrepareHardware = BtEvtPrepareHardware;
  pnp.EvtDeviceReleaseHardware = BtEvtReleaseHardware;
  pnp.EvtDeviceD0Entry = BtEvtD0Entry;
  pnp.EvtDeviceD0Exit = BtEvtD0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, XUN_BT_CONTEXT);
  status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
  LogPrint("WdfDeviceCreate(filter): %08x\n", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  ctx = GetBtContext(device);
  RtlZeroMemory(ctx, sizeof(*ctx));
  ctx->Device = device;
  KeInitializeEvent(&ctx->WorkEvent, SynchronizationEvent, FALSE);
  KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);

  status = BthxQueuesCreate(ctx);
  LogPrint("BthxQueuesCreate: %08x\n", status);
  return status;
}

static NTSTATUS BtEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw,
                                     WDFCMRESLIST Translated) {
  PXUN_BT_CONTEXT ctx = GetBtContext(Device);
  NTSTATUS status;

  UNREFERENCED_PARAMETER(Raw);
  UNREFERENCED_PARAMETER(Translated);

  status = UartHwInit(ctx);
  LogPrint("UartHwInit: %08x\n", status);
  return status;
}

static NTSTATUS BtEvtReleaseHardware(WDFDEVICE Device,
                                     WDFCMRESLIST Translated) {
  PXUN_BT_CONTEXT ctx = GetBtContext(Device);

  UNREFERENCED_PARAMETER(Translated);

  BthxDrainQueuesOnStop(ctx);
  UartHwDeinit(ctx);
  return STATUS_SUCCESS;
}

static NTSTATUS BtEvtD0Entry(WDFDEVICE Device,
                             WDF_POWER_DEVICE_STATE PreviousState) {
  PXUN_BT_CONTEXT ctx = GetBtContext(Device);
  HANDLE hThread = NULL;
  OBJECT_ATTRIBUTES oa;
  NTSTATUS status;

  UNREFERENCED_PARAMETER(PreviousState);

  if (!ctx->UartReady) {
    return STATUS_DEVICE_NOT_READY;
  }

  LogOpen();
  ctx->StopRequested = FALSE;
  ctx->FwReady = FALSE;
  ctx->InitFailed = FALSE;
  RtlZeroMemory(&ctx->RxParser, sizeof(ctx->RxParser));
  KeClearEvent(&ctx->StopEvent);

  InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
  status = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                BtWorkerThread, ctx);
  if (NT_SUCCESS(status)) {
    ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, NULL, KernelMode,
                              (PVOID *)&ctx->WorkerThread, NULL);
    ZwClose(hThread);
  }
  LogPrint("BtEvtD0Entry worker start: %08x\n", status);
  return status;
}

static NTSTATUS BtEvtD0Exit(WDFDEVICE Device,
                            WDF_POWER_DEVICE_STATE TargetState) {
  PXUN_BT_CONTEXT ctx = GetBtContext(Device);

  LogPrint("D0Exit target=%u\n", (ULONG)TargetState);
  ctx->StopRequested = TRUE;
  BthxDrainQueuesOnStop(ctx);
  KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
  KeSetEvent(&ctx->WorkEvent, IO_NO_INCREMENT, FALSE);
  if (ctx->WorkerThread != NULL) {
    KeWaitForSingleObject(ctx->WorkerThread, Executive, KernelMode, FALSE,
                          NULL);
    ObDereferenceObject(ctx->WorkerThread);
    ctx->WorkerThread = NULL;
  }
  ctx->FwReady = FALSE;
  LogClose();
  return STATUS_SUCCESS;
}
