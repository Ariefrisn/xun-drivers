/*
 * QRTR over the ADSP's IPCRTR channel (net/qrtr) + the apps services the ADSP asks for:
 *  - name service: answer HELLO, log the ADSP's NEW_SERVERs, answer NEW_LOOKUP (our servers +
 *    the empty end-of-list record, like Linux qrtr ns);
 *  - pd-mapper (QMI servreg-locator 0x40 v0x101): domains from adspr/adsps/adspua.jsn,
 *    msm/adsp/{root_pd,sensor_pd,audio_pd}, instance 74.
 * Same packet formats as drivers/XunModem/Glink.c + ModemSvc.c.
 */
#include "Audio.h"
#include "../common/xun_accel.h"

#define Out ModemOut
#define CHAN               "IPCRTR"

#define QRTR_NODE_APPS     1
#define QRTR_PORT_CTRL     0xFFFFFFFEu
#define QRTR_TYPE_DATA     1
#define QRTR_TYPE_HELLO    2
#define QRTR_TYPE_BYE      3
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define QRTR_TYPE_DEL_CLIENT 6
#define QRTR_TYPE_RESUME_TX 7
#define QRTR_TYPE_EXIT     8
#define QRTR_TYPE_PING     9
#define QRTR_TYPE_NEW_LOOKUP 10
#define QRTR_TYPE_DEL_LOOKUP 11

#define SVC_PDM            0x40
#define PORT_PDM           0x4001
#define SVC_SNS            0x190
#define PORT_SNS           0x4002
#define QMI_REQ            0
#define QMI_RESP           2
#define QMI_IND            4

#pragma pack(1)
typedef struct { UINT32 Version, Type, SrcNode, SrcPort, ConfirmRx, Size, DstNode, DstPort; } QRTR_HDR_V1;
typedef struct { UINT8 Version, Type, Flags, OptLen; UINT32 Size; UINT16 SrcNode, SrcPort, DstNode, DstPort; } QRTR_HDR_V2;
typedef struct { UINT32 Cmd, A, B, C, D; } QRTR_CTRL;
#pragma pack()

STATIC UINT32  mVer = 1, mNode = 0xFFFFFFFFu;
STATIC BOOLEAN mHelloRx, mHelloSent, mAnnounced;
STATIC struct { UINT32 Svc, Inst, Node, Port; } mSrv[64];
STATIC UINTN   mSrvCount, mLookups, mPdReqs, mOther, mResumes;

/* SNS_CLIENT_SVC (0x190) state for mir3da accelerometer */
STATIC BOOLEAN mSnsFound, mSnsSuidSent, mSnsCfgSent;
STATIC UINT32  mSnsNode, mSnsPort, mSnsLogCnt, mSnsSampleCnt;
STATIC UINT16  mSnsTxn = 1;
STATIC UINT64  mAccelSuidLo, mAccelSuidHi;
STATIC PCALLBACK_OBJECT mAccelCb;

#define T  (UINT32)(AudMs () / 1000), (UINT32)(AudMs () % 1000)

STATIC VOID QrtrSend(UINT32 Type, UINT32 SrcPort, UINT32 DstNode, UINT32 DstPort, CONST VOID *Payload, UINT32 Len)
{
  UINT8 buf[sizeof (QRTR_HDR_V1) + 512];
  UINT32 n;

  if (Len > 512) {
    return;
  }
  ZeroMem (buf, sizeof (buf));
  if (mVer == 2) {
    QRTR_HDR_V2 *h = (QRTR_HDR_V2 *)buf;
    h->Version = 2;
    h->Type    = (UINT8)Type;
    h->Flags   = (Type == QRTR_TYPE_DATA) ? 1 : 0;
    h->Size    = Len;
    h->SrcNode = QRTR_NODE_APPS;
    h->SrcPort = (UINT16)SrcPort;
    h->DstNode = (UINT16)DstNode;
    h->DstPort = (UINT16)DstPort;
    n = sizeof (*h);
  } else {
    QRTR_HDR_V1 *h = (QRTR_HDR_V1 *)buf;
    h->Version   = 1;
    h->Type      = Type;
    h->SrcNode   = QRTR_NODE_APPS;
    h->SrcPort   = SrcPort;
    h->ConfirmRx = (Type == QRTR_TYPE_DATA) ? 1 : 0;
    h->Size      = Len;
    h->DstNode   = DstNode;
    h->DstPort   = DstPort;
    n = sizeof (*h);
  }
  CopyMem (buf + n, Payload, Len);
  GlinkSend (CHAN, buf, ALIGN_VALUE (n + Len, 4));
}

