/*
 * Hardware side: the lab scripts of docs/P9_audio.md ("FIRST SOUND") in C, through XunAudio (ta.c).
 *   first Prepare: codec bring-up once (bringup.ps1 + smmu_audio.ps1 + rx_stock.ps1 + wcd_aux.ps1)
 *   Prepare: MEM_MAP ring + pos buffer, graph SH_MEM_PULL_MODE -> CODEC_DMA_SINK (RX_CODEC_DMA_RX_1)
 *   Run:     (prepare again after a stop) start, first time SoundWire ports + AUX PA, amp on, unmute
 *   Pause:   amp off, mute, stop;  Release: close graph, unmap
 * Position: the DSP writes {frame counter, read index, ts} into the pos buffer (read at DISPATCH).
 */
#include "tspk.h"
#include "ta.h"
#include "rxtab.h"
#include "xun_qup_lock.h"

#define RX_MACRO   0x0A600000ULL
#define VA_MACRO   0x0A730000ULL
#define TX_MACRO   0x0A620000ULL
#define RX_SWR     0x0A610000ULL
#define VA_SWR     0x0A740000ULL
#define LPI_TLMM   0x0A7C0000ULL
#define SMMU       0x0C600000ULL

#define SG    0x4001
#define CONT  0x4101
#define DMA   0x7001
#define PULL  0x7002

#define APM_GRAPH_OPEN    0x01001000
#define APM_GRAPH_PREPARE 0x01001001
#define APM_GRAPH_START   0x01001002
#define APM_GRAPH_STOP    0x01001003
#define APM_GRAPH_CLOSE   0x01001004
#define APM_SET_CFG       0x01001006
#define APM_MEM_MAP       0x0100100C
#define APM_MEM_UNMAP     0x0100100D

static BOOLEAN  g_CodecUp, g_PortsUp, g_GraphOpen, g_Stopped, g_Running;
static PULONG   g_PosVa;                       /* 4 KiB DSP position buffer */
static PHYSICAL_ADDRESS g_PosPa;
static ULONG    g_RingMap, g_PosMap;           /* DSP mem map handles */
static ULONG    g_SwrId;
static LONG     g_VolDb = -30;
static BOOLEAN  g_Mute;

/* ---------------- small helpers ---------------- */

static VOID SwrWr(ULONGLONG Master, ULONG Dev, ULONG Reg, ULONG Val)
{
    g_SwrId = (g_SwrId % 14) + 1;               /* SWR_REG_VAL_PACK: reg | id << 16 | dev << 20 | data << 24 */
    TaWr(Master + 0x300, (Val << 24) | (Dev << 20) | (g_SwrId << 16) | Reg);
    TaSleep(1);
}

static LONG SwrRd(ULONGLONG Master, ULONG Dev, ULONG Reg)
{
    ULONG i, v, id;

    g_SwrId = (g_SwrId % 14) + 1;
    id = g_SwrId;
    TaWr(Master + 0x304, (Dev << 20) | (id << 16) | Reg);
    for (i = 0; i < 20; i++) {
        TaSleep(1);
        v = TaRd(Master + 0x318);
        if (((v >> 8) & 0xFu) == id) {
            return (LONG)(v & 0xFFu);
        }
    }
    TaWr(Master + 0x308, 1);
    return -1;
}

static NTSTATUS Prm(const ULONG *Dw, ULONG N)
{
    ULONG op = 0, r[4] = { 0 };
    NTSTATUS status = TaGpr(2, 2, 0x0100100F, TRUE, Dw, N, &op, r, 4, 1000);

    if (NT_SUCCESS(status) && r[1] != 0) {
        LogPrint("prm %08x: status %u\r\n", Dw[1], r[1]);
        status = STATUS_UNSUCCESSFUL;
    }
    return status;
}

static VOID InitMaster(ULONGLONG M)
{
    ULONG cfg;

    TaWr(M + 0x008, 1); TaWr(M + 0x008, 1); TaWr(M + 0x1044, 1); TaSleep(2);
    TaWr(M + 0x101C, 0x2F0008); TaWr(M + 0x500, 1);
    cfg = TaRd(M + 0x1048);
    TaWr(M + 0x1048, (cfg & ~0x3E0000u) | (0x1Fu << 17));
    TaWr(M + 0x314, 3); TaWr(M + 0x1044, 2);
    TaWr(M + 0x04, 2); TaWr(M + 0x208, 0xFFFFFFFF); TaWr(M + 0x204, 0x1FDFD); TaWr(M + 0x210, 0x1FDFD);
    TaWr(M + 0x04, 3); TaWr(M + 0x314, 0x80000003);
}

