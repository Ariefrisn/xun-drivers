/*
 * Low-level hardware helpers for XunBluetooth:
 *   - TLMM EAST GPIO 8..11 (QUP0 SE3 UART) + GPIO 87 (bt-sw-ctrl), single-page mappings
 *   - GCC QUP0 S3 clock + DFSR / MND RCG programming, single-page mappings
 *   - QUPv3 GENI SE3 UART FIFO TX/RX, baudrate, and HW RTS/CTS control
 */
#include "driver.h"

static NTSTATUS MmioMap(PMMIO_RANGE Range, ULONGLONG Pa, SIZE_T Size)
{
    Range->Pa.QuadPart = (LONGLONG)Pa;
    Range->Size = Size;
    Range->Va = (volatile UCHAR *)MmMapIoSpaceEx(Range->Pa, Size, PAGE_READWRITE | PAGE_NOCACHE);
    return (Range->Va != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static VOID MmioUnmap(PMMIO_RANGE Range)
{
    if (Range->Va != NULL) {
        MmUnmapIoSpace((PVOID)Range->Va, Range->Size);
        Range->Va = NULL;
    }
}

/* ---- TLMM EAST ----------------------------------------------------------- */

static VOID TlmmConfigPinRange(PMMIO_RANGE Range, ULONG RelPin, ULONG Func, ULONG Pull, ULONG DriveMa, BOOLEAN Output)
{
    ULONG ctl = (Pull & 0x3u) | ((Func & 0xFu) << 2) | ((TLMM_DRV_MA(DriveMa) & 0x7u) << 6);
    if (Output) {
        ctl |= (1u << 9);
    }
    MmioWrite32(Range, RelPin * (ULONG)TLMM_PIN_STRIDE + TLMM_CTL, ctl);
}

static VOID TlmmSetupUartPins(PXUN_BT_CONTEXT Ctx)
{
    ULONG swCtl;
    ULONG swIo;

    /* Per xun DTS qupv3_se3_4uart_pins: GPIO 8..11 EAST func 1, 2mA */
    TlmmConfigPinRange(&Ctx->TlmmUart, BT_GPIO_CTS - BT_GPIO_CTS, BT_UART_FUNC_QUP0_S3, TLMM_PULL_NONE, 2, FALSE);
    TlmmConfigPinRange(&Ctx->TlmmUart, BT_GPIO_RTS - BT_GPIO_CTS, BT_UART_FUNC_QUP0_S3, TLMM_PULL_DOWN, 2, FALSE);
    TlmmConfigPinRange(&Ctx->TlmmUart, BT_GPIO_TX  - BT_GPIO_CTS, BT_UART_FUNC_QUP0_S3, TLMM_PULL_UP,   2, FALSE);
    TlmmConfigPinRange(&Ctx->TlmmUart, BT_GPIO_RX  - BT_GPIO_CTS, BT_UART_FUNC_QUP0_S3, TLMM_PULL_NONE, 2, FALSE);

    /* Configure GPIO 87 (bt-sw-ctrl) as pulled-down input and log its initial state */
    TlmmConfigPinRange(&Ctx->TlmmSwCtrl, 0, 0, TLMM_PULL_DOWN, 2, FALSE);
    swCtl = MmioRead32(&Ctx->TlmmSwCtrl, TLMM_CTL);
    swIo = MmioRead32(&Ctx->TlmmSwCtrl, TLMM_IO);
    LogPrint("tlmm: CTS=%08x RTS=%08x TX=%08x RX=%08x SW_CTRL(87) ctl=%08x io=%08x\n",
             MmioRead32(&Ctx->TlmmUart, (BT_GPIO_CTS - BT_GPIO_CTS) * (ULONG)TLMM_PIN_STRIDE + TLMM_CTL),
             MmioRead32(&Ctx->TlmmUart, (BT_GPIO_RTS - BT_GPIO_CTS) * (ULONG)TLMM_PIN_STRIDE + TLMM_CTL),
             MmioRead32(&Ctx->TlmmUart, (BT_GPIO_TX  - BT_GPIO_CTS) * (ULONG)TLMM_PIN_STRIDE + TLMM_CTL),
             MmioRead32(&Ctx->TlmmUart, (BT_GPIO_RX  - BT_GPIO_CTS) * (ULONG)TLMM_PIN_STRIDE + TLMM_CTL),
             swCtl, swIo);
}

/* ---- GCC QUP0 S3 & DFSR / RCG -------------------------------------------- */

static ULONG DecodeDfsrFreq(ULONG Cfg, ULONG M, ULONG NReg)
{
    ULONG src = (Cfg >> 8) & 0x7u;
    ULONG mode = (Cfg >> 12) & 0x3u;
    ULONG hid = Cfg & 0x1Fu;
    ULONG n = 0;
    ULONGLONG parentHz;
    ULONGLONG div;

    if (mode != 0) {
        n = ((~NReg) & 0xFFFFu) + (M & 0xFFFFu);
    }

    if (src == 0 && mode == 0 && (hid == 0 || hid == 1)) {
        return 19200000u;
    }
    if (mode == 2 && M == 384 && n == 15625)   return 7372800u;
    if (mode == 2 && M == 768 && n == 15625)   return 14745600u;
    if (mode == 2 && M == 4   && n == 25)      return 48000000u;
    if (mode == 2 && M == 16  && n == 75)      return 64000000u;
    if (mode == 2 && M == 128 && n == 375)     return 102400000u;

    parentHz = (src == 0) ? 19200000ULL : 300000000ULL;
    div = (hid > 1) ? ((hid + 1) / 2) : 1;
    if (mode == 2 && n > 0) {
        return (ULONG)((parentHz * (ULONGLONG)(M & 0xFFFFu)) / ((ULONGLONG)n * div));
    }
    return (ULONG)(parentHz / div);
}

static NTSTATUS GccEnableQup0S3(PXUN_BT_CONTEXT Ctx)
{
    ULONG voteOff = GCC_APCS_VOTE_REG & 0xFFFu;
    ULONG cbcrOff = GCC_QUP0_S3_CBCR & 0xFFFu;
    ULONG rcgOff = GCC_QUP0_S3_RCG & 0xFFFu;
    ULONG vote;
    ULONG cbcr = 0;
    ULONG dfsrCmd;
    ULONG i;

    vote = MmioRead32(&Ctx->GccVote, voteOff);
    MmioWrite32(&Ctx->GccVote, voteOff, vote | GCC_QUP0_S3_VOTE_MASK);

    for (i = 0; i < 200; i++) {
        cbcr = MmioRead32(&Ctx->GccQupS3, cbcrOff);
        if ((cbcr & GCC_CBCR_CLK_OFF) == 0) {
            break;
        }
        BusyWaitUs(50);
    }

    dfsrCmd = MmioRead32(&Ctx->GccQupS3, rcgOff + RCG_SE_CMD_DFSR_OFF);
    Ctx->DfsEnabled = (dfsrCmd & RCG_DFS_EN) != 0;
    LogPrint("gcc: vote %08x->%08x cbcr=%08x dfsr_cmd=%08x (dfs=%u)\n",
             vote, MmioRead32(&Ctx->GccVote, voteOff), cbcr, dfsrCmd, Ctx->DfsEnabled ? 1 : 0);

    if (Ctx->DfsEnabled) {
        for (i = 0; i < RCG_DFSR_LEVELS; i++) {
            ULONG cfg = MmioRead32(&Ctx->GccQupS3, rcgOff + RCG_SE_PERF_DFSR(i));
            ULONG m = MmioRead32(&Ctx->GccQupS3, rcgOff + RCG_SE_PERF_M_DFSR(i)) & 0xFFFFu;
            ULONG nReg = MmioRead32(&Ctx->GccQupS3, rcgOff + RCG_SE_PERF_N_DFSR(i)) & 0xFFFFu;
            if (cfg != 0 || m != 0 || nReg != 0) {
                Ctx->DfsFreqs[i] = DecodeDfsrFreq(cfg, m, nReg);
                LogPrint("gcc: dfsr[%u] cfg=%08x m=%u n_reg=%04x -> %u Hz\n",
                         i, cfg, m, nReg, Ctx->DfsFreqs[i]);
            }
        }
    }

    return ((cbcr & GCC_CBCR_CLK_OFF) == 0) ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static NTSTATUS GccProgramDirectRcg(PXUN_BT_CONTEXT Ctx, ULONG TargetRate)
{
    ULONG rcgOff = GCC_QUP0_S3_RCG & 0xFFFu;
    ULONG cfg;
    ULONG m = 0, n = 0;
    ULONG i;

    if (TargetRate == 19200000u) {
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_CFG_OFF, 0);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_M_OFF, 0);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_N_OFF, 0);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_D_OFF, 0);
    } else {
        if (TargetRate == 7372800u)       { m = 384; n = 15625; }
        else if (TargetRate == 14745600u) { m = 768; n = 15625; }
        else if (TargetRate == 48000000u) { m = 4;   n = 25; }
        else if (TargetRate == 64000000u) { m = 16;  n = 75; }
        else if (TargetRate == 102400000u){ m = 128; n = 375; }
        else { return STATUS_INVALID_PARAMETER; }

        /* src=1 (300 MHz GPLL0_OUT_EVEN), pre_div=1, dual-edge MND mode (2 << 12) */
        cfg = (1u << 0) | (1u << 8) | (2u << 12);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_CFG_OFF, cfg);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_M_OFF, m);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_N_OFF, (~(n - m)) & 0xFFFFu);
        MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_D_OFF, (~n) & 0xFFFFu);
    }

    MmioWrite32(&Ctx->GccQupS3, rcgOff + RCG_CMD_OFF,
                MmioRead32(&Ctx->GccQupS3, rcgOff + RCG_CMD_OFF) | RCG_CMD_UPDATE);
    for (i = 0; i < 200; i++) {
        if ((MmioRead32(&Ctx->GccQupS3, rcgOff + RCG_CMD_OFF) & RCG_CMD_UPDATE) == 0) {
            return STATUS_SUCCESS;
        }
        BusyWaitUs(10);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS SelectClockForBaud(PXUN_BT_CONTEXT Ctx, ULONG Baud, ULONG *OutClkIdx, ULONG *OutClkDiv, ULONG *OutRate)
{
    ULONG req = Baud * Ctx->SamplingRate;
    ULONG bestIdx = 0;
    ULONG bestDiv = 1;
    ULONG bestRate = 0;
    ULONG bestErr = 0xFFFFFFFFu;
    ULONG i;

    if (Ctx->DfsEnabled) {
        for (i = 0; i < RCG_DFSR_LEVELS; i++) {
            ULONG f = Ctx->DfsFreqs[i];
            ULONG div;
            ULONG actualReq;
            ULONG err;
            if (f == 0 || f < req) {
                continue;
            }
            div = (f + req / 2) / req;
            if (div == 0) div = 1;
            if (div > SER_CLK_DIV_MAX) continue;
            actualReq = f / div;
            err = (actualReq > req) ? (actualReq - req) : (req - actualReq);
            if (err < bestErr) {
                bestErr = err;
                bestIdx = i;
                bestDiv = div;
                bestRate = f;
            }
        }
        if (bestRate == 0) {
            /* Fallback: pick lowest non-zero DFS clock or direct 102.4 MHz */
            LogPrint("uart: no DFS clock >= %u for baud %u\n", req, Baud);
            return STATUS_NOT_SUPPORTED;
        }
        *OutClkIdx = bestIdx;
        *OutClkDiv = bestDiv;
        *OutRate = bestRate;
        return STATUS_SUCCESS;
    }

    /* Non-DFS: program RCG directly from khaje ftbl */
    {
        static const ULONG kRates[] = { 7372800u, 14745600u, 19200000u, 48000000u, 64000000u, 102400000u };
        NTSTATUS st;
        for (i = 0; i < ARRAYSIZE(kRates); i++) {
            ULONG f = kRates[i];
            ULONG div;
            ULONG actualReq;
            ULONG err;
            if (f < req) continue;
            div = (f + req / 2) / req;
            if (div == 0) div = 1;
            if (div > SER_CLK_DIV_MAX) continue;
            actualReq = f / div;
            err = (actualReq > req) ? (actualReq - req) : (req - actualReq);
            if (err < bestErr) {
                bestErr = err;
                bestDiv = div;
                bestRate = f;
            }
        }
        if (bestRate == 0) {
            return STATUS_NOT_SUPPORTED;
        }
        st = GccProgramDirectRcg(Ctx, bestRate);
        if (!NT_SUCCESS(st)) {
            return st;
        }
        *OutClkIdx = 0;
        *OutClkDiv = bestDiv;
        *OutRate = bestRate;
        return STATUS_SUCCESS;
    }
}

/* ---- GENI SE3 UART ------------------------------------------------------- */

static VOID GeniConfigPacking(PXUN_BT_CONTEXT Ctx)
{
    /* 4 x 8-bit characters packed per 32-bit FIFO word, LSB first */
    ULONG cfg0 = (7u << 0) | (0u << 4) | (0u << 5) |
                 (15u << 10) | (0u << 14) | (0u << 15);
    ULONG cfg1 = (23u << 0) | (0u << 4) | (0u << 5) |
                 (31u << 10) | (1u << 14) | (0u << 15);
    MmioWrite32(&Ctx->Se, SE_GENI_TX_PACKING_CFG0, cfg0);
    MmioWrite32(&Ctx->Se, SE_GENI_TX_PACKING_CFG1, cfg1);
    MmioWrite32(&Ctx->Se, SE_GENI_RX_PACKING_CFG0, cfg0);
    MmioWrite32(&Ctx->Se, SE_GENI_RX_PACKING_CFG1, cfg1);
}

static VOID GeniStopRx(PXUN_BT_CONTEXT Ctx)
{
    ULONG i;
    if (MmioRead32(&Ctx->Se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
        MmioWrite32(&Ctx->Se, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_CANCEL);
        for (i = 0; i < 1000; i++) {
            if (MmioRead32(&Ctx->Se, SE_GENI_S_IRQ_STATUS) & S_CMD_CANCEL_EN) {
                break;
            }
            BusyWaitUs(10);
        }
        if (MmioRead32(&Ctx->Se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
            MmioWrite32(&Ctx->Se, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_ABORT);
            for (i = 0; i < 1000; i++) {
                if (MmioRead32(&Ctx->Se, SE_GENI_S_IRQ_STATUS) & S_CMD_ABORT_EN) {
                    break;
                }
                BusyWaitUs(10);
            }
        }
    }
    MmioWrite32(&Ctx->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
}

static VOID GeniStartRx(PXUN_BT_CONTEXT Ctx)
{
    MmioWrite32(&Ctx->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    MmioWrite32(&Ctx->Se, SE_GENI_S_CMD0, UART_START_READ | UART_PARAM_RFR_OPEN);
}

VOID UartSetFlowControl(PXUN_BT_CONTEXT Ctx, BOOLEAN DisableFlow)
{
    ULONG txCfg = MmioRead32(&Ctx->Se, SE_UART_TX_TRANS_CFG);
    if (DisableFlow) {
        txCfg |= UART_CTS_MASK;
        MmioWrite32(&Ctx->Se, SE_UART_TX_TRANS_CFG, txCfg);
        MmioWrite32(&Ctx->Se, SE_UART_MANUAL_RFR, UART_MANUAL_RFR_EN | UART_RFR_NOT_READY);
    } else {
        txCfg &= ~UART_CTS_MASK;
        MmioWrite32(&Ctx->Se, SE_UART_TX_TRANS_CFG, txCfg);
        MmioWrite32(&Ctx->Se, SE_UART_MANUAL_RFR, 0);
    }
}

NTSTATUS UartSetBaud(PXUN_BT_CONTEXT Ctx, ULONG Baud)
{
    ULONG clkIdx = 0, clkDiv = 1, srcRate = 0;
    ULONG clkCfg;
    NTSTATUS st;

    st = SelectClockForBaud(Ctx, Baud, &clkIdx, &clkDiv, &srcRate);
    if (!NT_SUCCESS(st)) {
        LogPrint("uart: SelectClockForBaud(%u) failed: %08x\n", Baud, st);
        return st;
    }

    clkCfg = SER_CLK_EN | ((clkDiv & SER_CLK_DIV_MAX) << SER_CLK_DIV_SHFT);
    MmioWrite32(&Ctx->Se, GENI_SER_M_CLK_CFG, clkCfg);
    MmioWrite32(&Ctx->Se, GENI_SER_S_CLK_CFG, clkCfg);
    MmioWrite32(&Ctx->Se, SE_GENI_CLK_SEL, clkIdx & 0x7u);
    Ctx->CurrentBaud = Baud;
    LogPrint("uart: baud=%u (src=%u Hz, idx=%u, div=%u, samp=%u, actual=%u)\n",
             Baud, srcRate, clkIdx, clkDiv, Ctx->SamplingRate,
             srcRate / (clkDiv * Ctx->SamplingRate));
    return STATUS_SUCCESS;
}

VOID UartFlushRx(PXUN_BT_CONTEXT Ctx)
{
    UCHAR discard[128];
    ULONG n;
    ULONG guard = 0;
    do {
        n = UartReadPoll(Ctx, discard, sizeof(discard));
        guard++;
    } while (n > 0 && guard < 32);
}

NTSTATUS UartWriteSync(PXUN_BT_CONTEXT Ctx, const UCHAR *Data, ULONG Len, ULONG TimeoutMs)
{
    ULONG offset = 0;
    ULONGLONG startMs;
    ULONG irqSt;
    ULONG availWords;

    if (Len == 0) {
        return STATUS_SUCCESS;
    }

    /* Wait until any previous M_CMD finishes */
    startMs = GetTicksMs();
    while (MmioRead32(&Ctx->Se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
        if ((GetTicksMs() - startMs) > TimeoutMs) {
            LogPrint("uart: TX busy timeout status=%08x\n", MmioRead32(&Ctx->Se, SE_GENI_STATUS));
            MmioWrite32(&Ctx->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
            return STATUS_IO_TIMEOUT;
        }
        BusyWaitUs(20);
    }

    MmioWrite32(&Ctx->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    MmioWrite32(&Ctx->Se, SE_UART_TX_TRANS_LEN, Len);
    MmioWrite32(&Ctx->Se, SE_GENI_M_CMD0, UART_START_TX);

    startMs = GetTicksMs();
    while (offset < Len) {
        ULONG used = MmioRead32(&Ctx->Se, SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MASK;
        availWords = (Ctx->TxDepth > used) ? (Ctx->TxDepth - used) : 0;
        if (availWords == 0) {
            if ((GetTicksMs() - startMs) > TimeoutMs) {
                LogPrint("uart: TX FIFO stall at %u/%u (ios=%08x m_irq=%08x)\n",
                         offset, Len, MmioRead32(&Ctx->Se, SE_GENI_IOS),
                         MmioRead32(&Ctx->Se, SE_GENI_M_IRQ_STATUS));
                MmioWrite32(&Ctx->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
                return STATUS_IO_TIMEOUT;
            }
            BusyWaitUs(20);
            continue;
        }
        while (availWords > 0 && offset < Len) {
            ULONG word = 0;
            ULONG chunk = Len - offset;
            ULONG b;
            if (chunk > 4) chunk = 4;
            for (b = 0; b < chunk; b++) {
                word |= ((ULONG)Data[offset + b]) << (b * 8);
            }
            MmioWrite32(&Ctx->Se, SE_GENI_TX_FIFOn, word);
            offset += chunk;
            availWords--;
        }
    }

    startMs = GetTicksMs();
    for (;;) {
        irqSt = MmioRead32(&Ctx->Se, SE_GENI_M_IRQ_STATUS);
        if (irqSt & M_CMD_DONE_EN) {
            MmioWrite32(&Ctx->Se, SE_GENI_M_IRQ_CLEAR, irqSt);
            return STATUS_SUCCESS;
        }
        if ((GetTicksMs() - startMs) > TimeoutMs) {
            LogPrint("uart: TX M_CMD_DONE timeout (%uB, st=%08x m_irq=%08x ios=%08x)\n",
                     Len, MmioRead32(&Ctx->Se, SE_GENI_STATUS), irqSt,
                     MmioRead32(&Ctx->Se, SE_GENI_IOS));
            MmioWrite32(&Ctx->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
            return STATUS_IO_TIMEOUT;
        }
        BusyWaitUs(20);
    }
}

ULONG UartReadPoll(PXUN_BT_CONTEXT Ctx, UCHAR *OutBuf, ULONG MaxLen)
{
    ULONG irqSt;
    ULONG fifoSt;
    ULONG wc;
    BOOLEAN isLast;
    ULONG lastValid;
    ULONG readBytes = 0;
    ULONG w;

    irqSt = MmioRead32(&Ctx->Se, SE_GENI_S_IRQ_STATUS);
    if (irqSt != 0) {
        MmioWrite32(&Ctx->Se, SE_GENI_S_IRQ_CLEAR, irqSt);
    }

    fifoSt = MmioRead32(&Ctx->Se, SE_GENI_RX_FIFO_STATUS);
    wc = fifoSt & RX_FIFO_WC_MASK;
    if (wc == 0) {
        return 0;
    }

    isLast = (fifoSt & RX_LAST_BIT) != 0;
    lastValid = (fifoSt >> RX_LAST_VALID_SHFT) & RX_LAST_VALID_MASK;
    if (lastValid == 0) {
        lastValid = 4;
    }

    for (w = 0; w < wc; w++) {
        ULONG word;
        ULONG validBytes = (isLast && (w + 1 == wc)) ? lastValid : 4;
        ULONG b;

        if (readBytes + validBytes > MaxLen) {
            /* Stop before overflowing caller buffer; remaining words stay in RX FIFO */
            break;
        }
        word = MmioRead32(&Ctx->Se, SE_GENI_RX_FIFOn);
        for (b = 0; b < validBytes; b++) {
            OutBuf[readBytes++] = (UCHAR)((word >> (b * 8)) & 0xFFu);
        }
    }

    if ((MmioRead32(&Ctx->Se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) == 0) {
        GeniStartRx(Ctx);
    }
    return readBytes;
}

NTSTATUS UartHwInit(PXUN_BT_CONTEXT Ctx)
{
    NTSTATUS st;
    ULONG fwRev;
    ULONG proto;
    ULONG rxRfr;

    st = MmioMap(&Ctx->TlmmUart, TLMM_PIN_PA(TLMM_TILE_EAST, BT_GPIO_CTS), 4ULL * TLMM_PIN_STRIDE);
    if (!NT_SUCCESS(st)) goto Fail;
    st = MmioMap(&Ctx->TlmmSwCtrl, TLMM_PIN_PA(TLMM_TILE_EAST, BT_GPIO_SW_CTRL), TLMM_PIN_STRIDE);
    if (!NT_SUCCESS(st)) goto Fail;
    st = MmioMap(&Ctx->GccVote, GCC_BASE + (GCC_APCS_VOTE_REG & ~0xFFFULL), 0x1000ULL);
    if (!NT_SUCCESS(st)) goto Fail;
    st = MmioMap(&Ctx->GccQupS3, GCC_BASE + (GCC_QUP0_S3_CBCR & ~0xFFFULL), 0x1000ULL);
    if (!NT_SUCCESS(st)) goto Fail;
    st = MmioMap(&Ctx->Se, QUP0_SE3_PA, GENI_SE_SIZE);
    if (!NT_SUCCESS(st)) goto Fail;

    TlmmSetupUartPins(Ctx);

    st = GccEnableQup0S3(Ctx);
    if (!NT_SUCCESS(st)) {
        LogPrint("uart: GccEnableQup0S3 failed: %08x\n", st);
        goto Fail;
    }

    /* SM6225 QUPv3 GENI uses 16x oversampling (do not touch QUP wrap MMIO) */
    Ctx->SamplingRate = 16;

    fwRev = MmioRead32(&Ctx->Se, GENI_FW_REVISION_RO);
    proto = (fwRev >> 8) & 0xFFu;
    Ctx->TxDepth = (MmioRead32(&Ctx->Se, SE_HW_PARAM_0) >> 16) & 0x3Fu;
    Ctx->RxDepth = (MmioRead32(&Ctx->Se, SE_HW_PARAM_1) >> 16) & 0x3Fu;
    if (Ctx->TxDepth == 0) Ctx->TxDepth = 16;
    if (Ctx->RxDepth == 0) Ctx->RxDepth = 16;

    LogPrint("geni: samp=%u fw_rev=%08x proto=%u tx=%u rx=%u ios=%08x\n",
             Ctx->SamplingRate, fwRev, proto,
             Ctx->TxDepth, Ctx->RxDepth, MmioRead32(&Ctx->Se, SE_GENI_IOS));

    if (proto != GENI_PROTO_UART) {
        LogPrint("uart: ERROR QUP0 SE3 protocol is %u (expected UART=2)\n", proto);
        st = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Fail;
    }

    GeniStopRx(Ctx);
    if (MmioRead32(&Ctx->Se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
        MmioWrite32(&Ctx->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
        BusyWaitUs(500);
    }
    MmioWrite32(&Ctx->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    MmioWrite32(&Ctx->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    MmioWrite32(&Ctx->Se, SE_DMA_TX_IRQ_CLR, 0xFFFFFFFFu);
    MmioWrite32(&Ctx->Se, SE_DMA_RX_IRQ_CLR, 0xFFFFFFFFu);

    MmioWrite32(&Ctx->Se, GENI_CGC_CTRL, MmioRead32(&Ctx->Se, GENI_CGC_CTRL) | 0x7Fu);
    MmioWrite32(&Ctx->Se, GENI_OUTPUT_CTRL, 0x7Fu);
    MmioWrite32(&Ctx->Se, GENI_FORCE_DEFAULT_REG, 1);

    /* FIFO mode, no GSI/DMA */
    MmioWrite32(&Ctx->Se, SE_GENI_DMA_MODE_EN, MmioRead32(&Ctx->Se, SE_GENI_DMA_MODE_EN) & ~1u);
    MmioWrite32(&Ctx->Se, SE_GSI_EVENT_EN, 0);
    MmioWrite32(&Ctx->Se, SE_GENI_BYTE_GRAN, 0);

    rxRfr = (Ctx->RxDepth > 4) ? (Ctx->RxDepth - 4) : (Ctx->RxDepth - 1);
    MmioWrite32(&Ctx->Se, SE_GENI_TX_WATERMARK_REG, 2);
    MmioWrite32(&Ctx->Se, SE_GENI_RX_WATERMARK_REG, 1);
    MmioWrite32(&Ctx->Se, SE_GENI_RX_RFR_WATERMARK_REG, rxRfr);

    MmioWrite32(&Ctx->Se, SE_GENI_M_IRQ_EN, 0);
    MmioWrite32(&Ctx->Se, SE_GENI_S_IRQ_EN, 0);
    MmioWrite32(&Ctx->Se, SE_IRQ_EN, 0);

    GeniConfigPacking(Ctx);

    /* 8N1 framing, no loopback */
    MmioWrite32(&Ctx->Se, SE_UART_LOOPBACK_CFG, 0);
    MmioWrite32(&Ctx->Se, SE_UART_TX_PARITY_CFG, 0);
    MmioWrite32(&Ctx->Se, SE_UART_RX_PARITY_CFG, 0);
    MmioWrite32(&Ctx->Se, SE_UART_TX_WORD_LEN, 8);
    MmioWrite32(&Ctx->Se, SE_UART_RX_WORD_LEN, 8);
    MmioWrite32(&Ctx->Se, SE_UART_TX_STOP_BIT_LEN, 0); /* 1 stop bit */
    MmioWrite32(&Ctx->Se, SE_UART_RX_STALE_CNT, 16u * 10u);
    MmioWrite32(&Ctx->Se, SE_UART_RX_TRANS_CFG, 0);
    MmioWrite32(&Ctx->Se, SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
    MmioWrite32(&Ctx->Se, SE_UART_MANUAL_RFR, UART_MANUAL_RFR_EN | UART_RFR_NOT_READY);

    st = UartSetBaud(Ctx, 115200);
    if (!NT_SUCCESS(st)) {
        goto Fail;
    }

    GeniStartRx(Ctx);
    Ctx->UartReady = TRUE;
    return STATUS_SUCCESS;

Fail:
    UartHwDeinit(Ctx);
    return st;
}

VOID UartHwDeinit(PXUN_BT_CONTEXT Ctx)
{
    Ctx->UartReady = FALSE;
    if (Ctx->Se.Va != NULL) {
        GeniStopRx(Ctx);
    }
    MmioUnmap(&Ctx->Se);
    MmioUnmap(&Ctx->GccQupS3);
    MmioUnmap(&Ctx->GccVote);
    MmioUnmap(&Ctx->TlmmSwCtrl);
    MmioUnmap(&Ctx->TlmmUart);
}

