/*
 * SH366101 fuel gauge (QUP0 SE2 @ 0x55) + SC89890H/BQ2589x charger (QUP0 SE1 @ 0x6A) +
 * SC2150/RT1711H Type-C PD (QUP0 SE1 @ 0x4E) policy for Redmi Pad SE (xun).
 */
#include "driver.h"

/* ---- TCPC (SC2150 / RT1711H @ 0x4E on QUP0 SE1) ------------------------- */

static VOID TcpcDump(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    USHORT alert = 0, amask = 0;
    UCHAR tc = 0, role = 0, pc = 0, cc = 0, pwr = 0, flt = 0;

    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ALERT, &alert);
    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ALERT_MASK, &amask);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_TCPC_CTRL, &tc);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, &role);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_POWER_CTRL, &pc);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_CC_STATUS, &cc);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_POWER_STATUS, &pwr);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_FAULT_STATUS, &flt);
    LogPrint("tcpc %s: alert=%04x mask=%04x tc=%02x role=%02x pc=%02x cc=%02x pwr=%02x flt=%02x\n",
             Why, alert, amask, tc, role, pc, cc, pwr, flt);
}

static VOID TcpcInit(PDEVICE_CONTEXT Ctx)
{
    USHORT vid = 0, pid = 0;
    UCHAR role = 0;
    NTSTATUS t, w = STATUS_SUCCESS;

    Ctx->TcpcOk = FALSE;
    Ctx->TcpcIsRt1711h = FALSE;
    t = I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_VID, &vid);
    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_PID, &pid);
    LogPrint("tcpc: vid=%04x pid=%04x (%08x)\n", vid, pid, t);
    if (!NT_SUCCESS(t) || (vid != 0x311C && vid != 0x29CF)) {
        LogPrint("tcpc: not found -> cc detect OFF\n");
        return;
    }

    TcpcDump(Ctx, "before");
    if (vid == 0x311C) {
        /* Southchip SC2150 init from tcpc_sc2150.ko */
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, 0x9E, 0x80);
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, 0xA0, 0x01);
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_COMMAND, TCPC_CMD_WAKE_I2C);
    } else {
        Ctx->TcpcIsRt1711h = TRUE;
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL8, RT1711H_RTCTRL8_INIT);
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL11, RT1711H_RTCTRL11_INIT);
        w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL14, RT1711H_RTCTRL14_INIT);
    }
    {
        UCHAR clr[3] = { TCPC_REG_ALERT, 0xFF, 0xFF };
        w |= GeniI2cWrite(&Ctx->Bus, TCPC_ADDR, clr, sizeof(clr), TRUE);
    }
    KeStallExecutionProcessor(2000);
    TcpcDump(Ctx, "after");
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, &role);
    Ctx->TcpcOk = NT_SUCCESS(w);
    LogPrint("tcpc (%s): init writes %08x role=%02x -> %s\n",
             vid == 0x311C ? "SC2150" : "RT1711H", w, role, Ctx->TcpcOk ? "ok" : "FAILED");
}

/* ---- Type-C role state machine ------------------------------------------- */

#define TC_FALLBACK 0
#define TC_TOGGLING 1
#define TC_SRC      2
#define TC_SNK      3
static const PCSTR g_TcNames[] = { "fallback", "toggling", "source(hub)", "sink(charger)" };

static NTSTATUS ChgSetOtg(PDEVICE_CONTEXT Ctx, BOOLEAN On, PCSTR Why)
{
    UCHAR r03 = 0, want;
    NTSTATUS s = I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03);

    if (!NT_SUCCESS(s)) {
        return s;
    }
    want = On ? (UCHAR)(r03 | CHG_OTG_CONFIG) : (UCHAR)(r03 & ~CHG_OTG_CONFIG);
    if (want != r03) {
        s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, want);
        LogPrint("otg %s (%s): reg03 %02x -> %02x (%08x)\n", On ? "on" : "off", Why, r03, want, s);
    }
    return s;
}

static BOOLEAN TcReadCc(PDEVICE_CONTEXT Ctx, PUCHAR Cc)
{
    return NT_SUCCESS(I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_CC_STATUS, Cc)) ||
           NT_SUCCESS(I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_CC_STATUS, Cc));
}

static VOID TcSetState(PDEVICE_CONTEXT Ctx, ULONG State, PCSTR Why)
{
    LogPrint("type-c: %s -> %s (%s)\n", g_TcNames[Ctx->TcState], g_TcNames[State], Why);
    Ctx->TcState = State;
    Ctx->TcCandidate = 0;
    Ctx->TcDetachSteps = 0;
    Ctx->TcVbusNoCcSteps = 0;
    Ctx->TcChanged = TRUE;
}

