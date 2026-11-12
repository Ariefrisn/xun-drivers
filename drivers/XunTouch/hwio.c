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

NTSTATUS GccEnableQup0Se0(VOID)
{
    return STATUS_SUCCESS;
}

/* ---- TLMM GPIO 0..3 Bit-Bang SPI Mode 0 ---------------------------------- */

#define Tlmm Se

static __forceinline VOID TlmmSetPin(PGENI_SPI Bus, ULONG Pin, BOOLEAN High)
{
    MmioWrite32(&Bus->Tlmm, Pin * TLMM_PIN_STRIDE + TLMM_IO, High ? 0x2u : 0x0u);
}

static __forceinline BOOLEAN TlmmGetPin(PGENI_SPI Bus, ULONG Pin)
{
    return (MmioRead32(&Bus->Tlmm, Pin * TLMM_PIN_STRIDE + TLMM_IO) & 1u) != 0;
}

NTSTATUS GeniSpiInit(PGENI_SPI Bus, ULONGLONG SeBase)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(SeBase);

    /* Map TLMM WEST (0x00500000) for GPIO 0..3 bit-bang SPI */
    status = MmioMap(&Bus->Tlmm, TLMM_BASE + TLMM_TILE_WEST, 0x4000);
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

    /* Configure GPIO 0..3 for TLMM bit-bang SPI Mode 0:
     * - GPIO 0 (MISO): FUNC=0, OE=0 (input), PULL=UP (3), DRV=6mA
     * - GPIO 1 (MOSI): FUNC=0, OE=1 (output), PULL=NONE (0), DRV=6mA, IO=0
     * - GPIO 2 (CLK):  FUNC=0, OE=1 (output), PULL=NONE (0), DRV=6mA, IO=0 (CPOL=0)
     * - GPIO 3 (CS):   FUNC=0, OE=1 (output), PULL=UP (3), DRV=6mA, IO=1 (CS idle HIGH)
     */
    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_MISO * TLMM_PIN_STRIDE + TLMM_CTL,
                (TLMM_PULL_UP & 0x3) | ((0 & 0xF) << 2) | ((TLMM_DRV_MA(6) & 0x7) << 6));

    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_MOSI * TLMM_PIN_STRIDE + TLMM_IO, 0x0u);
    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_MOSI * TLMM_PIN_STRIDE + TLMM_CTL,
                (TLMM_PULL_NONE & 0x3) | ((0 & 0xF) << 2) | ((TLMM_DRV_MA(6) & 0x7) << 6) | (1u << 9));

    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_CLK * TLMM_PIN_STRIDE + TLMM_IO, 0x0u);
    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_CLK * TLMM_PIN_STRIDE + TLMM_CTL,
                (TLMM_PULL_NONE & 0x3) | ((0 & 0xF) << 2) | ((TLMM_DRV_MA(6) & 0x7) << 6) | (1u << 9));

    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_CS * TLMM_PIN_STRIDE + TLMM_IO, 0x2u);
    MmioWrite32(&Bus->Tlmm, TS_PIN_SPI_CS * TLMM_PIN_STRIDE + TLMM_CTL,
                (TLMM_PULL_UP & 0x3) | ((0 & 0xF) << 2) | ((TLMM_DRV_MA(6) & 0x7) << 6) | (1u << 9));

    LogPrint("GeniSpiInit: TLMM GPIO 0..3 bit-bang SPI Mode 0 ready\n");
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
    MmioUnmap(&Bus->Tlmm);
}

NTSTATUS GeniSpiXfer(PGENI_SPI Bus, const UCHAR *Tx, UCHAR *Rx, ULONG Len)
{
    ULONG i;
    int bit;

    if (Len == 0) {
        return STATUS_SUCCESS;
    }

    /* b) Set SCLK (GPIO 2) = 0, lalu CS (GPIO 3) = 0 (active low), KeStallExecutionProcessor(1) */
    TlmmSetPin(Bus, TS_PIN_SPI_CLK, FALSE);
    TlmmSetPin(Bus, TS_PIN_SPI_CS, FALSE);
    KeStallExecutionProcessor(1);

    /* c) Untuk setiap byte i dari 0 sampai Len - 1: */
    for (i = 0; i < Len; i++) {
        UCHAR txByte = (Tx != NULL) ? Tx[i] : 0x00;
        UCHAR rxByte = 0;

        for (bit = 7; bit >= 0; bit--) {
            TlmmSetPin(Bus, TS_PIN_SPI_MOSI, ((txByte >> bit) & 1u) != 0);
            KeStallExecutionProcessor(1);
            TlmmSetPin(Bus, TS_PIN_SPI_CLK, TRUE);
            KeStallExecutionProcessor(1);
            if (TlmmGetPin(Bus, TS_PIN_SPI_MISO)) {
                rxByte |= (UCHAR)(1u << bit);
            }
            TlmmSetPin(Bus, TS_PIN_SPI_CLK, FALSE);
        }

        if (Rx != NULL) {
            Rx[i] = rxByte;
        }
    }

    /* d) Set CS (GPIO 3) = 1 (idle high), KeStallExecutionProcessor(1) */
    TlmmSetPin(Bus, TS_PIN_SPI_CS, TRUE);
    KeStallExecutionProcessor(1);

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
    LogPrint("FtsSpiWrite busy cmd=%02x len=%lu rx0..3=%02x %02x %02x %02x\n",
             Buf[0], Len, Bus->RxScratch[0], Bus->RxScratch[1],
             Bus->RxScratch[2], Bus->RxScratch[3]);
    return NT_SUCCESS(status) ? STATUS_DEVICE_BUSY : status;
}

NTSTATUS FtsSpiRead(PGENI_SPI Bus, UCHAR Reg, UCHAR *Buf, ULONG Len)
{
    ULONG xfer = Len + 9;
    ULONG attempt;
    USHORT lastCalcCrc = 0, lastRxCrc = 0;
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
            lastCalcCrc = 0;
            lastRxCrc = 0;
            KeStallExecutionProcessor(300);
            continue;
        }
        calcCrc = FtsCrc16(Bus->RxScratch + 7, Len);
        rxCrc = (USHORT)Bus->RxScratch[7 + Len] | ((USHORT)Bus->RxScratch[8 + Len] << 8);
        lastCalcCrc = calcCrc;
        lastRxCrc = rxCrc;
        if (calcCrc == rxCrc) {
            RtlCopyMemory(Buf, Bus->RxScratch + 7, Len);
            return STATUS_SUCCESS;
        }
        status = STATUS_CRC_ERROR;
        KeStallExecutionProcessor(300);
    }
    LogPrint("FtsSpiRead busy/crc reg=%02x len=%lu rx0..3=%02x %02x %02x %02x rx4..9=%02x %02x %02x %02x %02x %02x calc=%04x rx=%04x\n",
             Reg, Len, Bus->RxScratch[0], Bus->RxScratch[1],
             Bus->RxScratch[2], Bus->RxScratch[3],
             Bus->RxScratch[4], Bus->RxScratch[5], Bus->RxScratch[6],
             Bus->RxScratch[7], Bus->RxScratch[8], Bus->RxScratch[9],
             lastCalcCrc, lastRxCrc);
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
