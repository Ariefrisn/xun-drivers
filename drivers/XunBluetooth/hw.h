/*
 * Redmi Pad SE (xun, SM6225 "khaje") Bluetooth hardware definitions.
 * Verified from xun DTS (qcom,qup_uart@4a8c000, bt_wcn3990, pm6125_l9/l16/l17/l23),
 * gcc-khaje.ko (gcc_qupv3_wrap0_s3_clk / s3_clk_src), and Linux msm_geni_serial / btqca.
 *
 *  - QUPv3 Wrap 0:  0x04AC0000 (HW_VER at +0x4)
 *  - QUP0 SE3 UART: 0x04A8C000 (0x4000 bytes)
 *  - TLMM EAST:     GPIO 8 CTS, GPIO 9 RTS, GPIO 10 TX, GPIO 11 RX (func 1), GPIO 87 bt-sw-ctrl
 *  - GCC QUP0 S3:   APCS vote 0x7900C bits {6,7,8,9,13}, CBCR 0x1F4D4, RCG 0x1F4D8
 *  - SPMI PM6125:   Observer 0x03E00000, Core 0x01C40000, SID 1 L9/L16/L17/L23
 *  - Controller:    WCN3990 / WCN3950 H4 + HCI_IBS (0xFC/0xFD/0xFE)
 */
#pragma once

/* ---- TLMM (pinctrl-khaje) ------------------------------------------------ */
#define TLMM_BASE                   0x00400000ULL
#define TLMM_TILE_EAST              0x00900000ULL
#define TLMM_PIN_STRIDE             0x1000
#define TLMM_EAST_MAP_SIZE          0x60000ULL  /* covers GPIO 0..95 on EAST tile */

#define TLMM_CTL                    0x0         /* pull[1:0] func[5:2] drv[8:6] oe[9] */
#define TLMM_IO                     0x4         /* in[0] out[1] */
#define TLMM_PULL_NONE              0
#define TLMM_PULL_DOWN              1
#define TLMM_PULL_UP                3
#define TLMM_DRV_MA(ma)             (((ma) / 2) - 1)

#define BT_GPIO_CTS                 8
#define BT_GPIO_RTS                 9
#define BT_GPIO_TX                  10
#define BT_GPIO_RX                  11
#define BT_GPIO_SW_CTRL             87
#define BT_UART_FUNC_QUP0_S3        1

/* ---- GCC (gcc-khaje) ----------------------------------------------------- */
#define GCC_BASE                    0x01400000ULL
#define GCC_MAP_SIZE                0x80000ULL
#define GCC_APCS_VOTE_REG           0x7900C
#define GCC_QUP0_M_AHB_BIT          6
#define GCC_QUP0_S_AHB_BIT          7
#define GCC_QUP0_CORE_BIT           8
#define GCC_QUP0_CORE2X_BIT         9
#define GCC_QUP0_S3_BIT             13
#define GCC_QUP0_S3_VOTE_MASK       ((1u << GCC_QUP0_M_AHB_BIT) | \
                                     (1u << GCC_QUP0_S_AHB_BIT) | \
                                     (1u << GCC_QUP0_CORE_BIT)  | \
                                     (1u << GCC_QUP0_CORE2X_BIT)| \
                                     (1u << GCC_QUP0_S3_BIT))

#define GCC_QUP0_S3_CBCR            0x1F4D4
#define GCC_QUP0_S3_RCG             0x1F4D8
#define GCC_CBCR_CLK_OFF            (1u << 31)

#define RCG_CMD_OFF                 0x00
#define RCG_CFG_OFF                 0x04
#define RCG_M_OFF                   0x08
#define RCG_N_OFF                   0x0C
#define RCG_D_OFF                   0x10
#define RCG_SE_CMD_DFSR_OFF         0x14
#define RCG_SE_PERF_DFSR(lvl)       (0x1C + 4u * (lvl))
#define RCG_SE_PERF_M_DFSR(lvl)     (0x5C + 4u * (lvl))
#define RCG_SE_PERF_N_DFSR(lvl)     (0x9C + 4u * (lvl))
#define RCG_DFSR_LEVELS             8

#define RCG_CMD_UPDATE              (1u << 0)
#define RCG_CMD_ROOT_OFF            (1u << 31)
#define RCG_DFS_EN                  (1u << 0)

/* ---- SPMI Arbiter v5 read-only observer (PM6125 LDO check) --------------- */
#define SPMI_CORE_PA                0x01C40000ULL
#define SPMI_OBS_PA                 0x03E00000ULL
#define SPMI_MAX_CHANNELS           512
#define SPMI_MAPPING_TABLE_REG(i)   (0x0900u + 4u * (i))
#define SPMI_OBS_STATUS(ch)         (0x10000u * (ch) + 0x08u)
#define SPMI_OBS_RDATA0(ch)         (0x10000u * (ch) + 0x18u)
#define SPMI_OBS_CMD(ch)            (0x10000u * (ch) + 0x00u)
#define SPMI_LDO_STATUS1_REG        0x08
#define SPMI_LDO_VSET_LB_REG        0x40
#define SPMI_LDO_EN_CTL_REG         0x46

