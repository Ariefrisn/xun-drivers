/*
 * Microsoft Bluetooth Extensible Transport DDI (bthxddi.h) + H4 / HCI_IBS pump.
 */
#include "driver.h"

#define XUN_MAX_HCI_ACL_SIZE 1021
#define BTHX_VALID_WRITE_PKT(t) ((t) == HciPacketCommand || (t) == HciPacketAclData)

/* Sends a single H4 frame directly to the controller (HCI_IBS sleep disabled). */
static NTSTATUS SendH4Frame(PXUN_BT_CONTEXT Ctx, UCHAR PktType, const UCHAR *Payload, ULONG Len)
{
    UCHAR *txBuf;
    NTSTATUS st;

    txBuf = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 1 + Len, XUN_BT_POOL_TAG);
    if (txBuf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    txBuf[0] = PktType;
    if (Len > 0) {
        RtlCopyMemory(txBuf + 1, Payload, Len);
    }

    st = UartWriteSync(Ctx, txBuf, 1 + Len, 500);
    ExFreePoolWithTag(txBuf, XUN_BT_POOL_TAG);
    Ctx->LastTxActivityMs = GetTicksMs();
    if (PktType == HCI_PKT_CMD) {
        Ctx->LastTxCmdMs = Ctx->LastTxActivityMs;
        Ctx->AwaitingEvt = TRUE;
    }
    Ctx->TxPackets++;
    if (Ctx->TxPackets <= 128) {
        LogHexDump((PktType == HCI_PKT_CMD) ? "hci -> CMD" : "hci -> ACL", Payload, Len);
    }
    return st;
}

static NTSTATUS TryCompleteOneRead(PXUN_BT_CONTEXT Ctx, WDFQUEUE Queue, PLIST_ENTRY ListHead, PULONG ListCount)
{
    NTSTATUS st;
    WDFREQUEST req = NULL;
    PLIST_ENTRY entry = NULL;
    PBT_RX_PACKET pkt = NULL;
    PVOID outBuf = NULL;
    size_t outLen = 0;
    size_t needed;
    PBTHX_HCI_READ_WRITE_CONTEXT hciOut;

    WdfSpinLockAcquire(Ctx->Lock);
    if (!IsListEmpty(ListHead)) {
        st = WdfIoQueueRetrieveNextRequest(Queue, &req);
        if (NT_SUCCESS(st)) {
            entry = RemoveHeadList(ListHead);
            (*ListCount)--;
            pkt = CONTAINING_RECORD(entry, BT_RX_PACKET, Link);
        }
    }
    WdfSpinLockRelease(Ctx->Lock);

    if (req == NULL || pkt == NULL) {
        return STATUS_NO_MORE_ENTRIES;
    }

    st = WdfRequestRetrieveOutputBuffer(req, sizeof(BTHX_HCI_READ_WRITE_CONTEXT), &outBuf, &outLen);
    if (!NT_SUCCESS(st)) {
        ExFreePoolWithTag(pkt, XUN_BT_POOL_TAG);
        WdfRequestComplete(req, st);
        return STATUS_SUCCESS;
    }

    needed = (size_t)FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + pkt->Len;
    if (outLen < needed) {
        ExFreePoolWithTag(pkt, XUN_BT_POOL_TAG);
        WdfRequestCompleteWithInformation(req, STATUS_BUFFER_TOO_SMALL, needed);
        return STATUS_SUCCESS;
    }

    hciOut = (PBTHX_HCI_READ_WRITE_CONTEXT)outBuf;
    hciOut->DataLen = pkt->Len;
    hciOut->Type = pkt->PktType;
    if (pkt->Len > 0) {
        RtlCopyMemory(hciOut->Data, pkt->Data, pkt->Len);
    }
    ExFreePoolWithTag(pkt, XUN_BT_POOL_TAG);
    WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, needed);
    return STATUS_SUCCESS;
}

VOID BthxFlushCompletedReads(PXUN_BT_CONTEXT Ctx)
{
    while (TryCompleteOneRead(Ctx, Ctx->ReadEventQueue, &Ctx->RxEventList, &Ctx->RxEventCount) == STATUS_SUCCESS) {
    }
    while (TryCompleteOneRead(Ctx, Ctx->ReadAclQueue, &Ctx->RxAclList, &Ctx->RxAclCount) == STATUS_SUCCESS) {
    }
}

