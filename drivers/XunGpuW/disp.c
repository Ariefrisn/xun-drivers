/*
 * Step C2 (docs/P8_gpu.md): the display side of XunGpuW on the ACPI device GPU0 (TPZG0610).
 * Dxgkrnl treats an ACPI-enumerated display adapter as the platform GPU that owns the internal
 * panel, so this adapter has 1 VidPN source and 1 target (the DSI panel, 1200x1920@90Hz XRGB8888).
 *
 * Scanout: the bootloader's MDP VIG0 pipeline feeds the UEFI GOP framebuffer (0x5C000000,
 * 1200x1920 XRGB8888); DirectSelect programs SSPP_SRC0_ADDR + SSPP_SRC_FORMAT (H+V 180-deg flip
 * bits 0xC000) directly for zero-copy BO scanout, falling back to SW 180-deg copy on non-BO
 * allocations (C:\xun\disp.noflip disables the 180-deg panel flip). Brightness uses KTZ8866 @ 0x11
 * on QUP0 SE1 + HWEN GPIO 102 SOUTH (shared with xun_qup_lock.h), with C:\xun\bl.dcs fallback.
 */
#include "tgpu.h"
#include "xun_ktz8866.h"

/* GUID_DEVINTERFACE_BRIGHTNESS (ntddvdeo.h, already included by dispmprt.h without initguid) */
static const GUID g_BrightnessGuid = { 0xFDE5BBA4, 0xB3F9, 0x46FB, { 0xBD, 0xAA, 0x07, 0x28, 0xCE, 0x31, 0x00, 0xB4 } };



/* panel timing (stock dtbo_b.img, m84_42_03_0c): h fp/pw/bp 50/20/36, v fp/pw/bp 410/6/10, 90 Hz */
#define H_ACTIVE    1200
#define V_ACTIVE    1920
#define H_TOTAL     (1200 + 50 + 20 + 36)
#define V_TOTAL     (1920 + 410 + 6 + 10)

static ULONG g_Refresh = TGPU_REFRESH_DEFAULT;

ULONG DispRefresh(VOID)
{
    return g_Refresh;
}

static PDXGKRNL_INTERFACE g_Dxgk;
static DXGK_DISPLAY_INFORMATION g_Post;
static PUCHAR g_Fb;                             /* WC mapping of the framebuffer */
static BOOLEAN g_Visible = TRUE;
static BOOLEAN g_Committed;
static XUN_QUP_LOCK g_BlQupLock;
static XUN_KTZ_BUS g_Ktz;

static TGPU_ALLOCATION * volatile g_ScanCur;    /* primary being "scanned out" */

/* direct scanout: the bootloader's MDP path is VIG0 -> LM0 -> CTL0 -> INTF1 -> DSI0, fetching the
   GOP framebuffer 0x5C000000 (stride 4800); the MDP SMMU context (SID 0x420 -> CB3) has translation off,
   so VIG0 can fetch any physical address. Flipping to a BO-backed primary = new SRC0_ADDR/YSTRIDE0 +
   SRC_OP_MODE (0x6000 = 180-deg panel mount flip) + CTL flush (double-buffered, latched at vsync).
   C:\xun\gpuw.nodirect disables direct scanout; C:\xun\disp.noflip disables the 180-deg flip. */
#define MDP_VIG0            0x5000
#define SSPP_SRC0_ADDR      0x14
#define SSPP_SRC_YSTRIDE0   0x24
#define SSPP_SRC_FORMAT     0x30
#define SSPP_SRC_OP_MODE    0x38
#define MDP_FLIP_LR         (1u << 13)
#define MDP_FLIP_UD         (1u << 14)
#define MDP_FLIP_180        (MDP_FLIP_LR | MDP_FLIP_UD)
#define MDP_CTL0            0x2000
#define CTL_FLUSH           0x18
#define CTL_FLUSH_VIG0      (1u << 0)
#define CTL_FLUSH_CTL       (1u << 17)
static volatile UCHAR *g_Mdp;
static BOOLEAN g_DirectOk;                      /* mapped and not disabled */
static BOOLEAN g_DispFlip = TRUE;               /* 180-deg physical mount compensation */
static volatile LONG g_DirectActive;            /* VIG0 currently fetches a BO */
static ULONG g_FbAddr, g_FbStride, g_FbFormat, g_FbOpMode; /* bootloader values, restored on fallback/stop */
static ULONG g_DirectFlips;

static VOID MdpScanFrom(ULONG Pa, ULONG Stride, BOOLEAN Flip)
{
    ULONG op = g_DispFlip ? ((g_FbOpMode & ~MDP_FLIP_180) | (Flip ? MDP_FLIP_180 : 0)) : g_FbOpMode;
    WRITE_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC0_ADDR), Pa);
    WRITE_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC_YSTRIDE0), Stride);
    WRITE_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC_FORMAT), g_FbFormat);
    WRITE_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC_OP_MODE), op);
    WRITE_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_CTL0 + CTL_FLUSH), CTL_FLUSH_VIG0 | CTL_FLUSH_CTL);
}

/* point VIG0 at Al when it is a BO-backed full-screen primary; TRUE = no CPU copy needed. Any IRQL. */
static BOOLEAN DirectSelect(TGPU_ALLOCATION *Al)
{
    if (g_DirectOk && Al != NULL && Al->Bo != NULL && Al->Desc.width == g_Post.Width &&
        Al->Desc.height == g_Post.Height && Al->Bo->Iova + Al->Desc.bo_offset < 0x100000000ULL) {
        MdpScanFrom((ULONG)(Al->Bo->Iova + Al->Desc.bo_offset), Al->Desc.pitch, g_DispFlip);
        InterlockedExchange(&g_DirectActive, 1);
        g_DirectFlips++;
        return TRUE;
    }
    if (g_DirectOk && InterlockedExchange(&g_DirectActive, 0)) {
        MdpScanFrom(g_FbAddr, g_FbStride, FALSE);
    }
    return FALSE;
}
static PHYSICAL_ADDRESS g_ScanPa;               /* its address as Dxgkrnl knows it (last flip) */
/* v0.45 async present: the UMD no longer waits for the GPU before presenting; a direct flip to a BO whose last
   submit is still running is programmed into the MDP only when that fence completed (1 ms timer) */
static TGPU_ALLOCATION *volatile g_DeferAl;
static volatile ULONG g_DeferFence;
static volatile LONG g_Deferred;
static ULONG g_DeferCount;
static PHYSICAL_ADDRESS g_ShownPa;              /* v0.40: address reported in CRTC_VSYNC = really on screen */
static ULONG g_FlipWaits;
static volatile LONG g_ScanDirty;
static volatile LONG g_FlipPending;
static volatile LONG g_VsyncOn;
static KMUTEX g_ScanLock;                       /* engine copies & aperture maps vs. DestroyAllocation */
static volatile LONG g_ScanLockInit;

static volatile LONG g_FlipCount;              /* v0.39: flips per second in the log */
static KTIMER g_VsyncTimer;
static KDPC g_VsyncDpc;
static BOOLEAN g_TimerOn;
static volatile LONG64 g_LastVsync;             /* KeQueryInterruptTime of the last vsync */
static ULONG g_VsyncCount;

/* ---------------------------------------------------------------- vsync */

static BOOLEAN VsyncSync(PVOID Ctx)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;

    UNREFERENCED_PARAMETER(Ctx);
    RtlZeroMemory(&d, sizeof(d));
    d.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    d.CrtcVsync.VidPnTargetId = 0;
    d.CrtcVsync.PhysicalAddress = g_ShownPa;
    g_Dxgk->DxgkCbNotifyInterrupt(g_Dxgk->DeviceHandle, &d);
    g_Dxgk->DxgkCbQueueDpc(g_Dxgk->DeviceHandle);
    return TRUE;
}

/* v0.41: vsync work. Called from a 1 ms high-resolution timer: a flip is reported done right after the MDP
   latched it (CTL0_FLUSH VIG0 bit cleared = real panel vsync just passed) and the 60 Hz vsync phase is
   re-aligned to that moment. Before, a 16 ms KTIMER (actually ~15.6 ms clock ticks, not synced to the panel)
   checked the latch once per period, so a flip often took two periods and DWM settled at ~30 FPS. */
