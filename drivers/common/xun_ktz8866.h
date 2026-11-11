/*
 * drivers/common/xun_ktz8866.h - KTZ8866 backlight controller on QUP0 SE1 I2C (0x04A84000, @0x11)
 * + HWEN TLMM GPIO 102 SOUTH (0x00966000) for Redmi Pad SE (xun, SM6225).
 *
 * Ensures GCC_QUP0_SE1 clock vote, TLMM GPIO 4/5 WEST func 1 pinmux, and GENI I2C FIFO packing/IRQ
 * setup (matching XunBattery\hwio.c) inside the caller's XunQupLockAcquire(&Lock, ...).
 */
#pragma once

#include "xun_qup_lock.h"

#define XUN_KTZ_TLMM_BASE           0x00400000ULL
#define XUN_KTZ_TLMM_WEST_PA        (XUN_KTZ_TLMM_BASE + 0x00100000ULL)
#define XUN_KTZ_TLMM_HWEN_PA        (XUN_KTZ_TLMM_BASE + 0x00500000ULL + 102ULL * 0x1000ULL)
#define XUN_KTZ_GCC_BASE            0x01400000ULL
#define XUN_KTZ_GCC_APCS_VOTE_OFF   0x7900C
#define XUN_KTZ_GCC_QUP0_SE1_VOTE   ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11))
#define XUN_KTZ_GCC_QUP0_S1_CBCR    0x1F274
#define XUN_KTZ_GCC_QUP0_S1_RCG     0x1F278
#define XUN_KTZ_QUP0_SE1_PA         0x04A84000ULL

#define XUN_KTZ_I2C_ADDR            0x11
#define XUN_KTZ_REG_BL_LSB          0x04
#define XUN_KTZ_REG_BL_MSB          0x05
#define XUN_KTZ_REG_BL_EN           0x09

#define XUN_KTZ_GENI_FORCE_DEF      0x20
#define XUN_KTZ_GENI_OUTPUT_CTRL    0x24
#define XUN_KTZ_GENI_CGC_CTRL       0x28
#define XUN_KTZ_SE_GENI_STATUS      0x40
#define XUN_KTZ_GENI_SER_M_CLK_CFG  0x48
#define XUN_KTZ_GENI_FW_REV_RO      0x68
#define XUN_KTZ_SE_GENI_CLK_SEL     0x7C
#define XUN_KTZ_SE_GENI_BYTE_GRAN   0x254
#define XUN_KTZ_SE_GENI_DMA_MODE_EN 0x258
#define XUN_KTZ_SE_TX_PACK0         0x260
#define XUN_KTZ_SE_TX_PACK1         0x264
#define XUN_KTZ_SE_I2C_TX_LEN       0x26C
#define XUN_KTZ_SE_I2C_SCL_CNT      0x278
#define XUN_KTZ_SE_RX_PACK0         0x284
#define XUN_KTZ_SE_RX_PACK1         0x288
#define XUN_KTZ_SE_M_CMD0           0x600
#define XUN_KTZ_SE_M_CMD_CTRL       0x604
#define XUN_KTZ_SE_M_IRQ_STATUS     0x610
#define XUN_KTZ_SE_M_IRQ_EN         0x614
#define XUN_KTZ_SE_M_IRQ_CLEAR      0x618
#define XUN_KTZ_SE_S_IRQ_CLEAR      0x648
#define XUN_KTZ_SE_TX_FIFOn         0x700
#define XUN_KTZ_SE_DMA_TX_CLR       0xC44
#define XUN_KTZ_SE_DMA_RX_CLR       0xD44
#define XUN_KTZ_SE_GSI_EVENT_EN     0xE18
#define XUN_KTZ_SE_IRQ_EN           0xE1C

#define XUN_KTZ_M_CMD_DONE          (1u << 0)
#define XUN_KTZ_M_CMD_CANCEL_EN     (1u << 4)
#define XUN_KTZ_M_ERR_MASK          ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 13))

typedef struct _XUN_KTZ_BUS {
    volatile UCHAR *Se1;
    volatile UCHAR *Hwen;
    BOOLEAN         Se1Ready;
    ULONG           LastFw;
    ULONG           LastCbcr;
    ULONG           LastIrq;
} XUN_KTZ_BUS;

static __inline ULONG XunKtzRd(volatile UCHAR *B, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(B + Off));
}

static __inline VOID XunKtzWrMmio(volatile UCHAR *B, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), Val);
}

