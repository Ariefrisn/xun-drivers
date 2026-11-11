/*
 * Redmi Pad SE (xun, SM6225 "khaje") touch hardware addresses.
 * Verified from vendor dtbo_b.img (/fragment@40 -> touch@0), pinctrl-khaje.ko,
 * gcc-khaje.ko, spi-msm-geni.ko, and focaltech_ts_spi disassembly:
 *   - QUP0 SE0 SPI @ 0x04A80000, GPIO 0..3 WEST (func 1 = qup0)
 *   - Touch IC: FocalTech FT8203 (SPI v2, reflected CRC16 poly 0x8408)
 *   - IRQ: GPIO 80 WEST (active low, pull-up), RESET: GPIO 86 WEST
 *   - Coords: focaltech,display-coords = [0, 0, 11999, 19199] (0x2EDF x 0x4AFF)
 */
#pragma once

/* ---- TLMM (pinctrl-khaje) ------------------------------------------------ */
#define TLMM_BASE            0x00400000ULL
#define TLMM_TILE_WEST       0x00100000ULL
#define TLMM_TILE_SOUTH      0x00500000ULL
#define TLMM_TILE_EAST       0x00900000ULL
#define TLMM_PIN_STRIDE      0x1000
#define TLMM_PIN_PA(tile, pin) (TLMM_BASE + (tile) + (ULONGLONG)(pin) * TLMM_PIN_STRIDE)

#define TLMM_CTL             0x0      /* pull[1:0] func[5:2] drv[8:6] oe[9] */
#define TLMM_IO              0x4      /* in[0] out[1] */
#define TLMM_PULL_NONE       0
#define TLMM_PULL_DOWN       1
#define TLMM_PULL_UP         3
#define TLMM_DRV_MA(ma)      (((ma) / 2) - 1)

/* ---- GCC (gcc-khaje) ----------------------------------------------------- */
#define GCC_BASE             0x01400000ULL
#define GCC_APCS_VOTE_REG    0x7900C
#define GCC_QUP0_M_AHB_BIT   6
#define GCC_QUP0_S_AHB_BIT   7
#define GCC_QUP0_CORE_BIT    8
#define GCC_QUP0_CORE2X_BIT  9
#define GCC_QUP0_S0_BIT      10       /* gcc_qupv3_wrap0_s0_clk (touch SPI SE0) */
#define GCC_QUP0_S0_CBCR     0x1F144
#define GCC_QUP0_S0_RCG_CMD  0x1F148
#define GCC_RCG_CMD_UPDATE   (1u << 0)
#define GCC_RCG_CFG_OFF      0x4
#define GCC_CBCR_CLK_OFF     (1u << 31)

/* ---- QUPv3 GENI SE0 (SPI) ------------------------------------------------ */
#define QUP0_SE0_BASE        0x04A80000ULL
#define GENI_SE_SIZE         0x4000