#define PM6125_SID                  1u
#define PM6125_LDO9_PERIPH          0x48u
#define PM6125_LDO16_PERIPH         0x4Fu
#define PM6125_LDO17_PERIPH         0x50u
#define PM6125_LDO23_PERIPH         0x56u

/* ---- QUPv3 Wrapper & SE3 UART -------------------------------------------- */
#define QUP0_WRAP_PA                0x04AC0000ULL
#define QUP0_WRAP_SIZE              0x4000ULL
#define QUPV3_HW_VER_REG            0x04
#define QUP_SE_VERSION_2_5          0x20050000u

#define QUP0_SE3_PA                 0x04A8C000ULL
#define GENI_SE_SIZE                0x4000ULL

#define GENI_FORCE_DEFAULT_REG      0x20
#define GENI_OUTPUT_CTRL            0x24
#define GENI_CGC_CTRL               0x28
#define SE_GENI_STATUS              0x40
#define GENI_SER_M_CLK_CFG          0x48
#define GENI_SER_S_CLK_CFG          0x4C
#define GENI_FW_REVISION_RO         0x68
#define SE_GENI_CLK_SEL             0x7C
#define SE_UART_LOOPBACK_CFG        0x22C
#define SE_UART_IO_MACRO_CTRL       0x240
#define SE_GENI_BYTE_GRAN           0x254
#define SE_GENI_DMA_MODE_EN         0x258
#define SE_UART_TX_TRANS_CFG        0x25C
#define SE_GENI_TX_PACKING_CFG0     0x260
#define SE_GENI_TX_PACKING_CFG1     0x264
#define SE_UART_TX_WORD_LEN         0x268
#define SE_UART_TX_STOP_BIT_LEN     0x26C
#define SE_UART_TX_TRANS_LEN        0x270
#define SE_UART_RX_TRANS_CFG        0x280
#define SE_GENI_RX_PACKING_CFG0     0x284
#define SE_GENI_RX_PACKING_CFG1     0x288
#define SE_UART_RX_WORD_LEN         0x28C
#define SE_UART_RX_STALE_CNT        0x294
#define SE_UART_TX_PARITY_CFG       0x2A4
#define SE_UART_RX_PARITY_CFG       0x2A8
#define SE_UART_MANUAL_RFR          0x2AC
#define SE_GENI_M_CMD0              0x600
#define SE_GENI_M_CMD_CTRL_REG      0x604
#define SE_GENI_M_IRQ_STATUS        0x610
#define SE_GENI_M_IRQ_EN            0x614
#define SE_GENI_M_IRQ_CLEAR         0x618
#define SE_GENI_M_IRQ_EN_SET        0x61C
#define SE_GENI_M_IRQ_EN_CLEAR      0x620
#define SE_GENI_S_CMD0              0x630
#define SE_GENI_S_CMD_CTRL_REG      0x634
#define SE_GENI_S_IRQ_STATUS        0x640
#define SE_GENI_S_IRQ_EN            0x644
#define SE_GENI_S_IRQ_CLEAR         0x648
#define SE_GENI_S_IRQ_EN_SET        0x64C
#define SE_GENI_S_IRQ_EN_CLEAR      0x650
#define SE_GENI_TX_FIFOn            0x700
#define SE_GENI_RX_FIFOn            0x780
#define SE_GENI_TX_FIFO_STATUS      0x800
#define SE_GENI_RX_FIFO_STATUS      0x804
#define SE_GENI_TX_WATERMARK_REG    0x80C
#define SE_GENI_RX_WATERMARK_REG    0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_GENI_IOS                 0x908
#define SE_DMA_TX_IRQ_CLR           0xC44
#define SE_DMA_RX_IRQ_CLR           0xD44
#define SE_GSI_EVENT_EN             0xE18
#define SE_IRQ_EN                   0xE1C
#define SE_HW_PARAM_0               0xE24
#define SE_HW_PARAM_1               0xE28

#define GENI_PROTO_UART             2

#define SER_CLK_EN                  (1u << 0)
#define SER_CLK_DIV_SHFT            4
#define SER_CLK_DIV_MAX             0xFFFu

#define M_GENI_CMD_ACTIVE           (1u << 0)
#define M_GENI_CMD_ABORT            (1u << 1)
#define M_GENI_CMD_CANCEL           (1u << 2)
#define S_GENI_CMD_ACTIVE           (1u << 0)
#define S_GENI_CMD_ABORT            (1u << 1)
#define S_GENI_CMD_CANCEL           (1u << 2)

#define M_CMD_DONE_EN               (1u << 0)
#define M_CMD_FAILURE_EN            (1u << 3)
#define M_CMD_CANCEL_EN             (1u << 4)
#define M_CMD_ABORT_EN              (1u << 5)
#define M_TX_FIFO_WATERMARK_EN      (1u << 30)