/* ---- SNS_CLIENT_SVC (0x190) QMI + Protobuf client for mir3da accel ---- */

STATIC INT32 FloatMps2ToMg(UINT32 Bits)
{
  UINT32 sign = (Bits >> 31) & 1u;
  INT32  exp  = (INT32)((Bits >> 23) & 0xFFu) - 127;
  INT64  mant = (INT64)(Bits & 0x7FFFFFu) | 0x800000LL;
  INT64  mg;

  if (((Bits >> 23) & 0xFFu) == 0 || ((Bits >> 23) & 0xFFu) == 0xFFu || exp < -12) {
    return 0;
  }
  if (exp > 10) {
    return sign ? -32000 : 32000;
  }
  mg = (mant * 102LL) >> (23 - exp);
  if (mg > 32000) mg = 32000;
  return sign ? -(INT32)mg : (INT32)mg;
}

STATIC UINT32 PbPutVarint(UINT8 *P, UINT32 V)
{
  UINT32 n = 0;
  while (V >= 0x80u) {
    P[n++] = (UINT8)((V & 0x7Fu) | 0x80u);
    V >>= 7;
  }
  P[n++] = (UINT8)V;
  return n;
}

STATIC BOOLEAN PbReadVarint(CONST UINT8 **Pp, CONST UINT8 *End, UINT64 *OutVal)
{
  CONST UINT8 *p = *Pp;
  UINT64 v = 0;
  UINT32 shift = 0;
  while (p < End && shift < 64) {
    UINT8 b = *p++;
    v |= (UINT64)(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) {
      *Pp = p;
      *OutVal = v;
      return TRUE;
    }
    shift += 7;
  }
  return FALSE;
}

STATIC VOID SnsSendClientReq(UINT64 SuidLo, UINT64 SuidHi, UINT32 MsgId, CONST UINT8 *InnerPb, UINT32 InnerLen)
{
  UINT8 reqPb[48], pb[128], qmi[160];
  UINT32 rLen = 0, pLen = 0, qLen;

  /* sns_std_request: field 2 (bytes payload) */
  if (InnerLen > 0) {
    reqPb[rLen++] = 0x12;
    rLen += PbPutVarint (reqPb + rLen, InnerLen);
    CopyMem (reqPb + rLen, InnerPb, InnerLen);
    rLen += InnerLen;
  }

  /* sns_client_request_msg:
   *   1: suid { 1: fixed64 lo, 2: fixed64 hi }
   *   2: msg_id (uint32)
   *   3: susp_config { 1: apss=0, 2: wakeup=0 }
   *   4: request (sns_std_request) */
  pb[pLen++] = 0x0A;
  pb[pLen++] = 18;
  pb[pLen++] = 0x09;
  CopyMem (pb + pLen, &SuidLo, 8); pLen += 8;
  pb[pLen++] = 0x11;
  CopyMem (pb + pLen, &SuidHi, 8); pLen += 8;

  pb[pLen++] = 0x10;
  pLen += PbPutVarint (pb + pLen, MsgId);

  pb[pLen++] = 0x1A; pb[pLen++] = 4;
  pb[pLen++] = 0x08; pb[pLen++] = 0;
  pb[pLen++] = 0x10; pb[pLen++] = 0;

  pb[pLen++] = 0x22;
  pLen += PbPutVarint (pb + pLen, rLen);
  if (rLen > 0) {
    CopyMem (pb + pLen, reqPb, rLen);
    pLen += rLen;
  }

  /* QMI SNS_CLIENT_REQ_V01 (0x0020): TLV 0x01 (2B len + 2B payload_len + pb), TLV 0x02 (use_jumbo=0) */
  qmi[0] = QMI_REQ;
  *(UINT16 *)(qmi + 1) = mSnsTxn++;
  *(UINT16 *)(qmi + 3) = 0x0020;
  *(UINT16 *)(qmi + 5) = (UINT16)(1 + 2 + 2 + pLen + 4);
  qLen = 7;
  qmi[qLen++] = 0x01;
  *(UINT16 *)(qmi + qLen) = (UINT16)(2 + pLen); qLen += 2;
  *(UINT16 *)(qmi + qLen) = (UINT16)pLen;       qLen += 2;
  CopyMem (qmi + qLen, pb, pLen);               qLen += pLen;
  qmi[qLen++] = 0x02;
  *(UINT16 *)(qmi + qLen) = 1;                  qLen += 2;
  qmi[qLen++] = 0;

  QrtrSend (QRTR_TYPE_DATA, PORT_SNS, mSnsNode, mSnsPort, qmi, qLen);
}