#define GENI_FORCE_DEFAULT_REG       0x20
#define GENI_OUTPUT_CTRL             0x24
#define GENI_CGC_CTRL                0x28
#define SE_GENI_STATUS               0x40
#define GENI_SER_M_CLK_CFG           0x48
#define GENI_FW_REVISION_RO          0x68
#define SE_GENI_CLK_SEL              0x7C
#define SE_SPI_CPHA                  0x224
#define SE_SPI_CPOL                  0x230
#define SE_SPI_DEMUX_OUTPUT_INV      0x248
#define SE_SPI_DEMUX_SEL             0x24C
#define SE_GENI_BYTE_GRAN            0x254
#define SE_GENI_DMA_MODE_EN          0x258
#define SE_SPI_TRANS_CFG             0x25C
#define SE_GENI_TX_PACKING_CFG0      0x260
#define SE_GENI_TX_PACKING_CFG1      0x264
#define SE_SPI_WORD_LEN              0x268
#define SE_SPI_TX_TRANS_LEN          0x26C
#define SE_SPI_RX_TRANS_LEN          0x270
#define SE_SPI_PRE_POST_CMD_DLY      0x274
#define SE_GENI_RX_PACKING_CFG0      0x284
#define SE_GENI_RX_PACKING_CFG1      0x288
#define SE_GENI_M_CMD0               0x600
#define SE_GENI_M_CMD_CTRL_REG       0x604
#define SE_GENI_M_IRQ_STATUS         0x610
#define SE_GENI_M_IRQ_EN             0x614
#define SE_GENI_M_IRQ_CLEAR          0x618
#define SE_GENI_S_IRQ_CLEAR          0x648
#define SE_GENI_TX_FIFOn             0x700
#define SE_GENI_RX_FIFOn             0x780
#define SE_GENI_TX_FIFO_STATUS       0x800
#define SE_GENI_RX_FIFO_STATUS       0x804
#define SE_GENI_TX_WATERMARK_REG     0x80C
#define SE_GENI_RX_WATERMARK_REG     0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_DMA_TX_IRQ_CLR            0xC44
#define SE_DMA_RX_IRQ_CLR            0xD44
#define SE_GSI_EVENT_EN              0xE18
#define SE_IRQ_EN                    0xE1C
#define SE_HW_PARAM_0                0xE24

#define GENI_PROTO_SPI               1
#define M_GENI_CMD_ACTIVE            (1u << 0)
#define M_GENI_CMD_ABORT             (1u << 1)
#define M_GENI_CMD_CANCEL            (1u << 2)
#define M_CMD_DONE_EN                (1u << 0)
#define M_CMD_OVERRUN_EN             (1u << 1)
#define M_ILLEGAL_CMD_EN             (1u << 2)
#define M_CMD_FAILURE_EN             (1u << 3)
#define M_CMD_CANCEL_EN              (1u << 4)
#define M_CMD_ABORT_EN               (1u << 5)
#define M_RX_FIFO_WATERMARK_EN       (1u << 26)
#define M_RX_FIFO_LAST_EN            (1u << 27)
#define M_TX_FIFO_WATERMARK_EN       (1u << 30)
#define M_SPI_ERR_MASK               (M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | M_CMD_FAILURE_EN | M_CMD_ABORT_EN)
#define TX_FIFO_WC_MASK              0x0FFFFFFFu
#define RX_FIFO_WC_MASK              0x01FFFFFFu

#define SPI_OP_TX_ONLY               1u
#define SPI_OP_RX_ONLY               2u
#define SPI_OP_TX_RX                 3u
#define M_OPCODE_SHIFT               27

/* 19.2 MHz XO / 3 = 6.4 MHz SPI SCLK (within FT8203 rated SPI frequency) */
#define SPI_6P4M_CLK_DIV             3u

/* ---- FT8203 Touch (SPI @ QUP0 SE0) --------------------------------------- */
#define TS_PIN_SPI_MISO          0    /* WEST, func 1 = qup0 */
#define TS_PIN_SPI_MOSI          1
#define TS_PIN_SPI_CLK           2
#define TS_PIN_SPI_CS            3
#define TS_PIN_SPI_FUNC          1
#define TS_PIN_IRQ               80   /* WEST, active low, pull-up */
#define TS_PIN_RESET             86   /* WEST, active low */

#define TS_MAX_CONTACTS          10
#define TS_DISPLAY_X             1200
#define TS_DISPLAY_Y             1920
#define TS_LOGICAL_MAX_X         11999 /* 0x2EDF */
#define TS_LOGICAL_MAX_Y         19199 /* 0x4AFF */

#define FTS_REG_TOUCH_DATA       0x01
#define FTS_REG_CHIP_ID          0xA3
#define FTS_REG_CHIP_ID2         0x9F
#define FTS_REG_FW_VER           0xA6
#define FTS_REG_VENDOR_ID        0xA8
#define FTS_TOUCH_DATA_LEN       (2 + 6 * TS_MAX_CONTACTS) /* 62 bytes from reg 0x01 */
