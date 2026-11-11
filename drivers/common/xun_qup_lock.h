/*
 * drivers/common/xun_qup_lock.h - Cross-driver QUPv3 SE1 / SE2 mutual exclusion for XUN (SM6225).
 *
 * QUP0 SE1 (0x04A84000) is shared by: XunBattery (sc89890h@6A + sc2150@4E), XunBacklight (ktz8866@11),
 * XunDisplay / XunGpuW (ktz8866@11), and XunAudio / XunSpeaker (left amp fs16xx@34/@35 or sipa@28/@29).
 * QUP0 SE2 (0x04A88000) is shared by: XunBattery (sh366101@55) and XunSpeaker (right amp).
 *
 * Uses a named kernel SynchronizationEvent (\BaseNamedObjects\XunQup0Se{1,2}Lock, initial state
 * Signaled) via IoCreateSynchronizationEvent so independent .sys drivers serialize FIFO transfers
 * cleanly with standard WDM APIs.
 */
#pragma once

#ifndef _WDMDDK_
#include <ntddk.h>
#endif

#define XUN_QUP_SE1 1u
#define XUN_QUP_SE2 2u

#define XUN_QUP_SE1_EVENT_NAME L"\\BaseNamedObjects\\XunQup0Se1Lock"
#define XUN_QUP_SE2_EVENT_NAME L"\\BaseNamedObjects\\XunQup0Se2Lock"

typedef struct _XUN_QUP_LOCK {
    HANDLE  EventHandle;
    PKEVENT Event;
    ULONG   SeIndex;
} XUN_QUP_LOCK, *PXUN_QUP_LOCK;

static __inline NTSTATUS XunQupLockInit(_Out_ PXUN_QUP_LOCK Lock, _In_ ULONG SeIndex)
{
    UNICODE_STRING name;

    Lock->EventHandle = NULL;
    Lock->Event = NULL;
    Lock->SeIndex = SeIndex;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_LEVEL;
    }

    if (SeIndex == XUN_QUP_SE1) {
        RtlInitUnicodeString(&name, XUN_QUP_SE1_EVENT_NAME);
    } else if (SeIndex == XUN_QUP_SE2) {
        RtlInitUnicodeString(&name, XUN_QUP_SE2_EVENT_NAME);
    } else {
        return STATUS_INVALID_PARAMETER;
    }

    Lock->Event = IoCreateSynchronizationEvent(&name, &Lock->EventHandle);
    if (Lock->Event == NULL) {
        Lock->EventHandle = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static __inline BOOLEAN XunQupLockAcquire(_Inout_ PXUN_QUP_LOCK Lock, _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER to;
    NTSTATUS st;

    if (Lock == NULL) {
        return TRUE;
    }
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        return FALSE;
    }
    if (Lock->Event == NULL && (Lock->SeIndex == XUN_QUP_SE1 || Lock->SeIndex == XUN_QUP_SE2)) {
        XunQupLockInit(Lock, Lock->SeIndex);
    }
    if (Lock->Event == NULL) {
        return TRUE; /* fallback: single-driver early bringup before Object Manager ready */
    }
    to.QuadPart = -10000LL * (LONGLONG)TimeoutMs;
    st = KeWaitForSingleObject(Lock->Event, Executive, KernelMode, FALSE, &to);
    return (st == STATUS_SUCCESS);
}

static __inline VOID XunQupLockRelease(_In_ PXUN_QUP_LOCK Lock)
{
    if (Lock != NULL && Lock->Event != NULL && KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        KeSetEvent(Lock->Event, IO_NO_INCREMENT, FALSE);
    }
}

static __inline VOID XunQupLockClose(_Inout_ PXUN_QUP_LOCK Lock)
{
    if (Lock != NULL && Lock->EventHandle != NULL) {
        ZwClose(Lock->EventHandle);
        Lock->EventHandle = NULL;
        Lock->Event = NULL;
    }
}