STATIC VOID SnsParseSensorEvent(CONST UINT8 *P, UINT32 Len)
{
  CONST UINT8 *end = P + Len;
  UINT32 axes[3] = { 0, 0, 0 }, nAxes = 0;

  while (P < end) {
    UINT64 tag = 0, l = 0;
    UINT32 fn, wt;
    if (!PbReadVarint (&P, end, &tag)) break;
    fn = (UINT32)(tag >> 3);
    wt = (UINT32)(tag & 7u);
    if (wt == 2) {
      if (!PbReadVarint (&P, end, &l) || P + l > end) break;
      if (fn == 1 && l >= 12) { /* packed repeated float data */
        CopyMem (axes, P, 12);
        nAxes = 3;
      }
      P += l;
    } else if (wt == 5) {
      if (P + 4 > end) break;
      if (fn == 1 && nAxes < 3) {
        CopyMem (&axes[nAxes++], P, 4);
      }
      P += 4;
    } else if (wt == 0) {
      if (!PbReadVarint (&P, end, &l)) break;
    } else if (wt == 1) {
      if (P + 8 > end) break;
      P += 8;
    } else {
      break;
    }
  }
  if (nAxes == 3) {
    XUN_ACCEL_SAMPLE s;
    s.AxMg = FloatMps2ToMg (axes[0]);
    s.AyMg = FloatMps2ToMg (axes[1]);
    s.AzMg = FloatMps2ToMg (axes[2]);
    s.Seq  = ++mSnsSampleCnt;
    if (mSnsSampleCnt <= 5) {
      Out ("  t=%u.%03u sns: accel #%u x=%d y=%d z=%d mG\r\n", T, s.Seq, s.AxMg, s.AyMg, s.AzMg);
    }
    if (mAccelCb != NULL) {
      ExNotifyCallback (mAccelCb, &s, NULL);
    }
  }
}

STATIC VOID SnsParseSuidEvent(CONST UINT8 *P, UINT32 Len)
{
  CONST UINT8 *end = P + Len;
  while (P < end) {
    UINT64 tag = 0, l = 0;
    UINT32 fn, wt;
    if (!PbReadVarint (&P, end, &tag)) break;
    fn = (UINT32)(tag >> 3);
    wt = (UINT32)(tag & 7u);
    if (wt == 2) {
      if (!PbReadVarint (&P, end, &l) || P + l > end) break;
      if (fn == 2 && mAccelSuidLo == 0 && mAccelSuidHi == 0) { /* suid */
        CONST UINT8 *sp = P, *send = P + l;
        UINT64 lo = 0, hi = 0;
        while (sp < send) {
          UINT64 stag = 0, sl = 0;
          if (!PbReadVarint (&sp, send, &stag)) break;
          if ((stag >> 3) == 1 && (stag & 7u) == 1 && sp + 8 <= send) {
            CopyMem (&lo, sp, 8); sp += 8;
          } else if ((stag >> 3) == 2 && (stag & 7u) == 1 && sp + 8 <= send) {
            CopyMem (&hi, sp, 8); sp += 8;
          } else if ((stag & 7u) == 2) {
            if (!PbReadVarint (&sp, send, &sl) || sp + sl > send) break;
            sp += sl;
          } else if ((stag & 7u) == 0) {
            if (!PbReadVarint (&sp, send, &sl)) break;
          } else if ((stag & 7u) == 5) {
            sp += 4;
          } else {
            break;
          }
        }
        if (lo != 0 || hi != 0) {
          mAccelSuidLo = lo;
          mAccelSuidHi = hi;
          Out ("  t=%u.%03u sns: accel suid=%016llx:%016llx\r\n", T, lo, hi);
          if (!mSnsCfgSent) {
            /* sns_std_sensor_config: field 1 float sample_rate = 10.0f (0x41200000) */
            UINT8 cfgPb[5] = { 0x0D, 0x00, 0x00, 0x20, 0x41 };
            SnsSendClientReq (mAccelSuidLo, mAccelSuidHi, 513, cfgPb, sizeof (cfgPb));
            mSnsCfgSent = TRUE;
            Out ("  t=%u.%03u sns: requested 10 Hz accel stream\r\n", T);
          }
        }
      }
      P += l;
    } else if (wt == 0) {
      if (!PbReadVarint (&P, end, &l)) break;
    } else if (wt == 1) {
      if (P + 8 > end) break;
      P += 8;
    } else if (wt == 5) {
      if (P + 4 > end) break;
      P += 4;
    } else {
      break;
    }
  }
}

