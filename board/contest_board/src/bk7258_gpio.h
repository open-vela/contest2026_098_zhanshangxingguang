/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_gpio.h
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

#ifndef __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_GPIO_H
#define __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Inline Register Access
 *
 * getreg32/putreg32 are arch-private inlines in arm_internal.h.
 * Board-level drivers that touch MMIO directly need their own copy.
 ****************************************************************************/

static inline uint32_t getreg32(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static inline void putreg32(uint32_t val, uintptr_t addr)
{
  *(volatile uint32_t *)addr = val;
}

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SoC base addresses (self-contained — no arch memorymap.h dependency) */

#define BK7258_AON_GPIO_BASE     0x44000400
#define BK7258_SYS_BASE          0x44010000

/* AON GPIO register: 4 bytes per pin */

#define BK7258_GPIO_CFG(n)       (BK7258_AON_GPIO_BASE + (n) * 4)

/* Per-GPIO config register bits (from ARMINO gpio_struct.h) */

#define GPIO_CFG_INPUT           (1u << 0) /* bit[0] gpio_input (RO) */
#define GPIO_CFG_OUTPUT          (1u << 1) /* bit[1] gpio_output (R/W) */
#define GPIO_CFG_INPUT_EN        (1u << 2) /* bit[2] input enable */
#define GPIO_CFG_OUTPUT_EN       (1u << 3) /* bit[3] output enable (ACTIVE LOW:
                                            * 0 = enable, 1 = disable.
                                            * Opposite of input_en which is
                                            * active high.) */
#define GPIO_CFG_PULL_UP         (1u << 4) /* bit[4] pull mode: 1=up */
#define GPIO_CFG_PULL_EN         (1u << 5) /* bit[5] pull enable */
#define GPIO_CFG_SECOND_FUNC     (1u << 6) /* bit[6] second function */

/* System GPIO function select registers (4 bits per pin, 8 pins per reg).
 * Pins 0-7:   @ BK7258_SYS_BASE + 0xC0
 * Pins 8-15:  @ BK7258_SYS_BASE + 0xC4
 * Pins 16-23: @ BK7258_SYS_BASE + 0xC8
 * Pins 24-31: @ BK7258_SYS_BASE + 0xCC
 * Pins 32-39: @ BK7258_SYS_BASE + 0xD0
 * Pins 40-47: @ BK7258_SYS_BASE + 0xD4
 * Pins 48-55: @ BK7258_SYS_BASE + 0xD8
 */

#define BK7258_SYS_GPIO_FUNC_BASE  (BK7258_SYS_BASE + 0xc0)
#define BK7258_SYS_GPIO_FUNC(pin)  (BK7258_SYS_GPIO_FUNC_BASE + ((pin) / 8) * 4)
#define BK7258_GPIO_FUNC_SHIFT(pin) (((pin) % 8) * 4)
#define BK7258_GPIO_FUNC_MASK       0xfu

#endif /* __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_GPIO_H */
