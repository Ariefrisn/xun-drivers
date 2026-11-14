#include "driver.h"
#include <stdarg.h>
#include <ntstrsafe.h>

static HANDLE    g_LogFile = NULL;
static KSPIN_LOCK g_LogLock;
static BOOLEAN   g_LogLockInit = FALSE;

VOID LogOpen(VOID)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st;

    if (!g_LogLockInit) {
        KeInitializeSpinLock(&g_LogLock);
        g_LogLockInit = TRUE;
    }

    if (g_LogFile != NULL) {
        return;
    }

    RtlInitUnicodeString(&path, L"\\??\\C:\\XunBluetooth.log");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&g_LogFile,
                      FILE_APPEND_DATA | SYNCHRONIZE,
                      &oa,
                      &iosb,
                      NULL,
                      FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      FILE_OPEN_IF,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                      NULL,
                      0);
    if (!NT_SUCCESS(st)) {
        g_LogFile = NULL;
    }
}

VOID LogClose(VOID)
{
    if (g_LogFile != NULL) {
        ZwClose(g_LogFile);
        g_LogFile = NULL;
    }
}

VOID LogPrint(PCSTR Fmt, ...)
{
    CHAR buf[512];
    CHAR stamped[560];
    va_list ap;
    size_t len = 0;
    IO_STATUS_BLOCK iosb;
    KIRQL oldIrql;

    va_start(ap, Fmt);
    RtlStringCbVPrintfA(buf, sizeof(buf), Fmt, ap);
    va_end(ap);

    RtlStringCbPrintfA(stamped, sizeof(stamped), "[%llu ms] %s", GetTicksMs(), buf);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "XunBluetooth: %s", stamped);

    if (g_LogFile != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        RtlStringCbLengthA(stamped, sizeof(stamped), &len);
        if (len > 0) {
            if (NT_SUCCESS(ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, stamped, (ULONG)len, NULL, NULL))) {
                ZwFlushBuffersFile(g_LogFile, &iosb);
            }
        }
    } else {
        UNREFERENCED_PARAMETER(oldIrql);
    }
}

VOID LogHexDump(PCSTR Prefix, const UCHAR *Data, ULONG Len)
{
    CHAR line[256];
    ULONG n = (Len > 32) ? 32 : Len;
    ULONG pos = 0;
    ULONG i;

    RtlStringCbPrintfA(line, sizeof(line), "%s (%uB):", Prefix, Len);
    RtlStringCbLengthA(line, sizeof(line), (size_t *)&pos);
    for (i = 0; i < n && pos + 4 < sizeof(line); i++) {
        RtlStringCbPrintfA(line + pos, sizeof(line) - pos, " %02x", Data[i]);
        pos += 3;
    }
    LogPrint("%s\n", line);
}
