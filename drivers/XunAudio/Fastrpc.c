/*
 * Fastrpc.c: Qualcomm FastRPC over GLINK ("fastrpcglink-apps-dsp") probe & bridge
 * for ADSP sensor_pd (sensorspd) on Redmi Pad SE (SM6225).
 */
#include "Audio.h"

#define Out  ModemOut
#define CHAN "fastrpcglink-apps-dsp"
#define T    (UINT32)(AudMs () / 1000), (UINT32)(AudMs () % 1000)

#pragma pack(1)
typedef struct {
  INT32  ClientId;
  INT32  Tid;
  UINT64 Ctx;
  UINT32 Handle;
  UINT32 Sc;
  UINT64 Addr;
  UINT64 Size;
} FASTRPC_MSG;

typedef struct {
  UINT64 Ctx;
  INT32  RetVal;
} FASTRPC_INVOKE_RSP;
#pragma pack()

STATIC BOOLEAN mUpLogged, mAttachSent;
STATIC UINT32  mRxCount;

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
  if (!mAttachSent && AudMs () >= 1500) {
    /* Try FASTRPC_RMID_INIT_CREATE_STATIC (method 8, 3 inbufs: 0x08030000) for sensorspd */
    PHYSICAL_ADDRESS low = { 0 }, high = { .QuadPart = 0xFFFFFFFFULL }, bound = { 0 };
    UINT8 *buf = (UINT8 *)MmAllocateContiguousMemorySpecifyCache (4096, low, high, bound, MmNonCached);
    mAttachSent = TRUE;
    if (buf != NULL) {
      PHYSICAL_ADDRESS pa = MmGetPhysicalAddress (buf);
      UINT64 basePa = (UINT64)pa.QuadPart;
      /* Layout: [0..47] 3x remote_buf64 {u64 pv, u64 len}, [64..79] "sensorspd\0", [80..95] pages {u64 addr, u64 size}, [96..103] attrs */
      UINT64 *ra = (UINT64 *)buf;
      FASTRPC_MSG msg;
      BOOLEAN ok;
      ZeroMem (buf, 4096);
      CopyMem (buf + 64, "sensorspd", 10);
      /* Remote heap: adsp_region 0xEF400000, size 0x100000 (or buf+512 2048B) */
      *(UINT64 *)(buf + 80) = basePa + 512;
      *(UINT64 *)(buf + 88) = 2048;
      ra[0] = basePa + 64;  ra[1] = 10;   /* name */
      ra[2] = basePa + 80;  ra[3] = 16;   /* mem */
      ra[4] = basePa + 96;  ra[5] = 4;    /* attrs */
      ZeroMem (&msg, sizeof (msg));
      msg.ClientId = 0;
      msg.Tid      = 0;
      msg.Ctx      = 0x1002ULL;
      msg.Handle   = 0;
      msg.Sc       = (8u << 24) | (3u << 16); /* FASTRPC_RMID_INIT_CREATE_STATIC, 3 inbufs */
      msg.Addr     = basePa;
      msg.Size     = 128;
      ok = GlinkSend (CHAN, &msg, sizeof (msg));
      Out ("  t=%u.%03u fastrpc: sent INIT_CREATE_STATIC(sensorspd) pa=%llx -> %u\r\n", T, basePa, ok);
    }
  }
}

VOID FastrpcSummary(VOID)
{
  Out ("  fastrpc: up=%u attach_sent=%u rx=%u\r\n", mUpLogged, mAttachSent, mRxCount);
}