/* identity context bank for the audio DMA stream 0x1C1 (smmu_audio.ps1) */
static NTSTATUS SmmuAudio(VOID)
{
    ULONG id1 = TaRd(SMMU + 0x24), nsmr = TaRd(SMMU + 0x20) & 0xFF, ncb = id1 & 0xFF;
    ULONG psize = (id1 & 0x80000000u) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
    ULONG i, slot = MAXULONG, cb = MAXULONG, smr, s2;
    ULONGLONG used = 0, cbp, gr1 = SMMU + psize;

    for (i = 0; i < nsmr && i < 128; i++) {
        smr = TaRd(SMMU + 0x800 + 4 * i);
        s2 = TaRd(SMMU + 0xC00 + 4 * i);
        if ((smr >> 31) && (smr & 0xFFFF) == 0x1C1) {
            LogPrint("smmu: sid 1c1 already in SMR%u\r\n", i);
            return STATUS_SUCCESS;
        }
        if (!(smr >> 31) && slot == MAXULONG) {
            slot = i;
        }
        if ((smr >> 31) && ((s2 >> 16) & 3) == 0) {
            used |= 1ull << (s2 & 0x3F);
        }
    }
    for (i = 4; i < ncb && i < 64; i++) {
        if (!(used & (1ull << i))) {
            cb = i;
            break;
        }
    }
    if (slot == MAXULONG || cb == MAXULONG) {
        LogPrint("smmu: no free SMR/CB\r\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    cbp = SMMU + (ULONGLONG)(npage + cb) * psize;
    TaWr(cbp + 0x58, 0xFFFFFFFF); TaWr(cbp + 0x20, 0); TaWr(cbp + 0x24, 0); TaWr(cbp + 0x30, 0);
    TaWr(cbp + 0x10, 0); TaWr(cbp + 0x38, 0); TaWr(cbp + 0x3C, 0);
    TaWr(gr1 + 0x800 + 4 * cb, 1); TaWr(gr1 + 4 * cb, 0x1F000); TaWr(cbp, 0xE0);
    TaWr(SMMU + 0xC00 + 4 * slot, cb); TaWr(SMMU + 0x800 + 4 * slot, 0x80000000u | 0x1C1);
    TaWr(SMMU + 0x70, 0);
    LogPrint("smmu: sid 1c1 -> SMR%u CB%u, SMR %08x\r\n", slot, cb, TaRd(SMMU + 0x800 + 4 * slot));
    return STATUS_SUCCESS;
}

static VOID ApplyVolume(VOID)
{
    LONG db = g_Mute ? -84 : g_VolDb;

    if (g_CodecUp) {
        TaWr(RX_MACRO + 0x414, (ULONG)db & 0xFF);   /* RX_RX0_RX_VOL_CTL, s8 dB (HPHL / EAR) */
        TaWr(RX_MACRO + 0x494, (ULONG)db & 0xFF);   /* RX_RX1_RX_VOL_CTL, s8 dB (HPHR) */
        TaWr(RX_MACRO + 0x514, (ULONG)db & 0xFF);   /* RX_RX2_RX_VOL_CTL, s8 dB (AUX) */
    }
}

/* ---------------- codec bring-up (once per boot) ---------------- */

static NTSTATUS CodecBringUp(VOID)
{
    static const ULONG pins[6] = { 0xD04, 0xD06, 0xD06, 0xD04, 0xD06, 0xD06 };
    ULONG dw[9], i;
    NTSTATUS status;

    if (!TaAdspReady()) {
        LogPrint("bring-up: ADSP / APM not ready (C:\\xun\\audio.arm + reboot?)\r\n");
        return STATUS_DEVICE_NOT_READY;
    }
    /* 1. PRM: DCODEC vote, TX core + TX NPL clocks only (stock) */
    dw[0] = 2; dw[1] = 0x08001032; dw[2] = 4; dw[3] = 0; dw[4] = 2;
    status = Prm(dw, 5);
    for (i = 0; i < 2 && NT_SUCCESS(status); i++) {
        dw[0] = 2; dw[1] = 0x0800102C; dw[2] = 20; dw[3] = 0; dw[4] = 1;
        dw[5] = 0x30C + i; dw[6] = 19200000; dw[7] = 1; dw[8] = 0;
        status = Prm(dw, 9);
    }
    LogPrint("bring-up: prm votes/clocks %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* 2. LPI pins + slew */
    for (i = 0; i < 6; i++) {
        TaWr(LPI_TLMM + 0x1000 * i, pins[i]);
    }
    TaWr(0x0A95A000, 0x3F3F);
    /* 3. VA macro fs-gen + SWR clock */
    TaWr(VA_MACRO, 1); TaWr(VA_MACRO + 4, 3); TaWr(VA_MACRO + 4, 1); TaWr(VA_MACRO + 0x80, 2);
    TaWr(VA_MACRO + 8, 2); TaWr(VA_MACRO + 8, 3); TaWr(VA_MACRO + 8, 1);
    /* 4. RX macro MCLK, FS, SWR clock; RX CGCR */
    TaWr(RX_MACRO + 0x100, 3); TaWr(RX_MACRO + 0x104, 3); TaWr(RX_MACRO + 0x104, 1);
    TaWr(RX_MACRO + 0x108, 2); TaWr(RX_MACRO + 0x108, 3); TaWr(RX_MACRO + 0x108, 1);
    TaWr(0x0A6A9098, 3); TaSleep(1); TaWr(0x0A6A9098, 1);
    /* 5. TX macro MCLK, FS, SWR clock; VA/TX CGCR */
    TaWr(TX_MACRO, 1); TaWr(TX_MACRO + 4, 3); TaWr(TX_MACRO + 4, 1);
    TaWr(TX_MACRO + 8, 2); TaWr(TX_MACRO + 8, 3); TaWr(TX_MACRO + 8, 1);
    TaWr(0x0A7EC100, 3); TaSleep(1); TaWr(0x0A7EC100, 1);
    /* 6. WCD937x reset (TLMM gpio92) low while both masters start, then high */
    TaWr(0x0055C004, 0); TaWr(0x0055C000, 0x200);
    InitMaster(RX_SWR);
    InitMaster(VA_SWR);
    TaSleep(20);
    TaWr(0x0055C004, 2);
    TaSleep(300);
    LogPrint("bring-up: swr slaves rx %x va %x, ids %08x %08x\r\n", TaRd(RX_SWR + 0x1090), TaRd(VA_SWR + 0x1090),
             TaRd(RX_SWR + 0x538), TaRd(VA_SWR + 0x538));
    /* 7. audio DMA SMMU identity */
    status = SmmuAudio();
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* 8. RX macro registers as on stock (RX2 path stays muted until Run) */
    for (i = 0; i < ARRAYSIZE(g_RxStock); i++) {
        TaWr(RX_MACRO + g_RxStock[i][0], g_RxStock[i][1]);
    }
    TaWr(RX_MACRO + 0x100, 0x03);                    /* keep MCLK_CONTROL enabled (g_RxStock had 0x00) */
    TaWr(RX_MACRO + 0x104, 0x01);                    /* FS_CNT_CONTROL enabled */
    TaWr(RX_MACRO + 0x108, 0x01);                    /* SWR_CONTROL enabled */
    TaWr(RX_MACRO + 0x180, 0x70);                    /* RX INT0_1 MIX1 INP1 = RX2 (AIF2_PB ch0) */
    TaWr(RX_MACRO + 0x188, 0x07);                    /* RX INT1_1 MIX1 INP0 = RX2 (AIF2_PB ch0) */
    TaWr(RX_MACRO + 0x190, 0x08);                    /* RX INT2_1 MIX1 INP0 = RX3 (AIF2_PB ch1) */
    TaWr(RX_MACRO + 0x200, 0x01);                    /* CLSH enable (rx_macro_config_classh) */
    TaWr(RX_MACRO + 0x208, 0x00);                    /* CLSH_HPH_V_PA (ear_mode=1 -> 0x00) */
    TaWr(RX_MACRO + 0x20C, 0x39);                    /* CLSH_DECAY_CTRL (ear_mode=1 -> 0x39) */
    TaWr(RX_MACRO + 0x21C, 0x00);                    /* CLSH_K2_MSB */
    TaWr(RX_MACRO + 0x220, 0xC0);                    /* CLSH_K2_LSB (0xC0 for INT0) */
    TaWr(RX_MACRO + 0x404, 0x48);                    /* INT0_CFG1: live xun ftrace (0x48) */
    TaWr(RX_MACRO + 0x408, 0x67);                    /* INT0_CFG0: 0x64 | EAR_LUT_BYPASS(0x02) | CLSH_DSM_OUT(0x01) */
    TaWr(RX_MACRO + 0x40C, 0x8F);                    /* INT0_CFG2: live xun ftrace (0x8F) */
    TaWr(RX_MACRO + 0x414, 0x00);                    /* INT0_VOL_CTL: live xun ftrace (0x00) */
    TaWr(RX_MACRO + 0x42C, 0x14);                    /* INT0_ESM_CTL */
    TaWr(RX_MACRO + 0x440, 0x09);                    /* INT0 interp/DSM out enable (preserve bit 3 = 0x08 from g_RxStock) */
    TaWr(RX_MACRO + 0x484, 0x18);                    /* INT1_CFG1: DSM_CLK|FS_RATE */
    TaWr(RX_MACRO + 0x4C0, 0x09);                    /* INT1 interp/DSM out enable (preserve bit 3 = 0x08 from g_RxStock) */
    TaWr(RX_MACRO + 0x504, 0x18);                    /* INT2_CFG1: DSM_CLK|FS_RATE */
    TaWr(RX_MACRO + 0x50C, 0x8F);                    /* INT2_CFG2: live xun ftrace (0x8F) */
    TaWr(RX_MACRO + 0x514, 0xFB);                    /* INT2_VOL_CTL: live xun ftrace (0xFB) */
    TaWr(RX_MACRO + 0x54C, 0x01);                    /* INT2 DSM_OUT_EN bit 0 */
    TaWr(RX_MACRO + 0x400, 4); TaWr(RX_MACRO + 0x480, 4); TaWr(RX_MACRO + 0x500, 4);
    {
        USHORT l14_st = 0x1EE, l14_cfg[7] = { 0 }, l9_st = 0x1EE, l9_cfg[7] = { 0 };
        NTSTATUS st1 = TaPmicRd(1, 0x4D08, &l14_st, 1);
        NTSTATUS st2 = TaPmicRd(1, 0x4D40, l14_cfg, 7);
        NTSTATUS st3 = TaPmicRd(1, 0x4808, &l9_st, 1);
        NTSTATUS st4 = TaPmicRd(1, 0x4840, l9_cfg, 7);
        LogPrint("pmic l14(%08x/%08x): st=%02x v=%02x/%02x en=%02x | l9(%08x/%08x): st=%02x v=%02x/%02x en=%02x\r\n",
                 st1, st2, l14_st, l14_cfg[0], l14_cfg[1], l14_cfg[6],
                 st3, st4, l9_st, l9_cfg[0], l9_cfg[1], l9_cfg[6]);
    }
    /* 9. WCD937x init + RX clocks + AUX DAC (Left) + EAR DAC (Right) (TX slave on VA bus, dev 1) */
    SwrWr(VA_SWR, 1, 0x3103, 0xD6); TaSleep(2);
    SwrWr(VA_SWR, 1, 0x312A, 0x00);
    SwrWr(VA_SWR, 1, 0x3029, 0x85);
    SwrWr(VA_SWR, 1, 0x3001, 0xC0); TaSleep(10); SwrWr(VA_SWR, 1, 0x3001, 0x80);
    SwrWr(VA_SWR, 1, 0x30E2, 0xD9);
    SwrWr(VA_SWR, 1, 0x306B, 0xFA); SwrWr(VA_SWR, 1, 0x306E, 0xFA); SwrWr(VA_SWR, 1, 0x3071, 0xFA);
    SwrWr(VA_SWR, 1, 0x30A5, 0xEB);
    SwrWr(VA_SWR, 1, 0x3409, 0x08); SwrWr(VA_SWR, 1, 0x3408, 0x01); SwrWr(VA_SWR, 1, 0x3008, 0x01);
    SwrWr(VA_SWR, 1, 0x340D, 0xBC); SwrWr(VA_SWR, 1, 0x340E, 0xBC); SwrWr(VA_SWR, 1, 0x340F, 0xBC);
    SwrWr(VA_SWR, 1, 0x3408, 0x03);
    SwrWr(VA_SWR, 1, 0x3408, 0x07); SwrWr(VA_SWR, 1, 0x3409, 0x0D); SwrWr(VA_SWR, 1, 0x344F, 0x01);
    SwrWr(VA_SWR, 1, 0x344E, 0x04); SwrWr(VA_SWR, 1, 0x3133, 0x81); SwrWr(VA_SWR, 1, 0x3414, 0x02);
    TaSleep(5);
    SwrWr(VA_SWR, 1, 0x340B, 0x15);                 /* AUX PATH Mode: live xun ftrace START (0x15) */
    SwrWr(VA_SWR, 1, 0x3128, 0x00);                 /* AUX PGA init 0x00 before enable */
    SwrWr(VA_SWR, 1, 0x3408, 0x01); SwrWr(VA_SWR, 1, 0x3008, 0x03);
    SwrWr(VA_SWR, 1, 0x3408, 0x03); SwrWr(VA_SWR, 1, 0x3409, 0x09);
    SwrWr(VA_SWR, 1, 0x30A4, 0x4A);
    SwrWr(VA_SWR, 1, 0x3008, 0x01); SwrWr(VA_SWR, 1, 0x30AF, 0xB0); SwrWr(VA_SWR, 1, 0x3098, 0x1C);
    SwrWr(VA_SWR, 1, 0x3008, 0x41); SwrWr(VA_SWR, 1, 0x30AF, 0xF0); SwrWr(VA_SWR, 1, 0x3008, 0xC1);
    SwrWr(VA_SWR, 1, 0x3098, 0x3A);
    SwrWr(VA_SWR, 1, 0x3408, 0x07); SwrWr(VA_SWR, 1, 0x3409, 0x0D);
    SwrWr(VA_SWR, 1, 0x3450, 0x00);                 /* RDAC3_MUX: live xun wcd937x.txt (0x00) */
    SwrWr(VA_SWR, 1, 0x30E9, 0x20);                 /* AUX PA bias: live xun wcd937x.txt (0x20->0x22) */
    SwrWr(VA_SWR, 1, 0x30E9, 0x22);
    SwrWr(VA_SWR, 1, 0x3009, 0x0C);
    g_CodecUp = TRUE;
    ApplyVolume();
    LogPrint("bring-up: codec ready\r\n");
    /* the speaker works this boot: arm the ADSP for the next one too (XunAudio consumes C:\xun\audio.arm at
       each boot, so a boot that dies before this point comes back without the ADSP) */
    {
        UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\xun\\audio.arm");
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE h;
        static const CHAR on[] = "on";
        NTSTATUS st;

        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        st = ZwCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
        if (NT_SUCCESS(st)) {
            ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)on, 2, NULL, NULL);
            ZwClose(h);
        }
        LogPrint("audio.arm re-armed: %08x\r\n", st);
    }
    return STATUS_SUCCESS;
}

/* SoundWire data ports (ports.ps1) + AUX PA (wcd_auxpa.ps1): after the first graph start */
static VOID ProgramSwrPortBank(ULONG Bank)
{
    ULONG mb = Bank << 6;
    ULONG sb = Bank << 4;

    /* Port 1 (HPH/EAR, rx_frame_params_default[0]: sint=3, off1=0, off2=0, hstart=0xff, hstop=0xff, wl=1, ch_en=1) */
    SwrWr(RX_SWR, 1, 0x103, 0x01);
    SwrWr(RX_SWR, 1, 0x120 + sb, 0x01);
    SwrWr(RX_SWR, 1, 0x122 + sb, 0x03);
    SwrWr(RX_SWR, 1, 0x123 + sb, 0x00);
    SwrWr(RX_SWR, 1, 0x124 + sb, 0x00);
    SwrWr(RX_SWR, 1, 0x125 + sb, 0x00);
    SwrWr(RX_SWR, 1, 0x126 + sb, 0xF0);
    TaWr(RX_SWR + 0x1124 + mb, 0x01000003);
    TaWr(RX_SWR + 0x113C + mb, 0x00);
    TaWr(RX_SWR + 0x112C, 0x01);
    TaWr(RX_SWR + 0x1134 + mb, 0xF0);

    /* Port 2 (CLSH, rx_frame_params_default[1]: sint=0x1f, off1=0, off2=0, hstart=3, hstop=6, wl=7, type=0, ch_en=1) */
    SwrWr(RX_SWR, 1, 0x203, 0x07);
    SwrWr(RX_SWR, 1, 0x220 + sb, 0x01);
    SwrWr(RX_SWR, 1, 0x222 + sb, 0x1F);
    SwrWr(RX_SWR, 1, 0x223 + sb, 0x00);
    SwrWr(RX_SWR, 1, 0x224 + sb, 0x00);
    SwrWr(RX_SWR, 1, 0x226 + sb, 0x63);
    SwrWr(RX_SWR, 1, 0x227 + sb, 0x00);
    TaWr(RX_SWR + 0x1224 + mb, 0x0100001F);
    TaWr(RX_SWR + 0x123C + mb, 0x00);
    TaWr(RX_SWR + 0x122C, 0x07);
    TaWr(RX_SWR + 0x1230 + mb, 0x00);
    TaWr(RX_SWR + 0x1234 + mb, 0x63);

    /* Port 4 (LO/AUX, rx_frame_params_default[3]: sint=7, off1=1, off2=0, hstart=0xff, hstop=0xff, wl=0xff, ch_en=1) */
    SwrWr(RX_SWR, 1, 0x420 + sb, 0x01);
    SwrWr(RX_SWR, 1, 0x422 + sb, 0x07);
    SwrWr(RX_SWR, 1, 0x423 + sb, 0x00);
    SwrWr(RX_SWR, 1, 0x424 + sb, 0x01);
    SwrWr(RX_SWR, 1, 0x426 + sb, 0xF0);
    TaWr(RX_SWR + 0x1424 + mb, 0x01000107);
    TaWr(RX_SWR + 0x143C + mb, 0x00);
    TaWr(RX_SWR + 0x1434 + mb, 0xF0);
}

static VOID PortsAndPa(VOID)
{
    ULONG mcpPre, curBank, nextBank, mcpPost;

    /* swrm_slvdev_datapath_control (swr_ctrl_dlkm.ko 0x48a0..0x4b40):
       1. Enable port interrupt mask at 0x0A50 ((5 * 33) << 4)
       2. Read SWRM_MCP_STATUS (0x104C) bit 0 = curBank, nextBank = curBank ^ 1
       3. Program Slave + Master ports in nextBank (inactive bank)
       4. Write SWRM_MCP_FRAME_CTRL_BANK(nextBank) = 0x0005000F
       5. Broadcast SCP_FrameCtrl (dev=0x0F, cmd_id=0x0F, reg=0x0060+(nextBank<<4), data=0x0F)
       6. Mirror Slave + Master ports in curBank (now-inactive bank) */
    TaWr(RX_SWR + 0x0A50, 0x0001FDFD);
    mcpPre = TaRd(RX_SWR + 0x104C);
    curBank = mcpPre & 1u;
    nextBank = curBank ^ 1u;
    ProgramSwrPortBank(nextBank);
    TaWr(RX_SWR + 0x101C + (nextBank << 6), 0x0005000F);
    TaWr(RX_SWR + 0x0300, (0x0Fu << 24) | (15u << 20) | (15u << 16) | (0x0060u + (nextBank << 4)));
    TaSleep(5);
    ProgramSwrPortBank(curBank);
    mcpPost = TaRd(RX_SWR + 0x104C);
    SwrWr(VA_SWR, 1, 0x3465, 0x13);                 /* live xun ftrace START */
    SwrWr(VA_SWR, 1, 0x3009, 0x0C);                 /* WCD937X_ANA_HPH: keep HPHL/HPHR PA off */
    SwrWr(VA_SWR, 1, 0x300B, 0x92);                 /* WCD937X_ANA_EAR: live xun ftrace START (0x92) */
    SwrWr(VA_SWR, 1, 0x300A, 0xC0);                 /* live xun ftrace START (0xC0) */
    TaSleep(6);
    SwrWr(VA_SWR, 1, 0x3008, 0xC3);                 /* RX_CTRL_0: live xun ftrace START (0xC3) */
    SwrWr(VA_SWR, 1, 0x346B, 0x4C);
    SwrWr(VA_SWR, 1, 0x346C, 0xDF);
    SwrWr(VA_SWR, 1, 0x346D, 0x0F);
    SwrWr(VA_SWR, 1, 0x3467, 0x05);
    SwrWr(VA_SWR, 1, 0x3128, 0x80);                 /* AUX PGA enable: live xun ftrace START (0x80) */
    SwrWr(VA_SWR, 1, 0x346B, 0x4C);
    SwrWr(VA_SWR, 1, 0x346C, 0x5F);
    SwrWr(VA_SWR, 1, 0x346D, 0x0F);
    LogPrint("ports: mcp %08x->%08x (bank %u->%u) frame b0=%08x b1=%08x, irq %08x\r\n",
             mcpPre, mcpPost, curBank, nextBank, TaRd(RX_SWR + 0x101C), TaRd(RX_SWR + 0x105C), TaRd(RX_SWR + 0x200));
    LogPrint("wcd[3008..300B,30A4,3128,3133]: %02x %02x %02x %02x %02x %02x %02x\r\n",
             SwrRd(VA_SWR, 1, 0x3008), SwrRd(VA_SWR, 1, 0x3009),
             SwrRd(VA_SWR, 1, 0x300A), SwrRd(VA_SWR, 1, 0x300B),
             SwrRd(VA_SWR, 1, 0x30A4), SwrRd(VA_SWR, 1, 0x3128), SwrRd(VA_SWR, 1, 0x3133));
    LogPrint("wcd[3408,3409,340B,344E..3450,3467,346B..346D]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
             SwrRd(VA_SWR, 1, 0x3408), SwrRd(VA_SWR, 1, 0x3409), SwrRd(VA_SWR, 1, 0x340B),
             SwrRd(VA_SWR, 1, 0x344E), SwrRd(VA_SWR, 1, 0x344F), SwrRd(VA_SWR, 1, 0x3450),
             SwrRd(VA_SWR, 1, 0x3467), SwrRd(VA_SWR, 1, 0x346B), SwrRd(VA_SWR, 1, 0x346C), SwrRd(VA_SWR, 1, 0x346D));
    LogPrint("rx_slv p1[103,130..136]=%02x/%02x,%02x,%02x,%02x,%02x p2[203,230..236]=%02x/%02x,%02x,%02x,%02x,%02x p4[403,430..436]=%02x/%02x,%02x,%02x,%02x,%02x\r\n",
             SwrRd(RX_SWR, 1, 0x103), SwrRd(RX_SWR, 1, 0x130), SwrRd(RX_SWR, 1, 0x132), SwrRd(RX_SWR, 1, 0x133), SwrRd(RX_SWR, 1, 0x134), SwrRd(RX_SWR, 1, 0x136),
             SwrRd(RX_SWR, 1, 0x203), SwrRd(RX_SWR, 1, 0x230), SwrRd(RX_SWR, 1, 0x232), SwrRd(RX_SWR, 1, 0x233), SwrRd(RX_SWR, 1, 0x234), SwrRd(RX_SWR, 1, 0x236),
             SwrRd(RX_SWR, 1, 0x403), SwrRd(RX_SWR, 1, 0x430), SwrRd(RX_SWR, 1, 0x432), SwrRd(RX_SWR, 1, 0x433), SwrRd(RX_SWR, 1, 0x434), SwrRd(RX_SWR, 1, 0x436));
    LogPrint("rx_mst p1=%08x/%02x/%02x p2=%08x/%02x/%02x p4=%08x/%02x/%02x\r\n",
             TaRd(RX_SWR + 0x1164), TaRd(RX_SWR + 0x112C), TaRd(RX_SWR + 0x1174),
             TaRd(RX_SWR + 0x1264), TaRd(RX_SWR + 0x122C), TaRd(RX_SWR + 0x1274),
             TaRd(RX_SWR + 0x1464), TaRd(RX_SWR + 0x142C), TaRd(RX_SWR + 0x1474));
    LogPrint("rx_b0 frame=%08x slv=%08x scp50=%02x en0=%02x/%02x/%02x wcd[3001,30A5,3465]=%02x/%02x/%02x\r\n",
             TaRd(RX_SWR + 0x101C), TaRd(RX_SWR + 0x1090), SwrRd(RX_SWR, 1, 0x0050),
             SwrRd(RX_SWR, 1, 0x120), SwrRd(RX_SWR, 1, 0x220), SwrRd(RX_SWR, 1, 0x420),
             SwrRd(VA_SWR, 1, 0x3001), SwrRd(VA_SWR, 1, 0x30A5), SwrRd(VA_SWR, 1, 0x3465));
    LogPrint("gpio106=%08x/%08x gpio70=%08x/%08x\r\n",
             TaRd(0x0096A000ULL), TaRd(0x0096A004ULL),
             TaRd(0x00546000ULL), TaRd(0x00546004ULL));
}

static const UCHAR g_FsAddrs[4] = { 0x34, 0x35, 0x80 | 0x34, 0x80 | 0x35 };
static BOOLEAN g_AmpInit;
static BOOLEAN g_AmpProbed;
static UCHAR   g_SipaMaskSe1;              /* bitmask of active 0x28/0x29/0x2B on SE1 */
static UCHAR   g_FsMask;                   /* bitmask of 0x34/0x35 on SE1 (bits 0..1) and SE2 (bits 2..3) */
static USHORT  g_FsDevId[4];

static VOID Fs1815DumpRegs(PCSTR Stage)
{
    ULONG k;

    for (k = 0; k < ARRAYSIZE(g_FsAddrs); k++) {
        USHORT r0 = 0xFFFF, r1 = 0xFFFF, r2 = 0xFFFF, r6 = 0xFFFF;
        USHORT r10 = 0xFFFF, r11 = 0xFFFF, r14 = 0xFFFF, r16 = 0xFFFF;
        USHORT r18 = 0xFFFF, r19 = 0xFFFF, r20 = 0xFFFF, rbc = 0xFFFF;
        USHORT r4 = 0xFFFF, r5 = 0xFFFF, re = 0xFFFF, r4c = 0xFFFF;

        if (!(g_FsMask & (1u << k))) {
            continue;
        }
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x00, &r0);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x01, &r1);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x02, &r2);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x04, &r4);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x05, &r5);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x06, &r6);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x0E, &re);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x10, &r10);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x11, &r11);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x14, &r14);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x16, &r16);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x18, &r18);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x19, &r19);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x20, &r20);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0x4C, &r4c);
        (VOID)TaI2cRead16(g_FsAddrs[k], 0xBC, &rbc);
        LogPrint("  fs1815[%s] %02x: st=%04x id=%04x/%04x pwr=%04x sys=%04x fmt=%04x ch=%04x vol=%04x/%04x r18=%04x r20=%04x bc=%04x ex=%04x/%04x/%04x/%04x\r\n",
                 Stage, g_FsAddrs[k], r0, r1, r2, r10, r11, r14, r19, r6, r16, r18, r20, rbc, r4, r5, re, r4c);
    }
}