static VOID DispatchCompletedPacket(PXUN_BT_CONTEXT Ctx, UCHAR PktType, const UCHAR *Data, ULONG Len)
{
    PBT_RX_PACKET pkt;

    if (PktType == HCI_PKT_EVENT) {
        Ctx->AwaitingEvt = FALSE;
        Ctx->RxEvents++;
        if (Ctx->RxEvents <= 128) {
            LogHexDump("hci <- EVT", Data, Len);
        }
    } else if (PktType == HCI_PKT_ACL) {
        Ctx->RxAcls++;
        if (Ctx->RxAcls <= 32) {
            LogHexDump("hci <- ACL", Data, Len);
        }
    } else {
        return;
    }

    pkt = (PBT_RX_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                         FIELD_OFFSET(BT_RX_PACKET, Data) + Len,
                                         XUN_BT_POOL_TAG);
    if (pkt == NULL) {
        return;
    }
    pkt->PktType = PktType;
    pkt->Len = Len;
    if (Len > 0) {
        RtlCopyMemory(pkt->Data, Data, Len);
    }

    WdfSpinLockAcquire(Ctx->Lock);
    if (PktType == HCI_PKT_EVENT) {
        /* Cap backlog so unconsumed events cannot exhaust nonpaged pool */
        if (Ctx->RxEventCount >= 256) {
            PLIST_ENTRY old = RemoveHeadList(&Ctx->RxEventList);
            Ctx->RxEventCount--;
            ExFreePoolWithTag(CONTAINING_RECORD(old, BT_RX_PACKET, Link), XUN_BT_POOL_TAG);
        }
        InsertTailList(&Ctx->RxEventList, &pkt->Link);
        Ctx->RxEventCount++;
    } else {
        if (Ctx->RxAclCount >= 256) {
            PLIST_ENTRY old = RemoveHeadList(&Ctx->RxAclList);
            Ctx->RxAclCount--;
            ExFreePoolWithTag(CONTAINING_RECORD(old, BT_RX_PACKET, Link), XUN_BT_POOL_TAG);
        }
        InsertTailList(&Ctx->RxAclList, &pkt->Link);
        Ctx->RxAclCount++;
    }
    WdfSpinLockRelease(Ctx->Lock);

    BthxFlushCompletedReads(Ctx);
}

VOID BthxFeedRxBytes(PXUN_BT_CONTEXT Ctx, const UCHAR *Data, ULONG Len)
{
    PH4_RX_PARSER p = &Ctx->RxParser;
    ULONG i;

    for (i = 0; i < Len; i++) {
        UCHAR ch = Data[i];

        if (p->Stage == H4_STAGE_TYPE) {
            if (ch == HCI_IBS_WAKE_IND) {
                UCHAR ack = HCI_IBS_WAKE_ACK;
                (VOID)UartWriteSync(Ctx, &ack, 1, 50);
                continue;
            }
            if (ch == HCI_IBS_WAKE_ACK) {
                Ctx->TxAwake = TRUE;
                continue;
            }
            if (ch == HCI_IBS_SLEEP_IND) {
                continue;
            }
            if (ch == HCI_PKT_EVENT) {
                p->PktType = ch;
                p->HdrExpected = 2;
                p->Pos = 0;
                p->Stage = H4_STAGE_HDR;
            } else if (ch == HCI_PKT_ACL) {
                p->PktType = ch;
                p->HdrExpected = 4;
                p->Pos = 0;
                p->Stage = H4_STAGE_HDR;
            } else if (ch == HCI_PKT_SCO) {
                p->PktType = ch;
                p->HdrExpected = 3;
                p->Pos = 0;
                p->Stage = H4_STAGE_HDR;
            } else {
                Ctx->UnexpBytesLogged++;
                if (Ctx->UnexpBytesLogged <= 16) {
                    LogPrint("h4: unexpected type byte %02x (ios=%08x st=%08x s_irq=%08x sw_ctrl=%u)\n",
                             ch,
                             MmioRead32(&Ctx->Se, SE_GENI_IOS),
                             MmioRead32(&Ctx->Se, SE_GENI_STATUS),
                             Ctx->LastRxIrqSt,
                             MmioRead32(&Ctx->TlmmSwCtrl, TLMM_IO) & 1u);
                }
            }
        } else if (p->Stage == H4_STAGE_HDR) {
            p->Buf[p->Pos++] = ch;
            if (p->Pos == p->HdrExpected) {
                if (p->PktType == HCI_PKT_EVENT) {
                    p->BodyExpected = (ULONG)p->Buf[1];
                } else if (p->PktType == HCI_PKT_ACL) {
                    p->BodyExpected = (ULONG)p->Buf[2] | ((ULONG)p->Buf[3] << 8);
                } else {
                    p->BodyExpected = (ULONG)p->Buf[2];
                }

                if (p->HdrExpected + p->BodyExpected > H4_MAX_FRAME_BYTES) {
                    LogPrint("h4: oversized frame (%u + %u), resetting parser\n",
                             p->HdrExpected, p->BodyExpected);
                    p->Stage = H4_STAGE_TYPE;
                    p->Pos = 0;
                } else if (p->BodyExpected == 0) {
                    DispatchCompletedPacket(Ctx, p->PktType, p->Buf, p->Pos);
                    p->Stage = H4_STAGE_TYPE;
                    p->Pos = 0;
                } else {
                    p->Stage = H4_STAGE_BODY;
                }
            }
        } else {
            p->Buf[p->Pos++] = ch;
            if (p->Pos == p->HdrExpected + p->BodyExpected) {
                DispatchCompletedPacket(Ctx, p->PktType, p->Buf, p->Pos);
                p->Stage = H4_STAGE_TYPE;
                p->Pos = 0;
            }
        }
    }
}

