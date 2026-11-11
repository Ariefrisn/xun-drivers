/*
 * Redmi Pad SE (xun, SM6225 "khaje") battery hardware addresses:
 *   - QUP0 SE1 I2C (0x04A84000, GPIO 4/5 WEST func 1): sc89890h/bq2589x@6A, sc2150/rt1711h@4E
 *   - QUP0 SE2 I2C (0x04A88000, GPIO 6/7 WEST func 1): sh366101@55 fuel gauge (SBS word regs)
 * Verified from sh366101_fg.ko (.rodata+0x3790), tcpc_sc2150.ko, dtbo_b.img, and live dmesg.
 */
#pragma once

/* ---- TLMM (pinctrl-khaje) ------------------------------------------------ */
#define TLMM_BASE            0x00400000ULL
#define TLMM_TILE_WEST       0x00100000ULL
#define TLMM_PIN_STRIDE      0x1000
#define TLMM_PIN_PA(tile, pin) (TLMM_BASE + (tile) + (ULONGLONG)(pin) * TLMM_PIN_STRIDE)
#define TLMM_CTL             0x0      /* pull[1:0] func[5:2] drv[8:6] oe[9] */
#define TLMM_IO              0x4      /* in[0] out[1] */
#define TLMM_PULL_NONE       0
#define TLMM_DRV_MA(ma)      (((ma) / 2) - 1)

/* ---- GCC (gcc-khaje) ----------------------------------------------------- */
#define GCC_BASE             0x01400000ULL
#define GCC_APCS_VOTE_REG    0x7900C
#define GCC_QUP0_SE1_VOTE    ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11))
#define GCC_QUP0_SE2_VOTE    ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 12))
#define GCC_QUP0_S1_CBCR     0x1F274
#define GCC_QUP0_S1_RCG      0x1F278
#define GCC_QUP0_S2_CBCR     0x1F3A4
#define GCC_QUP0_S2_RCG      0x1F3A8
#define GCC_RCG_PERF_DFSR(l) (0x1C + 4 * (l))
#define GCC_CBCR_CLK_OFF     (1u << 31)

/* ---- QUPv3 GENI SE1 & SE2 (I2C) ----------------------------------------- */
#define QUP0_SE1_BASE        0x04A84000ULL
#define QUP0_SE2_BASE        0x04A88000ULL
#define GENI_SE_SIZE         0x4000
#define QUP1_PIN_SDA         4        /* WEST, func 1 */
#define QUP1_PIN_SCL         5
#define QUP2_PIN_SDA         6        /* WEST, func 1 */
#define QUP2_PIN_SCL         7
#define QUP_I2C_FUNC         1

#define GENI_FORCE_DEFAULT_REG   0x20
#define GENI_OUTPUT_CTRL         0x24
#define GENI_CGC_CTRL            0x28
#define SE_GENI_STATUS           0x40
#define GENI_SER_M_CLK_CFG       0x48
#define GENI_FW_REVISION_RO      0x68
#define SE_GENI_CLK_SEL          0x7C
#define SE_GENI_BYTE_GRAN        0x254
#define SE_GENI_DMA_MODE_EN      0x258
#define SE_GENI_TX_PACKING_CFG0  0x260
#define SE_GENI_TX_PACKING_CFG1  0x264
#define SE_I2C_TX_TRANS_LEN      0x26C
#define SE_I2C_RX_TRANS_LEN      0x270
#define SE_I2C_SCL_COUNTERS      0x278
#define SE_GENI_RX_PACKING_CFG0  0x284
#define SE_GENI_RX_PACKING_CFG1  0x288
#define SE_GENI_M_CMD0           0x600
#define SE_GENI_M_CMD_CTRL_REG   0x604
#define SE_GENI_M_IRQ_STATUS     0x610
#define SE_GENI_M_IRQ_EN         0x614
#define SE_GENI_M_IRQ_CLEAR      0x618
#define SE_GENI_S_IRQ_CLEAR      0x648
#define SE_GENI_TX_FIFOn         0x700
#define SE_GENI_RX_FIFOn         0x780
#define SE_GENI_TX_FIFO_STATUS   0x800
#define SE_GENI_RX_FIFO_STATUS   0x804
#define SE_GENI_RX_WATERMARK_REG 0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_DMA_TX_IRQ_CLR        0xC44
#define SE_DMA_RX_IRQ_CLR        0xD44
#define SE_GSI_EVENT_EN          0xE18
#define SE_IRQ_EN                0xE1C
#define SE_HW_PARAM_0            0xE24

