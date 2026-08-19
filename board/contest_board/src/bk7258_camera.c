/****************************************************************************
 * board/contest_board/src/bk7258_camera.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * GC2145 Phase 1 — DVP IO + sensor init + PSRAM framebuf
 *
 * Phase 0: "camera id" — reads GC2145 chip ID via SCCB bit-bang
 * Phase 1: DVP data pin mux (P29-P39), GC2145 YUYV init, PSRAM buffer
 *
 * BK7258 GPIO CFG register (per-pin, at AON_GPIO_BASE + pin*4):
 *   bit[0]   gpio_input     (RO) — reads pin level
 *   bit[1]   gpio_output    (RW) — OUTPUT VALUE: 0=low, 1=high
 *   bit[2]   input_enable   (RW) — 1=enable input buffer
 *   bit[3]   output_enable  (RW, active-low) — 0=output enabled
 *   bit[4]   pull_mode      (RW) — 1=pull-up
 *   bit[5]   pull_enable    (RW) — 1=enable pull
 *   bit[6]   second_func    (RW) — 1=peripheral function, 0=GPIO
 *
 * Open-drain SCCB:
 *   Drive low  = output_enable=0, output_value=0, second_func=0
 *   Release    = output_enable=1(disabled), input_enable=1
 *
 * DVP data pins (P29-P39):
 *   Function select = 0 (JPEG_PCLK/HSYNC/VSYNC/PXDATA0-7)
 *   CFG: output_dis, input_dis, pull_dis, second_func=1
 *
 * GC2145 init table:
 *   Ported from ARMino dvp_gc2145.c (Apache-2.0, Galois Inc)
 *   Output format: reg 0x84 = 0x02, 640x480
 *   Actual PSRAM byte order: UYVY (verified by camera dump)
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "bk7258_gpio.h"
#include "bk7258_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CIS Controller (YUV_BUF) Register Base Address
 * From ARMino yuv_buf_reg.h: SOC_YUV_BUF_REG_BASE = 0x48020000
 * SOC_ADDR_OFFSET = 0 for non-secure world.
 */

#define YUV_BUF_REG_BASE          0x48020000u

/* Register offsets (word-addressed, multiply by 4 for byte offset) */

#define YUV_BUF_REG_0x04          (YUV_BUF_REG_BASE + 0x04 * 4)  /* CTRL */
#define YUV_BUF_REG_0x05          (YUV_BUF_REG_BASE + 0x05 * 4)  /* PIXEL */
#define YUV_BUF_REG_0x08          (YUV_BUF_REG_BASE + 0x08 * 4)  /* EM_BASE_ADDR */
#define YUV_BUF_REG_0x09          (YUV_BUF_REG_BASE + 0x09 * 4)  /* INT_EN */
#define YUV_BUF_REG_0x0a          (YUV_BUF_REG_BASE + 0x0a * 4)  /* INT_STATUS */

/* CTRL register (0x04) bit fields */

#define YUV_BUF_CTRL_YUV_MODE     (1u << 0)   /* bit[0]: 1=YUV buf mode */
#define YUV_BUF_CTRL_FMT_SEL_MASK 0x6u        /* bit[2:1]: YUV format */
#define YUV_BUF_CTRL_FMT_YUYV     (0u << 1)   /* 00=YUYV */
#define YUV_BUF_CTRL_FMT_UYVY     (1u << 1)   /* 01=UYVY */
#define YUV_BUF_CTRL_FMT_YYUV     (2u << 1)   /* 10=YYUV */
#define YUV_BUF_CTRL_FMT_UVYY     (3u << 1)   /* 11=UVYY */
#define YUV_BUF_CTRL_H264_MODE    (1u << 3)   /* bit[3] */
#define YUV_BUF_CTRL_MCLK_HOLD    (1u << 4)   /* bit[4] */
#define YUV_BUF_CTRL_VCK_EDGE     (1u << 5)   /* bit[5]: PCLK edge */
#define YUV_BUF_CTRL_HSYNC_REV    (1u << 6)   /* bit[6]: invert HSYNC */
#define YUV_BUF_CTRL_VSYNC_REV    (1u << 7)   /* bit[7]: invert VSYNC */
#define YUV_BUF_CTRL_SOI_HSYNC    (1u << 8)   /* bit[8] */
#define YUV_BUF_CTRL_BPS_CIS      (1u << 9)   /* bit[9] */
#define YUV_BUF_CTRL_MEMREV       (1u << 10)  /* bit[10] */
#define YUV_BUF_CTRL_BYTE_REV     (1u << 11)  /* bit[11] */
#define YUV_BUF_CTRL_JPEG_WRD_REV (1u << 12)  /* bit[12] */
#define YUV_BUF_CTRL_PARTIAL_EN   (1u << 13)  /* bit[13] */
#define YUV_BUF_CTRL_SYNC_EDGE    (1u << 14)  /* bit[14] */
#define YUV_BUF_CTRL_MCLK_DIV_MASK 0x30000u   /* bit[17:16] */
#define YUV_BUF_CTRL_MCLK_DIV_POS  16

/* PIXEL register (0x05) bit fields */

#define YUV_BUF_PIXEL_X_MASK      0xFFu       /* bit[7:0]: x_pixel (width/8) */
#define YUV_BUF_PIXEL_Y_MASK      0xFF00u     /* bit[15:8]: y_pixel (height/8) */
#define YUV_BUF_PIXEL_Y_POS       8
#define YUV_BUF_PIXEL_BLK_MASK    0xFFFF0000u /* bit[31:16]: frame_blk */
#define YUV_BUF_PIXEL_BLK_POS     16

/* INT_EN register (0x09) bit fields */

#define YUV_BUF_INT_VSYNC_NEGE    (1u << 0)   /* VSYNC falling edge */
#define YUV_BUF_INT_YUV_ARV       (1u << 1)   /* YUV arrived (frame done) */
#define YUV_BUF_INT_SM0_WR        (1u << 2)   /* SM0 write */
#define YUV_BUF_INT_SM1_WR        (1u << 3)   /* SM1 write */
#define YUV_BUF_INT_FIFO_FULL     (1u << 4)   /* FIFO full */
#define YUV_BUF_INT_ENC_LINE_DONE (1u << 5)   /* encode line done */
#define YUV_BUF_INT_RES_ERR       (1u << 6)   /* resolution error */
#define YUV_BUF_INT_H264_ERR      (1u << 7)   /* H264 error */
#define YUV_BUF_INT_ENC_SLOW      (1u << 8)   /* encode slow */

/* INT_STATUS register (0x0a) — same bit layout as INT_EN */

#define YUV_BUF_INT_STATUS_MASK   0x1FFu      /* bits[8:0] */

/* DVP interrupt IRQ number
 * From ARMino int_types.h: INT_SRC_YUVB = 58 (for BK7258)
 * NuttX IRQ = BK7258_IRQ_EXTINT + 58 = 16 + 58 = 74
 */

#define BK7258_IRQ_YUV_BUF        (BK7258_IRQ_EXTINT + 58)

/* Module Clock Enable Register (sys_reserver_reg0xd)
 * Address: SOC_SYS_REG_BASE + 0xd * 4 = 0x44010000 + 0x34
 * Same register as CAMERA_CLK_EN_REG.
 *
 * bit[0]: h264_cken — H264 module clock (needed for YUV_BUF bus)
 * bit[3]: yuv_cken  — YUV_BUF module clock
 * bit[9]: cis_auxs_cken — CIS AUXS MCLK (already used for sensor)
 *
 * From ARMino sys_video_driver.c:yuv_buf_init_common()
 *   sys_drv_set_h264_clk_en(1);   — bit[0]
 *   sys_drv_set_yuv_buf_clk_en(1); — bit[3]
 */

#define CAMERA_H264_CLKEN_BIT     (1u << 0)
#define CAMERA_YUV_BUF_CLKEN_BIT  (1u << 3)

/* System Interrupt Enable Register (cpu1_int_32_63_en)
 * Address: SOC_SYS_REG_BASE + 0x23 * 4 = 0x44010000 + 0x8C
 *
 * bit[26]: cpu0_yuvb_int_en — YUV_BUF interrupt enable to CPU0
 *
 * From ARMino sys_types.h:
 *   YUV_BUF_INTERRUPT_CTRL_BIT = (1 << 26)
 */

#define CAMERA_SYS_INT_32_63_REG  (CAMERA_SYS_BASE + 0x8Cu)
#define CAMERA_YUVB_INT_EN_BIT    (1u << 26)

/* YUV_BUF Global Control Register (REG_0x02)
 * Address: YUV_BUF_REG_BASE + 0x02 * 4
 *
 * bit[0]: soft_reset — 0=assert, 1=deassert
 * bit[1]: clk_gate_bypass — 1=bypass clock gate
 *
 * From ARMino yuv_buf_ll.h:yuv_buf_ll_init()
 *   hw->global_ctrl.soft_reset = 0;   // assert reset
 *   hw->global_ctrl.soft_reset = 1;   // deassert reset
 *   hw->global_ctrl.clk_gate_bypass = 1; // bypass clock gate
 */

#define YUV_BUF_REG_0x02          (YUV_BUF_REG_BASE + 0x02 * 4)
#define YUV_BUF_GLOBAL_SOFT_RESET (1u << 0)
#define YUV_BUF_GLOBAL_CLK_GATE   (1u << 1)

#define CAMERA_GPIO_UNCONFIGURED  (-1)
#define CAMERA_GPIO_MIN           0

/* BK7258 QFN88 exposes GPIO0..GPIO55 (see datasheet Table 3-5 Pin
 * Multiplexing, which lists up to GPIO55).  DVP_PWR_CTL is P49 on this
 * board, so the valid range must not stop at 47.
 */

#define CAMERA_GPIO_MAX           55

/* GC2145 chip ID */

#define GC2145_REG_ID_H    0xf0
#define GC2145_REG_ID_L    0xf1
#define GC2145_ID_H        0x21
#define GC2145_ID_L        0x45
#define GC2145_SCCB_ADDR   0x3c

/* BK7258 MCLK registers */

#define CAMERA_SYS_BASE           0x44010000u
#define CAMERA_CLK_DIV_REG        (CAMERA_SYS_BASE + 0x28u)
#define CAMERA_CKSEL_AUXS_CIS     3u
#define CAMERA_CKDIV_24MHZ        19u
#define CAMERA_CKSEL_POS          15u
#define CAMERA_CKSEL_MASK         0x3u
#define CAMERA_CKDIV_POS          17u
#define CAMERA_CKDIV_MASK         0x1fu
#define CAMERA_CLK_EN_REG         (CAMERA_SYS_BASE + 0x34u)
#define CAMERA_CIS_AUXS_CKEN_BIT  (1u << 9)
#define CAMERA_GPIO_FUNC_REG      (CAMERA_SYS_BASE + 0xCCu)
#define CAMERA_P27_FUNC_SHIFT     12u
#define CAMERA_P27_FUNC_MASK      0xFu
#define CAMERA_P27_FUNC_CIS_CLK   1u

#define CAMERA_MCLK_REQUIRED_PIN  27

/* SCCB timing */

#define SCCB_HALF_PERIOD_US  5
#define SCCB_TIMEOUT_ITER    1000

/* Reserved pins */

#define CAMERA_PIN_UART0_RX   10
#define CAMERA_PIN_UART0_TX   11
#define CAMERA_PIN_SWD_CLK    20
#define CAMERA_PIN_SWD_IO     21

/* DVP data pin range — P29 through P39 (11 pins)
 * P29=PCLK, P30=HSYNC, P31=VSYNC, P32-P39=PXDATA[0:7]
 * All use function select = 0 in their respective GPIO_FUNC registers.
 */

#define DVP_PIN_FIRST   29
#define DVP_PIN_LAST    39
#define DVP_PIN_COUNT   (DVP_PIN_LAST - DVP_PIN_FIRST + 1)

/* Frame buffer parameters — YUYV 640x480 */

#define CAMERA_HRES       640
#define CAMERA_VRES       480
#define CAMERA_BPP        2   /* YUYV = 2 bytes per pixel */
#define CAMERA_FRAME_SIZE (CAMERA_HRES * CAMERA_VRES * CAMERA_BPP)
#define CAMERA_PSRAM_BASE 0x60000000u
#define CAMERA_BUF_COUNT  2

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  uint32_t cfg;
  uint32_t func;      /* saved 4-bit function field only */
  int      pin;       /* needed for read-modify-write restore */
  uintptr_t cfg_addr;
  uintptr_t func_addr;
} gpio_state_t;

typedef struct
{
  uint32_t clk_div;
  uint32_t clk_en;
  uint32_t func2431;   /* full P24-P31 function register value */
  uint32_t p27_cfg;    /* P27 GPIO CFG register value */
  bool     valid;
} mclk_state_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int g_cam_sda_pin  = CONFIG_CAMERA_GPIO_SDA;
static const int g_cam_scl_pin  = CONFIG_CAMERA_GPIO_SCL;
static const int g_cam_rst_pin  = CONFIG_CAMERA_GPIO_RST;
static const int g_cam_pwr_pin  = CONFIG_CAMERA_GPIO_PWR_CTL;
static const int g_cam_pwdn_pin = CONFIG_CAMERA_GPIO_PWDNB;
static const int g_cam_mclk_pin = CONFIG_CAMERA_MCLK_GPIO;

/* DVP data pin state — saved before dvp_io_config(), restored by dvp_io_deconfig() */

static gpio_state_t g_dvp_state[DVP_PIN_COUNT];
static bool g_dvp_pins_configed = false;

/* Frame buffer pointers into PSRAM */

static volatile uint8_t *g_camera_buf[CAMERA_BUF_COUNT] = { NULL, NULL };
static bool g_framebuf_allocated = false;

/* Streaming state — ping-pong double buffering */

#define PREVIEW_W       160
#define PREVIEW_H       160
#define PREVIEW_SIZE    (PREVIEW_W * PREVIEW_H * 2)  /* 51200 bytes */
#define PREVIEW_ADDR    (CAMERA_PSRAM_BASE + 0x12C000u)  /* 0x6012C000 */

#define STREAM_BUF_MAX  1000

static volatile bool     g_stream_active   = false;
static volatile uint32_t g_pingpong_count  = 0;
static volatile uint32_t g_drop_count      = 0;
static volatile int      g_cur_buf         = 0;
static volatile int      g_ready_buf       = -1;
static volatile int      g_busy_buf        = -1;

/* LCD functions from bk7258_gc9d01.c (declared in board header) */

extern int  bk7258_lcd_preview_init(void);
extern void bk7258_lcd_preview_deinit(void);
extern void bk7258_lcd_blit_rgb565(int panel,
                                    uint16_t x0, uint16_t y0,
                                    uint16_t w, uint16_t h,
                                    const uint8_t *rgb565);

/* DVP controller state — saved before dvp_ctrl_config(), restored by
 * dvp_ctrl_deconfig().  Only field-level RMW on shared registers.
 */

typedef struct
{
  uint32_t ctrl;        /* full CTRL register value (0x04) */
  uint32_t pixel;       /* full PIXEL register value (0x05) */
  uint32_t em_base;     /* EM_BASE_ADDR register value (0x08) */
  uint32_t int_en;      /* INT_EN register value (0x09) */
  uint32_t int_status;  /* INT_STATUS register value (0x0a) — write to clear */
  bool     valid;
} dvp_ctrl_state_t;

static dvp_ctrl_state_t g_dvp_ctrl_saved = { .valid = false };
static bool g_dvp_ctrl_configed = false;
static bool g_dvp_irq_attached = false;

/* DVP interrupt counters — incremented in ISR context */

static volatile uint32_t g_dvp_vsync_count = 0;
static volatile uint32_t g_dvp_hsync_count = 0;  /* not used yet, reserved */
static volatile uint32_t g_dvp_frame_count = 0;

/****************************************************************************
 * GC2145 Init Table (YUYV 640x480)
 *
 * Ported from ARMino dvp_gc2145.c (Apache-2.0, Galois Inc).
 * Output format: register 0x84 = 0x02.
 * Actual PSRAM byte order: UYVY (verified by camera dump).
 * Frame rate section (#if 0) excluded; drive strength included.
 ****************************************************************************/

