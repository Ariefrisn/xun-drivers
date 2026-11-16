/*
 * Fastrpc.c: Qualcomm FastRPC over GLINK ("fastrpcglink-apps-dsp")
 * Safe Stage 1b: read-only APPS_SMMU (0x0C600000) survey for ADSP FastRPC SIDs (0x1C3..0x1C7).
 */
#include "Audio.h"

#define Out  ModemOut
#define CHAN "fastrpcglink-apps-dsp"
#define T    (UINT32)(AudMs () / 1000), (UINT32)(AudMs () % 1000)
#define APPS_SMMU_PA 0x0C600000ULL

STATIC BOOLEAN mUpLogged, mSurveyDone;
STATIC UINT32  mRxCount;

STATIC VOID SmmuSurveyFastrpc(VOID)
{
  PHYSICAL_ADDRESS pa;
  volatile UCHAR *smmu;
  UINT32 idr0, idr1, nsmr, pagesz, numpage, i, freeSmrs = 0;

  pa.QuadPart = (LONGLONG)APPS_SMMU_PA;
  smmu = (volatile UCHAR *)MmMapIoSpaceEx (pa, 0x2000, PAGE_READWRITE | PAGE_NOCACHE);
  if (smmu == NULL) {
    Out ("  fastrpc: APPS_SMMU map failed\r\n");
    return;
  }
  idr0 = *(volatile UINT32 *)(smmu + 0x20);
  idr1 = *(volatile UINT32 *)(smmu + 0x24);
  nsmr = idr0 & 0xFFu;
  pagesz = (idr1 & 0x80000000u) ? 0x10000u : 0x1000u;
  numpage = 1u << (((idr1 >> 28) & 7u) + 1u);
  Out ("  fastrpc: APPS_SMMU sCR0=%08x nsmr=%u pagesz=%x numpage=%u\r\n",
       *(volatile UINT32 *)smmu, nsmr, pagesz, numpage);
  for (i = 0; i < nsmr && i < 128; i++) {
    UINT32 smr  = *(volatile UINT32 *)(smmu + 0x800 + 4 * i);
    UINT32 s2cr = *(volatile UINT32 *)(smmu + 0xC00 + 4 * i);
    if ((smr & 0x80000000u) == 0) {
      freeSmrs++;
    } else {
      UINT32 id = smr & 0xFFFFu, mask = (smr >> 16) & 0x7FFFu;
      UINT32 s;
      for (s = 0x1C3; s <= 0x1C7; s++) {
        if ((s & ~mask) == (id & ~mask)) {
          UINT32 type = (s2cr >> 16) & 3u, cb = s2cr & 0xFFu;
          Out ("  fastrpc: SID %03x -> SMR%u id=%04x mask=%04x S2CR=%08x (type=%u cb=%u)\r\n",
               s, i, id, mask, s2cr, type, cb);
        }
      }
    }
  }
  Out ("  fastrpc: free SMRs=%u (no 0x1C3..0x1C7 match if none printed above)\r\n", freeSmrs);
  MmUnmapIoSpace ((PVOID)smmu, 0x2000);
}

STATIC VOID FastrpcRx(CONST UINT8 *P, UINT32 Len)
{
  mRxCount++;
  if (mRxCount <= 16) {
    UINT32 w0 = Len >= 4  ? *(CONST UINT32 *)(P + 0)  : 0;
    UINT32 w1 = Len >= 8  ? *(CONST UINT32 *)(P + 4)  : 0;
    UINT32 w2 = Len >= 12 ? *(CONST UINT32 *)(P + 8)  : 0;
    UINT32 w3 = Len >= 16 ? *(CONST UINT32 *)(P + 12) : 0;
    Out ("  t=%u.%03u fastrpc: rx #%u (%u B): %08x %08x %08x %08x\r\n", T, mRxCount, Len, w0, w1, w2, w3);
  }
}

VOID FastrpcInit(VOID)
{
  GlinkRegister (CHAN, FastrpcRx, TRUE);
}

VOID FastrpcPoll(VOID)
{
  if (!GlinkChanUp (CHAN)) {
    return;
  }
  if (!mUpLogged) {
    mUpLogged = TRUE;
    Out ("  t=%u.%03u fastrpc: channel %a UP\r\n", T, CHAN);
  }
  if (!mSurveyDone && AudMs () >= 1500) {
    mSurveyDone = TRUE;
    SmmuSurveyFastrpc ();
  }
}

VOID FastrpcSummary(VOID)
{
  Out ("  fastrpc: up=%u survey=%u rx=%u\r\n", mUpLogged, mSurveyDone, mRxCount);
}
