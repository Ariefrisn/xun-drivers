#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <ntstrsafe.h>

#include "hw.h"
#include "xun_qup_lock.h"

#define XUN_POOL_TAG 'BnuX'
#define XUN_BATTERY_TAG 1

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

/* ---- GENI I2C (FIFO mode, cross-driver mutex guarded) -------------------- */

typedef struct _GENI_I2C {
    MMIO_RANGE    Se;
    ULONG         TxDepth;
    ULONG         Proto;
    ULONG         LastIrqStatus;
    PXUN_QUP_LOCK Lock;
} GENI_I2C, *PGENI_I2C;

NTSTATUS GccEnableQup0Se(_In_ ULONG VoteBits, _In_ ULONG CbcrOff, _In_ ULONG RcgOff, _Out_ PULONG ClkSel);
NTSTATUS GeniI2cInit(_Inout_ PGENI_I2C Bus, _In_ ULONGLONG SeBase, _In_ ULONG ClkSel, _In_opt_ PXUN_QUP_LOCK Lock);
VOID     GeniI2cDeinit(_Inout_ PGENI_I2C Bus);
NTSTATUS GeniI2cWrite(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len, _In_ BOOLEAN Stop);
NTSTATUS GeniI2cRead(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _Out_writes_(Len) UCHAR *Buf, _In_ ULONG Len);
NTSTATUS GeniI2cReadReg(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_writes_(Len) UCHAR *Buf, _In_ ULONG Len);
NTSTATUS I2cReadWord(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_ PUSHORT Val);
NTSTATUS I2cReadByte(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_ PUCHAR Val);
NTSTATUS I2cWriteByte(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _In_ UCHAR Val);

/* ---- Log file (C:\XunBattery.log) --------------------------------------- */

VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID LogHex(_In_z_ PCSTR Prefix, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len);

/* ---- Battery state ------------------------------------------------------- */

typedef struct _BATT_SNAPSHOT {
    BOOLEAN Valid;
    BOOLEAN Present;
    ULONG   SocTenths;
    ULONG   VoltageMv;
    LONG    CurrentMa;
    LONG    TempTenthsC;
    ULONG   FullMah;
    ULONG   Cycles;
    UCHAR   ChgReg03, ChgReg0B;
    BOOLEAN OnLine, Charging, ChargeDone;
} BATT_SNAPSHOT, *PBATT_SNAPSHOT;

/* ---- USB-PD sink (pd.c) -------------------------------------------------- */

#define PD_ST_OFF          0
#define PD_ST_WAIT_CAPS    1
#define PD_ST_WAIT_ACCEPT  2
#define PD_ST_WAIT_PS_RDY  3
#define PD_ST_READY        4
#define PD_ST_NO_PD        5

typedef struct _PD_PORT {
    ULONG     State;
    ULONGLONG StateSince;
    ULONG     TxId;
    ULONG     RxId;
    ULONG     ReqMv, ReqMa;
    ULONG     ContractMv, ContractMa;
    ULONG     Rx;
    ULONG     HardResets;
    ULONG     Rev;
    ULONG     FixPos, FixMv, FixMa;
    ULONG     PpsPos, PpsMinMv, PpsMaxMv, PpsMaxMa;
    BOOLEAN   ReqPps;
    ULONG     PpsMv;
    ULONGLONG LastReq;
    BOOLEAN   PpsTested;
    ULONG     KeepAlives;
} PD_PORT, *PPD_PORT;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE     Device;
    WDFWAITLOCK   ClassInitLock;
    PVOID         ClassHandle;

    GENI_I2C      Bus;             /* QUP0 SE1: sc89890h@6A, sc2150/rt1711h@4E */
    GENI_I2C      FgBus;           /* QUP0 SE2: sh366101@55 */
    XUN_QUP_LOCK  LockSe1;
    XUN_QUP_LOCK  LockSe2;
    TLMM_PIN      PinSda1, PinScl1;
    TLMM_PIN      PinSda2, PinScl2;
    BOOLEAN       HwReady;
    BOOLEAN       FgReady;

    KSPIN_LOCK    SnapLock;
    BATT_SNAPSHOT Snap;
    ULONG         LastNotifiedState, LastNotifiedSoc;

    HANDLE        ThreadHandle;
    PKTHREAD      Thread;
    KEVENT        StopEvent;
    ULONG         Polls;
    ULONG         LastVbusStat;
    UCHAR         TcLastCc;
    ULONG         JeitaZone;
    BOOLEAN       TcpcOk;
    BOOLEAN       TcpcIsRt1711h;
    ULONG         TcState;
    ULONG         TcCandidate;
    ULONG         TcDetachSteps;
    ULONG         TcVbusNoCcSteps;
    BOOLEAN       TcChanged;
    PD_PORT       Pd;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

EVT_WDF_DRIVER_DEVICE_ADD        XunEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE  XunEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE  XunEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY          XunEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT           XunEvtD0Exit;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS XunWdmPreprocessDeviceControl;

NTSTATUS BattHwInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattHwDeinit(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattPoll(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS BattThreadStart(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattThreadStop(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattOtgOff(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattTcSinkOnly(_In_ PDEVICE_CONTEXT Ctx);
VOID     PdAttach(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR Cc);
VOID     PdService(_In_ PDEVICE_CONTEXT Ctx);
VOID     PdDetach(_In_ PDEVICE_CONTEXT Ctx);
PCSTR    PdStateName(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS BattClassInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattClassUnload(_In_ PDEVICE_CONTEXT Ctx);