static PEX_TIMER g_HrTimer;
static LONG64 g_NextVsync;                       /* interrupt time (100 ns) of the next 60 Hz tick */
#define VSYNC_PERIOD (10000000LL / g_Refresh)

/* v0.45.1: a WPR trace showed DxgKrnl getting only 31..40 vsyncs/s (intervals 30..35 ms): the "1 ms" timer
   callback arrived every ~15.6 ms, so `now >= g_NextVsync` held on every second callback only. Measure the
   real callback interval (logged with the fps line) and accept a tick up to half an interval early. */
static LONG64 g_CbLast, g_CbAvg;                 /* 100 ns; g_CbAvg = moving average (1/8) */
static LONG64 g_CbMin, g_CbMax;
static ULONG g_Ticks;                            /* vsyncs reported in the current second */
static BOOLEAN g_TimerRes;
static ULONG g_Shown;                            /* vsyncs with a new picture in the current second */
/* v0.45.3: with the mouse DWM flipped 99..166 times/s at 60 vsyncs/s and most flips never reached the panel: a flip
   was reported done as soon as the CTL flush bit read clear, which can happen sooner than one panel period after
   the previous one. Complete at most one flip per vsync period so DWM is paced to 60 Hz. */
static LONG64 g_LastFlipDone;
static BOOLEAN g_HeldThis;
/* v0.45.5: the stats window was 60 *ticks*, but a completed flip re-locks the tick phase, so while DWM flips ticks
   are rare and "60 vsyncs/s" was true by construction. Use a 1 s wall-clock window and count what DxgKrnl gets. */
static LONG64 g_StatStart;
static ULONG g_Notifies, g_Done;
static volatile LONG g_FlipOverrun, g_FlipImm;   /* v0.45.4: flips replacing a pending one / FlipImmediate flips */
static ULONG g_FlipHeld;                         /* flips held back for pacing in the current second */

static VOID VsyncNotify(BOOLEAN flip)
{
    BOOLEAN ret;
    if (g_Dxgk != NULL && (g_VsyncOn || flip)) {
        g_Dxgk->DxgkCbSynchronizeExecution(g_Dxgk->DeviceHandle, VsyncSync, NULL, 0, &ret);
        if (++g_VsyncCount <= 3 || (flip && g_VsyncCount < 50)) {
            LogPrint("vsync %u%s pa %llx\n", g_VsyncCount, flip ? " (flip done)" : "", g_ScanPa.QuadPart);
        }
    }
}

static VOID VsyncWork(BOOLEAN periodicTick)
{
    LONG64 now = (LONG64)KeQueryInterruptTime();
    BOOLEAN flip = FALSE, tick = FALSE;
    LONG64 slack = 0;

    if (!periodicTick) {
        if (g_CbLast != 0) {
            LONG64 d = now - g_CbLast;
            g_CbAvg = g_CbAvg == 0 ? d : g_CbAvg + (d - g_CbAvg) / 8;
            if (g_CbMin == 0 || d < g_CbMin) {
                g_CbMin = d;
            }
            if (d > g_CbMax) {
                g_CbMax = d;
            }
        }
        g_CbLast = now;
        slack = g_CbAvg / 2;
        if (slack > VSYNC_PERIOD / 2) {
            slack = VSYNC_PERIOD / 2;
        }
    }

    if (g_Deferred) {
        if ((LONG)(HwCompletedFence() - g_DeferFence) >= 0 && g_DeferAl == g_ScanCur) {
            InterlockedExchange(&g_Deferred, 0);
            if (!DirectSelect(g_DeferAl)) {
                InterlockedExchange(&g_ScanDirty, 1);
            }
        }
    }
    if (g_FlipPending && !g_Deferred) {
        if (g_DirectActive && g_Mdp != NULL &&
            (READ_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_CTL0 + CTL_FLUSH)) & CTL_FLUSH_VIG0)) {
            if (periodicTick || now >= g_NextVsync) {
                g_FlipWaits++;
            }
        } else if (now - g_LastFlipDone < VSYNC_PERIOD - VSYNC_PERIOD / 8) {
            if (!g_HeldThis) {                   /* latched, but the previous flip completed less than a period ago */
                g_HeldThis = TRUE;
                g_FlipHeld++;
            }
        } else {
            InterlockedExchange(&g_FlipPending, 0);
            if (g_ShownPa.QuadPart != g_ScanPa.QuadPart) {
                g_Shown++;
            }
            g_Done++;
            g_ShownPa = g_ScanPa;
            g_LastFlipDone = now;
            flip = TRUE;
            g_HeldThis = FALSE;
            if (g_DirectActive) {
                g_NextVsync = now + VSYNC_PERIOD;        /* the latch happened at the panel vsync: lock phase */
            }
        }
    }
    if (periodicTick || now + slack >= g_NextVsync) {
        tick = TRUE;
        g_Ticks++;
        if (!periodicTick) {
            g_NextVsync += VSYNC_PERIOD;
            if (g_NextVsync <= now) {
                g_NextVsync = now + VSYNC_PERIOD;
            }
        }
        g_LastVsync = now;
        if (InterlockedExchange(&g_ScanDirty, 0)) {
            EngKickScanout();
        }
    }
    if (flip || tick) {
        g_Notifies++;
        VsyncNotify(flip);
    }
    if (g_StatStart == 0) {
        g_StatStart = now;
    } else if (now - g_StatStart >= 10000000LL) {
        LONG f = InterlockedExchange(&g_FlipCount, 0);
        if (f != 0) {
            LogPrint("fps: %u shown/s, %ld flips/s, %u done, %u held, %ld overrun, %ld immediate | vsync notifies %u "
                     "(%u timer ticks) in %lld ms | timer cb %lld..%lld avg %lld us (%s, %u waits for the MDP latch, "
                     "%u deferred to the GPU fence)\n", g_Shown, f, g_Done, g_FlipHeld,
                     InterlockedExchange(&g_FlipOverrun, 0), InterlockedExchange(&g_FlipImm, 0), g_Notifies, g_Ticks,
                     (now - g_StatStart) / 10000, g_CbMin / 10, g_CbMax / 10, g_CbAvg / 10,
                     g_DirectActive ? "direct" : "copy", g_FlipWaits, g_DeferCount);
        }
        g_StatStart = now;
        g_Ticks = 0;
        g_Shown = 0;
        g_Done = 0;
        g_Notifies = 0;
        g_FlipHeld = 0;
        g_CbMin = 0;
        g_CbMax = 0;
    }
}

static KDEFERRED_ROUTINE VsyncDpc;
static VOID VsyncDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(A1);
    UNREFERENCED_PARAMETER(A2);
    VsyncWork(TRUE);                             /* fallback KTIMER path: one call per ~16 ms */
}

static EXT_CALLBACK HrTimerCb;
static VOID HrTimerCb(PEX_TIMER Timer, PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Timer);
    UNREFERENCED_PARAMETER(Ctx);
    VsyncWork(FALSE);
}

VOID DispVsyncEnable(BOOLEAN Enable)
{
    InterlockedExchange(&g_VsyncOn, Enable ? 1 : 0);
}