static VOID TcStartToggling(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    NTSTATUS a, b;

    ChgSetOtg(Ctx, FALSE, Why);
    PdDetach(Ctx);
    a = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_DRP_TOGGLE);
    b = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_COMMAND, TCPC_CMD_LOOK4CONNECTION);
    if (!NT_SUCCESS(a) || !NT_SUCCESS(b)) {
        LogPrint("type-c: start toggling failed (%08x %08x)\n", a, b);
    }
    TcSetState(Ctx, TC_TOGGLING, Why);
}

static VOID TcFallback(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
    TcSetState(Ctx, TC_FALLBACK, Why);
}

static VOID TcStep(PDEVICE_CONTEXT Ctx)
{
    UCHAR cc = 0, r0b = 0, pwr = 0;
    ULONG c1, c2, vbus;

    if (Ctx->TcState == TC_FALLBACK) {
        return;
    }
    if (!TcReadCc(Ctx, &cc) || !NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0B, &r0b))) {
        return;
    }
    c1 = TCPC_CC1(cc);
    c2 = TCPC_CC2(cc);
    vbus = CHG_VBUS_STAT(r0b);
    if (cc != Ctx->TcLastCc) {
        LogPrint("type-c: [%s] cc_status %02x -> %02x (looking=%u term=%s cc1=%u cc2=%u) vbus_stat=%u\n",
                 g_TcNames[Ctx->TcState], Ctx->TcLastCc, cc, (cc & TCPC_CC_LOOKING) ? 1 : 0,
                 (cc & TCPC_CC_TERM_RD) ? "Rd" : "Rp", c1, c2, vbus);
        Ctx->TcLastCc = cc;
    }

    switch (Ctx->TcState) {
    case TC_TOGGLING: {
        ULONG cand = 0;

        if (!(cc & TCPC_CC_LOOKING)) {
            if (cc & TCPC_CC_TERM_RD) {
                if (c1 != 0 || c2 != 0) {
                    cand = TC_SNK;
                }
            } else if (c1 == TCPC_CC_SRC_RD || c2 == TCPC_CC_SRC_RD) {
                cand = TC_SRC;
            }
            if (cand == 0) {
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_DRP_TOGGLE);
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_COMMAND, TCPC_CMD_LOOK4CONNECTION);
            }
        }
        if (cand != 0 && cand == Ctx->TcCandidate) {
            if (cand == TC_SRC) {
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SRC_RP_RP);
                ChgSetOtg(Ctx, TRUE, "hub attached");
                TcSetState(Ctx, TC_SRC, "partner Rd");
            } else {
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
                ChgSetOtg(Ctx, FALSE, "charger attached");
                TcSetState(Ctx, TC_SNK, "partner Rp");
                PdAttach(Ctx, cc);
            }
            return;
        }
        Ctx->TcCandidate = cand;
        if (cand == 0 && vbus >= 1 && vbus <= 6) {
            if (++Ctx->TcVbusNoCcSteps >= 5) {
                TcFallback(Ctx, "charger VBUS for 5 s but CC never resolved");
            }
        } else {
            Ctx->TcVbusNoCcSteps = 0;
        }
        break;
    }
    case TC_SRC:
        if (c1 != TCPC_CC_SRC_RD && c2 != TCPC_CC_SRC_RD) {
            if (++Ctx->TcDetachSteps >= 2) {
                TcStartToggling(Ctx, "hub removed");
            }
        } else {
            Ctx->TcDetachSteps = 0;
        }
        break;
    case TC_SNK:
        I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_POWER_STATUS, &pwr);
        if ((c1 == 0 && c2 == 0) || (vbus == 0 && !(pwr & TCPC_VBUS_PRESENT))) {
            if (++Ctx->TcDetachSteps >= 2) {
                TcStartToggling(Ctx, "charger removed");
            }
        } else {
            Ctx->TcDetachSteps = 0;
            PdService(Ctx);
        }
        break;
    }
}