static NTSTATUS Fs1815UpdateBits(UCHAR Addr, UCHAR Reg, UCHAR Shift, UCHAR Len, USHORT Val)
{
    USHORT mask, cur = 0, next;
    NTSTATUS st;

    if (Len >= 16) {
        return TaI2cWrite16(Addr, Reg, Val);
    }
    mask = (USHORT)(((1u << Len) - 1u) << Shift);
    st = TaI2cRead16(Addr, Reg, &cur);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    next = (USHORT)((cur & ~mask) | ((Val << Shift) & mask));
    return TaI2cWrite16(Addr, Reg, next);
}

static NTSTATUS Fs1815InitOne(UCHAR Addr)
{
    typedef struct _FS_REG_ENTRY {
        UCHAR  Reg;
        UCHAR  Shift;
        UCHAR  Len;
        USHORT Val;
    } FS_REG_ENTRY;
    /* Exact [COMMON] table decoded from xun vendor/firmware/fs1815.fsm */
    static const FS_REG_ENTRY commonRegs[] = {
        { 0x11, 0, 16, 0x0001 }, { 0x3E, 0, 16, 0x8500 }, { 0x3F, 0, 16, 0x8110 },
        { 0x4C, 0, 16, 0x7008 }, { 0x2A, 0, 16, 0x0030 }, { 0x58, 0, 16, 0x827A },
        { 0x59, 0, 16, 0xD821 }, { 0x5A, 0, 16, 0xD841 }, { 0x5B, 0, 16, 0xD07B },
        { 0x5C, 0, 16, 0xD800 }, { 0x88, 0, 16, 0xA000 }, { 0x89, 0, 16, 0x0032 },
        { 0xBC, 4,  5, 0x0002 }, { 0x4C, 3,  1, 0x0000 }, { 0x22, 0, 16, 0x0028 },
        { 0x23, 0, 16, 0x0027 }, { 0xB8, 0, 16, 0x0002 }, { 0x5D, 0, 16, 0xA001 }
    };
    /* Exact [SCENE 0x0001] (Music) table from fs1815.fsm; keep 0x1F=0xCA91 unlocked first for private regs */
    static const FS_REG_ENTRY sceneRegs[] = {
        { 0x20,  0, 16, 0x07C2 }, { 0x18,  0, 16, 0x000A },
        { 0x3F, 15,  1, 0x0000 }, { 0xBC, 15,  1, 0x0000 },
        { 0x1F,  0, 16, 0xCA91 }, { 0x58,  0, 16, 0x821A },
        { 0x59, 12,  4, 0x0000 }
    };
    USHORT tmp = 0, ch;
    ULONG i;
    NTSTATUS st;

    st = TaI2cWrite16(Addr, 0x10, 0x0002);                  /* soft reset */
    if (!NT_SUCCESS(st)) {
        return st;
    }
    (VOID)TaI2cRead16(Addr, 0x10, &tmp);
    TaSleep(15);
    st = TaI2cWrite16(Addr, 0x10, 0x0001);                  /* enter config power-down */
    for (i = 0; i < 5; i++) {
        tmp = 0;
        if (NT_SUCCESS(TaI2cRead16(Addr, 0x0E, &tmp)) && (tmp == 0x0003u || tmp == 0x0300u)) {
            break;
        }
        TaSleep(2);
    }
    (VOID)TaI2cWrite16(Addr, 0x14, 0x01CA);                 /* live xun ftrace 0x14 = 0x01CA */
    (VOID)TaI2cWrite16(Addr, 0x1F, 0xCA91);                 /* unlock private regs for commonRegs + sceneRegs */
    for (i = 0; i < ARRAYSIZE(commonRegs); i++) {
        (VOID)Fs1815UpdateBits(Addr, commonRegs[i].Reg, commonRegs[i].Shift,
                               commonRegs[i].Len, commonRegs[i].Val);
    }
    for (i = 0; i < ARRAYSIZE(sceneRegs); i++) {
        (VOID)Fs1815UpdateBits(Addr, sceneRegs[i].Reg, sceneRegs[i].Shift,
                               sceneRegs[i].Len, sceneRegs[i].Val);
    }
    /* SE1 (0x34/0x35) = 0x0A, SE2 (0x80|0x34/0x35) = 0x09 per fs1815.fsm */
    ch = (Addr & 0x80u) ? 0x0009u : 0x000Au;
    (VOID)Fs1815UpdateBits(Addr, 0x19, 0, 5, ch);
    (VOID)TaI2cWrite16(Addr, 0x5A, 0xF8E0);
    st = TaI2cWrite16(Addr, 0x1F, 0x0000);                  /* lock private regs */
    return st;
}