NTSTATUS DispGetScanLine(DXGKARG_GETSCANLINE *A)
{
    LONG64 since = (LONG64)KeQueryInterruptTime() - g_LastVsync;     /* 100 ns */
    LONG64 frame = 10000000LL / g_Refresh;
    ULONG line;

    if (since < 0 || since >= frame) {
        since = 0;
    }
    line = (ULONG)(since * V_TOTAL / frame);
    A->InVerticalBlank = line >= V_ACTIVE;
    A->ScanLine = line;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- scanout */

VOID DispInitLocks(VOID)
{
    if (InterlockedCompareExchange(&g_ScanLockInit, 1, 0) == 0) {
        KeInitializeMutex(&g_ScanLock, 0);
    }
}

VOID DispScanLock(VOID)
{
    DispInitLocks();
    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
}

VOID DispScanUnlock(VOID)
{
    KeReleaseMutex(&g_ScanLock, FALSE);
}

static SIZE_T AllocByteSize(const TGPU_ALLOCATION *Al)
{
    if (Al == NULL) {
        return 0;
    }
    if (Al->Bo != NULL) {
        return (Al->Bo->Size > Al->Desc.bo_offset) ? (Al->Bo->Size - Al->Desc.bo_offset) : 0;
    }
    return Al->ApBytes;
}

static VOID CopyRectToFb(PUCHAR Src, SIZE_T MaxBytes, ULONG SrcPitch, ULONG SrcW, ULONG SrcH, const RECT *R)
{
    LONG l = R->left, t = R->top, r = R->right, b = R->bottom, y;
    ULONG w = g_Post.Width, h = g_Post.Height;
    SIZE_T rowBytes;

    if (Src == NULL || g_Fb == NULL || SrcPitch < 4 || SrcW == 0 || SrcH == 0 || MaxBytes < 4) {
        return;
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)SrcW) r = SrcW;
    if (b > (LONG)SrcH) b = SrcH;
    if (r > (LONG)w) r = w;
    if (b > (LONG)h) b = h;
    if ((SIZE_T)r * 4 > SrcPitch) {
        r = (LONG)(SrcPitch / 4);
    }
    if (r <= l || b <= t) {
        return;
    }
    rowBytes = (SIZE_T)r * 4;
    if (rowBytes > MaxBytes) {
        return;
    }
    {
        LONG maxB = (LONG)((MaxBytes - rowBytes) / SrcPitch) + 1;
        if (b > maxB) {
            b = maxB;
        }
    }
    if (b <= t) {
        return;
    }
    if (!g_DispFlip) {
        for (y = t; y < b; y++) {
            RtlCopyMemory(g_Fb + (SIZE_T)y * g_Post.Pitch + (SIZE_T)l * 4, Src + (SIZE_T)y * SrcPitch + (SIZE_T)l * 4,
                          (SIZE_T)(r - l) * 4);
        }
        return;
    }
    for (y = t; y < b; y++) {
        const ULONG *sp = (const ULONG *)(Src + (SIZE_T)y * SrcPitch);
        volatile ULONG *dp = (volatile ULONG *)(g_Fb + (SIZE_T)(h - 1 - (ULONG)y) * g_Post.Pitch);
        LONG x;
        for (x = l; x < r; x++) {
            dp[w - 1 - (ULONG)x] = sp[x];
        }
    }
}

VOID DispScanoutWork(VOID)
{
    TGPU_ALLOCATION *al;
    PUCHAR px;
    RECT all;
    static ULONG count;

    DispScanLock();
    al = g_ScanCur;
    if (al != NULL && !g_DirectActive && g_Fb != NULL && g_Visible && (px = AllocPixels(al)) != NULL) {
        all.left = all.top = 0;
        all.right = al->Desc.width;
        all.bottom = al->Desc.height;
        CopyRectToFb(px, AllocByteSize(al), al->Desc.pitch, al->Desc.width, al->Desc.height, &all);
        if (++count <= 5) {
            LogPrint("scanout: %ux%u pitch %u from %p (%s)\n", al->Desc.width, al->Desc.height, al->Desc.pitch, al,
                     al->Bo != NULL ? "bo" : "vidmm");
        }
    }
    DispScanUnlock();
}

VOID DispPresentRects(TGPU_ALLOCATION *Dst, const RECT *Rects, ULONG Count)
{
    PUCHAR px;
    SIZE_T maxBytes;
    ULONG i;

    if (Dst != g_ScanCur || g_DirectActive || g_Fb == NULL || !g_Visible) {
        return;
    }
    DispScanLock();
    if (Dst == g_ScanCur && (px = AllocPixels(Dst)) != NULL) {
        maxBytes = AllocByteSize(Dst);
        for (i = 0; i < Count; i++) {
            CopyRectToFb(px, maxBytes, Dst->Desc.pitch, Dst->Desc.width, Dst->Desc.height, &Rects[i]);
        }
    }
    DispScanUnlock();
}

VOID DispAllocDestroyed(TGPU_ALLOCATION *Al)
{
    DispScanLock();
    if (g_DeferAl == Al) {                               /* never flip to a freed BO later */
        InterlockedExchange(&g_Deferred, 0);
        g_DeferAl = NULL;
    }
    if (g_ScanCur == Al) {
        DirectSelect(NULL);                              /* never fetch from a freed BO */
        g_ScanCur = NULL;
        LogPrint("scanout: primary %p destroyed while scanned out\n", Al);
    }
    DispScanUnlock();
}

VOID DispApertureWillUnmap(TGPU_ALLOCATION *Al)
{
    DispScanLock();
    if (Al != NULL && g_ScanCur == Al) {
        DirectSelect(NULL);
        g_ScanCur = NULL;
    }
    AllocUnmapAperture(Al);
    DispScanUnlock();
}

/* may run at DIRQL (MMIO-style flip): only plain stores, the vsync DPC does the rest */
NTSTATUS DispSetSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *A)
{
    static ULONG count;

    BOOLEAN direct;

    if (g_FlipPending) {
        InterlockedIncrement(&g_FlipOverrun);
    }
    if (A->Flags.FlipImmediate) {
        InterlockedIncrement(&g_FlipImm);
    }
    g_ScanCur = (TGPU_ALLOCATION *)A->hAllocation;
    g_ScanPa = A->PrimaryAddress;
    InterlockedIncrement(&g_FlipCount);
    if (g_DirectOk && g_ScanCur != NULL && g_ScanCur->Bo != NULL &&
        (LONG)(HwCompletedFence() - g_ScanCur->Bo->LastFence) < 0) {
        g_DeferAl = g_ScanCur;                           /* GPU still rendering it: flip at fence completion */
        g_DeferFence = g_ScanCur->Bo->LastFence;
        InterlockedExchange(&g_Deferred, 1);
        InterlockedExchange(&g_FlipPending, 1);
        g_DeferCount++;
        return STATUS_SUCCESS;
    }
    InterlockedExchange(&g_Deferred, 0);
    direct = DirectSelect(g_ScanCur);
    if (!direct) {
        InterlockedExchange(&g_ScanDirty, 1);
    }
    InterlockedExchange(&g_FlipPending, 1);
    if (++count <= 60) {
        LogPrint("SetVidPnSourceAddress: src %u alloc %p seg %u pa %llx flags %x%s\n", A->VidPnSourceId, A->hAllocation,
                 A->PrimarySegment, A->PrimaryAddress.QuadPart, A->Flags.Value, direct ? " (direct scanout)" : "");
    }
    return STATUS_SUCCESS;
}

static VOID BlPanel(BOOLEAN On);
static BOOLEAN DispFileExists(PCWSTR Path);

NTSTATUS DispSetVisibility(const DXGKARG_SETVIDPNSOURCEVISIBILITY *A)
{
    BOOLEAN visible = A->Visible ? TRUE : FALSE;

    LogPrint("SetVidPnSourceVisibility: src %u visible %u\n", A->VidPnSourceId, A->Visible);
    if (visible != g_Visible) {
        if (!visible && DispFileExists(L"\\??\\C:\\xun\\bl.always")) {
            LogPrint("%s\n", "SetVidPnSourceVisibility: bl.always present -> backlight kept ON");
        } else {
            BlPanel(visible);                                /* v0.45.6: screen off = backlight off */
        }
    }
    g_Visible = visible;
    if (g_Visible) {
        InterlockedExchange(&g_ScanDirty, 1);
    }
    return STATUS_SUCCESS;
}

VOID DispSystemWrite(PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y)
{
    UINT y;

    if (g_Fb == NULL) {
        return;
    }
    if (g_DirectOk && g_DirectActive) {                  /* bugcheck screen: back to the framebuffer */
        g_DirectActive = 0;
        MdpScanFrom(g_FbAddr, g_FbStride, FALSE);
    }
    for (y = 0; y < H && Y + y < g_Post.Height; y++) {
        UINT w = (X + W > g_Post.Width) ? (X < g_Post.Width ? g_Post.Width - X : 0) : W;
        if (!g_DispFlip) {
            RtlCopyMemory(g_Fb + (SIZE_T)(Y + y) * g_Post.Pitch + (SIZE_T)X * 4, (PUCHAR)Src + (SIZE_T)y * Stride,
                          (SIZE_T)w * 4);
        } else {
            const ULONG *sp = (const ULONG *)((PUCHAR)Src + (SIZE_T)y * Stride);
            volatile ULONG *dp = (volatile ULONG *)(g_Fb + (SIZE_T)(g_Post.Height - 1 - (Y + y)) * g_Post.Pitch);
            UINT x;
            for (x = 0; x < w; x++) {
                dp[g_Post.Width - 1 - (X + x)] = sp[x];
            }
        }
    }
}