NTSTATUS BattHwInit(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG clk1 = 0, clk2 = 0;
    USHORT mv = 0, soc = 0, fcc = 0;
    UCHAR r03 = 0, r0b = 0, r14 = 0;

    XunQupLockInit(&Ctx->LockSe1, XUN_QUP_SE1);
    XunQupLockInit(&Ctx->LockSe2, XUN_QUP_SE2);

    status = TlmmPinMap(&Ctx->PinSda1, TLMM_TILE_WEST, QUP1_PIN_SDA);
    if (NT_SUCCESS(status)) status = TlmmPinMap(&Ctx->PinScl1, TLMM_TILE_WEST, QUP1_PIN_SCL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    TlmmConfig(&Ctx->PinSda1, QUP_I2C_FUNC, TLMM_PULL_NONE, 2, FALSE);
    TlmmConfig(&Ctx->PinScl1, QUP_I2C_FUNC, TLMM_PULL_NONE, 2, FALSE);

    GccEnableQup0Se(GCC_QUP0_SE1_VOTE, GCC_QUP0_S1_CBCR, GCC_QUP0_S1_RCG, &clk1);
    status = GeniI2cInit(&Ctx->Bus, QUP0_SE1_BASE, clk1, &Ctx->LockSe1);
    LogPrint("GeniI2cInit SE1: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (NT_SUCCESS(TlmmPinMap(&Ctx->PinSda2, TLMM_TILE_WEST, QUP2_PIN_SDA)) &&
        NT_SUCCESS(TlmmPinMap(&Ctx->PinScl2, TLMM_TILE_WEST, QUP2_PIN_SCL))) {
        TlmmConfig(&Ctx->PinSda2, QUP_I2C_FUNC, TLMM_PULL_NONE, 2, FALSE);
        TlmmConfig(&Ctx->PinScl2, QUP_I2C_FUNC, TLMM_PULL_NONE, 2, FALSE);
        GccEnableQup0Se(GCC_QUP0_SE2_VOTE, GCC_QUP0_S2_CBCR, GCC_QUP0_S2_RCG, &clk2);
        if (NT_SUCCESS(GeniI2cInit(&Ctx->FgBus, QUP0_SE2_BASE, clk2, &Ctx->LockSe2))) {
            Ctx->FgReady = TRUE;
        }
    }
    if (Ctx->FgReady) {
        NTSTATUS fs = I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_VOLTAGE, &mv);
        I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_SOC, &soc);
        I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_FCC, &fcc);
        LogPrint("sh366101 (SE2@55): volt=%umV soc=%u%% fcc=%umAh (%08x)\n", mv, soc, fcc, fs);
    } else {
        LogPrint("sh366101 (SE2@55): bus init failed, continuing with charger telemetry\n");
    }

    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0B, &r0b);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG14, &r14);
    LogPrint("charger@6A (SE1): reg03=%02x reg0b=%02x reg14=%02x\n", r03, r0b, r14);

    TcpcInit(Ctx);
    Ctx->TcLastCc = 0xFF;
    Ctx->TcState = TC_FALLBACK;
    if (Ctx->TcpcOk) {
        Ctx->TcState = TC_TOGGLING;
    } else {
        TcFallback(Ctx, "tcpc not usable");
    }
    Ctx->LastVbusStat = (ULONG)~0;
    Ctx->JeitaZone = (ULONG)~0;
    Ctx->HwReady = TRUE;
    return STATUS_SUCCESS;
}

VOID BattHwDeinit(PDEVICE_CONTEXT Ctx)
{
    Ctx->HwReady = FALSE;
    Ctx->FgReady = FALSE;
    GeniI2cDeinit(&Ctx->FgBus);
    GeniI2cDeinit(&Ctx->Bus);
    TlmmPinUnmap(&Ctx->PinSda2);
    TlmmPinUnmap(&Ctx->PinScl2);
    TlmmPinUnmap(&Ctx->PinSda1);
    TlmmPinUnmap(&Ctx->PinScl1);
    XunQupLockClose(&Ctx->LockSe2);
    XunQupLockClose(&Ctx->LockSe1);
}

static VOID ChargerDump(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    UCHAR r[CHG_NREGS];
    CHAR prefix[64];
    ULONG i;

    RtlZeroMemory(r, sizeof(r));
    for (i = 0; i < CHG_NREGS; i++) {
        if (i == CHG_REG0C) {
            continue;
        }
        I2cReadByte(&Ctx->Bus, CHG_ADDR, (UCHAR)i, &r[i]);
    }
    RtlStringCbPrintfA(prefix, sizeof(prefix), "charger regs (%s) 00..14:", Why);
    LogHex(prefix, r, sizeof(r));
    LogPrint("charger cfg: iinlim=%umA ichg=%umA vreg=%umV reg02=%02x pn=%u rev=%u\n",
             100 + 50 * (r[CHG_REG00] & 0x3F), 64 * (r[CHG_REG04] & 0x7F),
             3840 + 16 * (r[CHG_REG06] >> 2), r[CHG_REG02], (r[CHG_REG14] >> 3) & 7, r[CHG_REG14] & 3);
}