#define GENI_PROTO_I2C           3
#define M_GENI_CMD_ACTIVE        (1u << 0)
#define M_GENI_CMD_ABORT         (1u << 1)
#define M_GENI_CMD_CANCEL        (1u << 2)
#define M_CMD_DONE_EN            (1u << 0)
#define M_CMD_CANCEL_EN          (1u << 4)
#define M_GP_IRQ_1_EN            (1u << 10)
#define M_I2C_ERR_MASK           ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 13))
#define M_RX_FIFO_WATERMARK_EN   (1u << 26)
#define M_RX_FIFO_LAST_EN        (1u << 27)
#define TX_FIFO_WC_MASK          0x0FFFFFFFu
#define RX_FIFO_WC_MASK          0x01FFFFFFu
#define I2C_OP_WRITE             0x1
#define I2C_OP_READ              0x2
#define M_OPCODE_SHIFT           27
#define I2C_STOP_STRETCH         (1u << 2)
#define I2C_SLV_ADDR_SHIFT       9
#define I2C_400K_CLK_DIV         2
#define I2C_400K_T_HIGH          5
#define I2C_400K_T_LOW           12
#define I2C_400K_T_CYCLE         24

/* ---- SH366101 fuel gauge (@ 0x55 on QUP0 SE2, 16-bit LE SBS words) ------- */
#define FG_ADDR                  0x55
#define FG_REG_TEMP              0x02      /* 0.1 K (Celsius*10 = val - 2731) */
#define FG_REG_VOLTAGE           0x04      /* mV */
#define FG_REG_FLAGS             0x06
#define FG_REG_RM                0x0C      /* Remaining Capacity mAh */
#define FG_REG_FCC               0x0E      /* Full Charge Capacity mAh */
#define FG_REG_CURRENT           0x10      /* signed s16 mA */
#define FG_REG_CYCLE             0x1A      /* Cycle count */
#define FG_REG_SOC               0x1C      /* 0..100 % */

/* ---- SC89890H / BQ2589x charger (@ 0x6A on QUP0 SE1) --------------------- */
#define CHG_ADDR                 0x6A
#define CHG_REG00                0x00
#define CHG_REG02                0x02
#define CHG_REG03                0x03
#define CHG_REG04                0x04
#define CHG_REG06                0x06
#define CHG_REG07                0x07
#define CHG_REG0B                0x0B
#define CHG_REG0C                0x0C
#define CHG_REG0D                0x0D
#define CHG_REG0E                0x0E
#define CHG_REG0F                0x0F
#define CHG_REG11                0x11
#define CHG_REG12                0x12
#define CHG_REG13                0x13
#define CHG_REG14                0x14
#define CHG_NREGS                0x15
#define CHG_CONV_START           0x80
#define CHG_CONV_RATE            0x40
#define CHG_FORCE_DPDM           0x02
#define CHG_OTG_CONFIG           0x20
#define CHG_CHG_CONFIG           0x10
#define CHG_VBUS_STAT(r)         (((r) >> 5) & 7)
#define CHG_CHRG_STAT(r)         (((r) >> 3) & 3)
#define CHG_PG_STAT(r)           (((r) >> 2) & 1)

/* ---- SC2150 / RT1711H Type-C PD (@ 0x4E on QUP0 SE1) --------------------- */
#define TCPC_ADDR                0x4E
#define TCPC_REG_VID             0x00      /* SC2150=0x311C, RT1711H=0x29CF */
#define TCPC_REG_PID             0x02      /* SC2150=0x2150, RT1711H=0x1711 */
#define TCPC_REG_ALERT           0x10
#define TCPC_REG_ALERT_MASK      0x12
#define TCPC_REG_ROLE_CTRL       0x1A
#define TCPC_REG_TCPC_CTRL       0x19
#define TCPC_REG_POWER_CTRL      0x1C
#define TCPC_REG_CC_STATUS       0x1D
#define TCPC_REG_POWER_STATUS    0x1E
#define TCPC_REG_FAULT_STATUS    0x1F
#define TCPC_REG_COMMAND         0x23
#define TCPC_VBUS_PRESENT        0x04
#define TCPC_ROLE_SINK_RD_RD     0x0A
#define TCPC_ROLE_SRC_RP_RP      0x05
#define TCPC_ROLE_DRP_TOGGLE     0x45
#define TCPC_CMD_LOOK4CONNECTION 0x99
#define TCPC_CMD_WAKE_I2C        0x33
#define TCPC_CC_LOOKING          0x20
#define TCPC_CC_TERM_RD          0x10
#define TCPC_CC_SRC_RD           2
#define TCPC_CC1(r)              ((r) & 3)
#define TCPC_CC2(r)              (((r) >> 2) & 3)

/* RT1711H fallback registers (also 0x9E=0x80/0xA0=0x01 for SC2150 init) */
#define RT1711H_RTCTRL8          0x9B
#define RT1711H_RTCTRL8_INIT     0x2A
#define RT1711H_RTCTRL11         0x9E
#define RT1711H_RTCTRL11_INIT    0x8F
#define RT1711H_RTCTRL14         0xA1
#define RT1711H_RTCTRL14_INIT    0x0F

#define CHG_ICHG_MAX_MA          2000
#define CHG_IINLIM_MAX_MA        2000
#define CHG_VREG_MV              4400
#define CHG_VREG_HOT_MV          4100
#define CHG_ICHG_HOT_MA          1024

/* ---- battery pack (xun: 8000 mAh nominal, 3.87 V) ------------------------ */
#define BATT_DESIGN_MAH          8000
#define BATT_NOMINAL_MV          3870