VOID BthxProcessTxAndSleep(PXUN_BT_CONTEXT Ctx)
{
    WDFREQUEST req = NULL;

    while (!Ctx->StopRequested && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->WriteQueue, &req))) {
        PVOID inBuf = NULL, outBuf = NULL;
        size_t inLen = 0, outLen = 0;
        NTSTATUS st;
        PBTHX_HCI_READ_WRITE_CONTEXT hciIn;
        BTHX_HCI_PACKET_TYPE pktType;

        st = WdfRequestRetrieveInputBuffer(req, sizeof(BTHX_HCI_READ_WRITE_CONTEXT), &inBuf, &inLen);
        if (NT_SUCCESS(st)) {
            st = WdfRequestRetrieveOutputBuffer(req, sizeof(BTHX_HCI_PACKET_TYPE), &outBuf, &outLen);
        }
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(req, st);
            continue;
        }

        hciIn = (PBTHX_HCI_READ_WRITE_CONTEXT)inBuf;
        pktType = (BTHX_HCI_PACKET_TYPE)hciIn->Type;
        if (inLen < (size_t)FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + hciIn->DataLen) {
            WdfRequestComplete(req, STATUS_INVALID_PARAMETER);
            continue;
        }

        st = SendH4Frame(Ctx, hciIn->Type, hciIn->Data, hciIn->DataLen);
        if (NT_SUCCESS(st)) {
            *((BTHX_HCI_PACKET_TYPE *)outBuf) = pktType;
            WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(BTHX_HCI_PACKET_TYPE));
        } else {
            WdfRequestComplete(req, st);
        }
    }
}

