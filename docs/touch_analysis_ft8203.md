# Analisis Root Cause & Solusi Driver XunTouch FT8203 (SM6225 / Xiaomi Redmi Pad SE)

Dokumen ini mencatat analisis mendalam terkait kegagalan driver **XunTouch v0.16** pada Xiaomi Redmi Pad SE (`xun`, panel Huaxing FocalTech FT8203 di Windows on ARM).

---

## 1. Ringkasan Masalah

Pada log `XunTouch.log`, terjadi dua masalah kritis secara bersamaan:
1. **Double Instance Initialization:** Driver ter-load dua kali (`EvtDeviceAdd` dieksekusi 2x), menyebabkan dua instance device mengakses register MMIO SPI `0x04A80000` (QUP0 SE0) secara konkuren.
2. **Kegagalan Transaksi SPI & Interrupt Bocor (`irq=00000021`):** Terjadi `STATUS_IO_TIMEOUT` (`c00000b5`) pada probe pertama, diikuti abort yang tidak di-clear secara bersih pada `SE_GENI_M_IRQ_CLEAR`, sehingga state `M_CMD_ABORT_EN (0x20) | M_CMD_DONE_EN (0x01) = 0x21` terus terbaca pada transaksi berikutnya (`STATUS_DEVICE_DATA_ERROR` / `c0000186`).
3. **Chip ID 0x00:** IC TDDI FocalTech FT8203 tidak merespons di jalur MISO karena timing reset romboot, status CS (Chip Select), atau pinmux TLMM.

---

## 2. Bedah Register & Kode Error

| Kode / Log | Definisi Teknis | Makna Hardware / Driver |
| --- | --- | --- |
| `status=c00000b5` | `STATUS_IO_TIMEOUT` | Controller GENI SPI mengirim perintah baca, tetapi tidak selesai dalam batas waktu (FIFO tidak terisi / clock SPI tidak jalan). |
| `status=c0000186` | `STATUS_DEVICE_DATA_ERROR` | Bit error terdeteksi pada register status interrupt GENI SE saat melakukan `SPI read`. |
| `irq=00000021` | `M_CMD_DONE (Bit 0)` + `M_CMD_ABORT (Bit 5)` | Nilai `0x21` pada register `SE_GENI_M_IRQ_STATUS` berarti bit `0x01` dan `0x20` aktif bersamaan. State machine GENI berada dalam status aborted atau ditimpa perintah baru sebelum transfer selesai. |
| `chip_id=00` & `irq_line=1` | MISO selalu 0x00 / INT High | IC FT8203 belum merespons (bisa karena chip belum power-up, tertahan reset, CS tidak low, atau timing unlock `0x55` meleset). |

---

## 3. Alur Tabrakan Dua Instance (Double Instance)

```
[ Instance #1 ]
   -> EvtDeviceAdd #1
   -> Map MMIO 0x04A80000 & Init GENI SPI
   -> Probe SPI -> Timeout (c00000b5) -> Abort (0x20)
   -> Thread Polling #1 Berjalan

[ Instance #2 (Dipanggil bersamaan oleh PnP) ]
   -> EvtDeviceAdd #2
   -> Map MMIO 0x04A80000 lagi
   -> Reset ulang GENI SPI di tengah jalan
   -> Thread Polling #2 Berjalan

[ Hardware QUP0 SE0 SPI 0x04A80000 ]
   -> State Machine rusak, register IRQ menggantung (0x21)
   -> Banjir error c0000186 di kedua thread
```

---

## 4. Solusi & Rencana Implementasi

### Langkah 1: Single-Instance Guard di Driver
Di `drivers/XunTouch/touch.c`:
```c
static volatile LONG g_InstanceCount = 0;

NTSTATUS EvtDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit) {
    if (InterlockedIncrement(&g_InstanceCount) > 1) {
        LogPrint("XunTouch: instance already created, skipping duplicate node\n");
        return STATUS_SUCCESS;
    }
    ...
}
```
Juga bersihkan node duplikat di Device Manager / ACPI jika ada ghost node.

### Langkah 2: Abort & Clear IRQ Sesuai Standar `spi-geni-qcom.c`
Di `drivers/XunTouch/hwio.c`:
```c
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_CLEAR     0x618
#define M_CMD_DONE_EN           (1U << 0)
#define M_CMD_ABORT_EN          (1U << 5)

VOID GeniSpiAbortAndClear(PUCHAR BaseAddr) {
    // 1. Trigger Abort (Bit 1 pada M_CMD_CTRL_REG)
    WRITE_REGISTER_ULONG((PULONG)(BaseAddr + SE_GENI_M_CMD_CTRL_REG), 0x2);

    // 2. Poll sampai M_CMD_ABORT_EN (0x20) menyala
    for (int i = 0; i < 1000; i++) {
        ULONG irq = READ_REGISTER_ULONG((PULONG)(BaseAddr + SE_GENI_M_IRQ_STATUS));
        if (irq & M_CMD_ABORT_EN) {
            break;
        }
        KeStallExecutionProcessor(10);
    }

    // 3. Bersihkan SEMUA status IRQ yang menggantung
    ULONG pending = READ_REGISTER_ULONG((PULONG)(BaseAddr + SE_GENI_M_IRQ_STATUS));
    WRITE_REGISTER_ULONG((PULONG)(BaseAddr + SE_GENI_M_IRQ_CLEAR), pending);
}
```

### Langkah 3: Timing Reset Romboot & Unlock FT8203
Berdasarkan driver Linux Android (`focaltech_spi_v2.c`):
- FT8203 adalah chip TDDI ramless (firmware di-upload ke PRAM saat boot).
- Urutan reset romboot:
  1. GPIO Reset (`GPIO 86`) ditarik **LOW selama 10 ms**.
  2. GPIO Reset ditarik **HIGH**.
  3. Tunggu **tepat 8 ms - 20 ms** (jika < 6ms atau > 50ms, IC gagal masuk romboot).
  4. Kirim byte unlock `0x55` via SPI (Mode 0: CPOL=0, CPHA=0).
  5. Kirim perintah `0x90` untuk membaca Chip ID (`0x8203`).
- Clock SPI saat romboot: batasi di **4 MHz - 6 MHz**.

