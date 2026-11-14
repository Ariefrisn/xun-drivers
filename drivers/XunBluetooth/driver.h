#pragma once

#include <ntddk.h>
#include <wdf.h>
#define INITGUID
#include <guiddef.h>
#include <BthXDDI.h>
#include "hw.h"

#define XUN_BT_VERSION   "v0.31"
#define XUN_BT_POOL_TAG  'XBtU'

typedef struct _MMIO_RANGE {
    PHYSICAL_ADDRESS Pa;
    SIZE_T           Size;
    volatile UCHAR  *Va;
} MMIO_RANGE, *PMMIO_RANGE;

static __forceinline ULONG MmioRead32(PMMIO_RANGE R, ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(R->Va + Offset));
}

static __forceinline VOID MmioWrite32(PMMIO_RANGE R, ULONG Offset, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(R->Va + Offset), Val);
}

static __forceinline ULONGLONG GetTicksMs(VOID)
{
    LARGE_INTEGER cnt, freq;
    cnt = KeQueryPerformanceCounter(&freq);
    if (freq.QuadPart == 0) {
        return 0;
    }
    return (ULONGLONG)((cnt.QuadPart * 1000ULL) / (ULONGLONG)freq.QuadPart);
}

static __forceinline VOID BusyWaitUs(ULONG Micros)
{
    KeStallExecutionProcessor(Micros);
}

static __forceinline VOID SleepMs(ULONG Millis)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10000LL * (LONGLONG)Millis;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

typedef enum _H4_RX_STAGE {
    H4_STAGE_TYPE = 0,
    H4_STAGE_HDR  = 1,
    H4_STAGE_BODY = 2
} H4_RX_STAGE;

#define H4_MAX_FRAME_BYTES 2048

typedef struct _H4_RX_PARSER {
    H4_RX_STAGE Stage;
    UCHAR       PktType;
    ULONG       HdrExpected;
    ULONG       BodyExpected;
    ULONG       Pos;
    UCHAR       Buf[H4_MAX_FRAME_BYTES];
} H4_RX_PARSER, *PH4_RX_PARSER;

typedef struct _BT_RX_PACKET {
    LIST_ENTRY Link;
    UCHAR      PktType;
    ULONG      Len;
    UCHAR      Data[1];
} BT_RX_PACKET, *PBT_RX_PACKET;

typedef struct _XUN_BT_CONTEXT {
    WDFDEVICE    Device;
    WDFQUEUE     WriteQueue;
    WDFQUEUE     ReadEventQueue;
    WDFQUEUE     ReadAclQueue;
    WDFSPINLOCK  Lock;

    MMIO_RANGE   Se;
    MMIO_RANGE   GccVote;
    MMIO_RANGE   GccQupS3;
    MMIO_RANGE   TlmmUart;
    MMIO_RANGE   TlmmSwCtrl;

    ULONG        TxDepth;
    ULONG        RxDepth;
    ULONG        SamplingRate;
    ULONG        CurrentBaud;
    BOOLEAN      DfsEnabled;
    ULONG        DfsFreqs[RCG_DFSR_LEVELS];
    BOOLEAN      UartReady;
    BOOLEAN      FwReady;
    BOOLEAN      InitFailed;

    QCA_BTSOC_VERSION SocVer;
    BOOLEAN           IsWcn3950;
    UCHAR             OperBaudCode;
    ULONG             OperBaudRate;

    /* Worker thread & synchronization */
    PKTHREAD     WorkerThread;
    KEVENT       WorkEvent;
    KEVENT       StopEvent;
    BOOLEAN      StopRequested;

    /* Buffered incoming packets & H4+IBS parser */
    LIST_ENTRY   RxEventList;
    ULONG        RxEventCount;
    LIST_ENTRY   RxAclList;
    ULONG        RxAclCount;
    H4_RX_PARSER RxParser;

    /* HCI_IBS TX state */
    BOOLEAN      TxAwake;
    BOOLEAN      ChipUartAsleep;
    ULONGLONG    LastTxActivityMs;
    ULONGLONG    LastWakeReqMs;
    ULONGLONG    LastKeepAliveMs;
    ULONG        TxPackets;
    ULONG        RxEvents;
    ULONG        RxAcls;

    /* Diagnostics */
    ULONG        LastRxIrqSt;
    ULONG        UnexpBytesLogged;
    ULONGLONG    LastTxCmdMs;
    UCHAR        LastTxCmdBuf[260];
    ULONG        LastTxCmdLen;
    BOOLEAN      AwaitingEvt;
    BOOLEAN      CmdRetried;
    BOOLEAN      DiagProbed;
    BOOLEAN      RadioDead;
} XUN_BT_CONTEXT, *PXUN_BT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUN_BT_CONTEXT, GetBtContext);

/* log.c */
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(PCSTR Fmt, ...);
VOID LogHexDump(PCSTR Prefix, const UCHAR *Data, ULONG Len);

/* uart.c */
NTSTATUS UartHwInit(PXUN_BT_CONTEXT Ctx);
VOID     UartHwDeinit(PXUN_BT_CONTEXT Ctx);
NTSTATUS UartSetBaud(PXUN_BT_CONTEXT Ctx, ULONG Baud);
VOID     UartSetFlowControl(PXUN_BT_CONTEXT Ctx, BOOLEAN DisableFlow);
VOID     UartFlushRx(PXUN_BT_CONTEXT Ctx);
NTSTATUS UartWriteSync(PXUN_BT_CONTEXT Ctx, const UCHAR *Data, ULONG Len, ULONG TimeoutMs);
ULONG    UartReadPoll(PXUN_BT_CONTEXT Ctx, UCHAR *OutBuf, ULONG MaxLen);
VOID     UartLogHwState(PXUN_BT_CONTEXT Ctx, PCSTR Tag);

/* wcn3990.c */
NTSTATUS WcnInitializeRadio(PXUN_BT_CONTEXT Ctx);
VOID     WcnProbeDeadRadio(PXUN_BT_CONTEXT Ctx);

/* bthx.c */
NTSTATUS BthxQueuesCreate(PXUN_BT_CONTEXT Ctx);
VOID     BthxIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG IoControlCode);
VOID     BthxIoInternalDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG IoControlCode);
VOID     BthxFeedRxBytes(PXUN_BT_CONTEXT Ctx, const UCHAR *Data, ULONG Len);
VOID     BthxDrainQueuesOnStop(PXUN_BT_CONTEXT Ctx);
VOID     BthxProcessTxAndSleep(PXUN_BT_CONTEXT Ctx);
VOID     BthxFlushCompletedReads(PXUN_BT_CONTEXT Ctx);