static VOID AmpOn(BOOLEAN On)
{
    static const UCHAR sipaRegs[][2] = { { 1, 0x7c }, { 2, 0x21 }, { 5, 0x01 }, { 3, 0x30 }, { 4, 0xc5 },
                                         { 6, 0x48 }, { 7, 0x5d }, { 8, 0x8a }, { 9, 0x0e }, { 10, 0xa4 } };
    static const UCHAR sipaAddrs[3] = { 0x28, 0x29, 0x2B };
    ULONG i, k;
    NTSTATUS status = STATUS_SUCCESS;

    if (!g_AmpProbed) {
        UCHAR buf[2];
        /* Pulse amp reset GPIOs LOW 10ms then HIGH 10ms (dtbo_b.img: GPIO 106 SOUTH = L, GPIO 70 WEST = R) */
        TaWr(0x00400000ULL + 0x00500000ULL + 106ULL * 0x1000ULL + 0x0, 0x200);
        TaWr(0x00400000ULL + 0x00500000ULL + 106ULL * 0x1000ULL + 0x4, 0);
        TaWr(0x00400000ULL + 0x00100000ULL + 70ULL * 0x1000ULL + 0x0, 0x200);
        TaWr(0x00400000ULL + 0x00100000ULL + 70ULL * 0x1000ULL + 0x4, 0);
        TaSleep(10);
        TaWr(0x00400000ULL + 0x00500000ULL + 106ULL * 0x1000ULL + 0x4, 2);
        TaWr(0x00400000ULL + 0x00100000ULL + 70ULL * 0x1000ULL + 0x4, 2);
        TaSleep(10);

        for (k = 0; k < ARRAYSIZE(sipaAddrs); k++) {
            buf[0] = 0;
            if (NT_SUCCESS(TaI2cRead(sipaAddrs[k], 0x00, buf, 1))) {
                g_SipaMaskSe1 |= (UCHAR)(1u << k);
                LogPrint("amp detect SE1 sipa@0x%02x reg00=%02x\r\n", sipaAddrs[k], buf[0]);
            }
        }
        for (k = 0; k < ARRAYSIZE(g_FsAddrs); k++) {
            USHORT reg01 = 0, devId = 0, revId = 0;
            if (NT_SUCCESS(TaI2cRead16(g_FsAddrs[k], 0x03, &revId)) ||
                NT_SUCCESS(TaI2cRead16(g_FsAddrs[k], 0x02, &devId))) {
                (VOID)TaI2cRead16(g_FsAddrs[k], 0x01, &reg01);
                (VOID)TaI2cRead16(g_FsAddrs[k], 0x02, &devId);
                g_FsMask |= (UCHAR)(1u << k);
                g_FsDevId[k] = devId;
                LogPrint("amp detect SE%u fs1815@0x%02x reg01=%04x id=%04x rev=%04x\r\n",
                         (g_FsAddrs[k] & 0x80u) ? 2u : 1u, g_FsAddrs[k] & 0x7Fu, reg01, devId, revId);
            }
        }
        if (g_SipaMaskSe1 == 0 && g_FsMask == 0) {
            g_SipaMaskSe1 = (1u << 2);     /* legacy fallback: 0x2B */
        }
        g_AmpProbed = TRUE;
    }

    for (k = 0; k < ARRAYSIZE(sipaAddrs); k++) {
        if (!(g_SipaMaskSe1 & (1u << k))) {
            continue;
        }
        if (On && !g_AmpInit) {
            for (i = 0; i < ARRAYSIZE(sipaRegs) && NT_SUCCESS(status); i++) {
                status = TaI2cWrite(sipaAddrs[k], sipaRegs[i][0], sipaRegs[i][1]);
            }
        } else {
            status = TaI2cWrite(sipaAddrs[k], 5, On ? 1 : 0);
        }
    }
    for (k = 0; k < ARRAYSIZE(g_FsAddrs); k++) {
        if (!(g_FsMask & (1u << k))) {
            continue;
        }
        if (On) {
            if (!g_AmpInit) {
                status = Fs1815InitOne(g_FsAddrs[k]);
            }
            if (g_FsDevId[k] == 0x0011u) {
                (VOID)TaI2cWrite16(g_FsAddrs[k], 0x1F, 0xCA91);
                (VOID)TaI2cWrite16(g_FsAddrs[k], 0xBC, 0x9433); /* fs1815_start_up dev_id==0x11 */
                (VOID)TaI2cWrite16(g_FsAddrs[k], 0x1F, 0x0000);
            }
            (VOID)TaI2cWrite16(g_FsAddrs[k], 0x11, 0x008F);     /* Boost/PLL/Class-D enable */
            status = TaI2cWrite16(g_FsAddrs[k], 0x10, 0x0000);  /* Power UP / unmute */
        } else {
            status = TaI2cWrite16(g_FsAddrs[k], 0x10, 0x0001);  /* Power down */
            (VOID)TaI2cWrite16(g_FsAddrs[k], 0x11, 0x0000);     /* Boost off */
        }
    }
    if (On && g_FsMask != 0) {
        TaSleep(10);
        LogPrint("amp post gpio106=%08x/%08x gpio70=%08x/%08x\r\n",
                 TaRd(0x0096A000ULL), TaRd(0x0096A004ULL),
                 TaRd(0x00546000ULL), TaRd(0x00546004ULL));
        Fs1815DumpRegs("amp post");
    }
    if (On && !g_AmpInit && NT_SUCCESS(status)) {
        g_AmpInit = TRUE;
    }
    LogPrint("amp %s: sipa=0x%x fs=0x%x status=%08x\r\n", On ? "on" : "off", g_SipaMaskSe1, g_FsMask, status);
}

