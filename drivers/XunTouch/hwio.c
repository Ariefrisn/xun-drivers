/*
 * Low level SoC access for XunTouch: MMIO, TLMM, GCC QUP0 SE0, GENI SPI full-duplex FIFO,
 * and FocalTech FT8203 SPI v2 protocol + reflected CRC16 (0x8408).
 */
#include "driver.h"

#define FTS_SPI_SCRATCH_SZ 2048

/* ---- MMIO ---------------------------------------------------------------- */

NTSTATUS MmioMap(PMMIO_RANGE Range, ULONGLONG Pa, SIZE_T Size)
{
    Range->Pa.QuadPart = (LONGLONG)Pa;
    Range->Size = Size;
    Range->Va = (volatile UCHAR *)MmMapIoSpaceEx(Range->Pa, Size, PAGE_READWRITE | PAGE_NOCACHE);
    return (Range->Va != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

VOID MmioUnmap(PMMIO_RANGE Range)
{
    if (Range->Va != NULL) {
        MmUnmapIoSpace((PVOID)Range->Va, Range->Size);
        Range->Va = NULL;
    }
}

/* ---- TLMM ---------------------------------------------------------------- */

NTSTATUS TlmmPinMap(PTLMM_PIN P, ULONGLONG Tile, ULONG Pin)
{
    P->Pin = Pin;
    return MmioMap(&P->Regs, TLMM_PIN_PA(Tile, Pin), TLMM_PIN_STRIDE);
}

VOID TlmmPinUnmap(PTLMM_PIN P)
{
    MmioUnmap(&P->Regs);
}

VOID TlmmConfig(PTLMM_PIN P, ULONG Func, ULONG Pull, ULONG DriveMa, BOOLEAN Output)
{
    ULONG ctl = (Pull & 0x3) | ((Func & 0xF) << 2) | ((TLMM_DRV_MA(DriveMa) & 0x7) << 6);
    if (Output) {
        ctl |= (1u << 9);
    }
    MmioWrite32(&P->Regs, TLMM_CTL, ctl);
}

VOID TlmmSetOutput(PTLMM_PIN P, BOOLEAN High)
{
    MmioWrite32(&P->Regs, TLMM_IO, High ? 0x2 : 0x0);
}

BOOLEAN TlmmGetInput(PTLMM_PIN P)
{
    return (MmioRead32(&P->Regs, TLMM_IO) & 0x1) != 0;
}

/* ---- GCC QUP0 SE0 -------------------------------------------------------- */

static NTSTATUS GccRcgSetXo(PMMIO_RANGE Page, ULONG Off)
{
    ULONG i;

    MmioWrite32(Page, Off + GCC_RCG_CFG_OFF, 0x1);
    MmioWrite32(Page, Off, MmioRead32(Page, Off) | GCC_RCG_CMD_UPDATE);
    for (i = 0; i < 1000; i++) {
        if ((MmioRead32(Page, Off) & GCC_RCG_CMD_UPDATE) == 0) {
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS GccEnableQup0Se0(VOID)
{
    MMIO_RANGE vote = {0}, qup = {0};
    NTSTATUS status;
    ULONG cbcr = 0, i;

    status = MmioMap(&vote, GCC_BASE + (GCC_APCS_VOTE_REG & ~0xFFFu), 0x1000);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = MmioMap(&qup, GCC_BASE + (GCC_QUP0_S0_CBCR & ~0xFFFu), 0x1000);
    if (!NT_SUCCESS(status)) {
        MmioUnmap(&vote);
        return status;
    }

    MmioWrite32(&vote, GCC_APCS_VOTE_REG & 0xFFF,
                MmioRead32(&vote, GCC_APCS_VOTE_REG & 0xFFF) |
                (1u << GCC_QUP0_M_AHB_BIT) | (1u << GCC_QUP0_S_AHB_BIT) |
                (1u << GCC_QUP0_CORE_BIT) | (1u << GCC_QUP0_CORE2X_BIT) |
                (1u << GCC_QUP0_S0_BIT));

    status = GccRcgSetXo(&qup, GCC_QUP0_S0_RCG_CMD & 0xFFF);
    LogPrint("GCC: vote=%08x rcg_update=%08x\n",
             MmioRead32(&vote, GCC_APCS_VOTE_REG & 0xFFF), status);

    for (i = 0; i < 1000; i++) {
        cbcr = MmioRead32(&qup, GCC_QUP0_S0_CBCR & 0xFFF);
        if ((cbcr & GCC_CBCR_CLK_OFF) == 0) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    LogPrint("GCC: qup0 s0 cbcr=%08x (%s)\n", cbcr, (cbcr & GCC_CBCR_CLK_OFF) ? "OFF" : "on");

    MmioUnmap(&qup);
    MmioUnmap(&vote);
    return (cbcr & GCC_CBCR_CLK_OFF) ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
}

/* ---- GENI SPI ------------------------------------------------------------ */

static VOID GeniConfigPacking(PGENI_SPI Bus)
{
    ULONG cfg[4] = {0};
    const int bpw = 8, packWords = 4;
    int idxStart = bpw - 1, idx = idxStart, i;
    int iter = (8 * packWords) / 8;
    ULONG cfg0, cfg1;

    for (i = 0; i < iter; i++) {
        cfg[i] = ((ULONG)idx << 5) | (1u << 4) | ((ULONG)(bpw - 1) << 1);
        idx = ((i + 1) * 8) + idxStart;
    }
    cfg[iter - 1] |= 1u;
    cfg0 = cfg[0] | (cfg[1] << 10);
    cfg1 = cfg[2] | (cfg[3] << 10);

    MmioWrite32(&Bus->Se, SE_GENI_TX_PACKING_CFG0, cfg0);
    MmioWrite32(&Bus->Se, SE_GENI_TX_PACKING_CFG1, cfg1);
    MmioWrite32(&Bus->Se, SE_GENI_RX_PACKING_CFG0, cfg0);
    MmioWrite32(&Bus->Se, SE_GENI_RX_PACKING_CFG1, cfg1);
    MmioWrite32(&Bus->Se, SE_GENI_BYTE_GRAN, bpw / 16);
}

static VOID GeniCancel(PGENI_SPI Bus);

static VOID GeniIrqClear(PGENI_SPI Bus)
{
    MmioWrite32(&Bus->Se, SE_GSI_EVENT_EN, 0);
    if (MmioRead32(&Bus->Se, SE_GENI_STATUS) & 1u) {
        GeniCancel(Bus);
    }
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_DMA_TX_IRQ_CLR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_DMA_RX_IRQ_CLR, 0xFFFFFFFF);
}

NTSTATUS GeniSpiInit(PGENI_SPI Bus, ULONGLONG SeBase)
{
    NTSTATUS status;
    ULONG v;
    MMIO_RANGE tlmm = {0};
    ULONG pin;

    /* 1. Direct TLMM pinmux: map TLMM WEST (0x00500000) for SPI pins GPIO 0..3 */
    status = MmioMap(&tlmm, TLMM_BASE + TLMM_TILE_WEST, 0x4000);
    if (NT_SUCCESS(status)) {
        for (pin = 0; pin < 4; pin++) {
            /* FUNC_SEL = 1 (qup0), PULL = 3 (pull-up), DRV = 2 (6mA), OE = 0 */
            ULONG ctl = (TLMM_PULL_UP & 0x3) | ((TS_PIN_SPI_FUNC & 0xF) << 2) | ((TLMM_DRV_MA(6) & 0x7) << 6);
            MmioWrite32(&tlmm, pin * TLMM_PIN_STRIDE + TLMM_CTL, ctl);
        }
        MmioUnmap(&tlmm);
        LogPrint("GeniSpiInit: programmed TLMM GPIO 0..3 FUNC_SEL=qup0 (0x%03x)\n",
                 (TLMM_PULL_UP & 0x3) | ((TS_PIN_SPI_FUNC & 0xF) << 2) | ((TLMM_DRV_MA(6) & 0x7) << 6));
    }

    status = MmioMap(&Bus->Se, SeBase, GENI_SE_SIZE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Bus->ScratchSize = FTS_SPI_SCRATCH_SZ;
    Bus->TxScratch = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Bus->ScratchSize, XUN_POOL_TAG);
    Bus->RxScratch = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Bus->ScratchSize, XUN_POOL_TAG);
    if (Bus->TxScratch == NULL || Bus->RxScratch == NULL) {
        GeniSpiDeinit(Bus);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Bus->Proto = (MmioRead32(&Bus->Se, GENI_FW_REVISION_RO) >> 8) & 0xFF;
    Bus->TxDepth = (MmioRead32(&Bus->Se, SE_HW_PARAM_0) >> 16) & 0x3F;
    LogPrint("GENI SPI %llx: fw_rev=%08x proto=%u tx_depth=%u status=%08x\n",
             SeBase, MmioRead32(&Bus->Se, GENI_FW_REVISION_RO), Bus->Proto, Bus->TxDepth,
             MmioRead32(&Bus->Se, SE_GENI_STATUS));
    if (Bus->Proto != GENI_PROTO_SPI) {
        GeniSpiDeinit(Bus);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Bus->TxDepth == 0) {
        Bus->TxDepth = 16;
    }

    GeniIrqClear(Bus);
    MmioWrite32(&Bus->Se, GENI_CGC_CTRL, MmioRead32(&Bus->Se, GENI_CGC_CTRL) | 0x7F);
    MmioWrite32(&Bus->Se, GENI_OUTPUT_CTRL, 0x7F);
    MmioWrite32(&Bus->Se, GENI_FORCE_DEFAULT_REG, 1);
    MmioWrite32(&Bus->Se, SE_IRQ_EN, MmioRead32(&Bus->Se, SE_IRQ_EN) | (1u << 2) | (1u << 3));

    /* Disable DMA mode explicitly (SE_DMA_MODE_EN = 0) */
    MmioWrite32(&Bus->Se, SE_GENI_DMA_MODE_EN, 0);
    MmioWrite32(&Bus->Se, SE_GSI_EVENT_EN, 0);
    MmioWrite32(&Bus->Se, SE_GENI_RX_WATERMARK_REG, Bus->TxDepth - 1);
    MmioWrite32(&Bus->Se, SE_GENI_RX_RFR_WATERMARK_REG, Bus->TxDepth);
    v = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_EN);
    v |= M_CMD_DONE_EN | M_SPI_ERR_MASK | M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN | M_TX_FIFO_WATERMARK_EN;
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_EN, v);
    GeniConfigPacking(Bus);

    /* SPI Mode 0 (CPOL=0, CPHA=0), 8-bit words, CS0 active-low continuous across command */
    MmioWrite32(&Bus->Se, SE_SPI_CPHA, 0);
    MmioWrite32(&Bus->Se, SE_SPI_CPOL, 0);
    MmioWrite32(&Bus->Se, SE_SPI_DEMUX_OUTPUT_INV, 0);
    MmioWrite32(&Bus->Se, SE_SPI_DEMUX_SEL, 0);
    MmioWrite32(&Bus->Se, SE_SPI_PRE_POST_CMD_DLY, 0);
    MmioWrite32(&Bus->Se, SE_SPI_TRANS_CFG, 0);
    MmioWrite32(&Bus->Se, SE_SPI_WORD_LEN, 8 - 4);
    MmioWrite32(&Bus->Se, SE_GENI_CLK_SEL, 0);

    /* Turn on GENI_SER_M_CLK_CFG */
    MmioWrite32(&Bus->Se, GENI_SER_M_CLK_CFG, (SPI_6P4M_CLK_DIV << 4) | 1u);
    LogPrint("GeniSpiInit: GENI_SER_M_CLK_CFG = %08x, DMA_MODE_EN = %08x\n",
             MmioRead32(&Bus->Se, GENI_SER_M_CLK_CFG),
             MmioRead32(&Bus->Se, SE_GENI_DMA_MODE_EN));
    return STATUS_SUCCESS;
}

VOID GeniSpiDeinit(PGENI_SPI Bus)
{
    if (Bus->TxScratch != NULL) {
        ExFreePoolWithTag(Bus->TxScratch, XUN_POOL_TAG);
        Bus->TxScratch = NULL;
    }
    if (Bus->RxScratch != NULL) {
        ExFreePoolWithTag(Bus->RxScratch, XUN_POOL_TAG);
        Bus->RxScratch = NULL;
    }
    Bus->ScratchSize = 0;
    MmioUnmap(&Bus->Se);
}

static VOID GeniCancel(PGENI_SPI Bus)
{
    ULONG i;

    MmioWrite32(&Bus->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
    for (i = 0; i < 1000; i++) {
        if (MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS) & M_CMD_CANCEL_EN) {
            break;
        }
        KeStallExecutionProcessor(10);
    }
    if ((MmioRead32(&Bus->Se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) != 0) {
        MmioWrite32(&Bus->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
        for (i = 0; i < 1000; i++) {
            if (MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS) & M_CMD_ABORT_EN) {
                break;
            }
            KeStallExecutionProcessor(10);
        }
    }
    {
        ULONG pending = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS);
        MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, pending | 0xFFFFFFFF);
        MmioWrite32(&Bus->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFF);
    }
}

NTSTATUS GeniSpiXfer(PGENI_SPI Bus, const UCHAR *Tx, UCHAR *Rx, ULONG Len)
{
    ULONG txDone = 0, rxDone = 0, spins = 0, st = 0;

    if (Len == 0) {
        return STATUS_SUCCESS;
    }
    if (MmioRead32(&Bus->Se, SE_GENI_STATUS) & 1u) {
        GeniCancel(Bus);
    }
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_SPI_TRANS_CFG, 0);
    MmioWrite32(&Bus->Se, SE_SPI_WORD_LEN, 8 - 4);
    MmioWrite32(&Bus->Se, SE_SPI_TX_TRANS_LEN, Len);
    MmioWrite32(&Bus->Se, SE_SPI_RX_TRANS_LEN, Len);
    MmioWrite32(&Bus->Se, SE_GENI_M_CMD0, (SPI_OP_TX_RX << M_OPCODE_SHIFT));

    while ((txDone < Len || rxDone < Len) && spins < 2000) {
        BOOLEAN progress = FALSE;
        ULONG stNow = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS);
        if (stNow & M_SPI_ERR_MASK) {
            st = stNow;
            break;
        }
        while (txDone < Len && (MmioRead32(&Bus->Se, SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MASK) < Bus->TxDepth) {
            ULONG word = 0, n = min(4u, Len - txDone);
            RtlCopyMemory(&word, Tx + txDone, n);
            MmioWrite32(&Bus->Se, SE_GENI_TX_FIFOn, word);
            txDone += n;
            progress = TRUE;
        }
        {
            ULONG rwords = MmioRead32(&Bus->Se, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK;
            while (rwords-- && rxDone < Len) {
                ULONG word = MmioRead32(&Bus->Se, SE_GENI_RX_FIFOn);
                ULONG n = min(4u, Len - rxDone);
                RtlCopyMemory(Rx + rxDone, &word, n);
                rxDone += n;
                progress = TRUE;
            }
        }
        if (!progress) {
            KeStallExecutionProcessor(1);
            spins++;
        }
    }

    for (spins = 0; spins < 1000; spins++) {
        st = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_SPI_ERR_MASK)) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    while (rxDone < Len) {
        ULONG rwords = MmioRead32(&Bus->Se, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK;
        if (rwords == 0) {
            break;
        }
        while (rwords-- && rxDone < Len) {
            ULONG word = MmioRead32(&Bus->Se, SE_GENI_RX_FIFOn);
            ULONG n = min(4u, Len - rxDone);
            RtlCopyMemory(Rx + rxDone, &word, n);
            rxDone += n;
        }
    }
    Bus->LastIrqStatus = st;
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, st);

    if ((st & M_SPI_ERR_MASK) || !(st & M_CMD_DONE_EN) || rxDone != Len) {
        GeniCancel(Bus);
        return (st & M_SPI_ERR_MASK) ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

/* ---- FocalTech SPI v2 protocol + CRC16 ----------------------------------- */

static USHORT FtsCrc16(const UCHAR *Data, ULONG Len)
{
    USHORT crc = 0xFFFF;
    ULONG i;
    int bit;

    for (i = 0; i < Len; i++) {
        crc ^= Data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

NTSTATUS FtsSpiWrite(PGENI_SPI Bus, const UCHAR *Buf, ULONG Len)
{
    ULONG xfer = (Len > 1) ? (Len + 6) : 4;
    ULONG attempt;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    if (Len == 0 || xfer > Bus->ScratchSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (attempt = 0; attempt < 3; attempt++) {
        RtlZeroMemory(Bus->TxScratch, xfer);
        RtlZeroMemory(Bus->RxScratch, xfer);
        Bus->TxScratch[0] = Buf[0];
        Bus->TxScratch[1] = 0x00;
        Bus->TxScratch[2] = (UCHAR)(((Len - 1) >> 8) & 0xFF);
        Bus->TxScratch[3] = (UCHAR)((Len - 1) & 0xFF);
        if (Len > 1) {
            RtlCopyMemory(Bus->TxScratch + 7, Buf + 1, Len - 1);
        }
        status = GeniSpiXfer(Bus, Bus->TxScratch, Bus->RxScratch, xfer);
        if (NT_SUCCESS(status) && (Bus->RxScratch[3] & 0xA0) == 0) {
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(300);
    }
    return NT_SUCCESS(status) ? STATUS_DEVICE_BUSY : status;
}

NTSTATUS FtsSpiRead(PGENI_SPI Bus, UCHAR Reg, UCHAR *Buf, ULONG Len)
{
    ULONG xfer = Len + 9;
    ULONG attempt;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    if (Len == 0 || xfer > Bus->ScratchSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (attempt = 0; attempt < 3; attempt++) {
        USHORT calcCrc, rxCrc;
        RtlZeroMemory(Bus->TxScratch, xfer);
        RtlZeroMemory(Bus->RxScratch, xfer);
        Bus->TxScratch[0] = Reg;
        Bus->TxScratch[1] = 0xA0;
        Bus->TxScratch[2] = (UCHAR)((Len >> 8) & 0xFF);
        Bus->TxScratch[3] = (UCHAR)(Len & 0xFF);
        status = GeniSpiXfer(Bus, Bus->TxScratch, Bus->RxScratch, xfer);
        if (!NT_SUCCESS(status) || (Bus->RxScratch[3] & 0xA0) != 0) {
            KeStallExecutionProcessor(300);
            continue;
        }
        calcCrc = FtsCrc16(Bus->RxScratch + 7, Len);
        rxCrc = (USHORT)Bus->RxScratch[7 + Len] | ((USHORT)Bus->RxScratch[8 + Len] << 8);
        if (calcCrc == rxCrc) {
            RtlCopyMemory(Buf, Bus->RxScratch + 7, Len);
            return STATUS_SUCCESS;
        }
        status = STATUS_CRC_ERROR;
        KeStallExecutionProcessor(300);
    }
    return NT_SUCCESS(status) ? STATUS_DEVICE_BUSY : status;
}

NTSTATUS FtsWriteReg(PGENI_SPI Bus, UCHAR Reg, UCHAR Val)
{
    UCHAR b[2] = { Reg, Val };
    return FtsSpiWrite(Bus, b, sizeof(b));
}

NTSTATUS FtsReadReg(PGENI_SPI Bus, UCHAR Reg, UCHAR *Val)
{
    return FtsSpiRead(Bus, Reg, Val, 1);
}