/* ---------------------------------------------------------------- start / stop */

static VOID InitBrightness(VOID);
static BOOLEAN DispFileExists(PCWSTR Path)
{
    UNICODE_STRING nd;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&nd, Path);
    InitializeObjectAttributes(&oa, &nd, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

NTSTATUS DispStart(PDXGKRNL_INTERFACE Dxgk)
{
    NTSTATUS st;
    LARGE_INTEGER due;

    g_Dxgk = Dxgk;
    DispInitLocks();
    RtlZeroMemory(&g_Post, sizeof(g_Post));
    st = Dxgk->DxgkCbAcquirePostDisplayOwnership(Dxgk->DeviceHandle, &g_Post);
    LogPrint("AcquirePostDisplayOwnership: %08x %ux%u pitch %u fmt %u pa %llx target %u acpi %u\n", st, g_Post.Width,
             g_Post.Height, g_Post.Pitch, g_Post.ColorFormat, g_Post.PhysicAddress.QuadPart, g_Post.TargetId, g_Post.AcpiId);
    if (!NT_SUCCESS(st) || g_Post.Width == 0 || g_Post.PhysicAddress.QuadPart == 0) {
        /* same fallback as XunDisplay: the GOP framebuffer the bootloader left running */
        g_Post.Width = TGPU_FB_WIDTH;
        g_Post.Height = TGPU_FB_HEIGHT;
        g_Post.Pitch = TGPU_FB_PITCH;
        g_Post.ColorFormat = D3DDDIFMT_X8R8G8B8;
        g_Post.PhysicAddress.QuadPart = (LONGLONG)TGPU_FB_PA;
        g_Post.TargetId = 0;
        LogPrint("POST info empty: using the fixed framebuffer %llx\n", TGPU_FB_PA);
    }
    g_DispFlip = !DispFileExists(L"\\??\\C:\\xun\\disp.noflip");
    LogPrint("disp panel flip: %s\n", g_DispFlip ? "180 deg" : "disabled (disp.noflip)");
    g_Refresh = DispFileExists(L"\\??\\C:\\xun\\disp.60hz") ? 60 : TGPU_REFRESH_DEFAULT;
    LogPrint("disp refresh: %u Hz%s\n", g_Refresh, g_Refresh == 60 ? " (disp.60hz)" : "");
    if (g_Fb == NULL) {
        g_Fb = (PUCHAR)MmMapIoSpaceEx(g_Post.PhysicAddress, (SIZE_T)g_Post.Pitch * g_Post.Height,
                                      PAGE_READWRITE | PAGE_WRITECOMBINE);
    }
    LogPrint("framebuffer mapped at %p\n", g_Fb);
    InitBrightness();
    DispSurveyMdp();
    if (g_Mdp == NULL) {
        PHYSICAL_ADDRESS mpa;
        BOOLEAN off = DispFileExists(L"\\??\\C:\\xun\\gpuw.nodirect");

        mpa.QuadPart = 0x05E00000LL;
        g_Mdp = (volatile UCHAR *)MmMapIoSpaceEx(mpa, 0x90000, PAGE_READWRITE | PAGE_NOCACHE);
        if (g_Mdp != NULL) {
            g_FbAddr = READ_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC0_ADDR));
            g_FbStride = READ_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC_YSTRIDE0));
            g_FbFormat = READ_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC_FORMAT));
            g_FbOpMode = READ_REGISTER_ULONG((volatile ULONG *)(g_Mdp + MDP_VIG0 + SSPP_SRC_OP_MODE));
            g_DirectOk = !off && g_FbAddr == (ULONG)g_Post.PhysicAddress.QuadPart && g_FbStride == g_Post.Pitch;
        }
        LogPrint("direct scanout: %s (VIG0 addr %08x stride %u fmt %08x op %08x%s)\n", g_DirectOk ? "on" : "off",
                 g_FbAddr, g_FbStride, g_FbFormat, g_FbOpMode, off ? ", gpuw.nodirect" : "");
    }

    g_NextVsync = (LONG64)KeQueryInterruptTime() + VSYNC_PERIOD;
    g_HrTimer = ExAllocateTimer(HrTimerCb, NULL, EX_TIMER_HIGH_RESOLUTION);
    if (g_HrTimer != NULL) {
        LogPrint("vsync: timer resolution %lu us (requested 1000)\n", ExSetTimerResolution(10000, TRUE) / 10);
        g_TimerRes = TRUE;
        ExSetTimer(g_HrTimer, -10000LL, 10000LL, NULL);     /* 1 ms poll, vsync phase in VsyncWork */
        LogPrint("vsync: 1 ms high-resolution timer, flip done at the MDP latch\n");
    } else {
        KeInitializeTimerEx(&g_VsyncTimer, NotificationTimer);
        KeInitializeDpc(&g_VsyncDpc, VsyncDpc, NULL);
        due.QuadPart = -10000LL * 16;
        KeSetTimerEx(&g_VsyncTimer, due, 1000 / g_Refresh, &g_VsyncDpc);
        LogPrint("vsync: ExAllocateTimer failed, 16 ms KTIMER\n");
    }
    g_TimerOn = TRUE;
    return STATUS_SUCCESS;
}

VOID DispStop(VOID)
{
    if (g_DirectOk && InterlockedExchange(&g_DirectActive, 0)) {
        MdpScanFrom(g_FbAddr, g_FbStride, FALSE);        /* hand the panel back on the framebuffer */
        LogPrint("direct scanout: back to the framebuffer (%u direct flips)\n", g_DirectFlips);
    }
    if (g_TimerOn) {
        if (g_HrTimer != NULL) {
            EXT_DELETE_PARAMETERS dp;
            ExInitializeDeleteTimerParameters(&dp);
            ExDeleteTimer(g_HrTimer, TRUE, TRUE, &dp);   /* cancel + wait for a running callback */
            g_HrTimer = NULL;
            if (g_TimerRes) {
                ExSetTimerResolution(0, FALSE);
                g_TimerRes = FALSE;
            }
        } else {
            KeCancelTimer(&g_VsyncTimer);
        }
        KeFlushQueuedDpcs();
        g_TimerOn = FALSE;
    }
    XunQupLockClose(&g_BlQupLock);
    XunKtzDeinit(&g_Ktz);
    DispScanLock();
    g_ScanCur = NULL;
    DispScanUnlock();
    g_Dxgk = NULL;
}

VOID DispGetPostInfo(DXGK_DISPLAY_INFORMATION *Info)
{
    *Info = g_Post;
}

/* ---------------------------------------------------------------- children / EDID */

static LONG ReadCfg(LONG *Vot, LONG *Hpd)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\xun\\gpuw.cfg");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    CHAR buf[32];
    LONG v[2] = { 0x7FFFFFFF, 0x7FFFFFFF };
    ULONG i, n = 0;
    BOOLEAN neg = FALSE, any = FALSE;
    LONG cur = 0;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        RtlZeroMemory(buf, sizeof(buf));
        if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf) - 1, NULL, NULL))) {
            for (i = 0; i <= iosb.Information && n < 2; i++) {
                CHAR c = buf[i];
                if (c == '-') {
                    neg = TRUE;
                } else if (c >= '0' && c <= '9') {
                    cur = cur * 10 + (c - '0');
                    any = TRUE;
                } else {
                    if (any) {
                        v[n++] = neg ? -cur : cur;
                    }
                    cur = 0;
                    any = neg = FALSE;
                }
            }
        }
        ZwClose(h);
    }
    *Vot = v[0];
    *Hpd = v[1];
    return (LONG)n;
}