static VOID ChargerAdc(PDEVICE_CONTEXT Ctx, BOOLEAN Log)
{
    UCHAR r00 = 0, r02 = 0, r04 = 0, r0e = 0, r0f = 0, r11 = 0, r12 = 0, r13 = 0;
    ULONG vbat, vsys, vbus, ichgr;

    if (!NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG02, &r02))) {
        return;
    }
    if (Log && !(r02 & CHG_CONV_START)) {
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG00, &r00);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG04, &r04);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0E, &r0e);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0F, &r0f);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG11, &r11);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG12, &r12);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG13, &r13);
        vbat = 2304 + 20 * (r0e & 0x7F);
        vsys = 2304 + 20 * (r0f & 0x7F);
        vbus = (r11 & 0x7F) ? 2600 + 100 * (r11 & 0x7F) : 0;
        ichgr = 50 * (r12 & 0x7F);
        LogPrint("chg adc: vbus=%umV(gd=%u) vbat=%umV vsys=%umV ichg=%umA iinlim=%umA r04=%02x\n",
                 vbus, r11 >> 7, vbat, vsys, ichgr, 100 + 50 * (r00 & 0x3F), r04);
    }
    if (!(r02 & (CHG_CONV_START | CHG_CONV_RATE))) {
        I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG02, (UCHAR)((r02 | CHG_CONV_START) & ~CHG_FORCE_DPDM));
    }
}

typedef struct _JEITA_ZONE {
    LONG  MinTenths;
    ULONG IchgMa;
    ULONG VregMv;
} JEITA_ZONE;

static const JEITA_ZONE g_Jeita[] = {
    { -1000,    0, 0 },
    {  -100,  490, CHG_VREG_MV },
    {     0,  950, CHG_VREG_MV },
    {    50, CHG_ICHG_MAX_MA, CHG_VREG_MV },
    {   480, CHG_ICHG_HOT_MA, CHG_VREG_HOT_MV },
    {   600,    0, 0 },
};
#define JEITA_ZONES (sizeof(g_Jeita) / sizeof(g_Jeita[0]))

static ULONG JeitaZoneOf(LONG Tenths)
{
    ULONG z = 0;
    while (z + 1 < JEITA_ZONES && Tenths >= g_Jeita[z + 1].MinTenths) {
        z++;
    }
    return z;
}

static ULONG JeitaUpdate(PDEVICE_CONTEXT Ctx, LONG Tenths)
{
    ULONG z = JeitaZoneOf(Tenths), last = Ctx->JeitaZone;

    if (last < JEITA_ZONES && z != last && g_Jeita[z].IchgMa >= g_Jeita[last].IchgMa &&
        (JeitaZoneOf(Tenths - 15) != z || JeitaZoneOf(Tenths + 15) != z)) {
        z = last;
    }
    if (z != last) {
        LogPrint("jeita: %d.%dC zone %d -> %u (ichg %umA vreg %umV)\n", Tenths / 10,
                 (Tenths < 0 ? -Tenths : Tenths) % 10, last < JEITA_ZONES ? (LONG)last : -1, z,
                 g_Jeita[z].IchgMa, g_Jeita[z].VregMv);
        Ctx->JeitaZone = z;
    }
    return z;
}

static ULONG InputLimitMa(PDEVICE_CONTEXT Ctx, UCHAR R0B, UCHAR Cc)
{
    ULONG rp = 0;

    if (Ctx->Pd.State == PD_ST_READY && Ctx->Pd.ContractMa != 0) {
        return min(Ctx->Pd.ContractMa, CHG_IINLIM_MAX_MA);
    }
    if (Cc != 0xFF) {
        rp = max(TCPC_CC1(Cc), TCPC_CC2(Cc));
    }
    if (rp == 3) {
        return CHG_IINLIM_MAX_MA;
    }
    if (rp == 2) {
        return 1500;
    }
    switch (CHG_VBUS_STAT(R0B)) {
    case 2:  return 1500;
    case 3:
    case 4:  return CHG_IINLIM_MAX_MA;
    default: return 0;
    }
}

