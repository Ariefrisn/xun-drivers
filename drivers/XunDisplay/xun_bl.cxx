/*
 * xun_bl.cxx - Windows brightness slider for Xiaomi Redmi Pad SE (xun, m84_42_03_0c + KTZ8866).
 *
 * Verified from ktz8866_leds.ko & dtbo_b.img:
 *   - Primary backlight controller: KTZ8866 @ I2C 0x11 on QUP0 SE1 (0x04A84000, GPIO 4/5 WEST)
 *   - HWEN pin: TLMM GPIO 102 (TLMM_TILE_SOUTH = 0x00500000)
 *   - Registers: 0x09 power (0x9C -> 5ms -> 0x9E enable; 0x98 off),
 *                0x04 brightness LSB[2:0] = val & 0x7, 0x05 brightness MSB[10:3] = val >> 3 (0..0x7FF)
 *   - Cross-driver locking via xun_qup_lock.h (\BaseNamedObjects\XunQup0Se1Lock) so XunBattery /
 *     XunBacklight / XunAudio never collide on QUP0 SE1.
 *   - Optional C:\xun\bl.dcs flag falls back to DSI0 DCS 0x51 if ever tested on another panel.
 */
#include <initguid.h>
#include "BDD.hxx"
#include "xun_qup_lock.h"

#define TLMM_BASE              0x00400000ULL
#define TLMM_TILE_WEST         0x00100000ULL
#define TLMM_TILE_SOUTH        0x00500000ULL
#define TLMM_PIN_PA(t, p)      (TLMM_BASE + (t) + (ULONGLONG)(p) * 0x1000ULL)
#define KTZ_HWEN_PIN           102

#define QUP0_SE1_PA            0x04A84000ULL
#define GENI_FW_REVISION_RO    0x68
#define SE_GENI_STATUS         0x40
#define GENI_SER_M_CLK_CFG     0x48
#define SE_GENI_CLK_SEL        0x7C
#define SE_GENI_FORCE_DEF      0x20
#define SE_GENI_OUTPUT_CTRL    0x24
#define SE_GENI_CGC_CTRL       0x28
#define SE_GENI_DMA_MODE_EN    0x258
#define SE_GENI_TX_PACK0       0x260
#define SE_GENI_TX_PACK1       0x264
#define SE_I2C_TX_TRANS_LEN    0x26C
#define SE_I2C_SCL_COUNTERS    0x278
#define SE_GENI_M_CMD0         0x600
#define SE_GENI_M_CMD_CTRL     0x604
#define SE_GENI_M_IRQ_STATUS   0x610
#define SE_GENI_M_IRQ_EN       0x614
#define SE_GENI_M_IRQ_CLEAR    0x618
#define SE_GENI_TX_FIFOn       0x700
#define SE_GSI_EVENT_EN        0xE18
#define SE_IRQ_EN              0xE1C
#define M_CMD_DONE             (1u << 0)
#define M_I2C_ERR_MASK         ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 13))

#define KTZ8866_ADDR           0x11
#define KTZ_REG_BL_LSB         0x04
#define KTZ_REG_BL_MSB         0x05
#define KTZ_REG_BL_EN          0x09

#define DSI0_PA                0x05E94000ULL
#define DSI0_SIZE              0x400
#define DSI_CTRL               0x004
#define DSI_CMD_DMA_CTRL       0x03C
#define DSI_DMA_CMD_OFFSET     0x048
#define DSI_DMA_CMD_LENGTH     0x04C
#define DSI_TRIG_CTRL          0x084
#define DSI_DMA_SW_TRIGGER     0x090
#define DSI_INT_CTRL           0x110
#define DSI_TPG_CTRL           0x15C
#define CTRL_ENABLE            (1u << 0)
#define CTRL_CMD_MODE_EN       (1u << 2)
#define DMA_CTRL_LOW_POWER     (1u << 26)
#define INT_CMD_DMA_DONE       (1u << 0)
#define TRIG_DMA_SW            4u
#define BL_MAX                 0x7FF

static volatile UCHAR *g_Se1;
static volatile UCHAR *g_Hwen;
static volatile UCHAR *g_Dsi;
static PUCHAR g_DmaVa;
static PHYSICAL_ADDRESS g_DmaPa;
static UCHAR g_Brightness = 80;
static BOOLEAN g_KtzOn = TRUE;
static KMUTEX g_BlLocalLock;
static XUN_QUP_LOCK g_QupLock;

static ULONG Rd(volatile UCHAR *B, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(B + Off)); }
static VOID Wr(volatile UCHAR *B, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), V); }

