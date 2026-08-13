/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/include/board.h
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

#ifndef __BOARDS_BK7258_DEVKIT_INCLUDE_BOARD_H
#define __BOARDS_BK7258_DEVKIT_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clocking
 *
 * BK7258: dual-core Armv8-M STAR-MC1 (Cortex-M33 compatible, with FPU), up
 * to 480 MHz.  Values below are from the BK7258 Datasheet
 * (DS-BK7258-E12 V2.1), section 4.3 clock management.
 */

#define BOARD_XTAL_FREQUENCY   26000000    /* High-freq XTAL = 26 MHz */
#define BOARD_XTALL_FREQUENCY  32768       /* Low-freq XTAL = 32.768 kHz */
#define BOARD_CPU_FREQUENCY    480000000   /* DPLL output, 480 MHz */

/* SysTick clock source frequency.  TODO(calibrate): the STAR-MC1 SysTick
 * source (CPU clock or a divider) must be checked against the actual clock
 * config; estimated from the CPU frequency for now.
 */

#define BOARD_SYSTICK_CLOCK    BOARD_CPU_FREQUENCY

/* Memory map
 *
 * Addresses from ARMINO SDK ap/include/soc/bk7258/reg_base.h.  Secure and
 * Non-secure addresses differ by 0x10000000; the values below are the
 * Secure view (Non-secure view = base + 0x10000000).
 *
 * The shared 640KB SRAM is SRAM0..SRAM5 laid out contiguously from
 * 0x28000000 in the data view.
 */

#define BOARD_SRAM_BASE        0x28000000
#define BOARD_SRAM_SIZE        (640 * 1024)

#define BOARD_ITCM_BASE        0x00000000  /* Per-core private ITCM */
#define BOARD_DTCM_BASE        0x20000000  /* Per-core private DTCM */

#define BOARD_PSRAM_BASE       0x60000000  /* PSRAM window (64MB) */
#define BOARD_PSRAM_SIZE       (16 * 1024 * 1024)  /* Measured: ID=0x8d08
                                                     * (APS128XXO 16MB).
                                                     * Was 8MB (suspect);
                                                     * corrected after
                                                     * S1 psram probe. */

#define BOARD_FLASH_BASE       0x02000000  /* Flash XIP data base */
#define BOARD_FLASH_SIZE       (8 * 1024 * 1024)

#define BOARD_QSPI0_BASE       0x64000000  /* QSPI0 data window */
#define BOARD_QSPI1_BASE       0x68000000  /* QSPI1 data window */

/* Peripheral register base addresses (Secure view, from reg_base.h) */

#define BOARD_UART0_REG_BASE    0x44820000
#define BOARD_UART1_REG_BASE    0x45830000
#define BOARD_UART2_REG_BASE    0x45840000
#define BOARD_SYS_REG_BASE      0x44010000
#define BOARD_AON_PMU_REG_BASE  0x44000000
#define BOARD_AON_GPIO_REG_BASE 0x44000400
#define BOARD_LCD_DISP_REG_BASE 0x48060000
#define BOARD_SLCD_REG_BASE     0x458e0000
#define BOARD_QSPI0_REG_BASE    0x46040000
#define BOARD_QSPI1_REG_BASE    0x46060000

/* UART console pins
 *
 * The board-mounted CH340 is UART0 (also the flash download DL_UART), so
 * download and console share one cable.  NuttX runs on CPU0 and uses UART0
 * as the NSH console.
 */

/* LED status mapping (optional) */

#define LED_STARTED       0
#define LED_HEAPALLOCATE  0
#define LED_IRQSENABLED   0
#define LED_STACKCREATED  0
#define LED_INIRQ         0
#define LED_SIGNAL        0
#define LED_ASSERTION     0
#define LED_PANIC         1

/* Display (QSPI LCD) -- filled in at the M3 stage.  The BK7258 DevKit has
 * two on-board QSPI panels.  TODO(datasheet): panel type, resolution, color
 * depth, QSPI timing, backlight/reset pins (BOARD_LCD_WIDTH/HEIGHT,
 * PIN_LCD_RESET, PIN_LCD_BACKLIGHT, ...).
 */

#endif /* __BOARDS_BK7258_DEVKIT_INCLUDE_BOARD_H */