static __inline NTSTATUS XunKtzInit(_Out_ XUN_KTZ_BUS *B)
{
    PHYSICAL_ADDRESS pa;

    RtlZeroMemory(B, sizeof(*B));
    pa.QuadPart = (LONGLONG)XUN_KTZ_QUP0_SE1_PA;
    B->Se1 = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)XUN_KTZ_TLMM_HWEN_PA;
    B->Hwen = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    return (B->Se1 != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static __inline VOID XunKtzDeinit(_Inout_ XUN_KTZ_BUS *B)
{
    if (B->Hwen != NULL) {
        MmUnmapIoSpace((PVOID)B->Hwen, 0x1000);
        B->Hwen = NULL;
    }
    if (B->Se1 != NULL) {
        MmUnmapIoSpace((PVOID)B->Se1, 0x4000);
        B->Se1 = NULL;
    }
    B->Se1Ready = FALSE;
}

static __inline BOOLEAN XunKtzEnsureSe1(_Inout_ XUN_KTZ_BUS *B)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *vote = NULL, *gcc = NULL, *pins = NULL;
    ULONG cbcr = 0, clkSel = 0, fw, i, lvl, perf;

    if (B->Se1 == NULL) {
        return FALSE;
    }
    if (B->Se1Ready && (XunKtzRd(B->Se1, XUN_KTZ_GENI_SER_M_CLK_CFG) & 1u) != 0 &&
        XunKtzRd(B->Se1, XUN_KTZ_SE_TX_PACK0) != 0) {
        return TRUE;
    }

    pa.QuadPart = (LONGLONG)(XUN_KTZ_GCC_BASE + (XUN_KTZ_GCC_APCS_VOTE_OFF & ~0xFFFull));
    vote = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)(XUN_KTZ_GCC_BASE + (XUN_KTZ_GCC_QUP0_S1_CBCR & ~0xFFFull));
    gcc = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (vote != NULL && gcc != NULL) {
        XunKtzWrMmio(vote, XUN_KTZ_GCC_APCS_VOTE_OFF & 0xFFF,
                     XunKtzRd(vote, XUN_KTZ_GCC_APCS_VOTE_OFF & 0xFFF) | XUN_KTZ_GCC_QUP0_SE1_VOTE);
        for (i = 0; i < 1000; i++) {
            cbcr = XunKtzRd(gcc, XUN_KTZ_GCC_QUP0_S1_CBCR & 0xFFF);
            if ((cbcr & (1u << 31)) == 0) {
                break;
            }
            KeStallExecutionProcessor(1);
        }
        B->LastCbcr = cbcr;
        for (lvl = 0; lvl < 8; lvl++) {
            perf = XunKtzRd(gcc, (XUN_KTZ_GCC_QUP0_S1_RCG & 0xFFF) + 0x1C + 4 * lvl);
            if (((perf >> 8) & 7) == 0 && ((perf >> 12) & 3) == 0 && (perf & 0x1F) <= 1) {
                clkSel = lvl;
                break;
            }
        }
    }
    if (gcc != NULL) {
        MmUnmapIoSpace((PVOID)gcc, 0x1000);
    }
    if (vote != NULL) {
        MmUnmapIoSpace((PVOID)vote, 0x1000);
    }

    /* TLMM WEST GPIO 4 (SDA) & GPIO 5 (SCL) -> func 1 (qup1), pull none, 2 mA */
    pa.QuadPart = (LONGLONG)(XUN_KTZ_TLMM_WEST_PA + 4ULL * 0x1000ULL);
    pins = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x2000, PAGE_READWRITE | PAGE_NOCACHE);
    if (pins != NULL) {
        XunKtzWrMmio(pins, 0x0000, (1u << 2));
        XunKtzWrMmio(pins, 0x1000, (1u << 2));
        MmUnmapIoSpace((PVOID)pins, 0x2000);
    }

    fw = XunKtzRd(B->Se1, XUN_KTZ_GENI_FW_REV_RO);
    B->LastFw = fw;
    if (((fw >> 8) & 0xFF) != 3) {
        B->Se1Ready = FALSE;
        return FALSE;
    }

    if (!B->Se1Ready || XunKtzRd(B->Se1, XUN_KTZ_SE_TX_PACK0) == 0) {
        ULONG cfg[4], idx = 7, cfg0, cfg1;

        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_GSI_EVENT_EN, 0);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_IRQ_CLEAR, 0xFFFFFFFF);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_S_IRQ_CLEAR, 0xFFFFFFFF);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_DMA_TX_CLR, 0xFFFFFFFF);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_DMA_RX_CLR, 0xFFFFFFFF);
        XunKtzWrMmio(B->Se1, XUN_KTZ_GENI_CGC_CTRL, XunKtzRd(B->Se1, XUN_KTZ_GENI_CGC_CTRL) | 0x7F);
        XunKtzWrMmio(B->Se1, XUN_KTZ_GENI_OUTPUT_CTRL, 0x7F);
        XunKtzWrMmio(B->Se1, XUN_KTZ_GENI_FORCE_DEF, 1);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_IRQ_EN, XunKtzRd(B->Se1, XUN_KTZ_SE_IRQ_EN) | (1u << 2) | (1u << 3));
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_GENI_DMA_MODE_EN, XunKtzRd(B->Se1, XUN_KTZ_SE_GENI_DMA_MODE_EN) & ~1u);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_IRQ_EN,
                     XunKtzRd(B->Se1, XUN_KTZ_SE_M_IRQ_EN) | XUN_KTZ_M_CMD_DONE | XUN_KTZ_M_ERR_MASK | (1u << 26) | (1u << 27));
        for (i = 0; i < 4; i++) {
            cfg[i] = (idx << 5) | (1u << 4) | (7u << 1);
            idx += 8;
        }
        cfg[3] |= 1u;
        cfg0 = cfg[0] | (cfg[1] << 10);
        cfg1 = cfg[2] | (cfg[3] << 10);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_TX_PACK0, cfg0);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_TX_PACK1, cfg1);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_RX_PACK0, cfg0);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_RX_PACK1, cfg1);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_GENI_BYTE_GRAN, 0);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_GENI_CLK_SEL, clkSel);
        XunKtzWrMmio(B->Se1, XUN_KTZ_GENI_SER_M_CLK_CFG, (2u << 4) | 1u);
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_I2C_SCL_CNT, (5u << 20) | (12u << 10) | 24u);
    }
    B->Se1Ready = TRUE;
    return TRUE;
}

