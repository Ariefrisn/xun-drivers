/*
 * Tiny file logger: C:\XunBattery.log (\??\C:\ - ".." is not resolved in NT paths).
 * Readable offline from TWRP (mount.ntfs) when Windows has no input.
 */
#include "driver.h"

static HANDLE    g_LogFile;
static KMUTEX    g_LogLock;  /* not FAST_MUTEX: ZwWriteFile needs PASSIVE_LEVEL */
static BOOLEAN   g_LogInit;

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

static VOID LogWrite(PCSTR Text, SIZE_T Len)
{
    IO_STATUS_BLOCK iosb;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "XunBattery: %s", Text);
    if (g_LogFile == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, (PVOID)Text, (ULONG)Len, NULL, NULL);
    KeReleaseMutex(&g_LogLock, FALSE);
}

VOID LogPrint(PCSTR Fmt, ...)
{
    CHAR buf[512];
    SIZE_T len = 0;
    va_list ap;

    if (g_LogFile == NULL) {
        return;
    }

    va_start(ap, Fmt);
    if (NT_SUCCESS(RtlStringCbVPrintfA(buf, sizeof(buf), Fmt, ap)) &&
        NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &len))) {
        LogWrite(buf, len);
    }
    va_end(ap);
}

VOID LogHex(PCSTR Prefix, const UCHAR *Buf, ULONG Len)
{
    CHAR line[256];
    ULONG i;
    size_t pos;

    if (g_LogFile == NULL) {
        return;
    }

    RtlStringCbCopyA(line, sizeof(line), Prefix);
    RtlStringCbLengthA(line, sizeof(line), &pos);
    for (i = 0; i < Len && pos + 4 < sizeof(line); i++) {
        RtlStringCbPrintfA(line + pos, sizeof(line) - pos, " %02x", Buf[i]);
        pos += 3;
    }
    RtlStringCbCatA(line, sizeof(line), "\n");
    RtlStringCbLengthA(line, sizeof(line), &pos);
    LogWrite(line, pos);
}
