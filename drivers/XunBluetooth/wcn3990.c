/*
 * WCN3990 / WCN3950 controller power-on sequence, EDL version query,
 * high-speed baud switch, TLV/NVM patching, firmware download, and HCI_Reset.
 */
#include "driver.h"
#include <ntstrsafe.h>

static VOID SendHciResetBestEffort(PXUN_BT_CONTEXT Ctx);

/* Reads an entire file into a non-paged buffer. Caller must ExFreePoolWithTag. */
static NTSTATUS ReadFirmwareFile(PCWSTR RelName, PUCHAR *OutBuf, ULONG *OutSize)
{
    static const PCWSTR kPrefixes[] = {
        L"\\??\\C:\\xun\\fw\\bt\\",
        L"\\??\\C:\\xun\\fw\\"
    };
    ULONG p;

    for (p = 0; p < ARRAYSIZE(kPrefixes); p++) {
        WCHAR fullPath[160];
        UNICODE_STRING uPath;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE hFile = NULL;
        FILE_STANDARD_INFORMATION fsi;
        NTSTATUS st;
        PUCHAR buf;
        ULONG sz;

        RtlStringCbPrintfW(fullPath, sizeof(fullPath), L"%s%s", kPrefixes[p], RelName);
        RtlInitUnicodeString(&uPath, fullPath);
        InitializeObjectAttributes(&oa, &uPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        st = ZwCreateFile(&hFile,
                          GENERIC_READ | SYNCHRONIZE,
                          &oa,
                          &iosb,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ,
                          FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                          NULL,
                          0);
        if (!NT_SUCCESS(st)) {
            continue;
        }

        RtlZeroMemory(&fsi, sizeof(fsi));
        st = ZwQueryInformationFile(hFile, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
        if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > (4 * 1024 * 1024)) {
            ZwClose(hFile);
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        sz = fsi.EndOfFile.LowPart;
        buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, XUN_BT_POOL_TAG);
        if (buf == NULL) {
            ZwClose(hFile);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        st = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, buf, sz, NULL, NULL);
        ZwClose(hFile);
        if (!NT_SUCCESS(st) || iosb.Information != sz) {
            ExFreePoolWithTag(buf, XUN_BT_POOL_TAG);
            return NT_SUCCESS(st) ? STATUS_END_OF_FILE : st;
        }

        *OutBuf = buf;
        *OutSize = sz;
        LogPrint("fw: loaded %S (%u bytes)\n", fullPath, sz);
        return STATUS_SUCCESS;
    }

    LogPrint("fw: missing %S in C:\\xun\\fw\\bt\\\n", RelName);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* Reads exactly one H4 packet from UART synchronously within TimeoutMs. */
static NTSTATUS ReadH4PacketSync(PXUN_BT_CONTEXT Ctx, UCHAR *OutPktType, UCHAR *OutBody, ULONG MaxBody, ULONG *OutBodyLen, ULONG TimeoutMs)
{
    ULONGLONG startMs = GetTicksMs();
    UCHAR stage = 0; /* 0=type, 1=hdr, 2=body */
    UCHAR pktType = 0;
    ULONG hdrNeed = 0;
    ULONG bodyNeed = 0;
    ULONG pos = 0;

    for (;;) {
        UCHAR tmp[256];
        ULONG n = UartReadPoll(Ctx, tmp, sizeof(tmp));
        ULONG i;

        if (n == 0) {
            if ((GetTicksMs() - startMs) >= TimeoutMs) {
                return STATUS_IO_TIMEOUT;
            }
            if (Ctx->StopRequested) {
                return STATUS_CANCELLED;
            }
            SleepMs(1);
            continue;
        }

        for (i = 0; i < n; i++) {
            UCHAR ch = tmp[i];
            if (stage == 0) {
                if (ch == HCI_IBS_WAKE_IND) {
                    UCHAR ack = HCI_IBS_WAKE_ACK;
                    (VOID)UartWriteSync(Ctx, &ack, 1, 20);
                    continue;
                }
                if (ch == HCI_IBS_SLEEP_IND || ch == HCI_IBS_WAKE_ACK) {
                    continue;
                }
                if (ch == HCI_PKT_EVENT) {
                    pktType = ch;
                    hdrNeed = 2;
                    pos = 0;
                    stage = 1;
                } else if (ch == HCI_PKT_ACL) {
                    pktType = ch;
                    hdrNeed = 4;
                    pos = 0;
                    stage = 1;
                } else {
                    LogPrint("wcn: sync skip byte %02x\n", ch);
                    continue;
                }
            } else if (stage == 1) {
                if (pos < MaxBody) {
                    OutBody[pos] = ch;
                }
                pos++;
                if (pos == hdrNeed) {
                    bodyNeed = (pktType == HCI_PKT_EVENT)
                             ? (ULONG)OutBody[1]
                             : ((ULONG)OutBody[2] | ((ULONG)OutBody[3] << 8));
                    if (hdrNeed + bodyNeed > MaxBody) {
                        return STATUS_BUFFER_OVERFLOW;
                    }
                    if (bodyNeed == 0) {
                        *OutPktType = pktType;
                        *OutBodyLen = pos;
                        return STATUS_SUCCESS;
                    }
                    stage = 2;
                }
            } else {
                OutBody[pos++] = ch;
                if (pos == hdrNeed + bodyNeed) {
                    *OutPktType = pktType;
                    *OutBodyLen = pos;
                    return STATUS_SUCCESS;
                }
            }
        }
    }
}

static NTSTATUS SendPowerPulse(PXUN_BT_CONTEXT Ctx, BOOLEAN On)
{
    UCHAR pulse = On ? QCA_WCN3990_POWERON_PULSE : QCA_WCN3990_POWEROFF_PULSE;
    NTSTATUS st;

    UartFlushRx(Ctx);
    UartSetFlowControl(Ctx, TRUE);
    st = UartWriteSync(Ctx, &pulse, 1, 100);
    UartSetFlowControl(Ctx, FALSE);
    if (!NT_SUCCESS(st)) {
        LogPrint("wcn: power pulse %02x failed: %08x\n", pulse, st);
        return st;
    }
    if (On) {
        SleepMs(100);
    } else {
        SleepMs(5);
    }
    LogPrint("wcn: power pulse %02x (%s) OK, ios=%08x\n",
             pulse, On ? "ON" : "OFF", MmioRead32(&Ctx->Se, SE_GENI_IOS));
    return STATUS_SUCCESS;
}

static NTSTATUS ReadSocVersion(PXUN_BT_CONTEXT Ctx)
{
    static const UCHAR cmd[5] = { HCI_PKT_CMD, 0x00, 0xFC, 0x01, EDL_PATCH_VER_REQ_CMD };
    UCHAR pktType = 0;
    UCHAR evt[270];
    ULONG evtLen = 0;
    NTSTATUS st;
    const QCA_EDL_EVENT_HDR *edl;
    ULONG payloadOff;

    UartFlushRx(Ctx);
    st = UartWriteSync(Ctx, cmd, sizeof(cmd), 200);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = ReadH4PacketSync(Ctx, &pktType, evt, sizeof(evt), &evtLen, 1000);
    if (!NT_SUCCESS(st)) {
        LogPrint("wcn: EDL version timeout/err: %08x (ios=%08x)\n", st, MmioRead32(&Ctx->Se, SE_GENI_IOS));
        return st;
    }

    LogHexDump("wcn: ver evt", evt, evtLen);
    if (pktType != HCI_PKT_EVENT || evtLen < 4) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if (evt[0] == HCI_EV_VENDOR) {
        /* WCN3990 / WCN3950 VSE: [0]=FF [1]=len [2]=cresp(00) [3]=rtype(02) [4..15]=qca_btsoc_version */
        payloadOff = 2;
        if (evtLen < 2 + sizeof(QCA_EDL_EVENT_HDR) + sizeof(QCA_BTSOC_VERSION)) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        edl = (const QCA_EDL_EVENT_HDR *)(evt + payloadOff);
        if (edl->Cresp != EDL_CMD_REQ_RES_EVT || edl->Rtype != EDL_APP_VER_RES_EVT) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        RtlCopyMemory(&Ctx->SocVer, evt + payloadOff + sizeof(QCA_EDL_EVENT_HDR), sizeof(QCA_BTSOC_VERSION));
    } else if (evt[0] == HCI_EV_CMD_COMPLETE && evtLen >= 5 + sizeof(QCA_EDL_EVENT_HDR) + 1 + sizeof(QCA_BTSOC_VERSION)) {
        /* Command Complete variant: [0]=0E [1]=len [2]=ncmd [3..4]=FC00 [5]=00 [6]=19 [7]=status [8..19]=ver */
        payloadOff = 5;
        edl = (const QCA_EDL_EVENT_HDR *)(evt + payloadOff);
        if (edl->Cresp != EDL_CMD_REQ_RES_EVT || edl->Rtype != EDL_PATCH_VER_REQ_CMD) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        RtlCopyMemory(&Ctx->SocVer, evt + payloadOff + sizeof(QCA_EDL_EVENT_HDR) + 1, sizeof(QCA_BTSOC_VERSION));
    } else {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    Ctx->IsWcn3950 = ((Ctx->SocVer.SocId & 0xFFFF0000u) == QCA_WCN3950_SOC_PREFIX);
    LogPrint("wcn: ProductId=%08x SocId=%08x RomVer=%04x PatchVer=%04x (%s)\n",
             Ctx->SocVer.ProductId, Ctx->SocVer.SocId, Ctx->SocVer.RomVer, Ctx->SocVer.PatchVer,
             Ctx->IsWcn3950 ? "WCN3950" : "WCN3990");

    if (Ctx->SocVer.SocId == 0 || Ctx->SocVer.RomVer == 0) {
        return STATUS_DEVICE_DATA_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SwitchOperBaud(PXUN_BT_CONTEXT Ctx)
{
    /* Prefer 3.2 Mbps (exact from 102.4 MHz DFSR), fallback 3.0 Mbps / 921600 */
    static const struct { ULONG Baud; UCHAR QcaCode; } kCandidates[] = {
        { 3000000u, QCA_BAUDRATE_3000000 },
        { 3200000u, QCA_BAUDRATE_3200000 },
        { 921600u,  QCA_BAUDRATE_921600  },
        { 115200u,  QCA_BAUDRATE_115200  },
    };
    ULONG i;
    ULONG chosenBaud = 115200u;
    UCHAR chosenCode = QCA_BAUDRATE_115200;
    NTSTATUS st = STATUS_SUCCESS;

    /* Probe which baud rate can be clocked accurately first (before asking chip to switch!) */
    for (i = 0; i < ARRAYSIZE(kCandidates); i++) {
        /* Test clock availability without switching yet */
        chosenBaud = kCandidates[i].Baud;
        chosenCode = kCandidates[i].QcaCode;
        st = UartSetBaud(Ctx, chosenBaud);
        if (NT_SUCCESS(st)) {
            break;
        }
    }
    /* Restore 115200 momentarily to send SET_BAUDRATE command cleanly */
    if (chosenBaud != 115200u) {
        UartSetBaud(Ctx, 115200u);
    }

    Ctx->OperBaudRate = chosenBaud;
    Ctx->OperBaudCode = chosenCode;
    if (chosenBaud == 115200u) {
        return STATUS_SUCCESS;
    }

    {
        UCHAR cmd[5] = { HCI_PKT_CMD, 0x48, 0xFC, 0x01, chosenCode };
        UCHAR pktType = 0;
        UCHAR evt[64];
        ULONG evtLen = 0;

        UartSetFlowControl(Ctx, TRUE);
        st = UartWriteSync(Ctx, cmd, sizeof(cmd), 200);
        if (!NT_SUCCESS(st)) {
            UartSetFlowControl(Ctx, FALSE);
            return st;
        }
        SleepMs(5);
        st = UartSetBaud(Ctx, chosenBaud);
        UartSetFlowControl(Ctx, FALSE);
        if (!NT_SUCCESS(st)) {
            return st;
        }

        /* WCN3990 sends a 0xFF vendor ack event after baud change; drain up to 150ms */
        st = ReadH4PacketSync(Ctx, &pktType, evt, sizeof(evt), &evtLen, 150);
        if (NT_SUCCESS(st)) {
            LogHexDump("wcn: baud rsp", evt, evtLen);
        } else {
            LogPrint("wcn: no baud VSE within 150ms (%08x), continuing\n", st);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SendTlvSegment(PXUN_BT_CONTEXT Ctx, const UCHAR *Data, ULONG SegSize, BOOLEAN ExpectAck)
{
    UCHAR pkt[4 + 2 + MAX_SIZE_PER_TLV_SEGMENT];
    NTSTATUS st;

    if (SegSize > MAX_SIZE_PER_TLV_SEGMENT) {
        return STATUS_INVALID_PARAMETER;
    }
    pkt[0] = HCI_PKT_CMD;
    pkt[1] = (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu);
    pkt[2] = (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu);
    pkt[3] = (UCHAR)(SegSize + 2);
    pkt[4] = EDL_PATCH_TLV_REQ_CMD;
    pkt[5] = (UCHAR)SegSize;
    RtlCopyMemory(pkt + 6, Data, SegSize);

    st = UartWriteSync(Ctx, pkt, 6 + SegSize, 500);
    if (!NT_SUCCESS(st) || !ExpectAck) {
        return st;
    }

    {
        UCHAR pktType = 0;
        UCHAR evt[128];
        ULONG evtLen = 0;
        st = ReadH4PacketSync(Ctx, &pktType, evt, sizeof(evt), &evtLen, 1000);
        if (!NT_SUCCESS(st)) {
            LogPrint("wcn: segment ack timeout/err: %08x\n", st);
            return st;
        }
        if (pktType == HCI_PKT_EVENT && evtLen >= 4 && evt[0] == HCI_EV_VENDOR) {
            if (evt[2] != EDL_CMD_REQ_RES_EVT || evt[3] != EDL_TLV_DNLD_RES_EVT || (evtLen >= 5 && evt[4] != 0)) {
                LogHexDump("wcn: seg VSE err", evt, evtLen);
                return STATUS_DEVICE_DATA_ERROR;
            }
        }
    }
    return STATUS_SUCCESS;
}

/* Downloads a TLV/NVM payload in 243-byte segments following QCA skip-event mode. */
static NTSTATUS DownloadSegmentedTlv(PXUN_BT_CONTEXT Ctx, const UCHAR *Buf, ULONG Size, UCHAR DnldMode, PCSTR Tag)
{
    ULONG pos = 0;
    ULONG segIdx = 0;

    while (pos < Size) {
        ULONG seg = Size - pos;
        BOOLEAN isLast;
        BOOLEAN expectAck;
        NTSTATUS st;

        if (seg > MAX_SIZE_PER_TLV_SEGMENT) {
            seg = MAX_SIZE_PER_TLV_SEGMENT;
        }
        isLast = (pos + seg == Size);
        /* btqca.c: last segment is always acked regardless of download_mode */
        expectAck = (DnldMode == QCA_SKIP_EVT_NONE) || isLast;

        st = SendTlvSegment(Ctx, Buf + pos, seg, expectAck);
        if (!NT_SUCCESS(st)) {
            LogPrint("wcn: %s seg %u (%u/%u) failed: %08x\n", Tag, segIdx, pos, Size, st);
            return st;
        }
        pos += seg;
        segIdx++;
    }
    LogPrint("wcn: %s downloaded (%u segments, %u bytes)\n", Tag, segIdx, Size);
    return STATUS_SUCCESS;
}

static NTSTATUS PatchAndDownloadNvm(PXUN_BT_CONTEXT Ctx, PUCHAR Buf, ULONG Size)
{
    QCA_TLV_HDR *tlv;
    ULONG typeLen;
    ULONG length;
    UCHAR type;
    ULONG hdrBytes;

    if (Size < sizeof(QCA_TLV_HDR)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    tlv = (QCA_TLV_HDR *)Buf;
    typeLen = tlv->TypeLen;
    type = (UCHAR)(typeLen & 0xFFu);
    length = typeLen >> 8;

    /* Unwrap enclosing type==4 header if present (matches btqca.c) */
    if (type == 4) {
        if (Size < 2 * sizeof(QCA_TLV_HDR)) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        tlv++;
        typeLen = tlv->TypeLen;
        type = (UCHAR)(typeLen & 0xFFu);
        length = typeLen >> 8;
    }

    hdrBytes = (ULONG)((PUCHAR)(tlv + 1) - Buf);
    if (type == 2 && hdrBytes + length <= Size) {
        PUCHAR data = (PUCHAR)(tlv + 1);
        ULONG idx = 0;

        while (idx + sizeof(QCA_TLV_NVM_HDR) <= length) {
            QCA_TLV_NVM_HDR *nvm = (QCA_TLV_NVM_HDR *)(data + idx);
            USHORT tagId = nvm->TagId;
            USHORT tagLen = nvm->TagLen;
            PUCHAR tagData = data + idx + sizeof(QCA_TLV_NVM_HDR);

            if (idx + sizeof(QCA_TLV_NVM_HDR) + tagLen > length) {
                return STATUS_INVALID_IMAGE_FORMAT;
            }
            if (tagId == EDL_TAG_ID_BD_ADDR && tagLen == 6) {
                LogPrint("wcn: NVM BD_ADDR=%02x:%02x:%02x:%02x:%02x:%02x\n",
                         tagData[5], tagData[4], tagData[3], tagData[2], tagData[1], tagData[0]);
            } else if (tagId == EDL_TAG_ID_HCI && tagLen >= 3) {
                UCHAR h0 = tagData[0];
                LogHexDump("wcn: NVM tag17 old", tagData, tagLen);
                if (h0 == 15u) {
                    tagData[0] = 14u;
                    if (idx >= 2) {
                        tagData[-2] &= (UCHAR)~0x80u;
                    }
                } else {
                    tagData[0] &= (UCHAR)~0x80u;
                    if (Ctx->IsWcn3950) {
                        tagData[1] = Ctx->OperBaudCode;
                    } else {
                        tagData[2] = Ctx->OperBaudCode;
                    }
                }
                LogPrint("wcn: patched NVM tag17 data[0..2]=%02x %02x %02x (was0=%02x baud=%u)\n",
                         tagData[0], tagData[1], tagData[2], h0, Ctx->OperBaudRate);
            } else if (tagId == EDL_TAG_ID_DEEP_SLEEP && tagLen >= 1) {
                LogHexDump("wcn: NVM tag27 old", tagData, tagLen);
                RtlZeroMemory(tagData, tagLen);
                LogPrint("wcn: NVM tag27 zeroed (%u bytes)\n", tagLen);
            }
            idx += sizeof(QCA_TLV_NVM_HDR) + tagLen;
        }
    }

    /* NVM always uses QCA_SKIP_EVT_NONE: every segment is acked */
    return DownloadSegmentedTlv(Ctx, Buf, Size, QCA_SKIP_EVT_NONE, "NVM");
}

static NTSTATUS DownloadPatchTlv(PXUN_BT_CONTEXT Ctx, PUCHAR Buf, ULONG Size)
{
    QCA_TLV_HDR *tlv;
    QCA_TLV_PATCH_HDR *patch;
    NTSTATUS st;

    if (Size < sizeof(QCA_TLV_HDR) + sizeof(QCA_TLV_PATCH_HDR)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    tlv = (QCA_TLV_HDR *)Buf;
    patch = (QCA_TLV_PATCH_HDR *)(tlv + 1);
    LogPrint("wcn: TLV dnld_mode=%u total=%u data=%u pid=%04x rom=%04x patch=%04x\n",
             patch->DownloadMode, patch->TotalSize, patch->DataLength,
             patch->ProductId, patch->RomBuild, patch->PatchVersion);

    st = DownloadSegmentedTlv(Ctx, Buf, Size, patch->DownloadMode, "TLV");
    if (NT_SUCCESS(st)) {
        SleepMs(10);
        UartFlushRx(Ctx);
    }
    return st;
}

static VOID SendHciResetBestEffort(PXUN_BT_CONTEXT Ctx)
{
    static const UCHAR cmd[4] = { HCI_PKT_CMD, 0x03, 0x0C, 0x00 };
    UCHAR pktType = 0;
    UCHAR evt[128];
    ULONG evtLen = 0;
    NTSTATUS st;

    st = UartWriteSync(Ctx, cmd, sizeof(cmd), 200);
    if (!NT_SUCCESS(st)) {
        LogPrint("wcn: HCI_Reset TX failed: %08x\n", st);
        return;
    }
    /* Read events up to 1.5s looking for Command Complete 0x0C03 */
    for (;;) {
        st = ReadH4PacketSync(Ctx, &pktType, evt, sizeof(evt), &evtLen, 1500);
        if (!NT_SUCCESS(st)) {
            LogPrint("wcn: HCI_Reset CC wait failed: %08x\n", st);
            return;
        }
        LogHexDump("wcn: reset evt", evt, evtLen);
        if (pktType == HCI_PKT_EVENT && evtLen >= 6 && evt[0] == HCI_EV_CMD_COMPLETE &&
            evt[3] == 0x03 && evt[4] == 0x0C) {
            LogPrint("wcn: HCI_Reset SUCCESS (status=%02x)\n", evt[5]);
            return;
        }
    }
}

NTSTATUS WcnInitializeRadio(PXUN_BT_CONTEXT Ctx)
{
    NTSTATUS st;
    ULONG romHiLo;
    WCHAR tlvName[64];
    WCHAR nvmName[64];
    PUCHAR fwBuf = NULL;
    ULONG fwSize = 0;
    ULONG attempt;

    /* Give XunRpm a moment if it is starting concurrently on boot */
    for (attempt = 0; attempt < 5 && !Ctx->StopRequested; attempt++) {
        SleepMs(100);
    }
    if (Ctx->StopRequested) {
        return STATUS_CANCELLED;
    }

    for (attempt = 0; attempt < 10; attempt++) {
        if (Ctx->StopRequested) {
            return STATUS_CANCELLED;
        }
        /* Clean reset pulse at 2400 baud, then power-on pulse at 115200 baud */
        st = UartSetBaud(Ctx, 2400);
        if (!NT_SUCCESS(st)) {
            LogPrint("wcn: 2400 baud unavailable (%08x), skipping OFF pulse\n", st);
        } else {
            (VOID)SendPowerPulse(Ctx, FALSE);
        }

        st = UartSetBaud(Ctx, 115200);
        if (!NT_SUCCESS(st)) {
            return st;
        }
        st = SendPowerPulse(Ctx, TRUE);
        if (!NT_SUCCESS(st)) {
            return st;
        }

        st = ReadSocVersion(Ctx);
        if (NT_SUCCESS(st)) {
            break;
        }
        if (attempt < 9) {
            LARGE_INTEGER to;
            to.QuadPart = -15000000LL; /* 1.5 s, interruptible on StopEvent */
            LogPrint("wcn: EDL version attempt %d failed (%08x), waiting for LDO rails and retrying\n",
                     attempt + 1, st);
            KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE, &to);
        }
    }
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = SwitchOperBaud(Ctx);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    /* Re-read SoC version at operating baud just like Linux hci_qca / btqca */
    st = ReadSocVersion(Ctx);
    if (!NT_SUCCESS(st)) {
        LogPrint("wcn: post-baud ver read failed (%08x), proceeding anyway\n", st);
    }

    romHiLo = (((ULONG)Ctx->SocVer.RomVer & 0x0F00u) >> 4) | ((ULONG)Ctx->SocVer.RomVer & 0x000Fu);
    if (Ctx->IsWcn3950) {
        PCWSTR suffix = L"";
        if (Ctx->SocVer.SocId == QCA_WCN3950_SOC_ID_T) suffix = L"T";
        else if (Ctx->SocVer.SocId == QCA_WCN3950_SOC_ID_S) suffix = L"S";
        RtlStringCbPrintfW(tlvName, sizeof(tlvName), L"CMBTFW%02X.TLV", romHiLo);
        RtlStringCbPrintfW(nvmName, sizeof(nvmName), L"CMNV%02X%s.BIN", romHiLo, suffix);
    } else {
        RtlStringCbPrintfW(tlvName, sizeof(tlvName), L"CRBTFW%02X.TLV", romHiLo);
        RtlStringCbPrintfW(nvmName, sizeof(nvmName), L"CRNV%02X.BIN", romHiLo);
    }
    LogPrint("wcn: firmware target tlv=%S nvm=%S\n", tlvName, nvmName);

    st = ReadFirmwareFile(tlvName, &fwBuf, &fwSize);
    if (!NT_SUCCESS(st) && !Ctx->IsWcn3950) {
        RtlStringCbPrintfW(tlvName, sizeof(tlvName), L"APBTFW%02X.TLV", romHiLo);
        RtlStringCbPrintfW(nvmName, sizeof(nvmName), L"APNV%02X.BIN", romHiLo);
        st = ReadFirmwareFile(tlvName, &fwBuf, &fwSize);
    }
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = DownloadPatchTlv(Ctx, fwBuf, fwSize);
    ExFreePoolWithTag(fwBuf, XUN_BT_POOL_TAG);
    fwBuf = NULL;
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = ReadFirmwareFile(nvmName, &fwBuf, &fwSize);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = PatchAndDownloadNvm(Ctx, fwBuf, fwSize);
    ExFreePoolWithTag(fwBuf, XUN_BT_POOL_TAG);
    fwBuf = NULL;
    if (!NT_SUCCESS(st)) {
        return st;
    }

    if (Ctx->IsWcn3950) {
        static const UCHAR disLog[6] = { HCI_PKT_CMD, 0x17, 0xFC, 0x02, EDL_DISABLE_LOG_SUB_OP, 0x00 };
        UCHAR rsp[32];
        ULONG rlen = 0;
        (VOID)UartWriteSync(Ctx, disLog, sizeof(disLog), 200);
        SleepMs(20);
        rlen = UartReadPoll(Ctx, rsp, sizeof(rsp));
        if (rlen > 0) {
            LogHexDump("wcn: disLog rsp", rsp, rlen);
        } else {
            LogPrint("wcn: disLog no rsp\n");
        }
        UartFlushRx(Ctx);
    }

    SendHciResetBestEffort(Ctx);
    LogPrint("wcn: post-reset ios=%08x sw_ctrl=%u\n",
             MmioRead32(&Ctx->Se, SE_GENI_IOS),
             MmioRead32(&Ctx->TlmmSwCtrl, TLMM_IO) & 1u);
    Ctx->TxAwake = TRUE;
    Ctx->LastTxActivityMs = GetTicksMs();
    return STATUS_SUCCESS;
}

VOID WcnProbeDeadRadio(PXUN_BT_CONTEXT Ctx)
{
    ULONG savedBaud = Ctx->CurrentBaud;
    NTSTATUS st;

    UartLogHwState(Ctx, "dead-probe");
    st = ReadSocVersion(Ctx);
    LogPrint("probe: ReadSocVersion@%u -> %08x\n", savedBaud, st);
    if (!NT_SUCCESS(st) && savedBaud != 115200) {
        if (NT_SUCCESS(UartSetBaud(Ctx, 115200))) {
            UartSetFlowControl(Ctx, TRUE);
            st = ReadSocVersion(Ctx);
            LogPrint("probe: ReadSocVersion@115200 -> %08x\n", st);
            (VOID)UartSetBaud(Ctx, savedBaud);
            UartSetFlowControl(Ctx, FALSE);
        }
    }
}