static const uint8_t gc2145_init[][2] =
{
  {0xfe, 0xf0},
  {0xfe, 0xf0},
  {0xfe, 0xf0},
  {0xfc, 0x06},
  {0xf6, 0x00},
  {0xf7, 0x1d},
  {0xf8, 0x84},
  {0xfa, 0x00},
  {0xf9, 0xfe},
  {0xf2, 0x00},
  {0xfe, 0x00},
  {0x03, 0x04},
  {0x04, 0xe2},
  {0x09, 0x00},
  {0x0a, 0x00},
  {0x0b, 0x00},
  {0x0c, 0x00},
  {0x0d, 0x04},
  {0x0e, 0xc0},
  {0x0f, 0x06},
  {0x10, 0x52},
  {0x12, 0x2e},
  {0x17, 0x14},
  {0x18, 0x22},
  {0x19, 0x0e},
  {0x1a, 0x01},
  {0x1b, 0x4b},
  {0x1c, 0x07},
  {0x1d, 0x10},
  {0x1e, 0x88},
  {0x1f, 0x78},
  {0x20, 0x03},
  {0x21, 0x40},
  {0x22, 0xa0},
  {0x24, 0x16},
  {0x25, 0x01},
  {0x26, 0x10},
  {0x2d, 0x60},
  {0x30, 0x01},
  {0x31, 0x90},
  {0x33, 0x06},
  {0x34, 0x01},
  {0xfe, 0x00},
  {0x80, 0x7f},
  {0x81, 0x26},
  {0x82, 0xfa},
  {0x83, 0x00},
  {0x84, 0x02},  /* output format (PSRAM byte order = UYVY) */
  {0x86, 0x03},
  {0x88, 0x03},
  {0x89, 0x03},
  {0x85, 0x08},
  {0x8a, 0x00},
  {0x8b, 0x00},
  {0xb0, 0x55},
  {0xc3, 0x00},
  {0xc4, 0x80},
  {0xc5, 0x90},
  {0xc6, 0x3b},
  {0xc7, 0x46},
  {0xec, 0x06},
  {0xed, 0x04},
  {0xee, 0x60},
  {0xef, 0x90},
  {0xb6, 0x01},
  {0x90, 0x01},
  {0x91, 0x00},
  {0x92, 0x00},
  {0x93, 0x00},
  {0x94, 0x00},
  {0x95, 0x04},
  {0x96, 0xb0},
  {0x97, 0x06},
  {0x98, 0x40},
  {0xfe, 0x00},
  {0x40, 0x42},
  {0x41, 0x00},
  {0x43, 0x5b},
  {0x5e, 0x00},
  {0x5f, 0x00},
  {0x60, 0x00},
  {0x61, 0x00},
  {0x62, 0x00},
  {0x63, 0x00},
  {0x64, 0x00},
  {0x65, 0x00},
  {0x66, 0x20},
  {0x67, 0x20},
  {0x68, 0x20},
  {0x69, 0x20},
  {0x76, 0x00},
  {0x6a, 0x08},
  {0x6b, 0x08},
  {0x6c, 0x08},
  {0x6d, 0x08},
  {0x6e, 0x08},
  {0x6f, 0x08},
  {0x70, 0x08},
  {0x71, 0x08},
  {0x76, 0x00},
  {0x72, 0xf0},
  {0x7e, 0x3c},
  {0x7f, 0x00},
  {0xfe, 0x02},
  {0x48, 0x15},
  {0x49, 0x00},
  {0x4b, 0x0b},
  {0xfe, 0x00},
  {0xfe, 0x01},
  {0x01, 0x04},
  {0x02, 0xc0},
  {0x03, 0x04},
  {0x04, 0x90},
  {0x05, 0x30},
  {0x06, 0x90},
  {0x07, 0x30},
  {0x08, 0x80},
  {0x09, 0x00},
  {0x0a, 0x82},
  {0x0b, 0x11},
  {0x0c, 0x10},
  {0x11, 0x10},
  {0x13, 0x7b},
  {0x17, 0x00},
  {0x1c, 0x11},
  {0x1e, 0x61},
  {0x1f, 0x35},
  {0x20, 0x40},
  {0x22, 0x40},
  {0x23, 0x20},
  {0xfe, 0x02},
  {0x0f, 0x04},
  {0xfe, 0x01},
  {0x12, 0x35},
  {0x15, 0xb0},
  {0x10, 0x31},
  {0x3e, 0x28},
  {0x3f, 0xb0},
  {0x40, 0x90},
  {0x41, 0x0f},
  {0xfe, 0x02},
  {0x90, 0x6c},
  {0x91, 0x03},
  {0x92, 0xcb},
  {0x94, 0x33},
  {0x95, 0x84},
  {0x97, 0x65},
  {0xa2, 0x11},
  {0xfe, 0x00},
  {0xfe, 0x02},
  {0x80, 0xc1},
  {0x81, 0x08},
  {0x82, 0x05},
  {0x83, 0x08},
  {0x84, 0x0a},
  {0x86, 0xf0},
  {0x87, 0x50},
  {0x88, 0x15},
  {0x89, 0xb0},
  {0x8a, 0x30},
  {0x8b, 0x10},
  {0xfe, 0x01},
  {0x21, 0x04},
  {0xfe, 0x02},
  {0xa3, 0x50},
  {0xa4, 0x20},
  {0xa5, 0x40},
  {0xa6, 0x80},
  {0xab, 0x40},
  {0xae, 0x0c},
  {0xb3, 0x46},
  {0xb4, 0x64},
  {0xb6, 0x38},
  {0xb7, 0x01},
  {0xb9, 0x2b},
  {0x3c, 0x04},
  {0x3d, 0x15},
  {0x4b, 0x06},
  {0x4c, 0x20},
  {0xfe, 0x00},
  {0xfe, 0x02},
  {0x10, 0x09},
  {0x11, 0x0d},
  {0x12, 0x13},
  {0x13, 0x19},
  {0x14, 0x27},
  {0x15, 0x37},
  {0x16, 0x45},
  {0x17, 0x53},
  {0x18, 0x69},
  {0x19, 0x7d},
  {0x1a, 0x8f},
  {0x1b, 0x9d},
  {0x1c, 0xa9},
  {0x1d, 0xbd},
  {0x1e, 0xcd},
  {0x1f, 0xd9},
  {0x20, 0xe3},
  {0x21, 0xea},
  {0x22, 0xef},
  {0x23, 0xf5},
  {0x24, 0xf9},
  {0x25, 0xff},
  {0xfe, 0x00},
  {0xc6, 0x20},
  {0xc7, 0x2b},
  {0xfe, 0x02},
  {0x26, 0x0f},
  {0x27, 0x14},
  {0x28, 0x19},
  {0x29, 0x1e},
  {0x2a, 0x27},
  {0x2b, 0x33},
  {0x2c, 0x3b},
  {0x2d, 0x45},
  {0x2e, 0x59},
  {0x2f, 0x69},
  {0x30, 0x7c},
  {0x31, 0x89},
  {0x32, 0x98},
  {0x33, 0xae},
  {0x34, 0xc0},
  {0x35, 0xcf},
  {0x36, 0xda},
  {0x37, 0xe2},
  {0x38, 0xe9},
  {0x39, 0xf3},
  {0x3a, 0xf9},
  {0x3b, 0xff},
  {0xfe, 0x02},
  {0xd1, 0x32},
  {0xd2, 0x32},
  {0xd3, 0x40},
  {0xd6, 0xf0},
  {0xd7, 0x10},
  {0xd8, 0xda},
  {0xdd, 0x14},
  {0xde, 0x86},
  {0xed, 0x80},
  {0xee, 0x00},
  {0xef, 0x3f},
  {0xd8, 0xd8},
  {0xfe, 0x01},
  {0x9f, 0x40},
  {0xfe, 0x01},
  {0xc2, 0x14},
  {0xc3, 0x0d},
  {0xc4, 0x0c},
  {0xc8, 0x15},
  {0xc9, 0x0d},
  {0xca, 0x0a},
  {0xbc, 0x24},
  {0xbd, 0x10},
  {0xbe, 0x0b},
  {0xb6, 0x25},
  {0xb7, 0x16},
  {0xb8, 0x15},
  {0xc5, 0x00},
  {0xc6, 0x00},
  {0xc7, 0x00},
  {0xcb, 0x00},
  {0xcc, 0x00},
  {0xcd, 0x00},
  {0xbf, 0x07},
  {0xc0, 0x00},
  {0xc1, 0x00},
  {0xb9, 0x00},
  {0xba, 0x00},
  {0xbb, 0x00},
  {0xaa, 0x01},
  {0xab, 0x01},
  {0xac, 0x00},
  {0xad, 0x05},
  {0xae, 0x06},
  {0xaf, 0x0e},
  {0xb0, 0x0b},
  {0xb1, 0x07},
  {0xb2, 0x06},
  {0xb3, 0x17},
  {0xb4, 0x0e},
  {0xb5, 0x0e},
  {0xd0, 0x09},
  {0xd1, 0x00},
  {0xd2, 0x00},
  {0xd6, 0x08},
  {0xd7, 0x00},
  {0xd8, 0x00},
  {0xd9, 0x00},
  {0xda, 0x00},
  {0xdb, 0x00},
  {0xd3, 0x0a},
  {0xd4, 0x00},
  {0xd5, 0x00},
  {0xa4, 0x00},
  {0xa5, 0x00},
  {0xa6, 0x77},
  {0xa7, 0x77},
  {0xa8, 0x77},
  {0xa9, 0x77},
  {0xa1, 0x80},
  {0xa2, 0x80},
  {0xfe, 0x01},
  {0xdf, 0x0d},
  {0xdc, 0x25},
  {0xdd, 0x30},
  {0xe0, 0x77},
  {0xe1, 0x80},
  {0xe2, 0x77},
  {0xe3, 0x90},
  {0xe6, 0x90},
  {0xe7, 0xa0},
  {0xe8, 0x90},
  {0xe9, 0xa0},
  {0xfe, 0x00},
  {0xfe, 0x01},
  {0x4f, 0x00},
  {0x4f, 0x00},
  {0x4b, 0x01},
  {0x4f, 0x00},
  {0x4c, 0x01},
  {0x4d, 0x71},
  {0x4e, 0x01},
  {0x4c, 0x01},
  {0x4d, 0x91},
  {0x4e, 0x01},
  {0x4c, 0x01},
  {0x4d, 0x70},
  {0x4e, 0x01},
  {0x4c, 0x01},
  {0x4d, 0x90},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0xb0},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0x8f},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0x6f},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0xaf},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0xd0},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0xf0},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0xcf},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0xef},
  {0x4e, 0x02},
  {0x4c, 0x01},
  {0x4d, 0x6e},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x8e},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xae},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xce},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x4d},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x6d},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x8d},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xad},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xcd},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x4c},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x6c},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x8c},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xac},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xcc},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xcb},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x4b},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x6b},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x8b},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0xab},
  {0x4e, 0x03},
  {0x4c, 0x01},
  {0x4d, 0x8a},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0xaa},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0xca},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0xca},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0xc9},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0x8a},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0x89},
  {0x4e, 0x04},
  {0x4c, 0x01},
  {0x4d, 0xa9},
  {0x4e, 0x04},
  {0x4c, 0x02},
  {0x4d, 0x0b},
  {0x4e, 0x05},
  {0x4c, 0x02},
  {0x4d, 0x0a},
  {0x4e, 0x05},
  {0x4c, 0x01},
  {0x4d, 0xeb},
  {0x4e, 0x05},
  {0x4c, 0x01},
  {0x4d, 0xea},
  {0x4e, 0x05},
  {0x4c, 0x02},
  {0x4d, 0x09},
  {0x4e, 0x05},
  {0x4c, 0x02},
  {0x4d, 0x29},
  {0x4e, 0x05},
  {0x4c, 0x02},
  {0x4d, 0x2a},
  {0x4e, 0x05},
  {0x4c, 0x02},
  {0x4d, 0x4a},
  {0x4e, 0x05},
  {0x4c, 0x02},
  {0x4d, 0x8a},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0x49},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0x69},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0x89},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0xa9},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0x48},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0x68},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0x69},
  {0x4e, 0x06},
  {0x4c, 0x02},
  {0x4d, 0xca},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xc9},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xe9},
  {0x4e, 0x07},
  {0x4c, 0x03},
  {0x4d, 0x09},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xc8},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xe8},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xa7},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xc7},
  {0x4e, 0x07},
  {0x4c, 0x02},
  {0x4d, 0xe7},
  {0x4e, 0x07},
  {0x4c, 0x03},
  {0x4d, 0x07},
  {0x4e, 0x07},
  {0x4f, 0x01},
  {0x50, 0x80},
  {0x51, 0xa8},
  {0x52, 0x47},
  {0x53, 0x38},
  {0x54, 0xc7},
  {0x56, 0x0e},
  {0x58, 0x08},
  {0x5b, 0x00},
  {0x5c, 0x74},
  {0x5d, 0x8b},
  {0x61, 0xdb},
  {0x62, 0xb8},
  {0x63, 0x86},
  {0x64, 0xc0},
  {0x65, 0x04},
  {0x67, 0xa8},
  {0x68, 0xb0},
  {0x69, 0x00},
  {0x6a, 0xa8},
  {0x6b, 0xb0},
  {0x6c, 0xaf},
  {0x6d, 0x8b},
  {0x6e, 0x50},
  {0x6f, 0x18},
  {0x73, 0xf0},
  {0x70, 0x0d},
  {0x71, 0x60},
  {0x72, 0x80},
  {0x74, 0x01},
  {0x75, 0x01},
  {0x7f, 0x0c},
  {0x76, 0x70},
  {0x77, 0x58},
  {0x78, 0xa0},
  {0x79, 0x5e},
  {0x7a, 0x54},
  {0x7b, 0x58},
  {0xfe, 0x00},
  {0xfe, 0x02},
  {0xc0, 0x01},
  {0xc1, 0x44},
  {0xc2, 0xfd},
  {0xc3, 0x04},
  {0xc4, 0xF0},
  {0xc5, 0x48},
  {0xc6, 0xfd},
  {0xc7, 0x46},
  {0xc8, 0xfd},
  {0xc9, 0x02},
  {0xca, 0xe0},
  {0xcb, 0x45},
  {0xcc, 0xec},
  {0xcd, 0x48},
  {0xce, 0xf0},
  {0xcf, 0xf0},
  {0xe3, 0x0c},
  {0xe4, 0x4b},
  {0xe5, 0xe0},
  {0xfe, 0x01},
  {0x9f, 0x40},
  {0xfe, 0x00},
  {0xfe, 0x00},
  {0xf2, 0x0f},
  {0xfe, 0x02},
  {0x40, 0xbf},
  {0x46, 0xcf},
  {0xfe, 0x00},
  {0xfe, 0x00},
  {0x24, 0xff},
  {0xfe, 0x00},
};

/****************************************************************************
 * GC2145 640x480 Resolution Table
 *
 * Sets crop window, output window, and clock divider for 640x480 output.
 ****************************************************************************/

static const uint8_t gc2145_640x480[][2] =
{
  {0xfe, 0x00},
  {0x05, 0x01},
  {0x06, 0x56},
  {0x07, 0x00},
  {0x08, 0x32},
  {0xfe, 0x01},
  {0x25, 0x00},
  {0x26, 0xfa},
  {0x27, 0x04},
  {0x28, 0xe2},
  {0x29, 0x04},
  {0x2a, 0xe2},
  {0x2b, 0x04},
  {0x2c, 0xe2},
  {0x2d, 0x04},
  {0x2e, 0xe2},
  {0xfe, 0x00},
  {0xfe, 0x00},
  {0xfe, 0x00},
  {0xf8, 0x85},
  {0xfa, 0x00},
  {0xfe, 0x00},
  {0x09, 0x00},
  {0x0a, 0x78},
  {0x0b, 0x00},
  {0x0c, 0xA0},
  {0x0d, 0x03},
  {0x0e, 0xd0},
  {0x0f, 0x05},
  {0x10, 0x10},
  {0xfd, 0x01},
  {0x90, 0x01},
  {0x91, 0x00},
  {0x92, 0x00},
  {0x93, 0x00},
  {0x94, 0x00},
  {0x95, 0x01},
  {0x96, 0xe0},
  {0x97, 0x02},
  {0x98, 0x80},
};

/****************************************************************************
 * GPIO Helpers
 *
 * register: CFG = AON_GPIO_BASE + pin*4
 *   bit[1] = output value (0=low, 1=high)
 *   bit[3] = output enable (active-low: 0=enabled)
 *   bit[2] = input enable
 *   bit[6] = second function
 ****************************************************************************/

static inline bool gpio_is_valid(int pin)
{
  return pin >= CAMERA_GPIO_MIN && pin <= CAMERA_GPIO_MAX;
}

static void gpio_save_state(int pin, gpio_state_t *s)
{
  int shift;
  uint32_t func_raw;

  s->pin       = pin;
  s->cfg_addr  = BK7258_GPIO_CFG(pin);
  s->func_addr = BK7258_SYS_GPIO_FUNC(pin);
  s->cfg       = getreg32(s->cfg_addr);

  /* Save only this pin's 4-bit function field */

  func_raw = getreg32(s->func_addr);
  shift    = (pin & 7) * 4;
  s->func  = (func_raw >> shift) & 0xFu;
}