#define S_CMD_CANCEL_EN             (1u << 4)
#define S_CMD_ABORT_EN              (1u << 5)
#define S_RX_FIFO_RD_ERR_EN         (1u << 24)
#define S_RX_FIFO_WR_ERR_EN         (1u << 25)
#define S_RX_FIFO_WATERMARK_EN      (1u << 26)
#define S_RX_FIFO_LAST_EN           (1u << 27)

#define UART_CTS_MASK               (1u << 1)
#define UART_MANUAL_RFR_EN          (1u << 31)
#define UART_RFR_NOT_READY          (1u << 1)
#define UART_RFR_READY              (1u << 0)

#define UART_START_TX               (1u << 27)
#define UART_START_READ             (1u << 27)
#define UART_PARAM_RFR_OPEN         (1u << 7)

#define RX_LAST_BIT                 (1u << 31)
#define RX_LAST_VALID_SHFT          28
#define RX_LAST_VALID_MASK          0x7u
#define RX_FIFO_WC_MASK             0x01FFFFFFu
#define TX_FIFO_WC_MASK             0x0FFFFFFFu

/* ---- QCA WCN3990 / WCN3950 EDL & HCI_IBS --------------------------------- */
#define QCA_WCN3990_POWEROFF_PULSE  0xC0
#define QCA_WCN3990_POWERON_PULSE   0xFC

#define HCI_PKT_CMD                 0x01
#define HCI_PKT_ACL                 0x02
#define HCI_PKT_SCO                 0x03
#define HCI_PKT_EVENT               0x04

#define HCI_IBS_WAKE_ACK            0xFC
#define HCI_IBS_WAKE_IND            0xFD
#define HCI_IBS_SLEEP_IND           0xFE

#define HCI_EV_CMD_COMPLETE         0x0E
#define HCI_EV_VENDOR               0xFF

#define EDL_PATCH_CMD_OPCODE        0xFC00
#define EDL_SET_BAUDRATE_OPCODE     0xFC48
#define EDL_DISABLE_LOG_OPCODE      0xFC17
#define HCI_RESET_OPCODE            0x0C03

#define EDL_PATCH_VER_REQ_CMD       0x19
#define EDL_PATCH_TLV_REQ_CMD       0x1E
#define EDL_DISABLE_LOG_SUB_OP      0x14

#define EDL_CMD_REQ_RES_EVT         0x00
#define EDL_APP_VER_RES_EVT         0x02
#define EDL_TLV_DNLD_RES_EVT        0x04
#define EDL_CMD_EXE_STATUS_EVT      0x00
#define EDL_SET_BAUDRATE_RSP_EVT    0x92

#define EDL_TAG_ID_BD_ADDR          2
#define EDL_TAG_ID_HCI              17
#define EDL_TAG_ID_DEEP_SLEEP       27
#define MAX_SIZE_PER_TLV_SEGMENT    243

#define QCA_SKIP_EVT_NONE           0
#define QCA_SKIP_EVT_VSE            1
#define QCA_SKIP_EVT_CC             2
#define QCA_SKIP_EVT_VSE_CC         3

#define QCA_BAUDRATE_115200         0
#define QCA_BAUDRATE_921600         10
#define QCA_BAUDRATE_2000000        13
#define QCA_BAUDRATE_3000000        14
#define QCA_BAUDRATE_3200000        17

#define QCA_WCN3950_SOC_PREFIX      0x40070000u
#define QCA_WCN3950_SOC_ID_T        0x40074130u
#define QCA_WCN3950_SOC_ID_S        0x40075130u

#pragma pack(push, 1)
typedef struct _QCA_EDL_EVENT_HDR {
    UCHAR Cresp;
    UCHAR Rtype;
} QCA_EDL_EVENT_HDR;

typedef struct _QCA_BTSOC_VERSION {
    ULONG  ProductId;
    USHORT PatchVer;
    USHORT RomVer;
    ULONG  SocId;
} QCA_BTSOC_VERSION;

typedef struct _QCA_TLV_HDR {
    ULONG TypeLen;
} QCA_TLV_HDR;

typedef struct _QCA_TLV_PATCH_HDR {
    ULONG  TotalSize;
    ULONG  DataLength;
    UCHAR  FormatVersion;
    UCHAR  Signature;
    UCHAR  DownloadMode;
    UCHAR  Reserved1;
    USHORT ProductId;
    USHORT RomBuild;
    USHORT PatchVersion;
    USHORT Reserved2;
    ULONG  Entry;
} QCA_TLV_PATCH_HDR;

typedef struct _QCA_TLV_NVM_HDR {
    USHORT TagId;
    USHORT TagLen;
    ULONG  Reserve1;
    ULONG  Reserve2;
} QCA_TLV_NVM_HDR;
#pragma pack(pop)