STATIC VOID SnsParseEventMsg(CONST UINT8 *P, UINT32 Len)
{
  CONST UINT8 *end = P + Len;
  while (P < end) {
    UINT64 tag = 0, l = 0;
    UINT32 fn, wt;
    if (!PbReadVarint (&P, end, &tag)) break;
    fn = (UINT32)(tag >> 3);
    wt = (UINT32)(tag & 7u);
    if (wt == 2) {
      if (!PbReadVarint (&P, end, &l) || P + l > end) break;
      if (fn == 2) { /* sns_client_event */
        CONST UINT8 *ep = P, *eend = P + l;
        UINT32 evMsgId = 0;
        CONST UINT8 *evPay = NULL;
        UINT32 evPayLen = 0;
        while (ep < eend) {
          UINT64 etag = 0, el = 0;
          UINT32 efn, ewt;
          if (!PbReadVarint (&ep, eend, &etag)) break;
          efn = (UINT32)(etag >> 3);
          ewt = (UINT32)(etag & 7u);
          if (ewt == 0) {
            if (!PbReadVarint (&ep, eend, &el)) break;
            if (efn == 1) evMsgId = (UINT32)el;
            else if (efn == 2) { /* timestamp */ }
          } else if (ewt == 2) {
            if (!PbReadVarint (&ep, eend, &el) || ep + el > eend) break;
            if (efn == 3) { evPay = ep; evPayLen = (UINT32)el; }
            ep += el;
          } else if (ewt == 1) {
            if (ep + 8 > eend) break;
            ep += 8;
          } else if (ewt == 5) {
            if (ep + 4 > eend) break;
            ep += 4;
          } else {
            break;
          }
        }
        if (evPay != NULL && evPayLen > 0) {
          if (evMsgId == 768) {
            SnsParseSuidEvent (evPay, evPayLen);
          } else if (evMsgId == 1025) {
            SnsParseSensorEvent (evPay, evPayLen);
          }
        }
      }
      P += l;
    } else if (wt == 0) {
      if (!PbReadVarint (&P, end, &l)) break;
    } else if (wt == 1) {
      if (P + 8 > end) break;
      P += 8;
    } else if (wt == 5) {
      if (P + 4 > end) break;
      P += 4;
    } else {
      break;
    }
  }
}