static void gpio_restore_state(const gpio_state_t *s)
{
  int shift;
  uint32_t func_val;

  putreg32(s->cfg, s->cfg_addr);

  /* Read-modify-write: only restore this pin's 4-bit function field */

  shift    = (s->pin & 7) * 4;
  func_val = getreg32(s->func_addr);
  func_val &= ~(0xFu << shift);
  func_val |= (s->func & 0xFu) << shift;
  putreg32(func_val, s->func_addr);
}

/****************************************************************************
 * Name: gpio_drive_low
 *   Output value=0, output enabled, GPIO mode.
 ****************************************************************************/

static void gpio_drive_low(int pin)
{
  uintptr_t addr = BK7258_GPIO_CFG(pin);
  uint32_t cfg = getreg32(addr);

  cfg &= ~GPIO_CFG_SECOND_FUNC;  /* bit6=0: GPIO mode */
  cfg &= ~GPIO_CFG_OUTPUT;       /* bit1=0: output value = LOW */
  cfg &= ~GPIO_CFG_OUTPUT_EN;    /* bit3=0: output enabled (active-low) */

  putreg32(cfg, addr);
}

/****************************************************************************
 * Name: gpio_set_output_high
 *   Output value=1, output enabled, GPIO mode.
 ****************************************************************************/

static void gpio_set_output_high(int pin)
{
  uintptr_t addr = BK7258_GPIO_CFG(pin);
  uint32_t cfg = getreg32(addr);

  cfg &= ~GPIO_CFG_SECOND_FUNC;  /* bit6=0: GPIO mode */
  cfg |=  GPIO_CFG_OUTPUT;       /* bit1=1: output value = HIGH */
  cfg &= ~GPIO_CFG_OUTPUT_EN;    /* bit3=0: output enabled (active-low) */

  putreg32(cfg, addr);
}

/****************************************************************************
 * Name: gpio_release
 *   Output disabled, input enabled, GPIO mode (high-Z).
 ****************************************************************************/

static void gpio_release(int pin)
{
  uintptr_t addr = BK7258_GPIO_CFG(pin);
  uint32_t cfg = getreg32(addr);

  cfg &= ~GPIO_CFG_SECOND_FUNC;  /* bit6=0: GPIO mode */
  cfg &= ~GPIO_CFG_OUTPUT;       /* bit1=0: value=0 (irrelevant when out dis) */
  cfg |=  GPIO_CFG_OUTPUT_EN;    /* bit3=1: output DISABLED (active-low) */
  cfg |=  GPIO_CFG_INPUT_EN;     /* bit2=1: input enabled */

  putreg32(cfg, addr);
}

static bool gpio_read_input(int pin)
{
  return (getreg32(BK7258_GPIO_CFG(pin)) & GPIO_CFG_INPUT) != 0;
}

/****************************************************************************
 * SCCB Bit-Bang
 ****************************************************************************/

static void sccb_sda_low(void)     { gpio_drive_low(g_cam_sda_pin); }
static void sccb_sda_release(void) { gpio_release(g_cam_sda_pin); }
static void sccb_scl_low(void)     { gpio_drive_low(g_cam_scl_pin); }
static void sccb_scl_release(void) { gpio_release(g_cam_scl_pin); }
static bool sccb_sda_read(void)    { return gpio_read_input(g_cam_sda_pin); }

static int sccb_scl_wait_high(void)
{
  int timeout = SCCB_TIMEOUT_ITER;
  while (timeout-- > 0)
    {
      if (gpio_read_input(g_cam_scl_pin)) return 0;
      up_udelay(SCCB_HALF_PERIOD_US);
    }
  syslog(LOG_ERR, "[camera] SCL stuck low\n");
  return -ETIMEDOUT;
}

static int sccb_start(void)
{
  int ret;

  sccb_sda_release();
  sccb_scl_release();
  up_udelay(SCCB_HALF_PERIOD_US);

  /* Bus-free check */

  if (!gpio_read_input(g_cam_scl_pin))
    {
      ret = sccb_scl_wait_high();
      if (ret < 0) return ret;
    }

  if (!gpio_read_input(g_cam_sda_pin))
    {
      /* SDA stuck low — 9-clock recovery */

      int i;
      for (i = 0; i < 9; i++)
        {
          sccb_scl_low();
          up_udelay(SCCB_HALF_PERIOD_US);
          sccb_scl_release();
          ret = sccb_scl_wait_high();
          if (ret < 0) return ret;
          up_udelay(SCCB_HALF_PERIOD_US);
          if (gpio_read_input(g_cam_sda_pin)) break;
        }
      if (!gpio_read_input(g_cam_sda_pin))
        {
          syslog(LOG_ERR, "[camera] SDA stuck low — recovery failed\n");
          return -EIO;
        }
    }

  /* START: SDA falls while SCL high */

  sccb_sda_low();
  up_udelay(SCCB_HALF_PERIOD_US);
  sccb_scl_low();
  up_udelay(SCCB_HALF_PERIOD_US);

  return 0;
}

static int sccb_stop(void)
{
  int ret;

  sccb_sda_low();
  up_udelay(SCCB_HALF_PERIOD_US);

  sccb_scl_release();
  ret = sccb_scl_wait_high();
  if (ret < 0) return ret;

  up_udelay(SCCB_HALF_PERIOD_US);

  sccb_sda_release();
  up_udelay(SCCB_HALF_PERIOD_US);

  return 0;
}

static int sccb_write_byte(uint8_t byte)
{
  int i, ret;
  bool ack;

  for (i = 7; i >= 0; i--)
    {
      if ((byte >> i) & 1) sccb_sda_release();
      else                 sccb_sda_low();

      up_udelay(SCCB_HALF_PERIOD_US);

      sccb_scl_release();
      ret = sccb_scl_wait_high();
      if (ret < 0) return ret;
      up_udelay(SCCB_HALF_PERIOD_US);

      sccb_scl_low();
      up_udelay(SCCB_HALF_PERIOD_US);
    }

  /* ACK clock */

  sccb_sda_release();
  up_udelay(SCCB_HALF_PERIOD_US);

  sccb_scl_release();
  ret = sccb_scl_wait_high();
  if (ret < 0) return ret;
  up_udelay(SCCB_HALF_PERIOD_US);

  ack = !sccb_sda_read();

  sccb_scl_low();
  up_udelay(SCCB_HALF_PERIOD_US);

  return ack ? 0 : -EIO;
}

static int sccb_read_byte(bool ack)
{
  uint8_t byte = 0;
  int i, ret;

  sccb_sda_release();

  for (i = 7; i >= 0; i--)
    {
      up_udelay(SCCB_HALF_PERIOD_US);
      sccb_scl_release();
      ret = sccb_scl_wait_high();
      if (ret < 0) return ret;
      up_udelay(SCCB_HALF_PERIOD_US);

      if (sccb_sda_read()) byte |= (1u << i);

      sccb_scl_low();
      up_udelay(SCCB_HALF_PERIOD_US);
    }

  if (ack) sccb_sda_low();
  else     sccb_sda_release();

  up_udelay(SCCB_HALF_PERIOD_US);

  sccb_scl_release();
  ret = sccb_scl_wait_high();
  if (ret < 0) return ret;
  up_udelay(SCCB_HALF_PERIOD_US);

  sccb_scl_low();
  up_udelay(SCCB_HALF_PERIOD_US);
  sccb_sda_release();

  return (int)byte;
}

static int sccb_read_reg(uint8_t reg_addr)
{
  int ret, value, stop_ret;

  /* Phase 1: write address */

  ret = sccb_start();
  if (ret < 0) return ret;

  ret = sccb_write_byte((uint8_t)(GC2145_SCCB_ADDR << 1) | 0);
  if (ret < 0) goto stop_fail;

  ret = sccb_write_byte(reg_addr);
  if (ret < 0) goto stop_fail;

  stop_ret = sccb_stop();
  if (stop_ret < 0) return stop_ret;

  /* Phase 2: read value */

  ret = sccb_start();
  if (ret < 0) return ret;

  ret = sccb_write_byte((uint8_t)(GC2145_SCCB_ADDR << 1) | 1);
  if (ret < 0) goto stop_fail;

  value = sccb_read_byte(false);
  if (value < 0) { ret = value; goto stop_fail; }

  stop_ret = sccb_stop();
  if (stop_ret < 0) return stop_ret;

  return value;

stop_fail:
  stop_ret = sccb_stop();
  return (stop_ret < 0) ? stop_ret : ret;
}

/****************************************************************************
 * Name: sccb_write_reg
 *   Write a single 8-bit value to a GC2145 register via SCCB.
 *   Same error handling quality as sccb_read_reg().
 ****************************************************************************/

static int sccb_write_reg(uint8_t reg_addr, uint8_t val)
{
  int ret, stop_ret;

  ret = sccb_start();
  if (ret < 0) return ret;

  ret = sccb_write_byte((uint8_t)(GC2145_SCCB_ADDR << 1) | 0);
  if (ret < 0) goto stop_fail;

  ret = sccb_write_byte(reg_addr);
  if (ret < 0) goto stop_fail;

  ret = sccb_write_byte(val);
  if (ret < 0) goto stop_fail;

  stop_ret = sccb_stop();
  if (stop_ret < 0) return stop_ret;

  return 0;

stop_fail:
  stop_ret = sccb_stop();
  return (stop_ret < 0) ? stop_ret : ret;
}

/****************************************************************************
 * MCLK
 *
 * mclk_enable_24mhz modifies 4 registers:
 *   1. GPIO FUNC (shared P24-P31) — only P27 bits
 *   2. P27 GPIO CFG — set second_func=1 (bit6)
 *   3. CLK DIV — cksel + ckdiv
 *   4. CLK EN — enable gate
 *
 * Restore uses read-modify-write on shared registers.
 ****************************************************************************/

static void mclk_save_state(mclk_state_t *ms)
{
  uint32_t val;

  /* Save only the MCLK-relevant bit fields from shared registers */

  val = getreg32(CAMERA_CLK_DIV_REG);
  ms->clk_div = val & ((CAMERA_CKSEL_MASK << CAMERA_CKSEL_POS) |
                        (CAMERA_CKDIV_MASK << CAMERA_CKDIV_POS));

  val = getreg32(CAMERA_CLK_EN_REG);
  ms->clk_en = val & CAMERA_CIS_AUXS_CKEN_BIT;

  val = getreg32(CAMERA_GPIO_FUNC_REG);
  ms->func2431 = val & (CAMERA_P27_FUNC_MASK << CAMERA_P27_FUNC_SHIFT);

  ms->p27_cfg = getreg32(BK7258_GPIO_CFG(CAMERA_MCLK_REQUIRED_PIN));
  ms->valid   = true;
}

static void mclk_restore_state(const mclk_state_t *ms)
{
  uint32_t val;

  if (!ms->valid) return;

  /* 1. Gate OFF first — avoid glitch while source/divider changes */

  val = getreg32(CAMERA_CLK_EN_REG);
  val &= ~CAMERA_CIS_AUXS_CKEN_BIT;
  putreg32(val, CAMERA_CLK_EN_REG);

  /* 2. Restore cksel/ckdiv — read-modify-write, safe while gate off */

  val = getreg32(CAMERA_CLK_DIV_REG);
  val &= ~((CAMERA_CKSEL_MASK << CAMERA_CKSEL_POS) |
            (CAMERA_CKDIV_MASK << CAMERA_CKDIV_POS));
  val |= ms->clk_div;
  putreg32(val, CAMERA_CLK_DIV_REG);

  /* 3. Restore P27 function select — read-modify-write FUNC reg */

  val = getreg32(CAMERA_GPIO_FUNC_REG);
  val &= ~(CAMERA_P27_FUNC_MASK << CAMERA_P27_FUNC_SHIFT);
  val |= ms->func2431;
  putreg32(val, CAMERA_GPIO_FUNC_REG);

  /* 4. Restore P27 GPIO CFG (direction/latch/pull) — per-pin, safe */

  putreg32(ms->p27_cfg, BK7258_GPIO_CFG(CAMERA_MCLK_REQUIRED_PIN));

  /* 5. Restore saved gate state — read-modify-write CLK_EN */

  val = getreg32(CAMERA_CLK_EN_REG);
  val &= ~CAMERA_CIS_AUXS_CKEN_BIT;
  val |= ms->clk_en;
  putreg32(val, CAMERA_CLK_EN_REG);
}

static void mclk_enable_24mhz(void)
{
  uint32_t val;

  /* 1. Gate OFF — avoid glitch while source/divider changes */

  val = getreg32(CAMERA_CLK_EN_REG);
  val &= ~CAMERA_CIS_AUXS_CKEN_BIT;
  putreg32(val, CAMERA_CLK_EN_REG);

  /* 2. Unmap P27 — clear function select (ARMino: gpio_dev_unmap) */

  val = getreg32(CAMERA_GPIO_FUNC_REG);
  val &= ~(CAMERA_P27_FUNC_MASK << CAMERA_P27_FUNC_SHIFT);
  putreg32(val, CAMERA_GPIO_FUNC_REG);

  /* 3. Remap P27 → CIS AUXS clock (ARMino: gpio_dev_map) */

  val = getreg32(CAMERA_GPIO_FUNC_REG);
  val |= (CAMERA_P27_FUNC_CIS_CLK << CAMERA_P27_FUNC_SHIFT);
  putreg32(val, CAMERA_GPIO_FUNC_REG);

  /* 4. Prep P27 GPIO: disable output/input/pull before switching function
   *    (ARMino: gpio_hal_func_map → gpio_hal_set_pull/ output_dis / input_dis)
   *    Saved in mclk_save_state(), restored in mclk_restore_state().
   */

  val = getreg32(BK7258_GPIO_CFG(CAMERA_MCLK_REQUIRED_PIN));
  val |=  GPIO_CFG_OUTPUT_EN;    /* bit3=1: output DISABLED (active-low) */
  val &= ~GPIO_CFG_INPUT_EN;     /* bit2=0: input disabled */
  val &= ~GPIO_CFG_PULL_EN;      /* bit5=0: pull disabled */
  val |=  GPIO_CFG_SECOND_FUNC;  /* bit6=1: peripheral function */
  putreg32(val, BK7258_GPIO_CFG(CAMERA_MCLK_REQUIRED_PIN));

  /* 5. Set clock source and divider — read-modify-write
   *    (ARMino: sys_drv_set_auxs_cis(3, 19))
   */

  val = getreg32(CAMERA_CLK_DIV_REG);
  val &= ~((CAMERA_CKSEL_MASK << CAMERA_CKSEL_POS) |
           (CAMERA_CKDIV_MASK << CAMERA_CKDIV_POS));
  val |= (CAMERA_CKSEL_AUXS_CIS << CAMERA_CKSEL_POS);
  val |= (CAMERA_CKDIV_24MHZ    << CAMERA_CKDIV_POS);
  putreg32(val, CAMERA_CLK_DIV_REG);

  /* 6. Enable clock gate — read-modify-write
   *    (ARMino: sys_drv_set_cis_auxs_clk_en(1))
   */

  val = getreg32(CAMERA_CLK_EN_REG);
  val |= CAMERA_CIS_AUXS_CKEN_BIT;
  putreg32(val, CAMERA_CLK_EN_REG);
}

/****************************************************************************
 * Power Control
 ****************************************************************************/

static void camera_power_on(gpio_state_t *pwr_saved, bool *pwr_ok,
                            gpio_state_t *pwdn_saved, bool *pwdn_ok)
{
  if (g_cam_pwr_pin != CAMERA_GPIO_UNCONFIGURED)
    {
      gpio_save_state(g_cam_pwr_pin, pwr_saved);
      *pwr_ok = true;

#ifdef CONFIG_CAMERA_PWR_ACTIVE_HIGH
      gpio_set_output_high(g_cam_pwr_pin);
#else
      gpio_drive_low(g_cam_pwr_pin);
#endif
      up_udelay(5000);
    }

  if (g_cam_pwdn_pin != CAMERA_GPIO_UNCONFIGURED)
    {
      gpio_save_state(g_cam_pwdn_pin, pwdn_saved);
      *pwdn_ok = true;

#ifdef CONFIG_CAMERA_PWDNB_ACTIVE_HIGH
      /* high=standby → drive LOW to exit standby */

      gpio_drive_low(g_cam_pwdn_pin);
#else
      /* low=standby → drive HIGH to exit standby */

      gpio_set_output_high(g_cam_pwdn_pin);
#endif
      up_udelay(1000);
    }
}

/****************************************************************************
 * Reset
 ****************************************************************************/

static void camera_reset(void)
{
#ifdef CONFIG_CAMERA_RST_ACTIVE_LOW
  gpio_drive_low(g_cam_rst_pin);
  up_udelay(10000);
  gpio_release(g_cam_rst_pin);
  up_udelay(5000);
#else
  gpio_set_output_high(g_cam_rst_pin);
  up_udelay(10000);
  gpio_drive_low(g_cam_rst_pin);
  up_udelay(5000);
#endif
}

