/*
 * drivers/common/xun_qup_lock.h - Cross-driver QUPv3 SE1 / SE2 mutual exclusion for XUN (SM6225).
 *
 * QUP0 SE1 (0x04A84000) is shared by: XunBattery (bq2589x@6A + sc2150@4E), XunBacklight (ktz8866@11),
 * XunAudio / XunSpeaker (left amp fs16xx@34/@35 or sipa@28/@29).
 * QUP0 SE2 (0x04A88000) is shared by: XunBattery (sh366101@55) and XunSpeaker (right amp).
 *
 * Uses a named kernel Mutant (\BaseNamedObjects\XunQup0Se{1,2}Lock) at PASSIVE_LEVEL so independent
 * .sys drivers serialize FIFO transfers without polling races or deadlocks (bounded 500 ms wait).
 */
#pragma once

#include <ntddk.h>

#define XUN_QUP_SE1_MUTANT_NAME L"\\BaseNamedObjects\\XunQup0Se1Lock"
#define XUN_QUP_SE2_MUTANT_NAME L"\\BaseNamedObjects\\XunQup0Se2Lock"

typedef struct _XUN_QUP_LOCK {
    HANDLE MutexHandle;
} XUN_QUP_LOCK, *PXUN_QUP_LOCK;

static __inline NTSTATUS XunQupLockInit(_Out_ PXUN_QUP_LOCK Lock, _In_ ULONG SeIndex)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;
    WCHAR buf[64];

    Lock->MutexHandle = NULL;
    if (SeIndex == 1) {
        RtlInitUnicodeString(&name, XUN_QUP_SE1_MUTANT_NAME);
    } else if (SeIndex == 2) {
        RtlInitUnicodeString(&name, XUN_QUP_SE2_MUTANT_NAME);
    } else {
        (void)buf;
        return STATUS_INVALID_PARAMETER;
    }

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, NULL);
    st = ZwCreateMutant(&Lock->MutexHandle, MUTANT_ALL_ACCESS, &oa, FALSE);
    if (!NT_SUCCESS(st)) {
        Lock->MutexHandle = NULL;
    }
    return st;
}

static __inline BOOLEAN XunQupLockAcquire(_In_ PXUN_QUP_LOCK Lock, _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER to;
    NTSTATUS st;

    if (Lock->MutexHandle == NULL) {
        return TRUE; /* fallback: single-driver early bringup before Object Manager ready */
    }
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        return FALSE;
    }
    to.QuadPart = -10000LL * (LONGLONG)TimeoutMs;
    st = ZwWaitForSingleObject(Lock->MutexHandle, FALSE, &to);
    return (st == STATUS_SUCCESS || st == STATUS_ABANDONED_WAIT_0);
}

static __inline VOID XunQupLockRelease(_In_ PXUN_QUP_LOCK Lock)
{
    if (Lock->MutexHandle != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        LONG prev = 0;
        ZwReleaseMutant(Lock->MutexHandle, &prev);
    }
}

static __inline VOID XunQupLockClose(_Inout_ PXUN_QUP_LOCK Lock)
{
    if (Lock->MutexHandle != NULL) {
        ZwClose(Lock->MutexHandle);
        Lock->MutexHandle = NULL;
    }
}

