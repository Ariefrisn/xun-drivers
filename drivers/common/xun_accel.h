#pragma once

#define XUN_ACCEL_CALLBACK_NAME L"\\Callback\\XunAccel"

typedef struct _XUN_ACCEL_SAMPLE {
    LONG  AxMg; /* X acceleration in milli-G (1000 = 1.0G) */
    LONG  AyMg; /* Y acceleration in milli-G */
    LONG  AzMg; /* Z acceleration in milli-G */
    ULONG Seq;
} XUN_ACCEL_SAMPLE, *PXUN_ACCEL_SAMPLE;