/****************************************************************************
 * DVP Data Pin Mux (P29-P39)
 *
 * Configure 11 DVP data pins for camera use:
 *   P29=PCLK, P30=HSYNC, P31=VSYNC, P32-P39=PXDATA[0:7]
 *
 * Each pin:
 *   1. Save full CFG + 4-bit function field (for restore)
 *   2. Set function select = 0 (JPEG/DVP function)
 *   3. Set CFG: output_dis, input_dis, pull_dis, second_func=1
 *
 * Function select register layout (shared per 8 pins):
 *   P24-P31: SYS_BASE + 0xCC  (P27=MCLK uses func=1, others=0)
 *   P32-P39: SYS_BASE + 0xD0  (all use func=0)
 ****************************************************************************/

static int dvp_io_config(void)
{
  int i;
  uint32_t val;

  for (i = 0; i < DVP_PIN_COUNT; i++)
    {
      int pin = DVP_PIN_FIRST + i;

      /* Save CFG + function field for restore */

      gpio_save_state(pin, &g_dvp_state[i]);

      /* Set function select = 0 via field-level RMW */

      val = getreg32(BK7258_SYS_GPIO_FUNC(pin));
      val &= ~(0xFu << ((pin & 7) * 4));  /* clear function field */
      putreg32(val, BK7258_SYS_GPIO_FUNC(pin));

      /* Set CFG: disable output, disable input, disable pull, enable
       * second function (ARMino gpio_hal_func_map prep sequence).
       */

      val = getreg32(BK7258_GPIO_CFG(pin));
      val |=  GPIO_CFG_OUTPUT_EN;    /* bit3=1: output DISABLED (active-low) */
      val &= ~GPIO_CFG_INPUT_EN;     /* bit2=0: input disabled */
      val &= ~GPIO_CFG_PULL_EN;      /* bit5=0: pull disabled */
      val |=  GPIO_CFG_SECOND_FUNC;  /* bit6=1: peripheral function */
      putreg32(val, BK7258_GPIO_CFG(pin));
    }

  g_dvp_pins_configed = true;
  return 0;
}

static void dvp_io_deconfig(void)
{
  int i;

  if (!g_dvp_pins_configed) return;

  /* Restore in reverse order */

  for (i = DVP_PIN_COUNT - 1; i >= 0; i--)
    {
      gpio_restore_state(&g_dvp_state[i]);
    }

  g_dvp_pins_configed = false;
}

/****************************************************************************
 * GC2145 Write Table
 *
 * Write a register table to GC2145 via SCCB.
 * Reports NACK with register address and entry index for debugging.
 ****************************************************************************/

static int gc2145_write_table(const uint8_t (*table)[2], int count)
{
  int i, ret;

  for (i = 0; i < count; i++)
    {
      ret = sccb_write_reg(table[i][0], table[i][1]);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "[camera] table[%d] reg 0x%02X NACK (err=%d)\n",
                 i, table[i][0], ret);
          return ret;
        }
    }

  return 0;
}

/****************************************************************************
 * PSRAM Frame Buffer
 *
 * Allocate two ping-pong frame buffers in PSRAM at 0x60000000.
 * Each buffer is 640 * 480 * 2 = 614400 bytes (YUYV).
 ****************************************************************************/

static int camera_framebuf_alloc(void)
{
  int ret;

  if (g_framebuf_allocated)
    {
      syslog(LOG_INFO, "[camera] Framebuf already allocated\n");
      return 0;
    }

  ret = bk7258_psram_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] PSRAM init failed: %d\n", ret);
      return ret;
    }

  g_camera_buf[0] = (volatile uint8_t *)(CAMERA_PSRAM_BASE);
  g_camera_buf[1] = (volatile uint8_t *)(CAMERA_PSRAM_BASE + CAMERA_FRAME_SIZE);
  g_framebuf_allocated = true;

  syslog(LOG_INFO,
         "[camera] Framebuf: buf0=0x%08lX buf1=0x%08lX size=%d\n",
         (unsigned long)g_camera_buf[0], (unsigned long)g_camera_buf[1],
         CAMERA_FRAME_SIZE);

  return 0;
}

static void camera_framebuf_free(void)
{
  g_camera_buf[0] = NULL;
  g_camera_buf[1] = NULL;
  g_framebuf_allocated = false;
}

/****************************************************************************
 * DVP Module Clock and Interrupt Enable
 *
 * From ARMino yuv_buf_driver.c:yuv_buf_init_common():
 *   1. Enable YUV_BUF power (PM voting — skipped, assume always on)
 *   2. Enable H264 clock (bit[0] of CLK_EN_REG) — needed for bus
 *   3. Enable YUV_BUF clock (bit[3] of CLK_EN_REG)
 *   4. Enable system interrupt (bit[26] of INT_32_63_EN_REG)
 *   5. Init global control (soft reset + clock gate bypass)
 *
 * All register writes use field-level RMW on shared registers.
 ****************************************************************************/

typedef struct
{
  uint32_t clk_en;       /* CLK_EN_REG bits we modify */
  uint32_t int_en;       /* INT_32_63_EN_REG bits we modify */
  uint32_t global_ctrl;  /* YUV_BUF REG_0x02 value */
  bool     valid;
} dvp_module_state_t;

static dvp_module_state_t g_dvp_module_saved = { .valid = false };
static bool g_dvp_module_enabled = false;

static int dvp_module_clk_enable(void)
{
  uint32_t val;

  if (g_dvp_module_enabled)
    {
      return 0;
    }

  /* Save register values for restore — only bits we modify */

  val = getreg32(CAMERA_CLK_EN_REG);
  g_dvp_module_saved.clk_en = val & (CAMERA_H264_CLKEN_BIT |
                                      CAMERA_YUV_BUF_CLKEN_BIT);

  val = getreg32(CAMERA_SYS_INT_32_63_REG);
  g_dvp_module_saved.int_en = val & CAMERA_YUVB_INT_EN_BIT;

  g_dvp_module_saved.global_ctrl = getreg32(YUV_BUF_REG_0x02);
  g_dvp_module_saved.valid = true;

  /* Step 1: Enable H264 clock — needed for YUV_BUF internal bus
   * From ARMino sys_drv_set_h264_clk_en(1)
   */

  val = getreg32(CAMERA_CLK_EN_REG);
  val |= CAMERA_H264_CLKEN_BIT;
  putreg32(val, CAMERA_CLK_EN_REG);

  /* Step 2: Enable YUV_BUF clock
   * From ARMino sys_drv_set_yuv_buf_clk_en(1)
   */

  val = getreg32(CAMERA_CLK_EN_REG);
  val |= CAMERA_YUV_BUF_CLKEN_BIT;
  putreg32(val, CAMERA_CLK_EN_REG);

  /* Step 3: Enable system interrupt for YUV_BUF
   * From ARMino sys_drv_int_group2_enable(YUV_BUF_INTERRUPT_CTRL_BIT)
   * YUV_BUF_INTERRUPT_CTRL_BIT = (1 << 26)
   */

  val = getreg32(CAMERA_SYS_INT_32_63_REG);
  val |= CAMERA_YUVB_INT_EN_BIT;
  putreg32(val, CAMERA_SYS_INT_32_63_REG);

  /* Step 4: Init global control — soft reset + clock gate bypass
   * From ARMino yuv_buf_ll_init():
   *   hw->global_ctrl.soft_reset = 0;    // assert reset
   *   hw->global_ctrl.soft_reset = 1;    // deassert reset
   *   hw->global_ctrl.clk_gate_bypass = 1; // bypass clock gate
   */

  val = getreg32(YUV_BUF_REG_0x02);
  val &= ~YUV_BUF_GLOBAL_SOFT_RESET;  /* assert reset */
  putreg32(val, YUV_BUF_REG_0x02);

  val |= YUV_BUF_GLOBAL_SOFT_RESET;   /* deassert reset */
  val |= YUV_BUF_GLOBAL_CLK_GATE;     /* bypass clock gate */
  putreg32(val, YUV_BUF_REG_0x02);

  g_dvp_module_enabled = true;

  syslog(LOG_INFO,
         "[camera] DVP module clock enabled: CLK_EN=0x%08lX INT_EN=0x%08lX\n",
         (unsigned long)getreg32(CAMERA_CLK_EN_REG),
         (unsigned long)getreg32(CAMERA_SYS_INT_32_63_REG));

  return 0;
}

static void dvp_module_clk_disable(void)
{
  uint32_t val;

  if (!g_dvp_module_enabled) return;

  /* Assert soft reset first */

  val = getreg32(YUV_BUF_REG_0x02);
  val &= ~YUV_BUF_GLOBAL_SOFT_RESET;
  putreg32(val, YUV_BUF_REG_0x02);

  /* Disable system interrupt for YUV_BUF */

  val = getreg32(CAMERA_SYS_INT_32_63_REG);
  val &= ~CAMERA_YUVB_INT_EN_BIT;
  val |= g_dvp_module_saved.int_en;  /* restore saved bits */
  putreg32(val, CAMERA_SYS_INT_32_63_REG);

  /* Disable YUV_BUF clock */

  val = getreg32(CAMERA_CLK_EN_REG);
  val &= ~CAMERA_YUV_BUF_CLKEN_BIT;
  val |= (g_dvp_module_saved.clk_en & CAMERA_YUV_BUF_CLKEN_BIT);
  putreg32(val, CAMERA_CLK_EN_REG);

  /* Disable H264 clock */

  val = getreg32(CAMERA_CLK_EN_REG);
  val &= ~CAMERA_H264_CLKEN_BIT;
  val |= (g_dvp_module_saved.clk_en & CAMERA_H264_CLKEN_BIT);
  putreg32(val, CAMERA_CLK_EN_REG);

  /* Restore global control register */

  putreg32(g_dvp_module_saved.global_ctrl, YUV_BUF_REG_0x02);

  g_dvp_module_enabled = false;

  syslog(LOG_INFO, "[camera] DVP module clock disabled\n");
}

/****************************************************************************
 * DVP Controller Configuration (CIS / YUV_BUF)
 *
 * The YUV_BUF controller at 0x48020000 captures DVP data and writes it
 * to external memory via its internal DMA.  Configuration steps:
 *   1. Save all register values for restore
 *   2. Set resolution (x_pixel = width/8, y_pixel = height/8)
 *   3. Set format (YUV mode, YUYV)
 *   4. Set polarity (HSYNC/VSYNC/PCLK from Kconfig)
 *   5. Set frame buffer base address
 *   6. Start YUV mode (set yuv_mode bit in CTRL)
 *
 * All register writes use field-level RMW on shared registers.
 ****************************************************************************/

static void dvp_ctrl_save_state(dvp_ctrl_state_t *s)
{
  s->ctrl       = getreg32(YUV_BUF_REG_0x04);
  s->pixel      = getreg32(YUV_BUF_REG_0x05);
  s->em_base    = getreg32(YUV_BUF_REG_0x08);
  s->int_en     = getreg32(YUV_BUF_REG_0x09);
  s->int_status = getreg32(YUV_BUF_REG_0x0a);
  s->valid      = true;
}

static void dvp_ctrl_restore_state(const dvp_ctrl_state_t *s)
{
  if (!s->valid) return;

  /* Disable interrupts first */

  putreg32(0, YUV_BUF_REG_0x09);

  /* Clear any pending interrupt status */

  putreg32(YUV_BUF_INT_STATUS_MASK, YUV_BUF_REG_0x0a);

  /* Restore registers in order: CTRL, PIXEL, EM_BASE, INT_EN */

  putreg32(s->ctrl, YUV_BUF_REG_0x04);
  putreg32(s->pixel, YUV_BUF_REG_0x05);
  putreg32(s->em_base, YUV_BUF_REG_0x08);
  putreg32(s->int_en, YUV_BUF_REG_0x09);
}

/****************************************************************************
 * Name: dvp_ctrl_config
 *
 * Description:
 *   Configure the CIS controller (YUV_BUF) for DVP capture.
 *   Resolution: CAMERA_HRES x CAMERA_VRES (640x480).
 *   Format: YUV mode, YUYV (matches GC2145 output).
 *   Polarity: from Kconfig (CONFIG_DVP_PCLK_RISING, etc.).
 *
 *   The controller's internal DMA writes captured data to the address
 *   in em_base_addr.
 *
 * Input Parameters:
 *   em_base - External memory base address for frame buffer output
 *
 ****************************************************************************/

static int dvp_ctrl_config(uint32_t em_base)
{
  int ret;
  uint32_t val;
  uint32_t x_pixel;
  uint32_t y_pixel;
  uint32_t frame_blk;

  if (g_dvp_ctrl_configed)
    {
      syslog(LOG_WARNING, "[camera] DVP ctrl already configured\n");
      return 0;
    }

  /* Enable module clock and system interrupt FIRST
   * This must be done before accessing YUV_BUF registers.
   */

  ret = dvp_module_clk_enable();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] DVP module clock enable failed: %d\n", ret);
      return ret;
    }

  /* Save all register values for restore */

  dvp_ctrl_save_state(&g_dvp_ctrl_saved);

  /* Calculate pixel parameters
   * x_pixel = width / 8
   * y_pixel = height / 8
   * frame_blk = x_pixel * y_pixel / 2 (from ARMino yuv_buf_hal.c)
   */

  x_pixel   = CAMERA_HRES / 8;               /* 640/8 = 80 */
  y_pixel   = CAMERA_VRES / 8;               /* 480/8 = 60 */
  frame_blk = x_pixel * y_pixel / 2;         /* 80*60/2 = 2400 */

  /* Configure CTRL register (0x04)
   * Start from saved value to preserve reserved bits.
   * Then set/clear fields as needed.
   */

  val = g_dvp_ctrl_saved.ctrl;

  /* Enable YUV mode */

  val |= YUV_BUF_CTRL_YUV_MODE;

  /* Set YUV format to YUYV (controller default).
   * NOTE: actual byte order in PSRAM is UYVY (verified by camera dump).
   * The controller format field does NOT reorder bytes — it matches
   * whatever the sensor outputs.  Software conversion uses UYVY parsing.
   */

  val &= ~YUV_BUF_CTRL_FMT_SEL_MASK;
  val |= YUV_BUF_CTRL_FMT_YUYV;

  /* Set PCLK edge — default is rising edge sampling.
   * If CONFIG_DVP_PCLK_FALLING is set, invert to sample on falling edge.
   */

#ifdef CONFIG_DVP_PCLK_FALLING
  val |= YUV_BUF_CTRL_VCK_EDGE;   /* sample on falling edge */
#else
  val &= ~YUV_BUF_CTRL_VCK_EDGE;  /* sample on rising edge (default) */
#endif

  /* Set HSYNC polarity — default is active-high (not inverted).
   * If CONFIG_DVP_HSYNC_ACTIVE_LOW is set, invert HSYNC.
   */

#ifdef CONFIG_DVP_HSYNC_ACTIVE_LOW
  val |= YUV_BUF_CTRL_HSYNC_REV;
#else
  val &= ~YUV_BUF_CTRL_HSYNC_REV;
#endif

  /* Set VSYNC polarity — default is active-high (not inverted).
   * If CONFIG_DVP_VSYNC_ACTIVE_LOW is set, invert VSYNC.
   */

#ifdef CONFIG_DVP_VSYNC_ACTIVE_LOW
  val |= YUV_BUF_CTRL_VSYNC_REV;
#else
  val &= ~YUV_BUF_CTRL_VSYNC_REV;
#endif

  /* Disable H264 mode — we're doing raw YUV capture */

  val &= ~YUV_BUF_CTRL_H264_MODE;

  /* Disable memrev and byte_rev — ARMino disables these for normal
   * YUV capture mode.  Only enable in nosensor_encode mode.
   */

  val &= ~YUV_BUF_CTRL_MEMREV;
  val &= ~YUV_BUF_CTRL_BYTE_REV;

  /* Write CTRL register */

  putreg32(val, YUV_BUF_REG_0x04);

  /* Configure PIXEL register (0x05)
   * bit[7:0]   = x_pixel (width/8)
   * bit[15:8]  = y_pixel (height/8)
   * bit[31:16] = frame_blk (total blocks)
   */

  val = (x_pixel & YUV_BUF_PIXEL_X_MASK) |
        ((y_pixel << YUV_BUF_PIXEL_Y_POS) & YUV_BUF_PIXEL_Y_MASK) |
        ((frame_blk << YUV_BUF_PIXEL_BLK_POS) & YUV_BUF_PIXEL_BLK_MASK);

  putreg32(val, YUV_BUF_REG_0x05);

  /* Set external memory base address (0x08) */

  putreg32(em_base, YUV_BUF_REG_0x08);

  /* Clear any pending interrupt status */

  putreg32(YUV_BUF_INT_STATUS_MASK, YUV_BUF_REG_0x0a);

  /* Start YUV mode — set bit[0] of CTRL (yuv_mode)
   * From ARMino bk_yuv_buf_start(YUV_MODE) -> yuv_buf_hal_start_yuv_mode()
   *   yuv_buf_ll_enable_yuv_buf_mode(hal->hw);  // bit[0] = 1
   *   yuv_buf_ll_disable_h264_encode_mode(hal->hw); // bit[3] = 0
   */

  val = getreg32(YUV_BUF_REG_0x04);
  val |= YUV_BUF_CTRL_YUV_MODE;     /* enable YUV buf mode */
  val &= ~YUV_BUF_CTRL_H264_MODE;   /* disable H264 mode */
  putreg32(val, YUV_BUF_REG_0x04);

  g_dvp_ctrl_configed = true;

  syslog(LOG_INFO,
         "[camera] DVP ctrl config: %dx%d xpix=%lu ypix=%lu blk=%lu "
         "em_base=0x%08lX\n",
         CAMERA_HRES, CAMERA_VRES,
         (unsigned long)x_pixel, (unsigned long)y_pixel,
         (unsigned long)frame_blk,
         (unsigned long)em_base);

  return 0;
}