STATIC VOID SnsRx(CONST UINT8 *Qmi, UINT32 Len)
{
  UINT8 ctl;
  UINT16 msgId, msgLen;
  UINT32 off = 7;

  if (Len < 7) return;
  ctl    = Qmi[0];
  msgId  = *(CONST UINT16 *)(Qmi + 3);
  msgLen = *(CONST UINT16 *)(Qmi + 5);
  if (mSnsLogCnt++ < 6) {
    Out ("  t=%u.%03u sns: rx qmi ctl=%02x msg=%04x len=%u\r\n", T, ctl, msgId, msgLen);
  }
  if (ctl == QMI_IND && (msgId == 0x0021 || msgId == 0x0022) && 7u + msgLen <= Len) {
    while (off + 3 <= 7u + msgLen) {
      UINT8 tag = Qmi[off];
      UINT16 tlen = *(CONST UINT16 *)(Qmi + off + 1);
      off += 3;
      if (off + tlen > 7u + msgLen) break;
      if (tag == 0x01 && tlen >= 2) {
        UINT16 pbLen = *(CONST UINT16 *)(Qmi + off);
        if (2u + pbLen <= tlen) {
          SnsParseEventMsg (Qmi + off + 2, pbLen);
        }
      }
      off += tlen;
    }
  }
}

STATIC VOID SendCtrl(UINT32 Type, UINT32 DstNode, UINT32 DstPort, UINT32 A, UINT32 B, UINT32 C, UINT32 D)
{
  QRTR_CTRL c = { Type, A, B, C, D };
  QrtrSend (Type, QRTR_PORT_CTRL, DstNode, DstPort, &c, sizeof (c));
}

STATIC CONST CHAR8 *SvcName(UINT32 Svc)
{
  switch (Svc) {
  case 0x2B: return "ssctl";
  case 0x40: return "servreg-loc";
  case 0x42: return "servreg-notif";
  default:   return "";
  }
}

/* ---------------- pd-mapper ---------------- */

STATIC CONST struct { CONST CHAR8 *Svc, *Domain; } mPdMap[] = {
  { "tms/servreg", "msm/adsp/root_pd" },
  { "tms/servreg", "msm/adsp/sensor_pd" },
  { "tms/servreg", "msm/adsp/audio_pd" },
  { "avs/audio",   "msm/adsp/audio_pd" },
};

STATIC CONST UINT8 *QmFind(CONST UINT8 *D, UINT32 Len, UINT8 Type, UINT16 *TLen)
{
  UINT32 o = 7;

  while (o + 3 <= Len) {
    UINT16 l = *(CONST UINT16 *)(D + o + 1);
    if (o + 3 + l > Len) {
      break;
    }
    if (D[o] == Type) {
      *TLen = l;
      return D + o + 3;
    }
    o += 3 + l;
  }
  return NULL;
}

STATIC UINT32 QmTlv(UINT8 *M, UINT32 N, UINT8 Type, CONST VOID *V, UINT16 Len)
{
  M[N] = Type;
  *(UINT16 *)(M + N + 1) = Len;
  CopyMem (M + N + 3, V, Len);
  return N + 3 + Len;
}

STATIC VOID PdMapperRx(UINT32 Node, UINT32 Port, UINT16 Txn, UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  UINT8 m[400], list[300];
  CHAR8 name[80];
  UINT16 r[2] = { 0, 0 }, v, l = 0;
  CONST UINT8 *s;
  UINT32 n = 7, ln = 1, i, cnt = 0, inst = 74, zero = 0;

  m[0] = QMI_RESP;
  *(UINT16 *)(m + 1) = Txn;
  *(UINT16 *)(m + 3) = Msg;
  name[0] = 0;
  s = QmFind (D, Len, 1, &l);
  if (s != NULL) {
    l = (UINT16)MIN ((UINTN)l, sizeof (name) - 1);
    CopyMem (name, s, l);
    name[l] = 0;
  }
  n = QmTlv (m, n, 2, r, 4);
  if (Msg == 0x21) {                            /* GET_DOMAIN_LIST */
    for (i = 0; i < ARRAY_SIZE (mPdMap); i++) {
      UINT32 dl;
      if (AsciiStrCmp (mPdMap[i].Svc, name) != 0) {
        continue;
      }
      dl = (UINT32)AsciiStrLen (mPdMap[i].Domain);
      list[ln++] = (UINT8)dl;
      CopyMem (list + ln, mPdMap[i].Domain, dl);
      ln += dl;
      CopyMem (list + ln, &inst, 4);
      ln += 4;
      list[ln++] = 0;                           /* service_data_valid */
      CopyMem (list + ln, &zero, 4);
      ln += 4;
      cnt++;
    }
    list[0] = (UINT8)cnt;
    v = (UINT16)cnt;
    n = QmTlv (m, n, 0x10, &v, 2);             /* total_domains */
    v = 1;
    n = QmTlv (m, n, 0x11, &v, 2);             /* db_rev_count */
    if (cnt != 0) {
      n = QmTlv (m, n, 0x12, list, (UINT16)ln);
    }
  }
  *(UINT16 *)(m + 5) = (UINT16)(n - 7);
  QrtrSend (QRTR_TYPE_DATA, PORT_PDM, Node, Port, m, n);
  mPdReqs++;
  Out ("  t=%u.%03u pd-mapper: msg %x \"%a\" -> %u domain(s)\r\n", T, Msg, name, cnt);
}