NTSTATUS BthxQueuesCreate(PXUN_BT_CONTEXT Ctx)
{
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS st;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Ctx->Device;
    st = WdfSpinLockCreate(&attrs, &Ctx->Lock);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    InitializeListHead(&Ctx->RxEventList);
    InitializeListHead(&Ctx->RxAclList);

    /* Default queue handles IOCTL_BTHX_* (and forwards anything else down) */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.PowerManaged = WdfFalse;
    qcfg.EvtIoDeviceControl = BthxIoDeviceControl;
    st = WdfIoQueueCreate(Ctx->Device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    st = WdfIoQueueCreate(Ctx->Device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->WriteQueue);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    st = WdfIoQueueCreate(Ctx->Device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->ReadEventQueue);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    st = WdfIoQueueCreate(Ctx->Device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->ReadAclQueue);
    return st;
}

VOID BthxDrainQueuesOnStop(PXUN_BT_CONTEXT Ctx)
{
    WdfIoQueuePurgeSynchronously(Ctx->WriteQueue);
    WdfIoQueuePurgeSynchronously(Ctx->ReadEventQueue);
    WdfIoQueuePurgeSynchronously(Ctx->ReadAclQueue);

    WdfSpinLockAcquire(Ctx->Lock);
    while (!IsListEmpty(&Ctx->RxEventList)) {
        PLIST_ENTRY e = RemoveHeadList(&Ctx->RxEventList);
        Ctx->RxEventCount--;
        ExFreePoolWithTag(CONTAINING_RECORD(e, BT_RX_PACKET, Link), XUN_BT_POOL_TAG);
    }
    while (!IsListEmpty(&Ctx->RxAclList)) {
        PLIST_ENTRY e = RemoveHeadList(&Ctx->RxAclList);
        Ctx->RxAclCount--;
        ExFreePoolWithTag(CONTAINING_RECORD(e, BT_RX_PACKET, Link), XUN_BT_POOL_TAG);
    }
    WdfSpinLockRelease(Ctx->Lock);
}

VOID BthxIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PXUN_BT_CONTEXT ctx = GetBtContext(device);
    PVOID inBuf = NULL, outBuf = NULL;
    size_t actualIn = 0, actualOut = 0;
    NTSTATUS st = STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    switch (IoControlCode) {
    case IOCTL_BTHX_GET_VERSION:
        st = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_VERSION), &outBuf, &actualOut);
        if (NT_SUCCESS(st)) {
            RtlCopyMemory(outBuf, &Microsoft_BTHX_DDI_Version, sizeof(BTHX_VERSION));
            LogPrint("bthx: GET_VERSION -> %u.%u\n",
                     (Microsoft_BTHX_DDI_Version.Version >> 16) & 0xFFFFu,
                     Microsoft_BTHX_DDI_Version.Version & 0xFFFFu);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_VERSION));
            return;
        }
        WdfRequestComplete(Request, st);
        return;

    case IOCTL_BTHX_SET_VERSION:
        st = WdfRequestRetrieveInputBuffer(Request, sizeof(BTHX_VERSION), &inBuf, &actualIn);
        if (NT_SUCCESS(st)) {
            PBTHX_VERSION ver = (PBTHX_VERSION)inBuf;
            LogPrint("bthx: SET_VERSION 0x%08x\n", ver->Version);
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;
        }
        WdfRequestComplete(Request, st);
        return;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        st = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_CAPABILITIES), &outBuf, &actualOut);
        if (NT_SUCCESS(st)) {
            PBTHX_CAPABILITIES caps = (PBTHX_CAPABILITIES)outBuf;
            RtlZeroMemory(caps, sizeof(*caps));
            caps->MaxAclTransferInSize = XUN_MAX_HCI_ACL_SIZE;
            caps->ScoSupport = ScoSupportHCIBypass;
            caps->MaxScoChannels = 1;
            caps->IsDeviceIdleCapable = FALSE;
            caps->IsDeviceWakeCapable = FALSE;
            LogPrint("bthx: QUERY_CAPABILITIES -> MaxAcl=%u\n", caps->MaxAclTransferInSize);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_CAPABILITIES));
            return;
        }
        WdfRequestComplete(Request, st);
        return;

    case IOCTL_BTHX_WRITE_HCI:
        st = WdfRequestRetrieveInputBuffer(Request, sizeof(BTHX_HCI_READ_WRITE_CONTEXT), &inBuf, &actualIn);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
            return;
        }
        st = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_HCI_PACKET_TYPE), &outBuf, &actualOut);
        if (!NT_SUCCESS(st) || actualOut != sizeof(BTHX_HCI_PACKET_TYPE)) {
            WdfRequestComplete(Request, NT_SUCCESS(st) ? STATUS_INVALID_PARAMETER : st);
            return;
        }
        {
            PBTHX_HCI_READ_WRITE_CONTEXT hci = (PBTHX_HCI_READ_WRITE_CONTEXT)inBuf;
            BTHX_HCI_PACKET_TYPE pt = (BTHX_HCI_PACKET_TYPE)hci->Type;
            if (!BTHX_VALID_WRITE_PKT(pt) ||
                actualIn < (size_t)FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + hci->DataLen) {
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
        }
        if (ctx->InitFailed) {
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
            return;
        }
        st = WdfRequestForwardToIoQueue(Request, ctx->WriteQueue);
        if (NT_SUCCESS(st)) {
            KeSetEvent(&ctx->WorkEvent, IO_NO_INCREMENT, FALSE);
            return;
        }
        WdfRequestComplete(Request, st);
        return;

    case IOCTL_BTHX_READ_HCI:
        st = WdfRequestRetrieveInputBuffer(Request, sizeof(BTHX_HCI_PACKET_TYPE), &inBuf, &actualIn);
        if (!NT_SUCCESS(st) || actualIn != sizeof(BTHX_HCI_PACKET_TYPE)) {
            WdfRequestComplete(Request, NT_SUCCESS(st) ? STATUS_INVALID_PARAMETER : st);
            return;
        }
        st = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_HCI_READ_WRITE_CONTEXT), &outBuf, &actualOut);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
            return;
        }
        {
            BTHX_HCI_PACKET_TYPE pt = *((BTHX_HCI_PACKET_TYPE *)inBuf);
            WDFQUEUE targetQ;
            if (pt == HciPacketEvent) {
                targetQ = ctx->ReadEventQueue;
            } else if (pt == HciPacketAclData) {
                targetQ = ctx->ReadAclQueue;
            } else {
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
            st = WdfRequestForwardToIoQueue(Request, targetQ);
            if (!NT_SUCCESS(st)) {
                WdfRequestComplete(Request, st);
                return;
            }
            if (ctx->FwReady) {
                KeSetEvent(&ctx->WorkEvent, IO_NO_INCREMENT, FALSE);
            }
            return;
        }

    default:
        /* Forward any non-BTHX IOCTLs down the filter stack */
        {
            WDF_REQUEST_SEND_OPTIONS opts;
            WdfRequestFormatRequestUsingCurrentType(Request);
            WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
            if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &opts)) {
                WdfRequestComplete(Request, WdfRequestGetStatus(Request));
            }
            return;
        }
    }
}