/****************************************************************************
 * Name: dvp_ctrl_deconfig
 *
 * Description:
 *   Restore all CIS controller registers to their saved values.
 *   Disables interrupts and clears pending status before restore.
 *
 ****************************************************************************/

static void dvp_ctrl_deconfig(void)
{
  if (!g_dvp_ctrl_configed) return;

  dvp_ctrl_restore_state(&g_dvp_ctrl_saved);
  g_dvp_ctrl_configed = false;

  /* Disable module clock after restoring registers */

  dvp_module_clk_disable();
}

/****************************************************************************
 * Frame Consumer API (critical-section protected)
 ****************************************************************************/

static uint32_t dvp_frame_get(void)
{
  irqstate_t flags;
  int idx;
  uint32_t addr;

  flags = enter_critical_section();
  idx = g_ready_buf;
  if (idx >= 0)
    {
      g_busy_buf  = idx;
      g_ready_buf = -1;
      addr = (uint32_t)(uintptr_t)g_camera_buf[idx];
    }
  else
    {
      addr = 0;
    }

  leave_critical_section(flags);
  return addr;
}

static void dvp_frame_put(void)
{
  irqstate_t flags;

  flags = enter_critical_section();
  g_busy_buf = -1;
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: dvp_irq_handler
 *
 * Description:
 *   ISR for DVP (YUV_BUF) controller interrupts.
 *   Handles VSYNC falling edge (frame start) and YUV arrived (frame done).
 *
 ****************************************************************************/

static int dvp_irq_handler(int irq, void *context, void *arg)
{
  uint32_t status;

  /* Read interrupt status */

  status = getreg32(YUV_BUF_REG_0x0a);

  /* VSYNC falling edge — frame start */

  if (status & YUV_BUF_INT_VSYNC_NEGE)
    {
      g_dvp_vsync_count++;
    }

  /* YUV arrived — frame data written to memory */

  if (status & YUV_BUF_INT_YUV_ARV)
    {
      g_dvp_frame_count++;

      /* Ping-pong: if streaming, advance to next buffer */

      if (g_stream_active)
        {
          int next_buf = 1 - g_cur_buf;

          if (g_ready_buf < 0 && next_buf != g_busy_buf)
            {
              g_ready_buf = g_cur_buf;
              g_cur_buf = next_buf;
              g_pingpong_count++;
              putreg32((uint32_t)(uintptr_t)g_camera_buf[g_cur_buf],
                       YUV_BUF_REG_0x08);
            }
          else
            {
              g_drop_count++;
            }
        }
    }

  /* Clear all pending interrupts by writing 1s */

  putreg32(status & YUV_BUF_INT_STATUS_MASK, YUV_BUF_REG_0x0a);

  return OK;
}

/****************************************************************************
 * Name: dvp_irq_attach
 *
 * Description:
 *   Attach DVP interrupt handler and enable VSYNC + YUV_ARRIVED interrupts.
 *
 ****************************************************************************/

static int dvp_irq_attach(void)
{
  int ret;

  if (g_dvp_irq_attached)
    {
      syslog(LOG_WARNING, "[camera] DVP IRQ already attached\n");
      return 0;
    }

  /* Reset counters */

  g_dvp_vsync_count = 0;
  g_dvp_hsync_count = 0;
  g_dvp_frame_count = 0;

  /* Attach ISR */

  ret = irq_attach(BK7258_IRQ_YUV_BUF, dvp_irq_handler, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] irq_attach failed: %d\n", ret);
      return ret;
    }

  /* Enable VSYNC falling edge and YUV arrived interrupts */

  putreg32(YUV_BUF_INT_VSYNC_NEGE | YUV_BUF_INT_YUV_ARV,
           YUV_BUF_REG_0x09);

  /* Enable the IRQ in NVIC */

  up_enable_irq(BK7258_IRQ_YUV_BUF);

  g_dvp_irq_attached = true;

  syslog(LOG_INFO, "[camera] DVP IRQ attached (IRQ %d)\n",
         BK7258_IRQ_YUV_BUF);

  return 0;
}

/****************************************************************************
 * Name: dvp_irq_detach
 *
 * Description:
 *   Disable DVP interrupts and detach the ISR.
 *
 ****************************************************************************/

static void dvp_irq_detach(void)
{
  if (!g_dvp_irq_attached) return;

  /* Disable IRQ in NVIC */

  up_disable_irq(BK7258_IRQ_YUV_BUF);

  /* Disable all DVP interrupts */

  putreg32(0, YUV_BUF_REG_0x09);

  /* Clear any pending status */

  putreg32(YUV_BUF_INT_STATUS_MASK, YUV_BUF_REG_0x0a);

  /* Detach ISR — restore default handler */

  irq_detach(BK7258_IRQ_YUV_BUF);

  g_dvp_irq_attached = false;

  syslog(LOG_INFO, "[camera] DVP IRQ detached\n");
}

/****************************************************************************
 * Config Validation
 ****************************************************************************/

static int camera_check_config(void)
{
  bool ok = true;

#ifdef CONFIG_CAMERA_SCCB_CONFIRMED
  const char *sccb_st = "y";
#else
  const char *sccb_st = "n";
#endif
#ifdef CONFIG_CAMERA_SDA_SCL_ROLE_CONFIRMED
  const char *role_st = "y";
#else
  const char *role_st = "n";
#endif
#ifdef CONFIG_CAMERA_RST_GPIO_CONFIRMED
  const char *rst_g_st = "y";
#else
  const char *rst_g_st = "n";
#endif
#ifdef CONFIG_CAMERA_RST_POLARITY_CONFIRMED
  const char *rst_p_st = "y";
#else
  const char *rst_p_st = "n";
#endif
#ifdef CONFIG_CAMERA_PWR_CONFIRMED
  const char *pwr_st = "y";
#else
  const char *pwr_st = "n";
#endif
#ifdef CONFIG_CAMERA_PWR_POLARITY_CONFIRMED
  const char *pwr_p_st = "y";
#else
  const char *pwr_p_st = "n";
#endif
#ifdef CONFIG_CAMERA_MCLK_CONFIRMED
  const char *mclk_st = "y";
#else
  const char *mclk_st = "n";
#endif

  syslog(LOG_INFO,
         "[camera] Config: SDA=%d(net=%s role=%s) SCL=%d "
         "RST=%d(gpio=%s pol=%s) PWR=%d(net=%s pol=%s) "
         "MCLK=%d(conf=%s)\n",
         g_cam_sda_pin, sccb_st, role_st,
         g_cam_scl_pin,
         g_cam_rst_pin, rst_g_st, rst_p_st,
         g_cam_pwr_pin, pwr_st, pwr_p_st,
         g_cam_mclk_pin, mclk_st);

  /* Stage 1: 7 confirmed sentinels */

#ifndef CONFIG_CAMERA_SCCB_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: SCCB net NOT confirmed\n"); ok = false;
#endif
#ifndef CONFIG_CAMERA_SDA_SCL_ROLE_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: SDA/SCL role NOT confirmed\n"); ok = false;
#endif
#ifndef CONFIG_CAMERA_RST_GPIO_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: RST GPIO NOT confirmed\n"); ok = false;
#endif
#ifndef CONFIG_CAMERA_RST_POLARITY_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: RST polarity NOT confirmed\n"); ok = false;
#endif
#ifndef CONFIG_CAMERA_PWR_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: PWR net NOT confirmed\n"); ok = false;
#endif
#ifndef CONFIG_CAMERA_PWR_POLARITY_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: PWR polarity NOT confirmed\n"); ok = false;
#endif
#ifndef CONFIG_CAMERA_MCLK_CONFIRMED
  syslog(LOG_ERR, "[camera] BLOCKER: MCLK NOT confirmed\n"); ok = false;
#endif

  if (!ok)
    {
      syslog(LOG_ERR,
             "[camera] ======== 7 BLOCKERS — no MMIO touched ========\n");
      return -ENOTSUP;
    }

  /* Stage 2: GPIO range + uniqueness */

  if (!gpio_is_valid(g_cam_sda_pin))
    { syslog(LOG_ERR, "[camera] SDA pin %d out of range\n", g_cam_sda_pin);
      ok = false; }
  if (!gpio_is_valid(g_cam_scl_pin))
    { syslog(LOG_ERR, "[camera] SCL pin %d out of range\n", g_cam_scl_pin);
      ok = false; }
  if (gpio_is_valid(g_cam_sda_pin) && gpio_is_valid(g_cam_scl_pin) &&
      g_cam_sda_pin == g_cam_scl_pin)
    { syslog(LOG_ERR, "[camera] SDA=SCL=%d conflict\n", g_cam_sda_pin);
      ok = false; }
  if (g_cam_rst_pin != CAMERA_GPIO_UNCONFIGURED &&
      !gpio_is_valid(g_cam_rst_pin))
    { syslog(LOG_ERR, "[camera] RST pin %d out of range\n", g_cam_rst_pin);
      ok = false; }
  if (g_cam_pwr_pin != CAMERA_GPIO_UNCONFIGURED &&
      !gpio_is_valid(g_cam_pwr_pin))
    { syslog(LOG_ERR, "[camera] PWR pin %d out of range\n", g_cam_pwr_pin);
      ok = false; }
  if (g_cam_pwdn_pin != CAMERA_GPIO_UNCONFIGURED &&
      !gpio_is_valid(g_cam_pwdn_pin))
    { syslog(LOG_ERR, "[camera] PWDN pin %d out of range\n", g_cam_pwdn_pin);
      ok = false; }
  if (g_cam_mclk_pin != CAMERA_GPIO_UNCONFIGURED &&
      !gpio_is_valid(g_cam_mclk_pin))
    { syslog(LOG_ERR, "[camera] MCLK pin %d out of range\n", g_cam_mclk_pin);
      ok = false; }

  {
    int pins[6];
    int n = 0;
    int i, j;

    if (gpio_is_valid(g_cam_sda_pin))  pins[n++] = g_cam_sda_pin;
    if (gpio_is_valid(g_cam_scl_pin))  pins[n++] = g_cam_scl_pin;
    if (g_cam_rst_pin != CAMERA_GPIO_UNCONFIGURED &&
        gpio_is_valid(g_cam_rst_pin))  pins[n++] = g_cam_rst_pin;
    if (g_cam_pwr_pin != CAMERA_GPIO_UNCONFIGURED &&
        gpio_is_valid(g_cam_pwr_pin))  pins[n++] = g_cam_pwr_pin;
    if (g_cam_pwdn_pin != CAMERA_GPIO_UNCONFIGURED &&
        gpio_is_valid(g_cam_pwdn_pin)) pins[n++] = g_cam_pwdn_pin;
    if (g_cam_mclk_pin != CAMERA_GPIO_UNCONFIGURED &&
        gpio_is_valid(g_cam_mclk_pin)) pins[n++] = g_cam_mclk_pin;

    for (i = 0; i < n; i++)
      for (j = i + 1; j < n; j++)
        if (pins[i] == pins[j])
          { syslog(LOG_ERR, "[camera] pin %d multi-role\n", pins[i]);
            ok = false; }
  }

  /* Stage 3: board-level reserved pin conflicts */

  {
    static const struct { int pin; const char *name; } rsv[] = {
      { CAMERA_PIN_UART0_RX, "UART0 RX" },
      { CAMERA_PIN_UART0_TX, "UART0 TX" },
      { CAMERA_PIN_SWD_CLK,  "SWD CLK"  },
      { CAMERA_PIN_SWD_IO,   "SWD IO"   },
      { 2,  "LCD left SCLK"  },
      { 3,  "LCD left CS"    },
      { 4,  "LCD left MOSI"  },
      { 5,  "LCD left DC"    },
      { 6,  "LCD right SCLK" },
      { 7,  "LCD right CS"   },
      { 22, "LCD right MOSI" },
      { 23, "LCD right DC"   },
      { 24, "LCD right RST"  },
      { 25, "LCD backlight"  },
      { 45, "LCD left RST"   },
      { 52, "LCD LDO_3V3_EN" },
    };

    static const int cpins[] = {
      CONFIG_CAMERA_GPIO_SDA,
      CONFIG_CAMERA_GPIO_SCL,
      CONFIG_CAMERA_GPIO_RST,
      CONFIG_CAMERA_GPIO_PWR_CTL,
      CONFIG_CAMERA_GPIO_PWDNB,
      CONFIG_CAMERA_MCLK_GPIO,
    };

    int i, r;
    for (i = 0; i < 6; i++)
      {
        if (cpins[i] == CAMERA_GPIO_UNCONFIGURED) continue;
        for (r = 0; r < (int)(sizeof(rsv) / sizeof(rsv[0])); r++)
          {
            if (cpins[i] == rsv[r].pin)
              {
                syslog(LOG_ERR,
                       "[camera] pin %d conflicts with %s\n",
                       cpins[i], rsv[r].name);
                ok = false;
              }
          }
      }

    /* Reject control pins (SDA/SCL/RST/PWR/MCLK) on DVP reserved range
     * P29-P39.  DVP DATA pins on P29-P39 are expected and allowed.
     */

    for (i = 0; i < 6; i++)
      {
        if (cpins[i] == CAMERA_GPIO_UNCONFIGURED) continue;
        if (cpins[i] >= DVP_PIN_FIRST && cpins[i] <= DVP_PIN_LAST)
          {
            syslog(LOG_ERR,
                   "[camera] control pin %d in DVP range P%d-P%d\n",
                   cpins[i], DVP_PIN_FIRST, DVP_PIN_LAST);
            ok = false;
          }
      }
  }

  /* Stage 4: MCLK must be P27 */

#ifdef CONFIG_CAMERA_MCLK_CONFIRMED
  if (g_cam_mclk_pin != CAMERA_MCLK_REQUIRED_PIN)
    {
      syslog(LOG_ERR,
             "[camera] MCLK_GPIO=%d but code requires %d (P27)\n",
             g_cam_mclk_pin, CAMERA_MCLK_REQUIRED_PIN);
      ok = false;
    }
#endif

  if (!ok)
    {
      syslog(LOG_ERR,
             "[camera] Cannot proceed — no MMIO touched.\n");
      return -ENODEV;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_camera_id
 *   Phase 0: read GC2145 chip ID via SCCB.  Restores all pins on exit.
 ****************************************************************************/

int bk7258_camera_id(void)
{
  gpio_state_t sda_saved, scl_saved, rst_saved;
  gpio_state_t pwr_saved, pwdn_saved;
  mclk_state_t mclk_saved = { .valid = false };
  int id_h = -1, id_l = -1;
  uint16_t chip_id;
  int ret, stop_ret;
  bool sda_ok = false, scl_ok = false, rst_ok = false;
  bool pwr_ok = false, pwdn_ok = false;
  bool mclk_on = false;

  /* ===== Config validation — NO MMIO before this ===== */

  ret = camera_check_config();
  if (ret < 0) return ret;

  syslog(LOG_INFO, "[camera] Config OK\n");

  /* ===== GPIO setup ===== */

  gpio_save_state(g_cam_sda_pin, &sda_saved); sda_ok = true;
  gpio_save_state(g_cam_scl_pin, &scl_saved); scl_ok = true;

  gpio_release(g_cam_sda_pin);
  gpio_release(g_cam_scl_pin);
  up_udelay(100);

  if (g_cam_rst_pin != CAMERA_GPIO_UNCONFIGURED)
    { gpio_save_state(g_cam_rst_pin, &rst_saved); rst_ok = true; }

  camera_power_on(&pwr_saved, &pwr_ok, &pwdn_saved, &pwdn_ok);

  if (g_cam_mclk_pin != CAMERA_GPIO_UNCONFIGURED)
    { mclk_save_state(&mclk_saved);
      mclk_enable_24mhz(); mclk_on = true;
      up_udelay(500); }

  if (rst_ok) camera_reset();

  /* ===== SCCB ID read ===== */

  id_h = sccb_read_reg(GC2145_REG_ID_H);
  if (id_h < 0)
    { syslog(LOG_ERR, "[camera] ID high read fail: %d\n", id_h);
      ret = id_h; goto cleanup; }

  id_l = sccb_read_reg(GC2145_REG_ID_L);
  if (id_l < 0)
    { syslog(LOG_ERR, "[camera] ID low read fail: %d\n", id_l);
      ret = id_l; goto cleanup; }

  chip_id = (uint16_t)((id_h << 8) | id_l);
  syslog(LOG_INFO,
         "[camera] GC2145 ID: 0x%04X (0xF0=0x%02X 0xF1=0x%02X)\n",
         chip_id, (uint8_t)id_h, (uint8_t)id_l);

  ret = (chip_id == 0x2145) ? 0 : -ENODEV;

cleanup:

  /* ===== Restore ALL saved GPIO states ===== */

  stop_ret = sccb_stop();

  if (mclk_on)     mclk_restore_state(&mclk_saved);
  if (rst_ok)      gpio_restore_state(&rst_saved);
  if (pwdn_ok)     gpio_restore_state(&pwdn_saved);
  if (pwr_ok)      gpio_restore_state(&pwr_saved);
  if (scl_ok)      gpio_restore_state(&scl_saved);
  if (sda_ok)      gpio_restore_state(&sda_saved);

  syslog(LOG_INFO, "[camera] Cleanup done — pins restored\n");

  if (ret == 0 && stop_ret < 0) ret = stop_ret;

  return ret;
}

/****************************************************************************
 * Name: bk7258_camera_io
 *   Phase 1 Step 1: configure DVP data pins (P29-P39), readback verify,
 *   deconfigure, readback verify restore.
 ****************************************************************************/

int bk7258_camera_io(void)
{
  int ret, i;
  uint32_t val, expected;

  ret = camera_check_config();
  if (ret < 0) return ret;

  /* Config DVP pins */

  ret = dvp_io_config();
  if (ret < 0) return ret;

  /* Readback verify: second_func=1, input_en=0, output_en=1(disabled) */

  for (i = 0; i < DVP_PIN_COUNT; i++)
    {
      int pin = DVP_PIN_FIRST + i;
      val = getreg32(BK7258_GPIO_CFG(pin));
      if (!(val & GPIO_CFG_SECOND_FUNC))
        {
          syslog(LOG_ERR, "[camera] P%d second_func not set: 0x%08X\n",
                 pin, val);
          ret = -EIO;
          goto cleanup;
        }
      if (val & GPIO_CFG_INPUT_EN)
        {
          syslog(LOG_ERR, "[camera] P%d input_en still set: 0x%08X\n",
                 pin, val);
          ret = -EIO;
          goto cleanup;
        }
    }

  syslog(LOG_INFO, "[camera] DVP IO config readback OK (P29-P39)\n");

cleanup:
  /* Deconfig and verify restore */

  dvp_io_deconfig();

  for (i = 0; i < DVP_PIN_COUNT; i++)
    {
      int pin = DVP_PIN_FIRST + i;
      val = getreg32(BK7258_GPIO_CFG(pin));
      expected = g_dvp_state[i].cfg;
      if (val != expected)
        {
          syslog(LOG_ERR,
                 "[camera] P%d CFG restore mismatch: 0x%08X vs 0x%08X\n",
                 pin, val, expected);
          if (ret == 0) ret = -EIO;
        }
    }

  if (ret == 0)
    syslog(LOG_INFO, "[camera] DVP IO deconfig readback OK\n");

  return ret;
}

/****************************************************************************
 * Name: bk7258_camera_init
 *   Phase 1 Step 2: power on, MCLK, reset, read ID, write GC2145 init
 *   table (YUYV 640x480), readback verify, keep MCLK/power on for
 *   oscilloscope verification.
 ****************************************************************************/

int bk7258_camera_init(void)
{
  gpio_state_t sda_saved, scl_saved, rst_saved;
  gpio_state_t pwr_saved, pwdn_saved;
  mclk_state_t mclk_saved = { .valid = false };
  int id_h, id_l;
  uint16_t chip_id;
  int ret, stop_ret;
  bool sda_ok = false, scl_ok = false, rst_ok = false;
  bool pwr_ok = false, pwdn_ok = false;
  bool mclk_on = false;
  bool dvp_ok = false;

  /* ===== Config validation ===== */

  ret = camera_check_config();
  if (ret < 0) return ret;

  /* ===== DVP data pin mux ===== */

  ret = dvp_io_config();
  if (ret < 0) return ret;
  dvp_ok = true;

  /* ===== SCCB pin setup ===== */

  gpio_save_state(g_cam_sda_pin, &sda_saved); sda_ok = true;
  gpio_save_state(g_cam_scl_pin, &scl_saved); scl_ok = true;
  gpio_release(g_cam_sda_pin);
  gpio_release(g_cam_scl_pin);
  up_udelay(100);

  /* ===== RST, power, MCLK ===== */

  if (g_cam_rst_pin != CAMERA_GPIO_UNCONFIGURED)
    { gpio_save_state(g_cam_rst_pin, &rst_saved); rst_ok = true; }

  camera_power_on(&pwr_saved, &pwr_ok, &pwdn_saved, &pwdn_ok);

  if (g_cam_mclk_pin != CAMERA_GPIO_UNCONFIGURED)
    { mclk_save_state(&mclk_saved);
      mclk_enable_24mhz(); mclk_on = true;
      up_udelay(500); }

  if (rst_ok) camera_reset();

  /* ===== Read chip ID ===== */

  id_h = sccb_read_reg(GC2145_REG_ID_H);
  if (id_h < 0)
    { syslog(LOG_ERR, "[camera] ID high read fail: %d\n", id_h);
      ret = id_h; goto cleanup; }

  id_l = sccb_read_reg(GC2145_REG_ID_L);
  if (id_l < 0)
    { syslog(LOG_ERR, "[camera] ID low read fail: %d\n", id_l);
      ret = id_l; goto cleanup; }

  chip_id = (uint16_t)((id_h << 8) | id_l);
  if (chip_id != 0x2145)
    {
      syslog(LOG_ERR, "[camera] Bad ID: 0x%04X (expected 0x2145)\n",
             chip_id);
      ret = -ENODEV;
      goto cleanup;
    }

  syslog(LOG_INFO, "[camera] GC2145 ID OK: 0x%04X\n", chip_id);

  /* ===== Write init table ===== */

  ret = gc2145_write_table(gc2145_init,
                           sizeof(gc2145_init) / sizeof(gc2145_init[0]));
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] Init table write failed\n");
      goto cleanup;
    }

  syslog(LOG_INFO, "[camera] Init table written (%d entries)\n",
         (int)(sizeof(gc2145_init) / sizeof(gc2145_init[0])));

  /* ===== Write 640x480 resolution table ===== */

  ret = gc2145_write_table(gc2145_640x480,
                           sizeof(gc2145_640x480) / sizeof(gc2145_640x480[0]));
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] 640x480 table write failed\n");
      goto cleanup;
    }

  syslog(LOG_INFO, "[camera] 640x480 table written (%d entries)\n",
         (int)(sizeof(gc2145_640x480) / sizeof(gc2145_640x480[0])));

  /* ===== Readback verify: key registers ===== */

  {
    int val84 = sccb_read_reg(0x84);
    if (val84 < 0)
      {
        syslog(LOG_ERR, "[camera] Reg 0x84 readback fail: %d\n", val84);
        ret = val84;
        goto cleanup;
      }
    if (val84 != 0x02)
      {
        syslog(LOG_ERR,
               "[camera] Reg 0x84 mismatch: 0x%02X (expected 0x02=YUYV)\n",
               val84);
        ret = -EIO;
        goto cleanup;
      }
  }

  {
    int val97 = sccb_read_reg(0x97);
    if (val97 < 0)
      {
        syslog(LOG_ERR, "[camera] Reg 0x97 readback fail: %d\n", val97);
        ret = val97;
        goto cleanup;
      }
    if (val97 != 0x02)
      {
        syslog(LOG_ERR,
               "[camera] Reg 0x97 mismatch: 0x%02X (expected 0x02=HOUT hi)\n",
               val97);
        ret = -EIO;
        goto cleanup;
      }
  }

  syslog(LOG_INFO,
         "[camera] GC2145 init complete — YUYV 640x480\n");
  syslog(LOG_INFO,
         "[camera] MCLK/power left ON for oscilloscope verification\n");

  /* Restore SCCB pins only — MCLK and power stay ON */

  stop_ret = sccb_stop();
  if (scl_ok) gpio_restore_state(&scl_saved);
  if (sda_ok) gpio_restore_state(&sda_saved);

  return 0;

