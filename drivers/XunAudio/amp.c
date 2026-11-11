/*
 * Speaker amp detection & locked I2C helper for Redmi Pad SE (xun).
 * Verified from dtbo_b.img:
 *   - QUP0 SE1 (0x04A84000, rst GPIO 106 SOUTH): fs16xx@34/@35,
 * sipa_i2c_L@28/@29, sia81xx@2B
 *   - QUP0 SE2 (0x04A88000, rst GPIO 70 WEST):   fs16xx@34/@35,
 * sipa_i2c_R@28/@29 Uses xun_qup_lock.h (\BaseNamedObjects\XunQup0Se1Lock /
 * XunQup0Se2Lock) so XunAudio, XunBattery, XunDisplay, XunGpuW, and
 * XunBacklight never collide on GENI I2C.
 */
#include "Audio.h"
#include "xun_qup_lock.h"

#define Out ModemOut

#define QUP0_SE1_PA 0x04A84000u
#define QUP0_SE2_PA 0x04A88000u
#define GENI_FW_REVISION_RO 0x68
#define SE_GENI_STATUS 0x40
#define GENI_SER_M_CLK_CFG 0x48
#define SE_GENI_CLK_SEL 0x7C
#define SE_I2C_TX_TRANS_LEN 0x26C
#define SE_I2C_RX_TRANS_LEN 0x270
#define SE_GENI_M_CMD0 0x600
#define SE_GENI_M_CMD_CTRL 0x604
#define SE_GENI_M_IRQ_STATUS 0x610
#define SE_GENI_M_IRQ_CLEAR 0x618
#define SE_GENI_TX_FIFOn 0x700
#define SE_GENI_RX_FIFOn 0x780
#define SE_GENI_RX_FIFO_STATUS 0x804
#define M_GENI_CMD_ACTIVE (1u << 0)
#define M_CMD_DONE (1u << 0)
#define M_CMD_CANCEL (1u << 4)
#define M_NACK (1u << 10)
#define M_I2C_ERR_MASK                                                         \
  ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) |   \
   (1u << 13))
#define I2C_OP_WRITE 1u
#define I2C_OP_READ 2u
#define I2C_STOP_STRETCH (1u << 2)

STATIC UINTN mSe1, mSe2;
STATIC XUN_QUP_LOCK mLock1, mLock2;
STATIC BOOLEAN mLockInit1, mLockInit2;

STATIC PXUN_QUP_LOCK LockFor(UINT32 SeIdx) {
  if (SeIdx == 2) {
    if (!mLockInit2) {
      XunQupLockInit(&mLock2, XUN_QUP_SE2);
      mLockInit2 = TRUE;
    }
    return &mLock2;
  }
  if (!mLockInit1) {
    XunQupLockInit(&mLock1, XUN_QUP_SE1);
    mLockInit1 = TRUE;
  }
  return &mLock1;
}

STATIC UINTN BusBase(UINT32 SeIdx, BOOLEAN Log) {
  UINTN *p = (SeIdx == 2) ? &mSe2 : &mSe1;
  UINT32 pa = (SeIdx == 2) ? QUP0_SE2_PA : QUP0_SE1_PA;
  UINT32 fw, mclk;

  if (*p == 0) {
    *p = (UINTN)MapPhys(pa, SIZE_16KB, FALSE);
  }
  if (*p == 0) {
    return 0;
  }
  fw = MmioRead32(*p + GENI_FW_REVISION_RO);
  mclk = MmioRead32(*p + GENI_SER_M_CLK_CFG);
  if (Log) {
    Out("  SE%u fw_rev %08x (proto %u) clk_sel %x m_clk_cfg %x status %08x\r\n",
        SeIdx, fw, (fw >> 8) & 0xFF, MmioRead32(*p + SE_GENI_CLK_SEL), mclk,
        MmioRead32(*p + SE_GENI_STATUS));
  }
  if (((fw >> 8) & 0xFF) != 3 || (mclk & 1) == 0) {
    return 0;
  }
  return *p;
}