/* ---------------- DSP graph ---------------- */

static NTSTATUS MemMap(PHYSICAL_ADDRESS Pa, ULONG Bytes, ULONG Prop, ULONG *Handle)
{
    ULONG base = (ULONG)Pa.QuadPart & ~0xFFFu, end = ((ULONG)Pa.QuadPart + Bytes + 0xFFF) & ~0xFFFu;
    ULONG dw[5] = { 3 | (1u << 16), Prop, base, 1, end - base }, op = 0, r[4] = { 0 };
    NTSTATUS status = TaGpr(1, 1, APM_MEM_MAP, FALSE, dw, 5, &op, r, 4, 1000);

    if (NT_SUCCESS(status) && op != 0x02001001) {
        LogPrint("mem map %x: reply %08x status %u\r\n", base, op, r[1]);
        status = STATUS_UNSUCCESSFUL;
    }
    *Handle = NT_SUCCESS(status) ? r[0] : 0;
    return status;
}

static VOID MemUnmap(ULONG *Handle)
{
    if (*Handle != 0) {
        (VOID)TaApm(APM_MEM_UNMAP, FALSE, Handle, 1, NULL, 0);
        *Handle = 0;
    }
}

static NTSTATUS SgCmd(ULONG Opcode)
{
    ULONG dw[6] = { 1, 0x08001005, 8, 0, 1, SG };

    return TaApm(Opcode, TRUE, dw, 6, NULL, 0);
}

