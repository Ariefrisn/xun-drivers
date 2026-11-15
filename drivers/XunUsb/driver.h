#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <stdarg.h>

#define XUN_USB_VERSION "v0.1"

VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