STATIC UINT32 WaitDone(UINTN Se) {
  UINT32 i, st = 0;

  for (i = 0; i < 20000; i++) {
    st = MmioRead32(Se + SE_GENI_M_IRQ_STATUS);
    if (st & (M_CMD_DONE | M_I2C_ERR_MASK)) {
      break;
    }
    KeStallExecutionProcessor(1);
  }
  MmioWrite32(Se + SE_GENI_M_IRQ_CLEAR, st);
  if ((st & M_I2C_ERR_MASK) || !(st & M_CMD_DONE)) {
    MmioWrite32(Se + SE_GENI_M_CMD_CTRL, 1u << 2);
    for (i = 0;
         i < 1000 && !(MmioRead32(Se + SE_GENI_M_IRQ_STATUS) & M_CMD_CANCEL);
         i++) {
      KeStallExecutionProcessor(1);
    }
    if (MmioRead32(Se + SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
      MmioWrite32(Se + SE_GENI_M_CMD_CTRL, 1u << 1);
      KeStallExecutionProcessor(100);
    }
    MmioWrite32(Se + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    return (st & M_NACK) ? 1 : (st & M_I2C_ERR_MASK) ? 2 : 3;
  }
  return 0;
}

STATIC UINT32 Xfer(UINTN Se, UINT8 Addr, CONST UINT8 *W, UINT32 WLen, UINT8 *R,
                   UINT32 RLen) {
  UINT32 r, got = 0, spins = 0, w, i, n;

  if (WLen != 0) {
    MmioWrite32(Se + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(Se + SE_I2C_TX_TRANS_LEN, WLen);
    MmioWrite32(Se + SE_GENI_M_CMD0, (I2C_OP_WRITE << 27) |
                                         ((UINT32)Addr << 9) |
                                         (RLen != 0 ? I2C_STOP_STRETCH : 0));
    for (i = 0; i < WLen; i += 4) {
      w = 0;
      n = MIN(4u, WLen - i);
      CopyMem(&w, W + i, n);
      MmioWrite32(Se + SE_GENI_TX_FIFOn, w);
    }
    r = WaitDone(Se);
    if (r != 0 || RLen == 0) {
      return r;
    }
  }
  MmioWrite32(Se + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32(Se + SE_I2C_RX_TRANS_LEN, RLen);
  MmioWrite32(Se + SE_GENI_M_CMD0, (I2C_OP_READ << 27) | ((UINT32)Addr << 9));
  while (got < RLen && spins < 20000) {
    if (MmioRead32(Se + SE_GENI_M_IRQ_STATUS) & M_I2C_ERR_MASK) {
      break;
    }
    if ((MmioRead32(Se + SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFu) == 0) {
      KeStallExecutionProcessor(1);
      spins++;
      continue;
    }
    w = MmioRead32(Se + SE_GENI_RX_FIFOn);
    n = MIN(4u, RLen - got);
    CopyMem(R + got, &w, n);
    got += n;
  }
  r = WaitDone(Se);
  return (r == 0 && got < RLen) ? 3 : r;
}

STATIC UINT32 AmpI2cXferBus(UINT32 SeIdx, UINT8 Addr, CONST UINT8 *W,
                            UINT32 WLen, UINT8 *R, UINT32 RLen) {
  PXUN_QUP_LOCK lock;
  UINTN se;
  UINT32 r;

  if (WLen > 16 || RLen > 16 || (WLen == 0 && RLen == 0) ||
      KeGetCurrentIrql() != PASSIVE_LEVEL) {
    return 4;
  }
  lock = LockFor(SeIdx);
  if (!XunQupLockAcquire(lock, 500)) {
    return 4;
  }
  se = BusBase(SeIdx, FALSE);
  r = (se != 0) ? Xfer(se, Addr, W, WLen, R, RLen) : 4;
  XunQupLockRelease(lock);
  return r;
}

UINT32 AmpI2cXfer(UINT8 Addr, CONST UINT8 *W, UINT32 WLen, UINT8 *R,
                  UINT32 RLen) {
  return AmpI2cXferBus(1, Addr, W, WLen, R, RLen);
}

STATIC CONST CHAR8 *Res(UINT32 R) {
  STATIC CONST CHAR8 *n[] = {"ok", "NACK", "error", "timeout", "nobus"};
  return (R <= 4) ? n[R] : "err";
}

STATIC VOID ProbeOneBus(UINT32 SeIdx) {
  STATIC CONST struct {
    UINT8 Addr;
    CONST CHAR8 *Name;
    UINT8 Reg;
    UINT8 Len;
  } probe[] = {
      {0x34, "fs16xx (FourSemi #1), id reg 0x03", 0x03, 2},
      {0x35, "fs16xx (FourSemi #2), id reg 0x03", 0x03, 2},
      {0x28, "sipa (SIA81xx #1), reg 0x00", 0x00, 1},
      {0x29, "sipa (SIA81xx #2), reg 0x00", 0x00, 1},
      {0x2B, "sia81xx legacy, reg 0x00", 0x00, 1},
      {0x59, "aw87xxx, chip id reg 0x00", 0x00, 1},
  };
  UINTN se;
  PXUN_QUP_LOCK lock = LockFor(SeIdx);
  UINT32 i;

  if (!XunQupLockAcquire(lock, 500)) {
    Out("  SE%u: mutex timeout\r\n", SeIdx);
    return;
  }
  se = BusBase(SeIdx, TRUE);
  if (se == 0) {
    XunQupLockRelease(lock);
    Out("  SE%u is not a configured I2C master: skipped\r\n", SeIdx);
    return;
  }
  for (i = 0; i < ARRAY_SIZE(probe); i++) {
    UINT8 v[4] = {0};
    UINT32 r = Xfer(se, probe[i].Addr, &probe[i].Reg, 1, v, probe[i].Len);
    Out("  SE%u 0x%02x %-34a: %a", SeIdx, probe[i].Addr, probe[i].Name, Res(r));
    if (r == 0) {
      Out(" value %02x %02x (%u byte%a)", v[0], v[1], probe[i].Len,
          probe[i].Len > 1 ? "s" : "");
    }
    Out("\r\n");
  }
  XunQupLockRelease(lock);
}

VOID AmpProbe(VOID) {
  Out("\r\n  ==== speaker amp probe (QUP0 SE1 + SE2, xun_qup_lock, read-only) "
      "====\r\n");
  ProbeOneBus(1);
  ProbeOneBus(2);
}