cleanup:

  stop_ret = sccb_stop();
  if (mclk_on)     mclk_restore_state(&mclk_saved);
  if (rst_ok)      gpio_restore_state(&rst_saved);
  if (pwdn_ok)     gpio_restore_state(&pwdn_saved);
  if (pwr_ok)      gpio_restore_state(&pwr_saved);
  if (scl_ok)      gpio_restore_state(&scl_saved);
  if (sda_ok)      gpio_restore_state(&sda_saved);
  if (dvp_ok)      dvp_io_deconfig();

  return ret;
}

/****************************************************************************
 * Name: bk7258_camera_stop
 *   Turn off MCLK, power off, deconfig DVP pins, free frame buffers.
 ****************************************************************************/

int bk7258_camera_stop(void)
{
  uint32_t val;

  /* Detach DVP IRQ first — stop interrupts before modifying registers */

  dvp_irq_detach();

  /* Restore DVP controller registers and disable module clock */

  dvp_ctrl_deconfig();

  /* Also disable module clock in case dvp_ctrl_config was never called
   * but dvp_module_clk_enable was called directly.
   */

  dvp_module_clk_disable();

  /* Turn off MCLK gate */

  val = getreg32(CAMERA_CLK_EN_REG);
  val &= ~CAMERA_CIS_AUXS_CKEN_BIT;
  putreg32(val, CAMERA_CLK_EN_REG);

  /* Power off — drive PWR pin inactive */

  if (g_cam_pwr_pin != CAMERA_GPIO_UNCONFIGURED)
    {
#ifdef CONFIG_CAMERA_PWR_ACTIVE_HIGH
      gpio_drive_low(g_cam_pwr_pin);
#else
      gpio_set_output_high(g_cam_pwr_pin);
#endif
    }

  /* Deconfig DVP pins */

  dvp_io_deconfig();

  /* Free frame buffers */

  camera_framebuf_free();

  syslog(LOG_INFO, "[camera] Stopped — MCLK off, power off, pins restored\n");
  return 0;
}

/****************************************************************************
 * Name: bk7258_camera_buf
 *   Phase 1 Step 3: init PSRAM, allocate two frame buffers, write test
 *   pattern to buf[0], sampled readback verify.
 ****************************************************************************/

int bk7258_camera_buf(void)
{
  int ret, i;
  volatile uint8_t *buf;

  ret = camera_framebuf_alloc();
  if (ret < 0) return ret;

  /* Write test pattern to buf[0] */

  buf = g_camera_buf[0];
  for (i = 0; i < CAMERA_FRAME_SIZE; i++)
    {
      buf[i] = (uint8_t)(i & 0xFF);
    }

  /* Readback verify:
   *   - First 256 bytes: full check
   *   - Last 256 bytes: full check
   *   - Every 1024 bytes: sampled check
   */

  for (i = 0; i < CAMERA_FRAME_SIZE; i++)
    {
      bool check = false;

      if (i < 256)                              check = true;
      else if (i >= CAMERA_FRAME_SIZE - 256)    check = true;
      else if ((i & 1023) == 0)                 check = true;

      if (check && buf[i] != (uint8_t)(i & 0xFF))
        {
          syslog(LOG_ERR,
                 "[camera] Framebuf[%d] mismatch: 0x%02X vs 0x%02X\n",
                 i, buf[i], (uint8_t)(i & 0xFF));
          return -EIO;
        }
    }

  syslog(LOG_INFO,
         "[camera] Framebuf verify OK (%d bytes, buf0=0x%08lX)\n",
         CAMERA_FRAME_SIZE, (unsigned long)g_camera_buf[0]);

  return 0;
}

/****************************************************************************
 * Name: bk7258_camera_dvp_active
 *   Runtime flag: returns true if DVP data pins (P29-P39) are currently
 *   configured.  Used by lcdtest for mutual exclusion.
 ****************************************************************************/

bool bk7258_camera_dvp_active(void)
{
  return g_dvp_pins_configed;
}

/****************************************************************************
 * Name: bk7258_camera_sync
 *
 * Description:
 *   Phase 1 Round 2 Step 4a: DVP controller config + interrupt counting.
 *
 *   Configures the CIS controller (YUV_BUF) for 640x480 YUV capture,
 *   attaches VSYNC/YUV_ARRIVED interrupt handlers, and waits for a
 *   configurable number of VSYNC edges (default 10) to verify the
 *   camera is producing valid sync signals.
 *
 *   Prerequisites: camera init must have been run (MCLK on, GC2145
 *   configured for output).  Frame buffers must be allocated.
 *
 *   On exit: interrupts are detached, controller registers restored.
 *
 ****************************************************************************/

int bk7258_camera_sync(void)
{
  int ret;
  int timeout_ms;
  uint32_t vsync_target;
  uint32_t vsync_prev;
  uint32_t vsync_now;

  /* Check DVP pins are configured (camera init must have been run) */

  if (!g_dvp_pins_configed)
    {
      syslog(LOG_ERR,
             "[camera] DVP pins not configured — run 'camera init' first\n");
      return -ENODEV;
    }

  /* Check frame buffers allocated */

  if (!g_framebuf_allocated)
    {
      syslog(LOG_ERR,
             "[camera] Frame buffers not allocated — run 'camera buf' first\n");
      return -ENOMEM;
    }

  /* Configure DVP controller — use buf[0] as output target.
   * The controller needs a valid em_base_addr even for sync-only test.
   */

  ret = dvp_ctrl_config((uint32_t)(uintptr_t)g_camera_buf[0]);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] DVP ctrl config failed: %d\n", ret);
      goto cleanup_ctrl;
    }

  /* Attach interrupt handler and enable interrupts */

  ret = dvp_irq_attach();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] DVP IRQ attach failed: %d\n", ret);
      goto cleanup_irq;
    }

  /* Wait for VSYNC edges — timeout 5 seconds */

  vsync_target = 10;
  timeout_ms   = 5000;
  vsync_prev   = g_dvp_vsync_count;

  syslog(LOG_INFO,
         "[camera] Waiting for %lu VSYNC edges (timeout %d ms)...\n",
         (unsigned long)vsync_target, timeout_ms);

  while (timeout_ms > 0)
    {
      vsync_now = g_dvp_vsync_count;
      if (vsync_now >= vsync_target)
        {
          break;
        }

      /* Print progress every 100 VSYNC edges */

      if (vsync_now != vsync_prev && (vsync_now % 100) == 0)
        {
          syslog(LOG_INFO, "[camera] VSYNC count: %lu\n",
                 (unsigned long)vsync_now);
          vsync_prev = vsync_now;
        }

      up_udelay(1000);  /* 1 ms */
      timeout_ms--;
    }

  /* Report results */

  vsync_now = g_dvp_vsync_count;

  syslog(LOG_INFO,
         "[camera] Sync result: VSYNC=%lu YUV_ARRIVED=%lu (target=%lu)\n",
         (unsigned long)vsync_now,
         (unsigned long)g_dvp_frame_count,
         (unsigned long)vsync_target);

  if (vsync_now < vsync_target)
    {
      syslog(LOG_ERR,
             "[camera] TIMEOUT: only %lu VSYNC edges in 5s "
             "(expected %lu)\n",
             (unsigned long)vsync_now, (unsigned long)vsync_target);
      syslog(LOG_ERR,
             "[camera] Check: MCLK on P27, sensor powered, "
             "DVP pins P29-P39\n");
      ret = -ETIMEDOUT;
    }
  else
    {
      syslog(LOG_INFO, "[camera] DVP sync OK — %lu VSYNC edges captured\n",
             (unsigned long)vsync_now);
      ret = 0;
    }

cleanup_irq:
  /* Always detach IRQ in sync test — we don't keep it running */

  dvp_irq_detach();

cleanup_ctrl:
  /* Always restore controller registers */

  dvp_ctrl_deconfig();

  return ret;
}

/****************************************************************************
 * Name: bk7258_camera_grab
 *
 * Description:
 *   Phase 1 Round 2 Step 4b: DMA single frame capture to PSRAM.
 *
 *   Configures the CIS controller, attaches interrupts, fills the
 *   target buffer with 0x5A pattern, waits for one complete frame
 *   (YUV_ARRIVED interrupt), then hexdumps the first 256 bytes.
 *
 *   Prerequisites: camera init + camera buf must have been run.
 *
 *   On exit: interrupts are detached, controller registers restored.
 *
 ****************************************************************************/