NTSTATUS HwStreamPrepare(WDFDEVICE Device, PHYSICAL_ADDRESS RingPa, ULONG RingBytes, ULONG Channels, ULONG Rate)
{
    static const ULONG open[] = {
        1, 0x08001001, 48, 0,  1,  SG, 3,  0x0800100E, 4, 2,  0x0800100F, 4, 1,  0x08001010, 4, 1,
        1, 0x08001000, 64, 0,  1,  CONT, 4,  0x08001011, 8, 1, 3,  0x08001012, 4, 1,  0x08001013, 4, 8192,  0x08001014, 4, 2,
        1, 0x08001002, 32, 0,  1,  SG, CONT, 2,  0x07001006, PULL,  0x07001023, DMA,
        1, 0x08001003, 56, 0,  2,  PULL, 1, 0x08001015, 8, 0, 1,  DMA, 1, 0x08001015, 8, 1, 0,  0,
        1, 0x08001004, 24, 0,  1,  PULL, 1, DMA, 2,  0
    };
    ULONG mf[12], pc[12], ep[8], intf[8];
    PHYSICAL_ADDRESS lo, hi, bound;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    if (Rate != 48000 || (Channels != 1 && Channels != 2)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!g_CodecUp) {
        status = CodecBringUp();
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    if (g_PosVa == NULL) {
        lo.QuadPart = 0; hi.QuadPart = 0xEFFFFFFF; bound.QuadPart = 0;
        g_PosVa = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, bound, MmNonCached);
        if (g_PosVa == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_PosVa, PAGE_SIZE);
        g_PosPa = MmGetPhysicalAddress(g_PosVa);
    }
    status = MemMap(RingPa, RingBytes, 0, &g_RingMap);
    if (NT_SUCCESS(status)) {
        status = MemMap(g_PosPa, PAGE_SIZE, 2, &g_PosMap);
    }
    if (NT_SUCCESS(status)) {
        status = TaApm(APM_GRAPH_OPEN, TRUE, open, ARRAYSIZE(open), NULL, 0);
        g_GraphOpen = NT_SUCCESS(status);
    }
    /* PULL media format: PCM 16-bit, Channels, LSB aligned, Q15, LE, channel map 1(,2) */
    mf[0] = PULL; mf[1] = 0x0800100C; mf[2] = 32; mf[3] = 0;
    mf[4] = 1; mf[5] = 0x09001000; mf[6] = 20; mf[7] = Rate;
    mf[8] = 16 | (1u << 16); mf[9] = 16 | (15u << 16); mf[10] = 1 | (Channels << 16);
    mf[11] = (Channels == 2) ? (1 | (2u << 8)) : 1;
    pc[0] = PULL; pc[1] = 0x0800100A; pc[2] = 28; pc[3] = 0;
    pc[4] = (ULONG)RingPa.QuadPart; pc[5] = 1; pc[6] = RingBytes; pc[7] = g_RingMap;
    pc[8] = (ULONG)g_PosPa.QuadPart; pc[9] = 1; pc[10] = g_PosMap; pc[11] = 0;
    ep[0] = DMA; ep[1] = 0x08001017; ep[2] = 12; ep[3] = 0; ep[4] = Rate; ep[5] = 16 | (Channels << 16); ep[6] = 1; ep[7] = 0;
    intf[0] = DMA; intf[1] = 0x08001063; intf[2] = 12; intf[3] = 0; intf[4] = 1; intf[5] = 2;   /* RXTX, RX_1 */
    intf[6] = (Channels == 2) ? 3 : 1; intf[7] = 0;
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, mf, 12, NULL, 0);
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, pc, 12, NULL, 0);
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, ep, 8, NULL, 0);
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, intf, 8, NULL, 0);
    if (NT_SUCCESS(status)) status = SgCmd(APM_GRAPH_PREPARE);
    g_Stopped = FALSE;
    LogPrint("hw prepare: ring %llx + %x, %u ch: %08x (maps %x %x)\r\n", (ULONGLONG)RingPa.QuadPart, RingBytes,
             Channels, status, g_RingMap, g_PosMap);
    if (!NT_SUCCESS(status)) {
        HwStreamRelease(Device);
    }
    return status;
}

