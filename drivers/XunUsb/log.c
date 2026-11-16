#include "driver.h"

static HANDLE  g_LogFile;
static KMUTEX  g_LogLock;
static BOOLEAN g_LogInit;

VOID LogOpen(VOID)
{
    /* Runtime file logging disabled for clean release performance. */
    return;
}

VOID LogClose(VOID)
{
    if (g_LogFile != NULL) {
        ZwClose(g_LogFile);
        g_LogFile = NULL;
    }
}

VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    CHAR line[512];
    va_list args;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER t;
    size_t len = 0;

    if (g_LogFile == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    t.QuadPart = (LONGLONG)(KeQueryInterruptTime() / 10000);
    RtlStringCbPrintfA(line, sizeof(line), "%8llu ", (ULONGLONG)t.QuadPart);
    RtlStringCbLengthA(line, sizeof(line), &len);
    va_start(args, Fmt);
    RtlStringCbVPrintfA(line + len, sizeof(line) - len, Fmt, args);
    va_end(args);
    RtlStringCbLengthA(line, sizeof(line), &len);
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, line, (ULONG)len, NULL, NULL);
    KeReleaseMutex(&g_LogLock, FALSE);
}