int bk7258_camera_grab(void)
{
  int ret;
  int timeout_ms;
  uint32_t frame_start;
  volatile uint8_t *buf;
  int i;

  /* Check DVP pins are configured */

  if (!g_dvp_pins_configed)
    {
      syslog(LOG_ERR,
             "[camera] DVP pins not configured — run 'camera init' first\n");
      return -ENODEV;
    }

  /* Check frame buffers allocated */

  if (!g_framebuf_allocated)
    {
      syslog(LOG_ERR,
             "[camera] Frame buffers not allocated — run 'camera buf' first\n");
      return -ENOMEM;
    }

  /* Fill target buffer with 0x5A pattern — allows detection of
   * whether DMA actually wrote data.
   */

  buf = g_camera_buf[0];
  syslog(LOG_INFO, "[camera] Filling buf[0] with 0x5A pattern (%d bytes)\n",
         CAMERA_FRAME_SIZE);

  for (i = 0; i < CAMERA_FRAME_SIZE; i++)
    {
      buf[i] = 0x5A;
    }

  /* Configure DVP controller — output to buf[0] */

  ret = dvp_ctrl_config((uint32_t)(uintptr_t)g_camera_buf[0]);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] DVP ctrl config failed: %d\n", ret);
      goto cleanup_ctrl;
    }

  /* Attach interrupt handler */

  ret = dvp_irq_attach();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] DVP IRQ attach failed: %d\n", ret);
      goto cleanup_irq;
    }

  /* Wait for one complete frame — YUV_ARRIVED interrupt.
   * Timeout 5 seconds.
   */

  frame_start = g_dvp_frame_count;
  timeout_ms  = 5000;

  syslog(LOG_INFO, "[camera] Waiting for frame (timeout %d ms)...\n",
         timeout_ms);

  while (timeout_ms > 0)
    {
      if (g_dvp_frame_count > frame_start)
        {
          break;
        }

      up_udelay(1000);  /* 1 ms */
      timeout_ms--;
    }

  if (g_dvp_frame_count <= frame_start)
    {
      syslog(LOG_ERR,
             "[camera] TIMEOUT: no frame received in 5s "
             "(VSYNC=%lu FRAME=%lu)\n",
             (unsigned long)g_dvp_vsync_count,
             (unsigned long)g_dvp_frame_count);
      ret = -ETIMEDOUT;
      goto cleanup_irq;
    }

  syslog(LOG_INFO, "[camera] Frame captured — VSYNC=%lu FRAME=%lu\n",
         (unsigned long)g_dvp_vsync_count,
         (unsigned long)g_dvp_frame_count);

  /* Hexdump first 256 bytes of captured frame.
   * Byte order: UYVY (U=b0 Y0=b1 V=b2 Y1=b3, verified by camera dump).
   * In neutral scenes: even bytes (U/V) ≈ 0x80, odd bytes (Y) vary.
   */

  syslog(LOG_INFO, "[camera] First 256 bytes of captured frame (UYVY):\n");

  for (i = 0; i < 256; i += 16)
    {
      syslog(LOG_INFO,
             "  %04X: %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X\n",
             i,
             buf[i + 0],  buf[i + 1],  buf[i + 2],  buf[i + 3],
             buf[i + 4],  buf[i + 5],  buf[i + 6],  buf[i + 7],
             buf[i + 8],  buf[i + 9],  buf[i + 10], buf[i + 11],
             buf[i + 12], buf[i + 13], buf[i + 14], buf[i + 15]);
    }

  /* Check if buffer still contains 0x5A pattern — if so, DMA didn't write */

  {
    bool all_5a = true;
    for (i = 0; i < 256; i++)
      {
        if (buf[i] != 0x5A)
          {
            all_5a = false;
            break;
          }
      }

    if (all_5a)
      {
        syslog(LOG_ERR,
               "[camera] WARNING: buf[0] still 0x5A — DMA may not have "
               "written data\n");
        ret = -EIO;
      }
    else
      {
        syslog(LOG_INFO,
               "[camera] DMA write verified — buf[0] contains non-0x5A data\n");
        ret = 0;
      }
  }

cleanup_irq:
  dvp_irq_detach();

cleanup_ctrl:
  dvp_ctrl_deconfig();

  return ret;
}

/****************************************************************************
 * Name: bk7258_camera_stream
 *
 * Description:
 *   Phase 1 Round 3 Step 5: continuous capture with ping-pong buffering.
 *
 ****************************************************************************/

int bk7258_camera_stream(int n_frames)
{
  int ret;
  uint32_t elapsed_ms;
  int captured;
  int timeout_ms;
  int i;

  if (n_frames <= 0) n_frames = 30;
  else if (n_frames > STREAM_BUF_MAX) n_frames = STREAM_BUF_MAX;

  if (!g_dvp_pins_configed)
    {
      syslog(LOG_ERR, "[camera] DVP pins not configured\n");
      return -ENODEV;
    }

  if (!g_framebuf_allocated)
    {
      syslog(LOG_ERR, "[camera] Frame buffers not allocated\n");
      return -ENOMEM;
    }

  ret = dvp_ctrl_config((uint32_t)(uintptr_t)g_camera_buf[0]);
  if (ret < 0) goto cleanup_ctrl;

  ret = dvp_irq_attach();
  if (ret < 0) goto cleanup_irq;

  g_dvp_vsync_count = 0;
  g_dvp_frame_count = 0;
  g_pingpong_count  = 0;
  g_drop_count      = 0;
  g_cur_buf         = 0;
  g_ready_buf       = -1;
  g_busy_buf        = -1;
  g_stream_active   = true;

  syslog(LOG_INFO,
         "[camera] Stream start: capturing %d frames "
         "(buf[0]=0x%08lX buf[1]=0x%08lX)\n",
         n_frames,
         (unsigned long)(uintptr_t)g_camera_buf[0],
         (unsigned long)(uintptr_t)g_camera_buf[1]);

  captured = 0;

  while (captured < n_frames)
    {
      uint32_t frame_addr;

      timeout_ms = 5000;
      while (timeout_ms > 0)
        {
          frame_addr = dvp_frame_get();
          if (frame_addr != 0) break;
          up_udelay(1000);
          timeout_ms--;
        }

      if (frame_addr == 0)
        {
          syslog(LOG_ERR, "[camera] TIMEOUT waiting for frame %d/%d\n",
                 captured + 1, n_frames);
          ret = -ETIMEDOUT;
          break;
        }

      captured++;

      {
        const uint8_t *buf = (const uint8_t *)(uintptr_t)frame_addr;
        uint32_t y_sum = 0;
        int samples = 0;

        for (i = 0; i < CAMERA_FRAME_SIZE; i += 16 * CAMERA_HRES)
          {
            int j;
            for (j = 0; j < CAMERA_HRES; j += 16)
              {
                y_sum += buf[i + j];
                samples++;
              }
          }

        if (captured == 1 || captured == n_frames || (captured % 10) == 0)
          {
            syslog(LOG_INFO,
                   "[camera] %d/%d  VSYNC=%lu  FRAME=%lu  "
                   "Y_avg=%lu  drop=%lu\n",
                   captured, n_frames,
                   (unsigned long)g_dvp_vsync_count,
                   (unsigned long)g_dvp_frame_count,
                   (unsigned long)(samples > 0 ? y_sum / samples : 0),
                   (unsigned long)g_drop_count);
          }
      }

      dvp_frame_put();
    }

  g_stream_active = false;

  elapsed_ms = g_dvp_frame_count > 0 ? (uint32_t)g_dvp_frame_count * 33 : 0;

  syslog(LOG_INFO,
         "[camera] Stream done: captured=%d  total=%lu  drop=%lu  vsync=%lu\n",
         captured,
         (unsigned long)g_pingpong_count,
         (unsigned long)g_drop_count,
         (unsigned long)g_dvp_vsync_count);

  if (elapsed_ms > 0)
    {
      syslog(LOG_INFO, "[camera] Elapsed: ~%lu ms  Avg FPS: ~%lu\n",
             (unsigned long)elapsed_ms,
             (unsigned long)(captured * 1000 / (elapsed_ms > 0 ? elapsed_ms : 1)));
    }

  ret = captured > 0 ? 0 : -EIO;

cleanup_irq:
  dvp_irq_detach();
cleanup_ctrl:
  dvp_ctrl_deconfig();
  return ret;
}

/****************************************************************************
 * UYVY → RGB565 scaled conversion
 *
 * GC2145 (reg 0x84 = 0x02) + BK7258 YUV_BUF (CTRL FMT=00) combination
 * produces UYVY byte order in PSRAM, verified by camera dump:
 *   byte0=U (~0x80 neutral), byte1=Y (luminance), byte2=V, byte3=Y
 *
 * UYVY 4-byte layout per 2 pixels:
 *   [U  Y0  V  Y1]
 *    b0  b1  b2  b3
 *
 * Source byte offset for pixel sx = sx * 2.
 *   even sx: Y=b0, U=b1, V=b3  (from p = row + sx*2)
 *   odd  sx: Y=b2, U=b1, V=b3  (from p = row + sx*2, same pair)
 * Simplified: always p = row + (sx & ~1)*2, then:
 *   Y = p[1 + (sx&1)*2],  U = p[0],  V = p[2]
 ****************************************************************************/

static void uyvy_to_rgb565_scaled(const uint8_t *src,
                                  uint8_t *dst,
                                  int src_w, int src_h,
                                  int dst_w, int dst_h)
{
  int sx, sy, dx, dy;
  uint8_t *out = dst;
  int scale_x = src_w / dst_w;
  int scale_y = src_h / dst_h;

  for (dy = 0; dy < dst_h; dy++)
    {
      sy = dy * scale_y;
      const uint8_t *row = src + sy * src_w * 2;

      for (dx = 0; dx < dst_w; dx++)
        {
          uint8_t y0;
          int cb_i, cr_i, r, g, b;
          uint16_t rgb;
          int pos;

          sx = dx * scale_x;

          /* UYVY: pair pointer = start of 4-byte group */

          const uint8_t *p = row + (sx & ~1) * 2;

          y0   = p[1 + (sx & 1) * 2];     /* Y0=p[1], Y1=p[3] */
          cb_i = (int)p[0] - 128;          /* U  = byte0 */
          cr_i = (int)p[2] - 128;          /* V  = byte2 */

          r = (int)y0 + cr_i + (cr_i * 51 / 128);
          g = (int)y0 - (cb_i * 28 / 81) - (cr_i * 365 / 512);
          b = (int)y0 + cb_i + (cb_i * 99 / 128);

          if (r < 0) r = 0;
          if (r > 255) r = 255;
          if (g < 0) g = 0;
          if (g > 255) g = 255;
          if (b < 0) b = 0;
          if (b > 255) b = 255;

          /* Store as big-endian RGB565 (hi byte first) to match
           * lcd_fill_rect byte order which drives this LCD correctly.
           */

          rgb = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
          pos = (dy * dst_w + dx) * 2;
          out[pos]     = (uint8_t)(rgb >> 8);    /* hi = R[4:0] G[5:3] */
          out[pos + 1] = (uint8_t)(rgb & 0xff);  /* lo = G[2:0] B[4:0] */
        }
    }
}

/****************************************************************************
 * Name: bk7258_camera_preview
 *
 * Description:
 *   Phase 1 Round 3 Step 6: capture + LCD preview.
 *
 ****************************************************************************/

int bk7258_camera_preview(int n_frames, int panel)
{
  int ret;
  int captured;
  int timeout_ms;
  int i;
  uint32_t t_start;
  uint32_t t_end;
  uint32_t now;
  uint32_t cap_ms;
  uint32_t disp_ms;
  volatile uint8_t *preview_buf;

  preview_buf = (volatile uint8_t *)PREVIEW_ADDR;

  if (n_frames <= 0) n_frames = 100;
  else if (n_frames > STREAM_BUF_MAX) n_frames = STREAM_BUF_MAX;

  if (!g_dvp_pins_configed)
    {
      syslog(LOG_ERR, "[camera] DVP pins not configured\n");
      return -ENODEV;
    }

  if (!g_framebuf_allocated)
    {
      syslog(LOG_ERR, "[camera] Frame buffers not allocated\n");
      return -ENOMEM;
    }

  ret = dvp_ctrl_config((uint32_t)(uintptr_t)g_camera_buf[0]);
  if (ret < 0) goto cleanup_ctrl;

  ret = dvp_irq_attach();
  if (ret < 0) goto cleanup_irq;

  g_dvp_vsync_count = 0;
  g_dvp_frame_count = 0;
  g_pingpong_count  = 0;
  g_drop_count      = 0;
  g_cur_buf         = 0;
  g_ready_buf       = -1;
  g_busy_buf        = -1;
  g_stream_active   = true;

  /* Initialize LCD (LDO_3V3 + backlight + SPI + GC9D01) */

  ret = bk7258_lcd_preview_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] LCD preview init failed: %d\n", ret);
      goto stop_stream;
    }

  syslog(LOG_INFO,
         "[camera] Preview start: %d frames to LCD (160x160) panel=%d\n",
         n_frames, panel);

  /* Warm-up — discard first 30 frames (sensor AE/AWB convergence) */

  for (i = 0; i < 30; i++)
    {
      uint32_t frame_addr;

      timeout_ms = 1000;
      while (timeout_ms > 0)
        {
          frame_addr = dvp_frame_get();
          if (frame_addr != 0) break;
          up_udelay(1000);
          timeout_ms--;
        }

      if (frame_addr == 0)
        {
          syslog(LOG_ERR, "[camera] TIMEOUT during warm-up frame %d\n", i);
          ret = -ETIMEDOUT;
          goto stop_stream;
        }

      dvp_frame_put();
    }

  captured = 0;
  t_start = 0;
  t_end   = 0;

  while (captured < n_frames)
    {
      uint32_t frame_addr;

      timeout_ms = 5000;
      while (timeout_ms > 0)
        {
          frame_addr = dvp_frame_get();
          if (frame_addr != 0) break;
          up_udelay(1000);
          timeout_ms--;
        }

      if (frame_addr == 0)
        {
          syslog(LOG_ERR, "[camera] TIMEOUT waiting for preview frame %d/%d\n",
                 captured + 1, n_frames);
          ret = -ETIMEDOUT;
          break;
        }

      now = g_dvp_frame_count * 33;
      if (captured == 0) t_start = now;

      uyvy_to_rgb565_scaled((const uint8_t *)(uintptr_t)frame_addr,
                            (uint8_t *)preview_buf,
                            CAMERA_HRES, CAMERA_VRES,
                            PREVIEW_W, PREVIEW_H);

      bk7258_lcd_blit_rgb565(panel, 0, 0, PREVIEW_W, PREVIEW_H,
                              (const uint8_t *)preview_buf);

      t_end = now;
      captured++;

      dvp_frame_put();

      if (captured % 10 == 0 || captured == n_frames)
        {
          cap_ms  = t_end - t_start;
          disp_ms = cap_ms > 0 ? cap_ms : 1;

          syslog(LOG_INFO,
                 "[camera] %d/%d  cap=%lu  drop=%lu  elapsed=%lums  fps=~%lu\n",
                 captured, n_frames,
                 (unsigned long)g_pingpong_count,
                 (unsigned long)g_drop_count,
                 (unsigned long)cap_ms,
                 (unsigned long)(captured * 1000 / disp_ms));
        }
    }

  ret = captured > 0 ? 0 : -EIO;

stop_stream:
  g_stream_active = false;

  cap_ms = t_end > t_start ? (t_end - t_start) : 0;

  syslog(LOG_INFO,
         "[camera] Preview done: captured=%d  total=%lu  drop=%lu  vsync=%lu\n",
         captured,
         (unsigned long)g_pingpong_count,
         (unsigned long)g_drop_count,
         (unsigned long)g_dvp_vsync_count);

  if (cap_ms > 0)
    {
      syslog(LOG_INFO,
             "[camera] Capture: ~%lu ms  Capture FPS: ~%lu  Display FPS: ~%lu\n",
             (unsigned long)cap_ms,
             (unsigned long)(captured * 1000 / cap_ms),
             (unsigned long)(captured * 1000 / cap_ms));
    }

  bk7258_lcd_preview_deinit();

cleanup_irq:
  dvp_irq_detach();
cleanup_ctrl:
  dvp_ctrl_deconfig();
  return ret;
}