NTSTATUS HwStreamRun(WDFDEVICE Device)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    if (!g_GraphOpen) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (g_Stopped) {
        (VOID)SgCmd(APM_GRAPH_PREPARE);
        g_Stopped = FALSE;
    }
    status = SgCmd(APM_GRAPH_START);
    if (NT_SUCCESS(status)) {
        if (!g_PortsUp) {
            PortsAndPa();
            g_PortsUp = TRUE;
        }
        AmpOn(TRUE);
        ApplyVolume();
        TaWr(RX_MACRO + 0x180, 0x70);                   /* RX INT0_1 MIX1 INP1 = RX2 (AIF2_PB ch0) */
        TaWr(RX_MACRO + 0x190, 0x08);                   /* RX INT2_1 MIX1 INP0 = RX3 (AIF2_PB ch1) */
        TaWr(RX_MACRO + 0x404, 0x48);
        TaWr(RX_MACRO + 0x408, 0x67);
        TaWr(RX_MACRO + 0x40C, 0x8F);
        TaWr(RX_MACRO + 0x414, 0x00);
        TaWr(RX_MACRO + 0x42C, 0x14);
        TaWr(RX_MACRO + 0x440, 0x09);
        TaWr(RX_MACRO + 0x484, 0x18);
        TaWr(RX_MACRO + 0x4C0, 0x09);
        TaWr(RX_MACRO + 0x504, 0x18);
        TaWr(RX_MACRO + 0x50C, 0x8F);
        TaWr(RX_MACRO + 0x514, 0xFB);
        TaWr(RX_MACRO + 0x54C, 0x01);
        TaWr(RX_MACRO + 0x418, 0x04);
        TaWr(RX_MACRO + 0x400, 0x24);                   /* RX0 path + interp clk on, unmuted (bit 4=0) */
        TaWr(RX_MACRO + 0x518, 0x04);
        TaWr(RX_MACRO + 0x500, 0x24);                   /* RX2 path + interp clk on, unmuted (bit 4=0, stock 0x24) */
        LogPrint("rx_macro ctl: 400=%02x 480=%02x 500=%02x 40c=%02x 48c=%02x 50c=%02x vol=%02x/%02x/%02x\r\n",
                 TaRd(RX_MACRO + 0x400), TaRd(RX_MACRO + 0x480), TaRd(RX_MACRO + 0x500),
                 TaRd(RX_MACRO + 0x40C), TaRd(RX_MACRO + 0x48C), TaRd(RX_MACRO + 0x50C),
                 TaRd(RX_MACRO + 0x414), TaRd(RX_MACRO + 0x494), TaRd(RX_MACRO + 0x514));
        LogPrint("rx_macro mux: 040=%02x 044=%02x 070=%02x 074=%02x 080=%02x 084=%02x 088=%02x 08c=%02x 180=%02x 190=%02x\r\n",
                 TaRd(RX_MACRO + 0x040), TaRd(RX_MACRO + 0x044),
                 TaRd(RX_MACRO + 0x070), TaRd(RX_MACRO + 0x074),
                 TaRd(RX_MACRO + 0x080), TaRd(RX_MACRO + 0x084),
                 TaRd(RX_MACRO + 0x088), TaRd(RX_MACRO + 0x08C),
                 TaRd(RX_MACRO + 0x180), TaRd(RX_MACRO + 0x190));
        LogPrint("rx_macro cfg: 100=%02x 104=%02x 108=%02x 200=%02x 204=%02x 220=%02x 234=%02x 404=%02x 418=%02x 484=%02x 498=%02x 504=%02x 518=%02x swr=%08x/%08x\r\n",
                 TaRd(RX_MACRO + 0x100), TaRd(RX_MACRO + 0x104), TaRd(RX_MACRO + 0x108),
                 TaRd(RX_MACRO + 0x200), TaRd(RX_MACRO + 0x204), TaRd(RX_MACRO + 0x220), TaRd(RX_MACRO + 0x234),
                 TaRd(RX_MACRO + 0x404), TaRd(RX_MACRO + 0x418),
                 TaRd(RX_MACRO + 0x484), TaRd(RX_MACRO + 0x498),
                 TaRd(RX_MACRO + 0x504), TaRd(RX_MACRO + 0x518),
                 TaRd(RX_SWR + 0x105C), TaRd(RX_SWR + 0x200));
        if (g_FsMask != 0) {
            Fs1815DumpRegs("hw run");
        }
        if (g_PosVa != NULL) {
            volatile ULONG *p = (volatile ULONG *)g_PosVa;
            ULONG f0 = p[0], i0 = p[1], f1, i1;
            TaSleep(30);
            f1 = p[0]; i1 = p[1];
            LogPrint("dsp pos: t0=%u/%u t30=%u/%u\r\n", f0, i0, f1, i1);
        }
        g_Running = TRUE;
    }
    LogPrint("hw run: %08x\r\n", status);
    return status;
}

