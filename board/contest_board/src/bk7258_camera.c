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
 * Phase 1: DVP data pin mux (P29-P39), GC2145 RGB565 init, PSRAM buffer
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
 *   Output format: reg 0x84 = 0x06 (RGB565), 640x480
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

#include "bk7258_gpio.h"
#include "bk7258_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

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

/* Frame buffer parameters — RGB565 640x480 */

#define CAMERA_HRES       640
#define CAMERA_VRES       480
#define CAMERA_BPP        2   /* RGB565 = 2 bytes per pixel */
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

/****************************************************************************
 * GC2145 Init Table (RGB565 640x480)
 *
 * Ported from ARMino dvp_gc2145.c (Apache-2.0, Galois Inc).
 * Output format: register 0x84 = 0x06 (RGB565).
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
  {0x84, 0x06},  /* RGB565 (was 0x02=YUYV in ARMino) */
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
 * Each buffer is 640 * 480 * 2 = 614400 bytes (RGB565).
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
        for (r = 0; r < 15; r++)
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
 *   table (RGB565 640x480), readback verify, keep MCLK/power on for
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
    if (val84 != 0x06)
      {
        syslog(LOG_ERR,
               "[camera] Reg 0x84 mismatch: 0x%02X (expected 0x06=RGB565)\n",
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
         "[camera] GC2145 init complete — RGB565 640x480\n");
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