/****************************************************************************
 * Name: yuv_to_rgb
 *
 * Description:
 *   Shared YUV→RGB helper for dump diagnostics.
 *   Input: Y, U, V as raw bytes (0-255).
 *   Output: r, g, b clamped to 0-255.
 *
 ****************************************************************************/

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                        int *r, int *g, int *b)
{
  int yi = (int)y;
  int cb = (int)u - 128;
  int cr = (int)v - 128;

  *r = yi + cr + (cr * 51 / 128);
  *g = yi - (cb * 28 / 81) - (cr * 365 / 512);
  *b = yi + cb + (cb * 99 / 128);

  if (*r < 0) *r = 0;  if (*r > 255) *r = 255;
  if (*g < 0) *g = 0;  if (*g > 255) *g = 255;
  if (*b < 0) *b = 0;  if (*b > 255) *b = 255;
}

/****************************************************************************
 * Name: bk7258_camera_dump
 *
 * Description:
 *   Capture one frame, hexdump first 64 bytes, then interpret under
 *   all 4 YUV byte-orderings and print RGB for each pixel.
 *
 *   Diagnostic key: in a neutral (grey/white) scene, U and V should
 *   be near 0x80.  The format where U and V cluster around 0x80 is
 *   the correct one.
 *
 ****************************************************************************/

int bk7258_camera_dump(void)
{
  int ret;
  int timeout_ms;
  uint32_t frame_start;
  volatile uint8_t *buf;
  int i;

  if (!g_dvp_pins_configed)
    {
      syslog(LOG_ERR, "[camera] DVP pins not configured\n");
      return -ENODEV;
    }

  if (!g_framebuf_allocated)
    {
      ret = camera_framebuf_alloc();
      if (ret < 0) return ret;
    }

  /* Fill with 0xCC to detect if DMA wrote */

  buf = g_camera_buf[0];
  for (i = 0; i < 256; i++)
    ((volatile uint8_t *)buf)[i] = 0xCC;

  /* Configure DVP and capture one frame */

  ret = dvp_ctrl_config((uint32_t)(uintptr_t)g_camera_buf[0]);
  if (ret < 0) return ret;

  ret = dvp_irq_attach();
  if (ret < 0) { dvp_ctrl_deconfig(); return ret; }

  frame_start = g_dvp_frame_count;
  timeout_ms = 5000;

  while (timeout_ms > 0)
    {
      if (g_dvp_frame_count > frame_start) break;
      up_udelay(1000);
      timeout_ms--;
    }

  dvp_irq_detach();
  dvp_ctrl_deconfig();

  if (g_dvp_frame_count <= frame_start)
    {
      syslog(LOG_ERR, "[camera] dump: TIMEOUT — no frame\n");
      return -ETIMEDOUT;
    }

  /* Check if DMA wrote (not still 0xCC) */

  if (buf[0] == 0xCC && buf[1] == 0xCC && buf[2] == 0xCC)
    {
      syslog(LOG_ERR,
             "[camera] dump: buf still 0xCC — DMA did not write\n");
      return -EIO;
    }

  /* Hexdump first 64 bytes */

  syslog(LOG_INFO, "[camera] dump: first 64 bytes of captured frame:\n");
  for (i = 0; i < 64; i += 16)
    {
      syslog(LOG_INFO,
             "  %04X: %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X\n",
             i,
             buf[i+0],  buf[i+1],  buf[i+2],  buf[i+3],
             buf[i+4],  buf[i+5],  buf[i+6],  buf[i+7],
             buf[i+8],  buf[i+9],  buf[i+10], buf[i+11],
             buf[i+12], buf[i+13], buf[i+14], buf[i+15]);
    }

  /* Interpret first 8 pixels (16 bytes) under all 4 formats */

  syslog(LOG_INFO, "[camera] dump: RGB under 4 YUV interpretations:\n");

  /* Format definitions:
   *   YUYV: [Y0  U  Y1  V ] [Y2  U  Y3  V ] ...
   *   UYVY: [U  Y0  V  Y1] [U  Y2  V  Y3] ...
   *   YYUV: [Y0 Y1  U   V ] [Y2 Y3  U   V ] ...
   *   UVYY: [U   V  Y0 Y1] [U   V  Y2 Y3] ...
   */

  for (i = 0; i < 8; i++)
    {
      int off = i * 2;
      uint8_t b0 = buf[off];
      uint8_t b1 = buf[off + 1];
      int r, g, b;
      char line[256];
      int n;

      /* For paired formats, determine which pair we're in */

      int pair_off = (i / 2) * 4;
      uint8_t pb0 = buf[pair_off];
      uint8_t pb1 = buf[pair_off + 1];
      uint8_t pb2 = buf[pair_off + 2];
      uint8_t pb3 = buf[pair_off + 3];

      n = snprintf(line, sizeof(line),
                   "  pix[%d] [%02X %02X]:", i, b0, b1);

      /* YUYV: Y=even byte, U=byte1 of pair, V=byte3 of pair */

      {
        uint8_t y = (i & 1) ? pb2 : pb0;
        uint8_t u = pb1;
        uint8_t v = pb3;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        n += snprintf(line + n, sizeof(line) - n,
                      " YUYV(Y=%02X U=%02X V=%02X>R%3dG%3dB%3d)",
                      y, u, v, r, g, b);
      }

      /* UYVY: U=byte0 of pair, Y=byte1 or byte3, V=byte2 of pair */

      {
        uint8_t u = pb0;
        uint8_t y = (i & 1) ? pb3 : pb1;
        uint8_t v = pb2;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        n += snprintf(line + n, sizeof(line) - n,
                      " UYVY(Y=%02X U=%02X V=%02X>R%3dG%3dB%3d)",
                      y, u, v, r, g, b);
      }

      /* YYUV: Y0=b0, Y1=b1, U=b2, V=b3 (per pair) */

      {
        uint8_t y = (i & 1) ? pb1 : pb0;
        uint8_t u = pb2;
        uint8_t v = pb3;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        n += snprintf(line + n, sizeof(line) - n,
                      " YYUV(Y=%02X U=%02X V=%02X>R%3dG%3dB%3d)",
                      y, u, v, r, g, b);
      }

      /* UVYY: U=b0, V=b1, Y0=b2, Y1=b3 (per pair) */

      {
        uint8_t u = pb0;
        uint8_t v = pb1;
        uint8_t y = (i & 1) ? pb3 : pb2;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        n += snprintf(line + n, sizeof(line) - n,
                      " UVYY(Y=%02X U=%02X V=%02X>R%3dG%3dB%3d)",
                      y, u, v, r, g, b);
      }

      syslog(LOG_INFO, "%s\n", line);
    }

  syslog(LOG_INFO,
         "[camera] dump: correct format = the one where U/V ≈ 0x80\n");

  return 0;
}

/****************************************************************************
 * Name: bk7258_camera_testpat
 *
 * Description:
 *   Bypass camera — generate known RGB565 color bars and blit to LCD.
 *   If bars display correctly, blit + byte order are fine and the
 *   problem is in YUYV conversion.  If bars also look wrong, the
 *   problem is in the LCD path.
 *
 ****************************************************************************/

int bk7258_camera_testpat(void)
{
  volatile uint8_t *buf = (volatile uint8_t *)PREVIEW_ADDR;
  uint8_t *p = (uint8_t *)buf;
  int ret;
  int x, y;

  /* 8 vertical color bars, each 20px wide (20*8 = 160) */

  static const uint16_t bars[8] =
    {
      0xF800,  /* red         11111_000000_00000 */
      0x07E0,  /* green       00000_111111_00000 */
      0x001F,  /* blue        00000_000000_11111 */
      0xFFE0,  /* yellow      11111_111111_00000 */
      0x07FF,  /* cyan        00000_111111_11111 */
      0xF81F,  /* magenta     11111_000000_11111 */
      0xFFFF,  /* white       11111_111111_11111 */
      0x0000,  /* black       00000_000000_00000 */
    };

  /* Auto-init PSRAM — testpat must be runnable standalone */

  if (!g_framebuf_allocated)
    {
      ret = camera_framebuf_alloc();
      if (ret < 0)
        {
          syslog(LOG_ERR, "[camera] testpat: PSRAM init failed: %d\n", ret);
          return ret;
        }
    }

  /* Generate big-endian RGB565 in preview buffer */

  for (y = 0; y < PREVIEW_H; y++)
    {
      for (x = 0; x < PREVIEW_W; x++)
        {
          int bar = x / 20;
          uint16_t c = bars[bar];
          int pos = (y * PREVIEW_W + x) * 2;
          p[pos]     = (uint8_t)(c >> 8);
          p[pos + 1] = (uint8_t)(c & 0xff);
        }
    }

  /* Readback verify — first pixel should be red (0xF8, 0x00) */

  syslog(LOG_INFO,
         "[camera] testpat: buf[0]=0x%02X buf[1]=0x%02X "
         "(expect 0xF8 0x00 = red)\n",
         buf[0], buf[1]);

  if (buf[0] != 0xF8 || buf[1] != 0x00)
    {
      syslog(LOG_ERR,
             "[camera] testpat: PSRAM write FAILED — "
             "data did not stick (got 0x%02X 0x%02X)\n",
             buf[0], buf[1]);
      return -EIO;
    }

  /* Spot-check a few more pixels */

  {
    int pos_g  = (20 * 2) * 2;   /* x=20, first green pixel */
    int pos_b  = (40 * 2) * 2;   /* x=40, first blue pixel */
    int pos_w  = (120 * 2) * 2;  /* x=120, first white pixel */

    syslog(LOG_INFO,
           "[camera] testpat: green=0x%02X%02X blue=0x%02X%02X "
           "white=0x%02X%02X\n",
           buf[pos_g], buf[pos_g + 1],
           buf[pos_b], buf[pos_b + 1],
           buf[pos_w], buf[pos_w + 1]);
  }

  /* Initialize LCD and blit */

  ret = bk7258_lcd_preview_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[camera] testpat: LCD init failed: %d\n", ret);
      return ret;
    }

  bk7258_lcd_blit_rgb565(0, 0, 0, PREVIEW_W, PREVIEW_H, (const uint8_t *)buf);

  syslog(LOG_INFO,
         "[camera] testpat: 8 color bars blitted to LCD (160x160)\n");
  syslog(LOG_INFO,
         "[camera] Expected: R G B Y C M W K from left to right\n");

  return 0;
}

/****************************************************************************
 * DEBUG_JOURNAL - GC2145/DVP Hardware Notes
 ****************************************************************************
 *
 * Companion to the DEBUG_JOURNAL block in bk7258_gc9d01.c (SPI/LCD side)
 * and to board/contest_board/DEBUG_JOURNAL_zh-cn.md section 24.
 *
 * ========================================================================
 * Confirmed Hardware Facts
 * ========================================================================
 *
 * SCCB (sensor control bus):
 *   Bit-banged, open-drain.  GC2145 7-bit address 0x3C.
 *   Chip ID: reg 0xF0 = 0x21, reg 0xF1 = 0x45.
 *   "Drive low" = output_enable(bit3)=0 + output_value(bit1)=0 +
 *   second_func(bit6)=0.  "Release" = output_enable=1 (disabled) +
 *   input_enable(bit2)=1.  Never drive high: the bus has an external
 *   pull-up and driving high fights other devices.
 *
 * MCLK:
 *   Must be on P27 with function select = 1.  No other pin works; the
 *   clock output is hard-wired to that pad function.
 *   Source select and divider live in the 0x44010000 SYS register block.
 *
 * DVP data bus:
 *   P29-P39 (11 pins: PCLK, HSYNC, VSYNC, PXDATA0-7), function select 0.
 *   CFG per pin: output disabled, input disabled, pull disabled,
 *   second_func = 1.  Note function select 0 and second_func 1 are two
 *   different fields in two different registers; both are required.
 *
 * Interrupt path:
 *   The DVP (YUV_BUF/CIS) interrupt does NOT reach the NVIC directly.
 *   BK7258 has a SYS-level interrupt aggregator in front of the NVIC:
 *   cpu1_int_32_63_en at 0x4401008C must be enabled as well.  This is
 *   the same class of trap as the UART RX issue in section 18.27 of the
 *   markdown journal - two gates in series, enabling only the NVIC one
 *   leaves the interrupt permanently silent.
 *
 * Module clock gate:
 *   sys_reserver_reg0xd at 0x44010034, bit[9] = cis_auxs_cken.
 *
 * Frame buffers:
 *   YUYV 640x480 = 614400 bytes per frame, two buffers in PSRAM at
 *   0x60000000.  DVP DMA needs 32/64-byte alignment, which plain
 *   mm_malloc cannot guarantee - this is why bk7258_psram_memalign()
 *   was added during the PSRAM S4 stage (see markdown section 22.12).
 *   Preview scratch buffer at 0x6012C000, 160x160x2 = 51200 bytes.
 *
 * ========================================================================
 * Pixel Format: UYVY, not YUYV
 * ========================================================================
 *
 * GC2145 register 0x84 = 0x02 produces UYVY.  ARMino's dvp_gc2145.c
 * annotates this value as "yuyv", which is wrong.  Verified by dumping
 * PSRAM after a capture: the actual byte order is U Y0 V Y1.
 *
 * Getting this backwards does not produce garbage - it produces a
 * plausible-looking but wrongly-tinted image, which is why it survived
 * several debugging rounds.  The symptom that finally exposed it was
 * colour channels swapping when the sampling offset shifted by one byte.
 *
 * Identifier naming in this file still says YUYV in places because it
 * tracks the ARMino register table it was ported from; the runtime
 * conversion path (uyvy_to_rgb565_scaled) uses the correct order.
 *
 * ========================================================================
 * Ping-Pong Buffer Ownership
 * ========================================================================
 *
 * Three-state ownership avoids the DVP-vs-consumer race:
 *
 *   g_cur_buf    the buffer DVP is currently writing into
 *   g_ready_buf  a complete frame waiting to be consumed (-1 = none)
 *   g_busy_buf   the buffer a consumer is currently reading (-1 = none)
 *
 * On YUV_ARV (frame complete) in ISR context:
 *   advance only if g_ready_buf < 0 AND next_buf != g_busy_buf.
 *   Otherwise increment g_drop_count and keep writing the same buffer.
 *
 * The point is that the ISR never hands DVP a buffer someone else is
 * reading, and never overwrites an undelivered frame.  Dropping a frame
 * is always preferable to tearing one.  The DVP write address register
 * (YUV_BUF_REG_0x08) is reprogrammed inside the ISR, so the handoff is
 * atomic with respect to the consumer.
 *
 * Before this change the consumer read whichever buffer DVP happened to
 * be filling, producing intermittent horizontal tearing that looked like
 * an SPI timing problem.  Worth remembering: a display artefact is not
 * necessarily a display bug.
 *
 * ========================================================================
 * Preview Performance (about 7 fps)
 * ========================================================================
 *
 * camera preview 30 left: about 127.6 ms/frame
 *   blit                about 50 ms   SPI transfer at 13MHz/burst 32
 *   downsample+convert  about 78 ms   dominant cost
 *
 * The 78 ms is not arithmetic, it is PSRAM access pattern.
 * uyvy_to_rgb565_scaled() reads 3 source bytes per output pixel, so a
 * 160x160 output means roughly 76800 scattered PSRAM reads.  PSRAM sits
 * behind QSPI, where per-access overhead dominates and only sequential
 * bursts approach the nominal bandwidth.
 *
 * Deliberately NOT optimised.  Full-screen preview is a bring-up and
 * demo path, not the production one.  Production is: capture to PSRAM,
 * run face detection there, emit a direction, and redraw only the eyes.
 * A 60x60 iris region is 7200 bytes, about 7 ms at 1000 KB/s, which is
 * comfortable for smooth animation.
 *
 * If it ever does need optimising, in order of preference:
 *   a) stage source scanlines into SRAM with memcpy first, turning
 *      scattered PSRAM reads into sequential bursts;
 *   b) configure DVP hardware crop so capture is already 160x160 and
 *      conversion becomes 1:1, removing the scatter entirely.
 *
 * This measurement is the input to the next work item (face detection):
 * the algorithm's memory access pattern, not its instruction count, is
 * what decides whether it fits.
 *
 * ========================================================================
 * Gotchas
 * ========================================================================
 *
 * a) 0x44010028 and 0x44010030 are SHARED between LCD and DVP clock
 *    configuration.  Always read-modify-write.  A blank overwrite
 *    silently corrupts the other subsystem's clock and the failure
 *    surfaces far away from the write.
 *
 * b) Save and restore the full 4-bit function field plus the pin CFG
 *    when taking over or releasing pins (gpio_state_t, mclk_state_t).
 *    Restoring a guessed default instead of the saved value breaks
 *    whatever owned the pin before.
 *
 * c) SDA and SCL must be distinct pins.  The code asserts this because
 *    a misconfigured Kconfig produced a silent "SDA=SCL" bus that
 *    ACKed nothing and looked like a dead sensor.
 *
 * d) Board-level reserved pins are checked before claiming DVP pins,
 *    because P29 was previously the left LCD reset line.  Pin
 *    reassignment between subsystems is the most common source of
 *    "it worked yesterday" regressions on this board.
 *
 ****************************************************************************/
