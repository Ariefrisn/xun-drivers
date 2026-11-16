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
    FASTRPC_MSG msg;
    BOOLEAN ok;
    ZeroMem (&msg, sizeof (msg));
    msg.ClientId = 0;
    msg.Tid      = 0;
    msg.Ctx      = 0x1001ULL;
    msg.Handle   = 0; /* FASTRPC_INIT_HANDLE */
    msg.Sc       = 0; /* FASTRPC_RMID_INIT_ATTACH */
    msg.Addr     = 0;
    msg.Size     = 0;
    ok = GlinkSend (CHAN, &msg, sizeof (msg));
    mAttachSent = TRUE;
    Out ("  t=%u.%03u fastrpc: sent INIT_ATTACH (%u B) -> %u\r\n", T, (UINT32)sizeof (msg), ok);
  }
}

VOID FastrpcSummary(VOID)
{
  Out ("  fastrpc: up=%u attach_sent=%u rx=%u\r\n", mUpLogged, mAttachSent, mRxCount);
}