static BOOLEAN FileExists(PCWSTR Path)
{
    UNICODE_STRING n;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    HANDLE h;

    RtlInitUnicodeString(&n, Path);
    InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &io, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

static BOOLEAN Se1EnsureI2c(VOID)
{
    ULONG fw;
    if (g_Se1 == NULL) {
        return FALSE;
    }
    fw = (Rd(g_Se1, GENI_FW_REVISION_RO) >> 8) & 0xFF;
    if (fw != 3) {
        return FALSE;
    }
    if ((Rd(g_Se1, GENI_SER_M_CLK_CFG) & 1u) == 0) {
        Wr(g_Se1, GENI_SER_M_CLK_CFG, (2u << 4) | 1u);
        Wr(g_Se1, SE_I2C_SCL_COUNTERS, (5u << 20) | (12u << 10) | 24u);
    }
    return TRUE;
}

static BOOLEAN KtzWriteReg(UCHAR Reg, UCHAR Val)
{
    ULONG word = (ULONG)Reg | ((ULONG)Val << 8);
    ULONG i, st = 0;

    if (!Se1EnsureI2c()) {
        return FALSE;
    }
    Wr(g_Se1, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    Wr(g_Se1, SE_I2C_TX_TRANS_LEN, 2);
    Wr(g_Se1, SE_GENI_M_CMD0, (1u << 27) | ((ULONG)KTZ8866_ADDR << 9));
    Wr(g_Se1, SE_GENI_TX_FIFOn, word);
    for (i = 0; i < 20000; i++) {
        st = Rd(g_Se1, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE | M_I2C_ERR_MASK)) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    Wr(g_Se1, SE_GENI_M_IRQ_CLEAR, st);
    if ((st & M_I2C_ERR_MASK) || !(st & M_CMD_DONE)) {
        Wr(g_Se1, SE_GENI_M_CMD_CTRL, (1u << 2));
        KeStallExecutionProcessor(50);
        Wr(g_Se1, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN DsiLongWrite(const UCHAR *Payload, ULONG Len)
{
    ULONG size = (4 + Len + 3) & ~3u, ctrl, dmaCtrl, trig, waited;
    BOOLEAN done = FALSE;

    if (g_Dsi == NULL || g_DmaVa == NULL || Len > 60) {
        return FALSE;
    }
    RtlFillMemory(g_DmaVa, 64, 0xFF);
    g_DmaVa[0] = (UCHAR)Len;
    g_DmaVa[1] = (UCHAR)(Len >> 8);
    g_DmaVa[2] = 0x39;
    g_DmaVa[3] = 0x80 | 0x40;
    RtlCopyMemory(g_DmaVa + 4, Payload, Len);
    KeMemoryBarrier();

    ctrl = Rd(g_Dsi, DSI_CTRL);
    trig = Rd(g_Dsi, DSI_TRIG_CTRL);
    if ((trig & 7) != TRIG_DMA_SW) {
        Wr(g_Dsi, DSI_TRIG_CTRL, (trig & ~7u) | TRIG_DMA_SW);
    }
    Wr(g_Dsi, DSI_CTRL, ctrl | CTRL_CMD_MODE_EN | CTRL_ENABLE);
    Wr(g_Dsi, DSI_INT_CTRL, Rd(g_Dsi, DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(g_Dsi, DSI_TPG_CTRL, 0);
    Wr(g_Dsi, DSI_DMA_CMD_OFFSET, g_DmaPa.LowPart);
    Wr(g_Dsi, DSI_DMA_CMD_LENGTH, size);
    dmaCtrl = Rd(g_Dsi, DSI_CMD_DMA_CTRL);
    Wr(g_Dsi, DSI_CMD_DMA_CTRL, dmaCtrl & ~DMA_CTRL_LOW_POWER);
    Wr(g_Dsi, DSI_DMA_SW_TRIGGER, 1);
    for (waited = 0; waited < 50; waited++) {
        if (Rd(g_Dsi, DSI_INT_CTRL) & INT_CMD_DMA_DONE) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(1000);
    }
    Wr(g_Dsi, DSI_INT_CTRL, Rd(g_Dsi, DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(g_Dsi, DSI_CMD_DMA_CTRL, dmaCtrl);
    Wr(g_Dsi, DSI_CTRL, ctrl);
    if (trig != Rd(g_Dsi, DSI_TRIG_CTRL)) {
        Wr(g_Dsi, DSI_TRIG_CTRL, trig);
    }
    return done;
}

static NTSTATUS SendBrightness(UCHAR Pct)
{
    ULONG level = (ULONG)Pct * BL_MAX / 100;
    BOOLEAN done = FALSE;

    if (Pct > 0 && level < 16) {
        level = 16;
    }

    KeWaitForSingleObject(&g_BlLocalLock, Executive, KernelMode, FALSE, NULL);
    if (FileExists(L"\\??\\C:\\xun\\bl.dcs")) {
        UCHAR cmd[3] = { 0x51, (UCHAR)(level >> 8), (UCHAR)level };
        if (level < 1) {
            level = 1;
        }
        cmd[1] = (UCHAR)(level >> 8);
        cmd[2] = (UCHAR)level;
        done = DsiLongWrite(cmd, sizeof(cmd));
    } else if (XunQupLockAcquire(&g_QupLock, 500)) {
        if (Pct == 0) {
            done = KtzWriteReg(KTZ_REG_BL_LSB, 0) && KtzWriteReg(KTZ_REG_BL_MSB, 0) &&
                   KtzWriteReg(KTZ_REG_BL_EN, 0x9C) && KtzWriteReg(KTZ_REG_BL_EN, 0x98);
            g_KtzOn = FALSE;
        } else {
            if (g_Hwen != NULL) {
                Wr(g_Hwen, 0x0, (1u << 9));
                Wr(g_Hwen, 0x4, 0x2);
            }
            if (!g_KtzOn) {
                LARGE_INTEGER delay;
                KtzWriteReg(KTZ_REG_BL_EN, 0x9C);
                delay.QuadPart = -50000LL;
                KeDelayExecutionThread(KernelMode, FALSE, &delay);
                KtzWriteReg(KTZ_REG_BL_EN, 0x9E);
                g_KtzOn = TRUE;
            }
            done = KtzWriteReg(KTZ_REG_BL_LSB, (UCHAR)(level & 0x7)) &&
                   KtzWriteReg(KTZ_REG_BL_MSB, (UCHAR)((level >> 3) & 0xFF));
        }
        XunQupLockRelease(&g_QupLock);
    }
    KeReleaseMutex(&g_BlLocalLock, FALSE);

    LogPrint("bl: SetBrightness %u%% -> 0x%03x (%s)\n", Pct, level, done ? "ok" : "deferred");
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY GetPossibleBrightness(PVOID Ctx, ULONG BufSize, UCHAR *Count, UCHAR *Levels)
{
    ULONG i;
    UNREFERENCED_PARAMETER(Ctx);
    *Count = 101;
    if (BufSize < 101) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (i = 0; i <= 100; i++) {
        Levels[i] = (UCHAR)i;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY SetBrightness(PVOID Ctx, UCHAR Brightness)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (Brightness > 100) {
        Brightness = 100;
    }
    g_Brightness = Brightness;
    return SendBrightness(Brightness);
}

static NTSTATUS APIENTRY GetBrightness(PVOID Ctx, UCHAR *Brightness)
{
    UNREFERENCED_PARAMETER(Ctx);
    *Brightness = g_Brightness;
    return STATUS_SUCCESS;
}

static VOID APIENTRY IntRef(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }
static VOID APIENTRY IntDeref(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }

static BOOLEAN DsdtHasGpu0(VOID)
{
    static const CHAR pat[] = "TPZG0610";
    UNICODE_STRING base;
    OBJECT_ATTRIBUTES oa;
    HANDLE hk = NULL;
    ULONG i, len;
    PKEY_VALUE_PARTIAL_INFORMATION v = NULL;
    BOOLEAN found = FALSE;

    RtlInitUnicodeString(&base, L"\\Registry\\Machine\\HARDWARE\\ACPI\\DSDT");
    InitializeObjectAttributes(&oa, &base, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hk, KEY_ENUMERATE_SUB_KEYS, &oa))) {
        return FALSE;
    }
    v = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536, 'bnuX');
    if (v != NULL) {
        WCHAR sub1[64], sub2[64];
        PKEY_BASIC_INFORMATION k1 = (PKEY_BASIC_INFORMATION)v;
        for (i = 0; !found && NT_SUCCESS(ZwEnumerateKey(hk, i, KeyBasicInformation, k1, 512, &len)); i++) {
            HANDLE h1 = NULL;
            UNICODE_STRING u1;
            ULONG n1 = min(k1->NameLength / sizeof(WCHAR), (ULONG)63);
            RtlCopyMemory(sub1, k1->Name, n1 * sizeof(WCHAR));
            sub1[n1] = 0;
            RtlInitUnicodeString(&u1, sub1);
            InitializeObjectAttributes(&oa, &u1, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hk, NULL);
            if (!NT_SUCCESS(ZwOpenKey(&h1, KEY_ENUMERATE_SUB_KEYS, &oa))) {
                continue;
            }
            for (ULONG j = 0; !found && NT_SUCCESS(ZwEnumerateKey(h1, j, KeyBasicInformation, k1, 512, &len)); j++) {
                HANDLE h2 = NULL;
                UNICODE_STRING u2;
                ULONG n2 = min(k1->NameLength / sizeof(WCHAR), (ULONG)63);
                RtlCopyMemory(sub2, k1->Name, n2 * sizeof(WCHAR));
                sub2[n2] = 0;
                RtlInitUnicodeString(&u2, sub2);
                InitializeObjectAttributes(&oa, &u2, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, h1, NULL);
                if (NT_SUCCESS(ZwOpenKey(&h2, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &oa))) {
                    HANDLE h3 = NULL;
                    UNICODE_STRING u3, vname;
                    if (NT_SUCCESS(ZwEnumerateKey(h2, 0, KeyBasicInformation, k1, 512, &len))) {
                        ULONG n3 = min(k1->NameLength / sizeof(WCHAR), (ULONG)63);
                        WCHAR sub3[64];
                        RtlCopyMemory(sub3, k1->Name, n3 * sizeof(WCHAR));
                        sub3[n3] = 0;
                        RtlInitUnicodeString(&u3, sub3);
                        InitializeObjectAttributes(&oa, &u3, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, h2, NULL);
                        if (NT_SUCCESS(ZwOpenKey(&h3, KEY_QUERY_VALUE, &oa))) {
                            RtlInitUnicodeString(&vname, L"00000000");
                            if (NT_SUCCESS(ZwQueryValueKey(h3, &vname, KeyValuePartialInformation, v, 65536, &len))) {
                                for (ULONG p = 0; p + 8 <= v->DataLength; p++) {
                                    if (RtlCompareMemory(v->Data + p, pat, 8) == 8) {
                                        found = TRUE;
                                        break;
                                    }
                                }
                            }
                            ZwClose(h3);
                        }
                    }
                    ZwClose(h2);
                }
            }
            ZwClose(h1);
        }
        ExFreePoolWithTag(v, 'bnuX');
    }
    ZwClose(hk);
    return found;
}

BOOLEAN g_XunGpuOwnsPanel = FALSE;

VOID XunCheckGpuOwner(VOID)
{
    g_XunGpuOwnsPanel = DsdtHasGpu0();
}

VOID XunBlInit(VOID)
{
    PHYSICAL_ADDRESS pa, lo, hi, skip;

    g_XunGpuOwnsPanel = DsdtHasGpu0();
    KeInitializeMutex(&g_BlLocalLock, 0);
    XunQupLockInit(&g_QupLock, 1);

    pa.QuadPart = (LONGLONG)QUP0_SE1_PA;
    g_Se1 = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)TLMM_PIN_PA(TLMM_TILE_SOUTH, KTZ_HWEN_PIN);
    g_Hwen = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);

    pa.QuadPart = (LONGLONG)DSI0_PA;
    g_Dsi = (volatile UCHAR *)MmMapIoSpaceEx(pa, DSI0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    lo.QuadPart = 0; hi.QuadPart = 0xFFFFFFFF; skip.QuadPart = 0;
    g_DmaVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, skip, MmNonCached);
    if (g_DmaVa != NULL) {
        g_DmaPa = MmGetPhysicalAddress(g_DmaVa);
    }
    LogPrint("GPU0 (TPZG0610) in DSDT: %s; KTZ8866 SE1=%p HWEN=%p\n",
             g_XunGpuOwnsPanel ? "yes" : "no", g_Se1, g_Hwen);
}

VOID XunBlDeinit(VOID)
{
    XunQupLockClose(&g_QupLock);
    if (g_Se1 != NULL) { MmUnmapIoSpace((PVOID)g_Se1, 0x4000); g_Se1 = NULL; }
    if (g_Hwen != NULL) { MmUnmapIoSpace((PVOID)g_Hwen, 0x1000); g_Hwen = NULL; }
    if (g_Dsi != NULL) { MmUnmapIoSpace((PVOID)g_Dsi, DSI0_SIZE); g_Dsi = NULL; }
    if (g_DmaVa != NULL) {
        MmFreeContiguousMemorySpecifyCache(g_DmaVa, PAGE_SIZE, MmNonCached);
        g_DmaVa = NULL;
    }
}

BOOLEAN XunBlGpu0OwnsPanel(VOID) { return g_XunGpuOwnsPanel; }

NTSTATUS XunBlQueryInterface(PQUERY_INTERFACE Q)
{
    DXGK_BRIGHTNESS_INTERFACE *bi;

    if (memcmp(Q->InterfaceType, &GUID_DEVINTERFACE_BRIGHTNESS, sizeof(GUID)) != 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Q->Size < sizeof(*bi)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    bi = (DXGK_BRIGHTNESS_INTERFACE *)Q->Interface;
    bi->Size = sizeof(*bi);
    bi->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
    bi->Context = NULL;
    bi->InterfaceReference = IntRef;
    bi->InterfaceDereference = IntDeref;
    bi->GetPossibleBrightness = GetPossibleBrightness;
    bi->SetBrightness = SetBrightness;
    bi->GetBrightness = GetBrightness;
    return STATUS_SUCCESS;
}