NTSTATUS HwStreamPause(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    if (!g_Running) {
        return STATUS_SUCCESS;
    }
    g_Running = FALSE;
    AmpOn(FALSE);
    TaWr(RX_MACRO + 0x400, 0x04);
    TaWr(RX_MACRO + 0x480, 0x04);
    TaWr(RX_MACRO + 0x500, 0x04);
    (VOID)SgCmd(APM_GRAPH_STOP);
    g_Stopped = TRUE;
    LogPrint("hw pause\r\n");
    return STATUS_SUCCESS;
}

VOID HwStreamRelease(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    if (g_Running) {
        (VOID)HwStreamPause(Device);
    }
    if (g_GraphOpen) {
        if (!g_Stopped) {
            (VOID)SgCmd(APM_GRAPH_STOP);
        }
        (VOID)SgCmd(APM_GRAPH_CLOSE);
        g_GraphOpen = FALSE;
    }
    MemUnmap(&g_RingMap);
    MemUnmap(&g_PosMap);
    LogPrint("hw release\r\n");
}

BOOLEAN HwReadPosition(ULONG *ByteIndex)
{
    if (!g_Running || g_PosVa == NULL) {
        *ByteIndex = 0;
        return FALSE;
    }
    *ByteIndex = ((volatile ULONG *)g_PosVa)[1];     /* {frame_counter, index, ts lo, ts hi} */
    return TRUE;
}

VOID HwSetVolume(WDFDEVICE Device, LONG Level, BOOLEAN Mute)
{
    UNREFERENCED_PARAMETER(Device);
    g_VolDb = Level / 65536;
    g_Mute = Mute;
    ApplyVolume();
    LogPrint("volume %d dB%s\r\n", g_VolDb, Mute ? " (muted)" : "");
}