static __inline VOID XunKtzSetHwen(_In_ XUN_KTZ_BUS *B)
{
    if (B->Hwen != NULL) {
        XunKtzWrMmio(B->Hwen, 0x0, (1u << 9));
        XunKtzWrMmio(B->Hwen, 0x4, 0x2);
    }
}

static __inline BOOLEAN XunKtzWriteReg(_Inout_ XUN_KTZ_BUS *B, _In_ UCHAR Reg, _In_ UCHAR Val)
{
    ULONG i, st = 0;

    if (!XunKtzEnsureSe1(B)) {
        return FALSE;
    }
    XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_IRQ_CLEAR, 0xFFFFFFFF);
    XunKtzWrMmio(B->Se1, XUN_KTZ_SE_I2C_TX_LEN, 2);
    XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_CMD0, (1u << 27) | ((ULONG)XUN_KTZ_I2C_ADDR << 9));
    XunKtzWrMmio(B->Se1, XUN_KTZ_SE_TX_FIFOn, (ULONG)Reg | ((ULONG)Val << 8));
    for (i = 0; i < 20000; i++) {
        st = XunKtzRd(B->Se1, XUN_KTZ_SE_M_IRQ_STATUS);
        if (st & (XUN_KTZ_M_CMD_DONE | XUN_KTZ_M_ERR_MASK)) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    B->LastIrq = st;
    XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_IRQ_CLEAR, st);
    if ((st & XUN_KTZ_M_ERR_MASK) || !(st & XUN_KTZ_M_CMD_DONE)) {
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_CMD_CTRL, (1u << 2));
        for (i = 0; i < 1000; i++) {
            if (XunKtzRd(B->Se1, XUN_KTZ_SE_M_IRQ_STATUS) & XUN_KTZ_M_CMD_CANCEL_EN) {
                break;
            }
            KeStallExecutionProcessor(1);
        }
        if ((XunKtzRd(B->Se1, XUN_KTZ_SE_GENI_STATUS) & 1u) != 0) {
            XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_CMD_CTRL, (1u << 1));
            KeStallExecutionProcessor(100);
        }
        XunKtzWrMmio(B->Se1, XUN_KTZ_SE_M_IRQ_CLEAR, 0xFFFFFFFF);
        return FALSE;
    }
    return TRUE;
}