/* ---------------- RX ---------------- */

STATIC VOID QrtrRx(CONST UINT8 *P, UINT32 Len)
{
  UINT32 type, srcNode, srcPort, dstPort, size, hlen, confirm;
  CONST QRTR_CTRL *c;

  if (Len >= sizeof (QRTR_HDR_V1) && *(CONST UINT32 *)P == 1) {
    CONST QRTR_HDR_V1 *h = (CONST QRTR_HDR_V1 *)P;
    type = h->Type; srcNode = h->SrcNode; srcPort = h->SrcPort; dstPort = h->DstPort;
    size = h->Size; hlen = sizeof (*h); confirm = h->ConfirmRx;
    mVer = 1;
  } else if (Len >= sizeof (QRTR_HDR_V2) && P[0] == 2) {
    CONST QRTR_HDR_V2 *h = (CONST QRTR_HDR_V2 *)P;
    type = h->Type; srcNode = h->SrcNode; srcPort = h->SrcPort; dstPort = h->DstPort;
    size = h->Size; hlen = sizeof (*h) + h->OptLen * 4; confirm = h->Flags & 1;
    srcPort = (srcPort == 0xFFFE) ? QRTR_PORT_CTRL : srcPort;
    dstPort = (dstPort == 0xFFFE) ? QRTR_PORT_CTRL : dstPort;
    mVer = 2;
  } else {
    Out ("  t=%u.%03u qrtr: bad packet, %u bytes\r\n", T, Len);
    return;
  }
  if (hlen + size > Len) {
    Out ("  t=%u.%03u qrtr: truncated (%u + %u > %u)\r\n", T, hlen, size, Len);
    return;
  }
  c = (CONST QRTR_CTRL *)(P + hlen);

  switch (type) {
  case QRTR_TYPE_HELLO:
    mNode = srcNode;
    mHelloRx = TRUE;
    mHelloSent = FALSE;                         /* (re)answer with our HELLO */
    Out ("  t=%u.%03u qrtr v%u HELLO from node %u\r\n", T, mVer, srcNode);
    break;
  case QRTR_TYPE_NEW_SERVER:
    if (c->A == 0 && c->B == 0) {
      break;                                    /* end of a lookup answer */
    }
    if (mSrvCount < ARRAY_SIZE (mSrv)) {
      mSrv[mSrvCount].Svc  = c->A;
      mSrv[mSrvCount].Inst = c->B;
      mSrv[mSrvCount].Node = c->C;
      mSrv[mSrvCount].Port = c->D;
      mSrvCount++;
    }
    Out ("  t=%u.%03u ADSP service %x v%u inst %x node %u port %x %a\r\n", T, c->A, c->B & 0xFF, c->B >> 8, c->C, c->D,
         SvcName (c->A));
    if (c->A == SVC_SNS && !mSnsFound) {
      mSnsNode = c->C;
      mSnsPort = c->D;
      mSnsFound = TRUE;
    }
    break;
  case QRTR_TYPE_NEW_LOOKUP:
    mLookups++;
    Out ("  t=%u.%03u ADSP lookup svc %x inst %x from %u:%x\r\n", T, c->A, c->B, srcNode, srcPort);
    if (c->A == 0 || c->A == SVC_PDM) {
      SendCtrl (QRTR_TYPE_NEW_SERVER, srcNode, srcPort, SVC_PDM, 0x101, QRTR_NODE_APPS, PORT_PDM);
    }
    SendCtrl (QRTR_TYPE_NEW_SERVER, srcNode, srcPort, 0, 0, 0, 0);
    break;
  case QRTR_TYPE_DATA:
    if (confirm) {
      QRTR_CTRL r = { QRTR_TYPE_RESUME_TX, QRTR_NODE_APPS, dstPort, 0, 0 };
      QrtrSend (QRTR_TYPE_RESUME_TX, dstPort, srcNode, srcPort, &r, sizeof (r));
    }
    if (dstPort == PORT_PDM && size >= 7 && P[hlen] == QMI_REQ) {
      PdMapperRx (srcNode, srcPort, *(CONST UINT16 *)(P + hlen + 1), *(CONST UINT16 *)(P + hlen + 3), P + hlen, size);
    } else if (dstPort == PORT_SNS && size >= 7) {
      SnsRx (P + hlen, size);
    } else if (mOther++ < 16) {
      Out ("  t=%u.%03u qrtr DATA %u:%x -> port %x, %u B, qmi %02x txn %04x msg %04x\r\n", T, srcNode, srcPort, dstPort,
           size, P[hlen], *(CONST UINT16 *)(P + hlen + 1), *(CONST UINT16 *)(P + hlen + 3));
    }
    break;
  case QRTR_TYPE_RESUME_TX:
    mResumes++;
    break;
  default:
    if (mOther++ < 16) {
      Out ("  t=%u.%03u qrtr type %u from %u:%x: %x %x %x %x\r\n", T, type, srcNode, srcPort, c->A, c->B, c->C, c->D);
    }
    break;
  }
}