static VOID ChargerLimits(PDEVICE_CONTEXT Ctx, ULONG Zone, UCHAR R0B, UCHAR Cc)
{
    UCHAR r00 = 0, r04 = 0, r06 = 0, w00, w04, w06;
    ULONG iin = InputLimitMa(Ctx, R0B, Cc);

    if (!NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG00, &r00)) ||
        !NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG04, &r04)) ||
        !NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG06, &r06))) {
        return;
    }
    w04 = r04;
    w06 = r06;
    if (g_Jeita[Zone].IchgMa != 0) {
        w04 = (UCHAR)((r04 & 0x80) | (g_Jeita[Zone].IchgMa / 64));
        w06 = (UCHAR)((r06 & 0x03) | (((g_Jeita[Zone].VregMv - 3840) / 16) << 2));
    }
    w00 = r00;
    if (iin != 0) {
        w00 = (UCHAR)((r00 & 0xC0) | ((iin - 100) / 50));
    }
    if (w00 != r00 || w04 != r04 || w06 != r06) {
        if (w06 != r06) I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG06, w06);
        if (w04 != r04) I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG04, w04);
        if (w00 != r00) I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG00, w00);
        LogPrint("charger limits: zone %u iinlim=%umA ichg=%umA vreg=%umV\n",
                 Zone, 100 + 50 * (w00 & 0x3F), 64 * (w04 & 0x7F), 3840 + 16 * (w06 >> 2));
    }
}

static VOID ChargerPolicy(PDEVICE_CONTEXT Ctx, UCHAR R03, UCHAR R0B, BOOLEAN Allow)
{
    ULONG vbus = CHG_VBUS_STAT(R0B);
    UCHAR want = R03;
    UCHAR chg = Allow ? CHG_CHG_CONFIG : 0;

    if (vbus >= 1 && vbus <= 6) {
        want = (UCHAR)(((R03 & ~CHG_CHG_CONFIG) | chg) & ~CHG_OTG_CONFIG);
    } else if (vbus == 0 && Ctx->TcState == TC_FALLBACK) {
        want = (UCHAR)(R03 | CHG_OTG_CONFIG);
    }
    if (want != R03) {
        NTSTATUS s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, want);
        LogPrint("charger policy: [%s] vbus=%u reg03 %02x -> %02x (%08x)\n",
                 g_TcNames[Ctx->TcState], vbus, R03, want, s);
    }
}