NTSTATUS DispQueryChildRelations(PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size)
{
    LONG vot = (LONG)D3DKMDT_VOT_INTERNAL, hpd = (LONG)HpdAwarenessAlwaysConnected, cv, ch;

    /* POST/ACPI adapter: the panel is the internal display (C:\xun\gpuw.cfg "<vot> <hpd>" overrides) */
    ReadCfg(&cv, &ch);
    if (cv != 0x7FFFFFFF) vot = cv;
    if (ch != 0x7FFFFFFF) hpd = ch;
    LogPrint("QueryChildRelations: size %u (%u slots) vot %d hpd %d\n", Size, Size / sizeof(*Rel), vot, hpd);
    if (Size < 2 * sizeof(*Rel)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(Rel, sizeof(*Rel));
    Rel[0].ChildDeviceType = TypeVideoOutput;
    Rel[0].ChildCapabilities.HpdAwareness = (DXGK_CHILD_DEVICE_HPD_AWARENESS)hpd;
    Rel[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = (D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY)vot;
    Rel[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    Rel[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    Rel[0].AcpiUid = 0;
    Rel[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DispQueryChildStatus(PDXGK_CHILD_STATUS St)
{
    switch (St->Type) {
    case StatusConnection:
        St->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        St->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* EDID 1.4 of the panel (as drivers/XunDisplay/xun_edid.cxx): DSI has no DDC */
static VOID BuildEdid(UCHAR *e)
{
    static const UCHAR header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    static const UCHAR chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
    static const char name[13] = { 'X','u','n',' ','P','a','n','e','l',' ',' ','\n',' ' };
    const ULONG hActive = H_ACTIVE, hFp = 50, hSync = 20, hBp = 36;
    const ULONG vActive = V_ACTIVE, vFp = 410, vSync = 6, vBp = 10;
    const ULONG hBlank = hFp + hSync + hBp, vBlank = vFp + vSync + vBp;
    const ULONG clk10k = (ULONG)(((ULONGLONG)(hActive + hBlank) * (vActive + vBlank) * g_Refresh + 5000ULL) / 10000ULL);
    const ULONG wMm = 148, hMm = 237;
    UCHAR *d;
    ULONG i, sum = 0;

    RtlZeroMemory(e, 128);
    RtlCopyMemory(e, header, sizeof(header));
    e[8] = 0x62; e[9] = 0xAE;                       /* "XUN" */
    e[10] = 0x25; e[11] = 0x62;                     /* 0x6225 */
    e[16] = 1; e[17] = 2026 - 1990;
    e[18] = 1; e[19] = 4;
    e[20] = 0xA0;
    e[21] = (UCHAR)((wMm + 5) / 10);
    e[22] = (UCHAR)((hMm + 5) / 10);
    e[23] = 0x78;
    e[24] = 0x02;
    RtlCopyMemory(&e[25], chroma, sizeof(chroma));
    for (i = 38; i < 54; i++) {
        e[i] = 0x01;
    }
    d = &e[54];
    d[0] = (UCHAR)(clk10k & 0xFF);
    d[1] = (UCHAR)(clk10k >> 8);
    d[2] = (UCHAR)(hActive & 0xFF);
    d[3] = (UCHAR)(hBlank & 0xFF);
    d[4] = (UCHAR)(((hActive >> 8) << 4) | (hBlank >> 8));
    d[5] = (UCHAR)(vActive & 0xFF);
    d[6] = (UCHAR)(vBlank & 0xFF);
    d[7] = (UCHAR)(((vActive >> 8) << 4) | (vBlank >> 8));
    d[8] = (UCHAR)(hFp & 0xFF);
    d[9] = (UCHAR)(hSync & 0xFF);
    d[10] = (UCHAR)(((vFp & 0xF) << 4) | (vSync & 0xF));
    d[11] = (UCHAR)(((hFp >> 8) << 6) | ((hSync >> 8) << 4) | ((vFp >> 4) << 2) | (vSync >> 4));
    d[12] = (UCHAR)(wMm & 0xFF);
    d[13] = (UCHAR)(hMm & 0xFF);
    d[14] = (UCHAR)(((wMm >> 8) << 4) | (hMm >> 8));
    d[17] = 0x1E;
    d = &e[72];
    d[3] = 0xFC;
    RtlCopyMemory(&d[5], name, sizeof(name));
    e[90 + 3] = 0x10;
    e[108 + 3] = 0x10;
    for (i = 0; i < 127; i++) {
        sum += e[i];
    }
    e[127] = (UCHAR)(256 - (sum & 0xFF));
}

NTSTATUS DispQueryDeviceDescriptor(ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc)
{
    UCHAR edid[128];
    ULONG n;

    if (Uid != 0 || Desc->DescriptorOffset >= sizeof(edid)) {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
    BuildEdid(edid);
    n = sizeof(edid) - Desc->DescriptorOffset;
    if (n > Desc->DescriptorLength) {
        n = Desc->DescriptorLength;
    }
    RtlCopyMemory(Desc->DescriptorBuffer, edid + Desc->DescriptorOffset, n);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- VidPN */

static VOID FillSignal(D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = H_TOTAL;
    S->TotalSize.cy = V_TOTAL;
    S->ActiveSize.cx = H_ACTIVE;
    S->ActiveSize.cy = V_ACTIVE;
    S->VSyncFreq.Numerator = g_Refresh;
    S->VSyncFreq.Denominator = 1;
    S->HSyncFreq.Numerator = V_TOTAL * g_Refresh;
    S->HSyncFreq.Denominator = 1;
    S->PixelRate = (SIZE_T)H_TOTAL * V_TOTAL * g_Refresh;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS AddSourceMode(const DXGK_VIDPNSOURCEMODESET_INTERFACE *If, D3DKMDT_HVIDPNSOURCEMODESET Set)
{
    D3DKMDT_VIDPN_SOURCE_MODE *m = NULL;
    NTSTATUS st = If->pfnCreateNewModeInfo(Set, &m);

    if (!NT_SUCCESS(st)) {
        return st;
    }
    m->Type = D3DKMDT_RMT_GRAPHICS;
    m->Format.Graphics.PrimSurfSize.cx = H_ACTIVE;
    m->Format.Graphics.PrimSurfSize.cy = V_ACTIVE;
    m->Format.Graphics.VisibleRegionSize = m->Format.Graphics.PrimSurfSize;
    m->Format.Graphics.Stride = H_ACTIVE * 4;
    m->Format.Graphics.PixelFormat = D3DDDIFMT_X8R8G8B8;
    m->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
    m->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
    st = If->pfnAddMode(Set, m);
    if (!NT_SUCCESS(st)) {
        If->pfnReleaseModeInfo(Set, m);
        return st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS AddTargetMode(const DXGK_VIDPNTARGETMODESET_INTERFACE *If, D3DKMDT_HVIDPNTARGETMODESET Set)
{
    D3DKMDT_VIDPN_TARGET_MODE *m = NULL;
    NTSTATUS st = If->pfnCreateNewModeInfo(Set, &m);

    if (!NT_SUCCESS(st)) {
        return st;
    }
    FillSignal(&m->VideoSignalInfo);
    m->Preference = D3DKMDT_MP_PREFERRED;
    st = If->pfnAddMode(Set, m);
    if (!NT_SUCCESS(st)) {
        If->pfnReleaseModeInfo(Set, m);
        return st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : st;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispRecommendMonitorModes(const DXGKARG_RECOMMENDMONITORMODES *A)
{
    D3DKMDT_MONITOR_SOURCE_MODE *m = NULL;
    NTSTATUS st = A->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(A->hMonitorSourceModeSet, &m);

    if (!NT_SUCCESS(st)) {
        return st;
    }
    FillSignal(&m->VideoSignalInfo);
    m->Origin = D3DKMDT_MCO_DRIVER;
    m->Preference = D3DKMDT_MP_PREFERRED;
    m->ColorBasis = D3DKMDT_CB_SRGB;
    m->ColorCoeffDynamicRanges.FirstChannel = 8;
    m->ColorCoeffDynamicRanges.SecondChannel = 8;
    m->ColorCoeffDynamicRanges.ThirdChannel = 8;
    m->ColorCoeffDynamicRanges.FourthChannel = 8;
    st = A->pMonitorSourceModeSetInterface->pfnAddMode(A->hMonitorSourceModeSet, m);
    if (!NT_SUCCESS(st)) {
        A->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(A->hMonitorSourceModeSet, m);
        return st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : st;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispEnumCofuncModality(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *A)
{
    const DXGK_VIDPN_INTERFACE *vi = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topo = 0;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next = NULL;
    NTSTATUS st;

    st = Dxgk->DxgkCbQueryVidPnInterface(A->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = vi->pfnGetTopology(A->hConstrainingVidPn, &topo, &ti);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = ti->pfnAcquireFirstPathInfo(topo, &path);
    if (st == STATUS_GRAPHICS_DATASET_IS_EMPTY) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(st)) {
        return st;
    }
    while (path != NULL) {
        D3DKMDT_HVIDPNSOURCEMODESET sset = 0;
        D3DKMDT_HVIDPNTARGETMODESET tset = 0;
        const DXGK_VIDPNSOURCEMODESET_INTERFACE *si = NULL;
        const DXGK_VIDPNTARGETMODESET_INTERFACE *tgi = NULL;
        const D3DKMDT_VIDPN_SOURCE_MODE *spin = NULL;
        const D3DKMDT_VIDPN_TARGET_MODE *tpin = NULL;
        D3DKMDT_VIDPN_PRESENT_PATH local;
        BOOLEAN modified = FALSE;

        /* source modes */
        if (!(A->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && A->EnumPivot.VidPnSourceId == path->VidPnSourceId)) {
            st = vi->pfnAcquireSourceModeSet(A->hConstrainingVidPn, path->VidPnSourceId, &sset, &si);
            if (!NT_SUCCESS(st)) {
                break;
            }
            st = si->pfnAcquirePinnedModeInfo(sset, &spin);
            if (NT_SUCCESS(st) && spin == NULL) {
                vi->pfnReleaseSourceModeSet(A->hConstrainingVidPn, sset);
                sset = 0;
                st = vi->pfnCreateNewSourceModeSet(A->hConstrainingVidPn, path->VidPnSourceId, &sset, &si);
                if (NT_SUCCESS(st)) {
                    st = AddSourceMode(si, sset);
                }
                if (NT_SUCCESS(st)) {
                    st = vi->pfnAssignSourceModeSet(A->hConstrainingVidPn, path->VidPnSourceId, sset);
                    if (NT_SUCCESS(st)) {
                        sset = 0;
                    }
                }
            } else if (spin != NULL) {
                si->pfnReleaseModeInfo(sset, spin);
            }
            if (sset != 0) {
                vi->pfnReleaseSourceModeSet(A->hConstrainingVidPn, sset);
            }
            if (!NT_SUCCESS(st)) {
                break;
            }
        }
        /* target modes */
        if (!(A->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && A->EnumPivot.VidPnTargetId == path->VidPnTargetId)) {
            st = vi->pfnAcquireTargetModeSet(A->hConstrainingVidPn, path->VidPnTargetId, &tset, &tgi);
            if (!NT_SUCCESS(st)) {
                break;
            }
            st = tgi->pfnAcquirePinnedModeInfo(tset, &tpin);
            if (NT_SUCCESS(st) && tpin == NULL) {
                vi->pfnReleaseTargetModeSet(A->hConstrainingVidPn, tset);
                tset = 0;
                st = vi->pfnCreateNewTargetModeSet(A->hConstrainingVidPn, path->VidPnTargetId, &tset, &tgi);
                if (NT_SUCCESS(st)) {
                    st = AddTargetMode(tgi, tset);
                }
                if (NT_SUCCESS(st)) {
                    st = vi->pfnAssignTargetModeSet(A->hConstrainingVidPn, path->VidPnTargetId, tset);
                    if (NT_SUCCESS(st)) {
                        tset = 0;
                    }
                }
            } else if (tpin != NULL) {
                tgi->pfnReleaseModeInfo(tset, tpin);
            }
            if (tset != 0) {
                vi->pfnReleaseTargetModeSet(A->hConstrainingVidPn, tset);
            }
            if (!NT_SUCCESS(st)) {
                break;
            }
        }
        /* scaling / rotation support: identity only */
        local = *path;
        if (!(A->EnumPivotType == D3DKMDT_EPT_SCALING && A->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
              A->EnumPivot.VidPnTargetId == path->VidPnTargetId) &&
            path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
            RtlZeroMemory(&local.ContentTransformation.ScalingSupport, sizeof(local.ContentTransformation.ScalingSupport));
            local.ContentTransformation.ScalingSupport.Identity = 1;
            modified = TRUE;
        }
        if (!(A->EnumPivotType == D3DKMDT_EPT_ROTATION && A->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
              A->EnumPivot.VidPnTargetId == path->VidPnTargetId) &&
            path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
            RtlZeroMemory(&local.ContentTransformation.RotationSupport, sizeof(local.ContentTransformation.RotationSupport));
            local.ContentTransformation.RotationSupport.Identity = 1;
            local.ContentTransformation.RotationSupport.Offset0 = 1;
            modified = TRUE;
        }
        if (modified) {
            st = ti->pfnUpdatePathSupportInfo(topo, &local);
            if (!NT_SUCCESS(st)) {
                break;
            }
        }
        st = ti->pfnAcquireNextPathInfo(topo, path, &next);
        ti->pfnReleasePathInfo(topo, path);
        path = NULL;
        if (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) {
            st = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(st)) {
            break;
        }
        path = next;
    }
    if (path != NULL) {
        ti->pfnReleasePathInfo(topo, path);
    }
    if (!NT_SUCCESS(st)) {
        LogPrint("EnumVidPnCofuncModality: %08x\n", st);
    }
    return st;
}

NTSTATUS DispCommitVidPn(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_COMMITVIDPN *A)
{
    const DXGK_VIDPN_INTERFACE *vi = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topo = 0;
    D3DKMDT_HVIDPNSOURCEMODESET sset = 0;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *pin = NULL;
    SIZE_T paths = 0;
    NTSTATUS st;

    if (A->Flags.PathPoweredOff) {
        return STATUS_SUCCESS;
    }
    st = Dxgk->DxgkCbQueryVidPnInterface(A->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (NT_SUCCESS(st)) {
        st = vi->pfnGetTopology(A->hFunctionalVidPn, &topo, &ti);
    }
    if (NT_SUCCESS(st)) {
        st = ti->pfnGetNumPaths(topo, &paths);
    }
    if (NT_SUCCESS(st) && paths != 0) {
        st = vi->pfnAcquireSourceModeSet(A->hFunctionalVidPn, A->AffectedVidPnSourceId, &sset, &si);
        if (NT_SUCCESS(st)) {
            st = si->pfnAcquirePinnedModeInfo(sset, &pin);
        }
    }
    if (NT_SUCCESS(st)) {
        g_Committed = pin != NULL;
        LogPrint("CommitVidPn: src %u paths %u mode %ux%u stride %u fmt %u primary %p\n", A->AffectedVidPnSourceId,
                 (ULONG)paths, pin ? pin->Format.Graphics.PrimSurfSize.cx : 0, pin ? pin->Format.Graphics.PrimSurfSize.cy : 0,
                 pin ? pin->Format.Graphics.Stride : 0, pin ? pin->Format.Graphics.PixelFormat : 0, A->hPrimaryAllocation);
        if (!g_Committed) {
            DirectSelect(NULL);
            DispScanLock();
            g_ScanCur = NULL;
            DispScanUnlock();
        } else if (A->hPrimaryAllocation != NULL) {
            g_ScanCur = (TGPU_ALLOCATION *)A->hPrimaryAllocation;
            if (!DirectSelect(g_ScanCur)) {
                InterlockedExchange(&g_ScanDirty, 1);
            }
        }
    } else {
        LogPrint("CommitVidPn: %08x\n", st);
    }
    if (pin != NULL) {
        si->pfnReleaseModeInfo(sset, pin);
    }
    if (sset != 0) {
        vi->pfnReleaseSourceModeSet(A->hFunctionalVidPn, sset);
    }
    return st;
}

/* ---------------------------------------------------------------- brightness (KTZ8866 QUP0 SE1 + DCS 0x51 fallback) */

#define DSI0_PA             0x05E94000ULL
#define DSI_CTRL            0x004
#define DSI_CMD_DMA_CTRL    0x03C
#define DSI_DMA_CMD_OFFSET  0x048
#define DSI_DMA_CMD_LENGTH  0x04C
#define DSI_TRIG_CTRL       0x084
#define DSI_DMA_SW_TRIGGER  0x090
#define DSI_INT_CTRL        0x110
#define DSI_TPG_CTRL        0x15C

static volatile UCHAR *g_Dsi;
static PUCHAR g_BlVa;
static PHYSICAL_ADDRESS g_BlPa;
static UCHAR g_Brightness = 80;
static BOOLEAN g_KtzOn = FALSE;
static KMUTEX g_BlLock;

static ULONG DsiRd(ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Dsi + Off)); }
static VOID DsiWr(ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Dsi + Off), V); }

static VOID InitBrightness(VOID)
{
    PHYSICAL_ADDRESS pa, lo, hi, skip;

    if (g_Dsi != NULL || g_Ktz.Se1 != NULL) {
        return;
    }
    KeInitializeMutex(&g_BlLock, 0);
    XunQupLockInit(&g_BlQupLock, XUN_QUP_SE1);
    XunKtzInit(&g_Ktz);
    pa.QuadPart = (LONGLONG)DSI0_PA;
    g_Dsi = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x400, PAGE_READWRITE | PAGE_NOCACHE);
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    g_BlVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, skip, MmNonCached);
    if (g_BlVa != NULL) {
        g_BlPa = MmGetPhysicalAddress(g_BlVa);
    }
    LogPrint("brightness: KTZ SE1=%p HWEN=%p, DSI0=%p DMA=%llx\n",
             g_Ktz.Se1, g_Ktz.Hwen, g_Dsi, g_BlPa.QuadPart);
}

static BOOLEAN DsiDcsLong(const UCHAR *Payload, ULONG Len)
{
    ULONG size = (4 + Len + 3) & ~3u, ctrl, dma, trig, i;
    BOOLEAN done = FALSE;

    if (g_Dsi == NULL || g_BlVa == NULL || Len > 60) {
        return FALSE;
    }
    RtlFillMemory(g_BlVa, 64, 0xFF);
    g_BlVa[0] = (UCHAR)Len;
    g_BlVa[1] = (UCHAR)(Len >> 8);
    g_BlVa[2] = 0x39;                           /* DCS long write, VC 0 */
    g_BlVa[3] = 0xC0;                           /* last, long */
    RtlCopyMemory(g_BlVa + 4, Payload, Len);
    KeMemoryBarrier();
    ctrl = DsiRd(DSI_CTRL);
    trig = DsiRd(DSI_TRIG_CTRL);
    if ((trig & 7) != 4) {
        DsiWr(DSI_TRIG_CTRL, (trig & ~7u) | 4);
    }
    DsiWr(DSI_CTRL, ctrl | 4 | 1);
    DsiWr(DSI_INT_CTRL, DsiRd(DSI_INT_CTRL) | 1);
    DsiWr(DSI_TPG_CTRL, 0);
    DsiWr(DSI_DMA_CMD_OFFSET, g_BlPa.LowPart);
    DsiWr(DSI_DMA_CMD_LENGTH, size);
    dma = DsiRd(DSI_CMD_DMA_CTRL);
    DsiWr(DSI_CMD_DMA_CTRL, dma & ~(1u << 26));
    DsiWr(DSI_DMA_SW_TRIGGER, 1);
    for (i = 0; i < 50; i++) {
        if (DsiRd(DSI_INT_CTRL) & 1) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(1000);
    }
    DsiWr(DSI_INT_CTRL, DsiRd(DSI_INT_CTRL) | 1);
    DsiWr(DSI_CMD_DMA_CTRL, dma);
    DsiWr(DSI_CTRL, ctrl);
    if (trig != DsiRd(DSI_TRIG_CTRL)) {
        DsiWr(DSI_TRIG_CTRL, trig);
    }
    return done;
}

static BOOLEAN BlApply(UCHAR Pct, BOOLEAN On)
{
    ULONG level = On ? ((ULONG)Pct * 0x7FF / 100) : 0;
    BOOLEAN done = FALSE, forceDcs = DispFileExists(L"\\??\\C:\\xun\\bl.dcs");
    static BOOLEAN ktzWarned, ktzOkLogged;

    if (On && Pct > 0 && level < 16) {
        level = 16;
    }
    KeWaitForSingleObject(&g_BlLock, Executive, KernelMode, FALSE, NULL);
    if (!forceDcs && g_Ktz.Se1 != NULL && XunQupLockAcquire(&g_BlQupLock, 500)) {
        if (!On || Pct == 0) {
            done = XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_LSB, 0) &&
                   XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_MSB, 0) &&
                   XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_EN, 0x9C) &&
                   XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_EN, 0x98);
            g_KtzOn = FALSE;
        } else {
            BOOLEAN chipOk = XunKtzInitChip(&g_Ktz);
            if (chipOk && !g_KtzOn) {
                chipOk = XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_EN, 0x9C);
                XunKtzDelayUs(5000);
                chipOk = chipOk && XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_EN, 0x9E);
                if (chipOk) {
                    g_KtzOn = TRUE;
                }
            }
            if (chipOk) {
                done = XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_LSB, (UCHAR)(level & 0x7)) &&
                       XunKtzWriteReg(&g_Ktz, XUN_KTZ_REG_BL_MSB, (UCHAR)((level >> 3) & 0xFF));
            }
            if (done && !ktzOkLogged) {
                UCHAR id = 0, cfg1 = 0, en = 0;
                ktzOkLogged = TRUE;
                XunKtzReadRegAddr(&g_Ktz, XUN_KTZ_I2C_ADDR, XUN_KTZ_REG_DEV_ID, &id);
                XunKtzReadRegAddr(&g_Ktz, XUN_KTZ_I2C_ADDR, XUN_KTZ_REG_BL_CFG1, &cfg1);
                XunKtzReadRegAddr(&g_Ktz, XUN_KTZ_I2C_ADDR, XUN_KTZ_REG_BL_EN, &en);
                LogPrint("brightness: KTZ8866 QUP0 SE1 ok (hwen %08x/%08x->%08x/%08x id=%02x cfg1=%02x en=%02x level=%03x)\n",
                         g_Ktz.HwenBeforeCtl, g_Ktz.HwenBeforeIo,
                         g_Ktz.HwenAfterCtl, g_Ktz.HwenAfterIo, id, cfg1, en, level);
            }
        }
        if (!done && !ktzWarned) {
            ULONG ktzIrq = g_Ktz.LastIrq;
            UCHAR chg = 0, wusb = 0, ktz = 0;
            BOOLEAN chgOk, wusbOk, ktzOk;
            ktzWarned = TRUE;
            chgOk = XunKtzReadRegAddr(&g_Ktz, 0x6A, 0x03, &chg);
            wusbOk = XunKtzReadRegAddr(&g_Ktz, 0x60, 0x07, &wusb);
            ktzOk = XunKtzReadRegAddr(&g_Ktz, XUN_KTZ_I2C_ADDR, XUN_KTZ_REG_DEV_ID, &ktz);
            LogPrint("brightness: KTZ8866 QUP0 SE1 failed (fw=%08x cbcr=%08x irq=%08x hwen=%08x/%08x->%08x/%08x probe 6A=%u:%02x 60=%u:%02x 11=%u:%02x)\n",
                     g_Ktz.LastFw, g_Ktz.LastCbcr, ktzIrq,
                     g_Ktz.HwenBeforeCtl, g_Ktz.HwenBeforeIo,
                     g_Ktz.HwenAfterCtl, g_Ktz.HwenAfterIo,
                     chgOk, chg, wusbOk, wusb, ktzOk, ktz);
        }
        XunQupLockRelease(&g_BlQupLock);
    }
    if (forceDcs && On && Pct > 0) {
        UCHAR cmd[3];
        ULONG dcs = (On && level < 1) ? 1 : level;
        cmd[0] = 0x51;
        cmd[1] = (UCHAR)((dcs >> 3) & 0xFF);
        cmd[2] = (UCHAR)(dcs & 0x7);
        done = DsiDcsLong(cmd, sizeof(cmd));
    }
    KeReleaseMutex(&g_BlLock, FALSE);
    return done;
}

static VOID APIENTRY BlRef(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }
static VOID APIENTRY BlDeref(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }

static NTSTATUS APIENTRY BlGetPossible(HANDLE Ctx, ULONG BufferSize, UCHAR *LevelCount, UCHAR *Levels)
{
    ULONG i, n = BufferSize < 101 ? BufferSize : 101;

    UNREFERENCED_PARAMETER(Ctx);
    if (LevelCount == NULL || Levels == NULL || n == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < n; i++) {
        Levels[i] = (UCHAR)i;
    }
    *LevelCount = (UCHAR)n;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY BlSet(HANDLE Ctx, UCHAR Pct)
{
    BOOLEAN done;

    UNREFERENCED_PARAMETER(Ctx);
    if (Pct > 100) {
        Pct = 100;
    }
    g_Brightness = Pct;
    if (!g_Visible) {
        LogPrint("brightness %u%% stored (screen off)\n", Pct);
        return STATUS_SUCCESS;
    }
    done = BlApply(Pct, TRUE);
    LogPrint("brightness %u%%: %s\n", Pct, done ? "done" : "NO DONE");
    return done ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static VOID BlPanel(BOOLEAN On)
{
    BOOLEAN done;

    if ((g_Ktz.Se1 == NULL && g_Dsi == NULL) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!On && (DispFileExists(L"\\??\\C:\\xun\\bl.always") || (g_Done < 10 && !DispFileExists(L"\\??\\C:\\xun\\bl.off")))) {
        LogPrint("panel off deferred / ignored (done=%u, bl.always=%u)\n", g_Done, DispFileExists(L"\\??\\C:\\xun\\bl.always"));
        return;
    }
    done = BlApply(g_Brightness, On);
    LogPrint("panel %s (backlight %u%%): %s\n", On ? "on" : "off", g_Brightness, done ? "done" : "NO DONE");
}

static NTSTATUS APIENTRY BlGet(HANDLE Ctx, UCHAR *Pct)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (Pct == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Pct = g_Brightness;
    return STATUS_SUCCESS;
}

NTSTATUS DispBrightnessQueryInterface(PQUERY_INTERFACE Qi)
{
    DXGK_BRIGHTNESS_INTERFACE *b;

    if (Qi == NULL || Qi->InterfaceType == NULL ||
        RtlCompareMemory(Qi->InterfaceType, &g_BrightnessGuid, sizeof(GUID)) != sizeof(GUID)) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((g_Ktz.Se1 == NULL && (g_Dsi == NULL || g_BlVa == NULL)) ||
        Qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE) || Qi->Interface == NULL) {
        LogPrint("QueryInterface brightness v%u size %u: not available\n", Qi->Version, Qi->Size);
        return STATUS_NOT_SUPPORTED;
    }
    b = (DXGK_BRIGHTNESS_INTERFACE *)Qi->Interface;
    RtlZeroMemory(b, sizeof(*b));
    b->Size = sizeof(*b);
    b->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
    b->InterfaceReference = BlRef;
    b->InterfaceDereference = BlDeref;
    b->GetPossibleBrightness = BlGetPossible;
    b->SetBrightness = BlSet;
    b->GetBrightness = BlGet;
    LogPrint("QueryInterface brightness v%u: provided\n", Qi->Version);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- v0.37: MDP / SMMU survey (read-only)
 * Goal: scan out the flipped BO directly (SSPP source address) instead of copying it. First learn what
 * the bootloader left running: which SSPP fetches the GOP framebuffer, its stride/format, the CTL/LM
 * routing, and whether the apps SMMU translates the MDP stream (SID 0x420/0x421, stock DT) or
 * bypasses it. Offsets: stock DT sde-sspp-off VIG0 0x5000, DMA0 0x25000, ctl 0x2000, mixer 0x45000. */

#define MDP_PA        0x05E00000ULL
#define MDP_SIZE      0x90000
#define APPS_SMMU_PA  0x0C600000ULL

static ULONG MRd(volatile UCHAR *B, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(B + Off)); }

static VOID DumpSspp(volatile UCHAR *Mdp, PCSTR Name, ULONG Base)
{
    LogPrint("MDP %s @+%05x: src_size %08x img %08x xy %08x out_size %08x out_xy %08x addr0 %08x addr1 %08x "
             "ystride0 %08x format %08x unpack %08x op %08x\n", Name, Base, MRd(Mdp, Base + 0x00), MRd(Mdp, Base + 0x04),
             MRd(Mdp, Base + 0x08), MRd(Mdp, Base + 0x0C), MRd(Mdp, Base + 0x10), MRd(Mdp, Base + 0x14),
             MRd(Mdp, Base + 0x18), MRd(Mdp, Base + 0x24), MRd(Mdp, Base + 0x30), MRd(Mdp, Base + 0x34),
             MRd(Mdp, Base + 0x38));
}

VOID DispSurveyMdp(VOID)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *mdp, *smmu;
    ULONG idr0, idr1, nsmr, i, pagesz, numpage;

    pa.QuadPart = (LONGLONG)MDP_PA;
    mdp = (volatile UCHAR *)MmMapIoSpaceEx(pa, MDP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (mdp != NULL) {
        LogPrint("MDP hw rev %08x (top +0x1000: %08x)\n", MRd(mdp, 0x0), MRd(mdp, 0x1000));
        DumpSspp(mdp, "VIG0", 0x5000);
        DumpSspp(mdp, "DMA0", 0x25000);
        LogPrint("MDP CTL0 @+2000: layer0 %08x layer1 %08x top %08x flush %08x start %08x intf_active %08x\n",
                 MRd(mdp, 0x2000), MRd(mdp, 0x2004), MRd(mdp, 0x2014), MRd(mdp, 0x2018), MRd(mdp, 0x201C),
                 MRd(mdp, 0x20F4));
        LogPrint("MDP LM0 @+45000: op %08x out_size %08x border %08x stage0 fg %08x\n", MRd(mdp, 0x45000),
                 MRd(mdp, 0x45004), MRd(mdp, 0x45008), MRd(mdp, 0x45020));
        LogPrint("MDP INTF1 @+6b800: timing_en %08x hsync %08x vsync0 %08x disp_hctl %08x\n", MRd(mdp, 0x6b800),
                 MRd(mdp, 0x6b800 + 0x8), MRd(mdp, 0x6b800 + 0x10), MRd(mdp, 0x6b800 + 0x3C));
        MmUnmapIoSpace((PVOID)mdp, MDP_SIZE);
    }

    pa.QuadPart = (LONGLONG)APPS_SMMU_PA;
    smmu = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x2000, PAGE_READWRITE | PAGE_NOCACHE);
    if (smmu == NULL) {
        return;
    }
    idr0 = MRd(smmu, 0x20);
    idr1 = MRd(smmu, 0x24);
    nsmr = idr0 & 0xFF;
    pagesz = (idr1 & 0x80000000) ? 0x10000 : 0x1000;
    numpage = 1u << (((idr1 >> 28) & 7) + 1);
    LogPrint("apps SMMU: sCR0 %08x IDR0 %08x IDR1 %08x (%u SMRs, %u CBs, page %x, numpage %u)\n", MRd(smmu, 0),
             idr0, idr1, nsmr, idr1 & 0xFF, pagesz, numpage);
    for (i = 0; i < nsmr && i < 128; i++) {
        ULONG smr = MRd(smmu, 0x800 + 4 * i), s2cr = MRd(smmu, 0xC00 + 4 * i);
        ULONG id = smr & 0xFFFF, mask = (smr >> 16) & 0x7FFF;
        if ((smr & 0x80000000) && ((0x420 & ~mask) == (id & ~mask) || (0x421 & ~mask) == (id & ~mask))) {
            ULONG type = (s2cr >> 16) & 3, cb = s2cr & 0xFF;
            LogPrint("  SMR%u id %04x mask %04x -> S2CR %08x (type %u: %s, cb %u)\n", i, id, mask, s2cr, type,
                     type == 0 ? "translate" : type == 1 ? "bypass" : "fault", cb);
            if (type == 0) {
                volatile UCHAR *cbm;
                pa.QuadPart = (LONGLONG)(APPS_SMMU_PA + (ULONGLONG)numpage * pagesz + (ULONGLONG)cb * pagesz);
                cbm = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x100, PAGE_READWRITE | PAGE_NOCACHE);
                if (cbm != NULL) {
                    LogPrint("    CB%u: SCTLR %08x TCR %08x TTBR0 %08x%08x CBAR %08x\n", cb, MRd(cbm, 0x0),
                             MRd(cbm, 0x30), MRd(cbm, 0x24), MRd(cbm, 0x20),
                             MRd(smmu, 0x1000 + 4 * cb) /* CBAR in GR1 */);
                    MmUnmapIoSpace((PVOID)cbm, 0x100);
                }
            }
        }
    }
    MmUnmapIoSpace((PVOID)smmu, 0x2000);
}
