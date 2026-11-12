#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include <ntstrsafe.h>

#include "hw.h"

#define XUN_POOL_TAG 'TnuX'

/* ---- Mapped I/O ---------------------------------------------------------- */

typedef struct _MMIO_RANGE {
    PHYSICAL_ADDRESS Pa;
    SIZE_T           Size;
    volatile UCHAR  *Va;
} MMIO_RANGE, *PMMIO_RANGE;

NTSTATUS MmioMap(_Inout_ PMMIO_RANGE Range, _In_ ULONGLONG Pa, _In_ SIZE_T Size);
VOID     MmioUnmap(_Inout_ PMMIO_RANGE Range);

__forceinline ULONG MmioRead32(_In_ PMMIO_RANGE R, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(R->Va + Off));
}

__forceinline VOID MmioWrite32(_In_ PMMIO_RANGE R, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(R->Va + Off), Val);
}

/* ---- TLMM ---------------------------------------------------------------- */

typedef struct _TLMM_PIN {
    ULONG      Pin;
    MMIO_RANGE Regs;
} TLMM_PIN, *PTLMM_PIN;

NTSTATUS TlmmPinMap(_Out_ PTLMM_PIN P, _In_ ULONGLONG Tile, _In_ ULONG Pin);
VOID     TlmmPinUnmap(_Inout_ PTLMM_PIN P);
VOID     TlmmConfig(_In_ PTLMM_PIN P, _In_ ULONG Func, _In_ ULONG Pull, _In_ ULONG DriveMa, _In_ BOOLEAN Output);
VOID     TlmmSetOutput(_In_ PTLMM_PIN P, _In_ BOOLEAN High);
BOOLEAN  TlmmGetInput(_In_ PTLMM_PIN P);

/* ---- GENI SPI (FIFO mode, polled full-duplex) ---------------------------- */

typedef struct _GENI_SPI {
    MMIO_RANGE Se;
    ULONG      TxDepth;
    ULONG      Proto;
    ULONG      LastIrqStatus;
    PUCHAR     TxScratch;
    PUCHAR     RxScratch;
    ULONG      ScratchSize;
    BOOLEAN          UseDma;
    PUCHAR           DmaTxBuf;
    PUCHAR           DmaRxBuf;
    PHYSICAL_ADDRESS DmaTxPa;
    PHYSICAL_ADDRESS DmaRxPa;
} GENI_SPI, *PGENI_SPI;

NTSTATUS GccEnableQup0Se0(VOID);
NTSTATUS GeniSpiInit(_Inout_ PGENI_SPI Bus, _In_ ULONGLONG SeBase);
VOID     GeniSpiDeinit(_Inout_ PGENI_SPI Bus);
NTSTATUS GeniSpiXfer(_In_ PGENI_SPI Bus, _In_reads_bytes_(Len) const UCHAR *Tx, _Out_writes_bytes_(Len) UCHAR *Rx, _In_ ULONG Len);

/* ---- FocalTech SPI v2 protocol + CRC16 (poly 0x8408) --------------------- */

NTSTATUS FtsSpiWrite(_In_ PGENI_SPI Bus, _In_reads_bytes_(Len) const UCHAR *Buf, _In_ ULONG Len);
NTSTATUS FtsSpiRead(_In_ PGENI_SPI Bus, _In_ UCHAR Reg, _Out_writes_bytes_(Len) UCHAR *Buf, _In_ ULONG Len);
NTSTATUS FtsWriteReg(_In_ PGENI_SPI Bus, _In_ UCHAR Reg, _In_ UCHAR Val);
NTSTATUS FtsReadReg(_In_ PGENI_SPI Bus, _In_ UCHAR Reg, _Out_ PUCHAR Val);

/* ---- Log file (C:\XunTouch.log) ---------------------------------------- */

VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID LogHex(_In_z_ PCSTR Prefix, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len);

/* ---- Device -------------------------------------------------------------- */

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE    Device;
    VHFHANDLE    Vhf;
    BOOLEAN      VhfStarted;

    GENI_SPI     Bus;
    TLMM_PIN     PinSpi[4], PinIrq, PinReset;
    BOOLEAN      HwReady;
    BOOLEAN      Flip180;

    HANDLE       ThreadHandle;
    PKTHREAD     Thread;
    KEVENT       StopEvent;

    ULONG        RawLogged;
    ULONG        MaxRawX, MaxRawY;
    USHORT       ActiveMask;
    USHORT       LastX[TS_MAX_CONTACTS], LastY[TS_MAX_CONTACTS];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

EVT_WDF_DRIVER_DEVICE_ADD        XunEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE  XunEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE  XunEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY          XunEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT           XunEvtD0Exit;

NTSTATUS TouchVhfCreate(_In_ PDEVICE_CONTEXT Ctx);
VOID     TouchVhfDelete(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS TouchHwInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     TouchHwDeinit(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS TouchThreadStart(_In_ PDEVICE_CONTEXT Ctx);
VOID     TouchThreadStop(_In_ PDEVICE_CONTEXT Ctx);
