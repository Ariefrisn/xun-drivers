/*
 * xun-woa: EDID 1.4 for Xiaomi Redmi Pad SE (xun, m84_42_03_0c_fhdp_video, 1200x1920@90Hz LCD).
 * Verified from stock dtbo_b.img panel timings:
 *   hActive=1200, hFp=50 (0x32), hSync=20 (0x14), hBp=36 (0x24) -> hTotal=1306
 *   vActive=1920, vFp=410 (0x19a), vSync=6 (0x6), vBp=10 (0xa) -> vTotal=2346
 *   pclk @ 90 Hz = 1306 * 2346 * 90 = 275,748,840 Hz (27575 * 10 kHz)
 *   pclk @ 60 Hz = 1306 * 2346 * 60 = 183,832,560 Hz (18383 * 10 kHz)
 *   Physical active area ~ 148 x 237 mm (11.0" 16:10).
 */
#include "BDD.hxx"

#pragma code_seg("PAGE")

static VOID FillDtd(BYTE* d, UINT hActive, UINT hFp, UINT hSync, UINT hBp,
                    UINT vActive, UINT vFp, UINT vSync, UINT vBp, UINT refreshHz,
                    UINT wMm, UINT hMm)
{
    const UINT hBlank = hFp + hSync + hBp;
    const UINT vBlank = vFp + vSync + vBp;
    const UINT clk10k = ((ULONGLONG)(hActive + hBlank) * (vActive + vBlank) * refreshHz + 5000ULL) / 10000ULL;

    d[0] = (BYTE)(clk10k & 0xFF);
    d[1] = (BYTE)((clk10k >> 8) & 0xFF);
    d[2] = (BYTE)(hActive & 0xFF);
    d[3] = (BYTE)(hBlank & 0xFF);
    d[4] = (BYTE)(((hActive >> 8) << 4) | ((hBlank >> 8) & 0xF));
    d[5] = (BYTE)(vActive & 0xFF);
    d[6] = (BYTE)(vBlank & 0xFF);
    d[7] = (BYTE)(((vActive >> 8) << 4) | ((vBlank >> 8) & 0xF));
    d[8] = (BYTE)(hFp & 0xFF);
    d[9] = (BYTE)(hSync & 0xFF);
    d[10] = (BYTE)(((vFp & 0xF) << 4) | (vSync & 0xF));
    d[11] = (BYTE)(((hFp >> 8) << 6) | (((hSync >> 8) & 3) << 4) | (((vFp >> 4) & 3) << 2) | ((vSync >> 4) & 3));
    d[12] = (BYTE)(wMm & 0xFF);
    d[13] = (BYTE)(hMm & 0xFF);
    d[14] = (BYTE)(((wMm >> 8) << 4) | ((hMm >> 8) & 0xF));
    d[17] = 0x1E;                        /* digital separate sync, +h +v */
}

VOID BuildXunEdid(BYTE* e)
{
    PAGED_CODE();

    static const BYTE header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    const UINT hActive = 1200, hFp = 50, hSync = 20, hBp = 36;
    const UINT vActive = 1920, vFp = 410, vSync = 6, vBp = 10;
    const UINT wMm = 148, hMm = 237;
    BYTE* d;
    UINT i, sum = 0;

    RtlZeroMemory(e, EDID_V1_BLOCK_SIZE);
    RtlCopyMemory(e, header, sizeof(header));
    /* EISA PNP ID "XUN": X=24 (11000), U=21 (10101), N=14 (01110) -> 0x62AE */
    e[8] = 0x62; e[9] = 0xAE;
    e[10] = 0x25; e[11] = 0x62;          /* product 0x6225 */
    e[16] = 1; e[17] = 2026 - 1990;
    e[18] = 1; e[19] = 4;                /* EDID 1.4 */
    e[20] = 0xA0;                        /* digital, 8 bpc */
    e[21] = (BYTE)((wMm + 5) / 10);
    e[22] = (BYTE)((hMm + 5) / 10);
    e[23] = 0x78;                        /* gamma 2.2 */
    e[24] = 0x02;                        /* preferred timing is native */
    {
        static const BYTE chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
        RtlCopyMemory(&e[25], chroma, sizeof(chroma));
    }
    for (i = 38; i < 54; i++) {
        e[i] = 0x01;
    }

    /* Descriptor 1 (bytes 54..71): native 1200x1920 @ 90 Hz */
    FillDtd(&e[54], hActive, hFp, hSync, hBp, vActive, vFp, vSync, vBp, 90, wMm, hMm);

    /* Descriptor 2 (bytes 72..89): fallback 1200x1920 @ 60 Hz */
    FillDtd(&e[72], hActive, hFp, hSync, hBp, vActive, vFp, vSync, vBp, 60, wMm, hMm);

    /* Descriptor 3 (bytes 90..107): monitor name */
    d = &e[90];
    d[3] = 0xFC;
    {
        static const char name[13] = { 'X','u','n',' ','P','a','n','e','l',' ',' ','\n',' ' };
        RtlCopyMemory(&d[5], name, sizeof(name));
    }
    e[108 + 3] = 0x10;                   /* dummy descriptor */

    for (i = 0; i < 127; i++) {
        sum += e[i];
    }
    e[127] = (BYTE)(256 - (sum & 0xFF));
}