VOID BattPoll(PDEVICE_CONTEXT Ctx)
{
    BATT_SNAPSHOT s;
    USHORT tempK = 0, volt = 0, flags = 0, rm = 0, fcc = 0, curr = 0, cyc = 0, soc = 0;
    NTSTATUS fgErr = STATUS_UNSUCCESSFUL;
    KIRQL irql;
    ULONG state, notifySoc;
    BOOLEAN notify;

    if (!Ctx->HwReady) {
        return;
    }
    RtlZeroMemory(&s, sizeof(s));

    if (Ctx->FgReady) {
        fgErr = STATUS_SUCCESS;
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_TEMP, &tempK);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_VOLTAGE, &volt);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_FLAGS, &flags);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_RM, &rm);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_FCC, &fcc);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_CURRENT, &curr);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_CYCLE, &cyc);
        fgErr |= I2cReadWord(&Ctx->FgBus, FG_ADDR, FG_REG_SOC, &soc);
    }
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &s.ChgReg03);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0B, &s.ChgReg0B);

    s.Valid = NT_SUCCESS(fgErr) && volt >= 2500 && volt <= 5000;
    s.Present = TRUE;
    s.SocTenths = min(soc, 100u) * 10;
    s.VoltageMv = volt;
    s.CurrentMa = (LONG)(SHORT)curr;
    s.TempTenthsC = (LONG)tempK - 2731;
    s.FullMah = (fcc >= 1000 && fcc <= 12000) ? fcc : BATT_DESIGN_MAH;
    s.Cycles = cyc;

    s.OnLine = CHG_VBUS_STAT(s.ChgReg0B) >= 1 && CHG_VBUS_STAT(s.ChgReg0B) <= 6 && CHG_PG_STAT(s.ChgReg0B);
    s.Charging = s.OnLine && (CHG_CHRG_STAT(s.ChgReg0B) == 1 || CHG_CHRG_STAT(s.ChgReg0B) == 2);
    s.ChargeDone = s.OnLine && CHG_CHRG_STAT(s.ChgReg0B) == 3;

    if (CHG_VBUS_STAT(s.ChgReg0B) != Ctx->LastVbusStat) {
        CHAR why[32];
        RtlStringCbPrintfA(why, sizeof(why), "vbus_stat %d->%u",
                           Ctx->LastVbusStat == (ULONG)~0 ? -1 : (LONG)Ctx->LastVbusStat, CHG_VBUS_STAT(s.ChgReg0B));
        ChargerDump(Ctx, why);
        Ctx->LastVbusStat = CHG_VBUS_STAT(s.ChgReg0B);
    }

    {
        ULONG zone = s.Valid ? JeitaUpdate(Ctx, s.TempTenthsC) : Ctx->JeitaZone;
        BOOLEAN allow = (zone >= JEITA_ZONES || g_Jeita[zone].IchgMa != 0);

        ChargerPolicy(Ctx, s.ChgReg03, s.ChgReg0B, allow);
        if (zone < JEITA_ZONES && s.OnLine) {
            ChargerLimits(Ctx, zone, s.ChgReg0B, Ctx->TcState == TC_SNK ? Ctx->TcLastCc : 0xFF);
        }
    }

    state = (s.OnLine ? 1 : 0) | (s.Charging ? 2 : 0) | (s.ChargeDone ? 4 : 0);
    notifySoc = s.SocTenths / 10;

    KeAcquireSpinLock(&Ctx->SnapLock, &irql);
    Ctx->Snap = s;
    KeReleaseSpinLock(&Ctx->SnapLock, irql);

    notify = state != Ctx->LastNotifiedState || notifySoc != Ctx->LastNotifiedSoc;
    ChargerAdc(Ctx, s.OnLine || (Ctx->Polls % 12) == 0);
    if ((Ctx->Polls++ % 12) == 0 || notify) {
        LogPrint("poll: fg=%08x soc=%u%% v=%umV i=%dmA t=%d.%dC rm=%u fcc=%umAh cyc=%u flags=%04x | chg r03=%02x r0b=%02x online=%u tc=%s pd=%s\n",
                 fgErr, soc, s.VoltageMv, s.CurrentMa, s.TempTenthsC / 10,
                 (s.TempTenthsC < 0 ? -s.TempTenthsC : s.TempTenthsC) % 10, rm, s.FullMah, s.Cycles,
                 flags, s.ChgReg03, s.ChgReg0B, s.OnLine, g_TcNames[Ctx->TcState], PdStateName(Ctx));
    }
    if (notify) {
        Ctx->LastNotifiedState = state;
        Ctx->LastNotifiedSoc = notifySoc;
        if (Ctx->ClassHandle != NULL) {
            BatteryClassStatusNotify(Ctx->ClassHandle);
        }
    }
}

VOID BattOtgOff(PDEVICE_CONTEXT Ctx)
{
    UCHAR r03 = 0;
    NTSTATUS s;

    if (!Ctx->HwReady) {
        return;
    }
    s = I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03);
    if (NT_SUCCESS(s) && (r03 & CHG_OTG_CONFIG)) {
        s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, (UCHAR)(r03 & ~CHG_OTG_CONFIG));
    }
    LogPrint("OTG off on D0 exit: reg03=%02x (%08x)\n", r03, s);
}

VOID BattTcSinkOnly(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS s;

    if (!Ctx->HwReady || !Ctx->TcpcOk) {
        return;
    }
    PdDetach(Ctx);
    s = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
    LogPrint("type-c: [%s] -> Rd/Rd on D0 exit (%08x)\n", g_TcNames[Ctx->TcState], s);
}

static KSTART_ROUTINE BattThread;

static VOID BattThread(PVOID Context)
{
    PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;
    ULONG tick = 0;

    period.QuadPart = -10000LL * 1000;
    LogPrint("thread started\n");
    while (KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        if (Ctx->HwReady) {
            TcStep(Ctx);
        }
        if (++tick % 5 == 0 || Ctx->TcChanged) {
            Ctx->TcChanged = FALSE;
            BattPoll(Ctx);
        }
    }
    LogPrint("thread exit (polls=%u)\n", Ctx->Polls);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS BattThreadStart(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    if (Ctx->HwReady && Ctx->TcState != TC_FALLBACK) {
        TcStartToggling(Ctx, "D0 entry");
    }
    KeInitializeEvent(&Ctx->StopEvent, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&Ctx->ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, BattThread, Ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(Ctx->ThreadHandle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                       (PVOID *)&Ctx->Thread, NULL);
    ZwClose(Ctx->ThreadHandle);
    Ctx->ThreadHandle = NULL;
    return status;
}

VOID BattThreadStop(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Thread != NULL) {
        KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }
}