VOID QrtrInit(VOID)
{
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES oa;

  RtlInitUnicodeString (&name, XUN_ACCEL_CALLBACK_NAME);
  InitializeObjectAttributes (&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
  (VOID)ExCreateCallback (&mAccelCb, &oa, TRUE, TRUE);
  GlinkRegister (CHAN, QrtrRx, FALSE);
}

VOID QrtrPoll(VOID)
{
  if (!GlinkChanUp (CHAN)) {
    return;
  }
  if (!mHelloSent) {
    SendCtrl (QRTR_TYPE_HELLO, mNode, QRTR_PORT_CTRL, 0, 0, 0, 0);
    mHelloSent = TRUE;
    Out ("  t=%u.%03u sent qrtr HELLO to node %x\r\n", T, mNode);
  }
  if (mHelloRx && !mAnnounced) {
    SendCtrl (QRTR_TYPE_NEW_SERVER, mNode, QRTR_PORT_CTRL, SVC_PDM, 0x101, QRTR_NODE_APPS, PORT_PDM);
    mAnnounced = TRUE;
    Out ("  t=%u.%03u announced pd-mapper 0x40\r\n", T);
  }
  if (mHelloRx && mSnsFound && !mSnsSuidSent) {
    /* sns_suid_req: 1: data_type="accel", 2: register_updates=0, 3: default_only=1 */
    STATIC CONST UINT8 suidReq[11] = {
      0x0A, 0x05, 'a', 'c', 'c', 'e', 'l',
      0x10, 0x00,
      0x18, 0x01
    };
    SnsSendClientReq (0xABABABABABABABABULL, 0xABABABABABABABABULL, 512, suidReq, sizeof (suidReq));
    mSnsSuidSent = TRUE;
    Out ("  t=%u.%03u sns: sent SUID lookup for accel to %u:%x\r\n", T, mSnsNode, mSnsPort);
  }
}

VOID QrtrSummary(VOID)
{
  UINTN i;

  Out ("  qrtr: adsp node %x, hello %u, %u services, %u lookups, pd-mapper %u reqs, %u RESUME_TX\r\n", mNode, mHelloRx,
       (UINT32)mSrvCount, (UINT32)mLookups, (UINT32)mPdReqs, (UINT32)mResumes);
  for (i = 0; i < mSrvCount; i++) {
    Out ("   svc %x v%u inst %x port %x %a\r\n", mSrv[i].Svc, mSrv[i].Inst & 0xFF, mSrv[i].Inst >> 8, mSrv[i].Port,
         SvcName (mSrv[i].Svc));
  }
}
