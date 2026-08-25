/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_accel.c
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
 * SC7A20H 3-axis accelerometer (Silan, LIS2DH12-register-compatible).
 * Bit-bang open-drain I2C on IIC1: SCL=P20, SDA=P21 (4.7k pull-ups on board,
 * CS tied high = I2C mode).  INT1=P48 (unused for now, polling).
 *
 * NOTE: P20/P21 double as SWD (SWCLK/SWDIO).  Using them as GPIO I2C
 * disables SWD debug — fine here since we flash via UART bootloader.
 ****************************************************************************/

#include <nuttx/config.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <debug.h>
#include <nuttx/arch.h>

#include "bk7258_gpio.h"

/****************************************************************************
 * Pins / timing
 ****************************************************************************/

#define ACCEL_SCL_PIN   20
#define ACCEL_SDA_PIN   21
#define ACCEL_INT_PIN   48        /* G_INT1, unused for now */

#define I2C_DELAY_US    10        /* ~50 kHz, 稳字当头 */

/* SC7A20H / LIS2DH register map */

#define SC7_ADDR_LO     0x18      /* SA0=0 */
#define SC7_ADDR_HI     0x19      /* SA0=1 */
#define SC7_WHO_AM_I    0x0f
#define SC7_CTRL_REG1   0x20
#define SC7_CTRL_REG4   0x23
#define SC7_OUT_X_L     0x28
#define SC7_AUTO_INC    0x80      /* sub-addr bit7: multi-byte auto-increment */

#define SC7_CTRL_REG3   0x22
#define SC7_CLICK_CFG   0x38
#define SC7_CLICK_SRC   0x39
#define SC7_CLICK_THS   0x3a
#define SC7_TIME_LIMIT  0x3b

/****************************************************************************
 * GPIO open-drain helpers (self-contained, no camera.c dependency)
 ****************************************************************************/

static void accel_pin_gpio_mode(int pin)
{
  uintptr_t fn = BK7258_SYS_GPIO_FUNC(pin);
  uint32_t v = getreg32(fn);
  v &= ~(BK7258_GPIO_FUNC_MASK << BK7258_GPIO_FUNC_SHIFT(pin));
  putreg32(v, fn);   /* select GPIO (first) function; frees P20/P21 from SWD */
}

static void pin_drive_low(int pin)
{
  uintptr_t addr = BK7258_GPIO_CFG(pin);
  uint32_t cfg = getreg32(addr);
  cfg &= ~GPIO_CFG_SECOND_FUNC;  /* GPIO mode        */
  cfg &= ~GPIO_CFG_OUTPUT;       /* value = 0        */
  cfg &= ~GPIO_CFG_OUTPUT_EN;    /* output enabled   */
  putreg32(cfg, addr);
}

static void pin_drive_high(int pin)   /* push-pull HIGH (for SCL) */
{
  uintptr_t addr = BK7258_GPIO_CFG(pin);
  uint32_t cfg = getreg32(addr);
  cfg &= ~GPIO_CFG_SECOND_FUNC;  /* GPIO mode      */
  cfg |=  GPIO_CFG_OUTPUT;       /* value = 1      */
  cfg &= ~GPIO_CFG_OUTPUT_EN;    /* output enabled */
  putreg32(cfg, addr);
}

static void pin_release(int pin)   /* high-Z: pulled up by external 4.7k */
{
  uintptr_t addr = BK7258_GPIO_CFG(pin);
  uint32_t cfg = getreg32(addr);
  cfg &= ~GPIO_CFG_SECOND_FUNC;
  cfg |=  GPIO_CFG_OUTPUT_EN;     /* output DISABLED (active-low bit) */
  cfg |=  GPIO_CFG_INPUT_EN;      /* input enabled    */
  putreg32(cfg, addr);
}

static bool pin_read(int pin)
{
  return (getreg32(BK7258_GPIO_CFG(pin)) & GPIO_CFG_INPUT) != 0;
}

/****************************************************************************
 * I2C bit-bang primitives (open-drain, no clock stretch)
 ****************************************************************************/

static void scl_high(void) { pin_drive_high(ACCEL_SCL_PIN); up_udelay(I2C_DELAY_US); }
static void scl_low(void)  { pin_drive_low(ACCEL_SCL_PIN);  up_udelay(I2C_DELAY_US); }
static void sda_high(void) { pin_release(ACCEL_SDA_PIN);  }   /* open-drain: 靠上拉 */
static void sda_low(void)  { pin_drive_low(ACCEL_SDA_PIN); }

static void i2c_start(void)
{
  sda_high(); scl_high();
  sda_low();  up_udelay(I2C_DELAY_US);
  scl_low();
}

static void i2c_stop(void)
{
  sda_low();  scl_high();
  sda_high(); up_udelay(I2C_DELAY_US);
}

/* returns true if ACK received */

static bool i2c_write_byte(uint8_t b)
{
  bool ack;
  int i;

  for (i = 7; i >= 0; i--)
    {
      if (b & (1u << i)) sda_high();
      else               sda_low();
      up_udelay(I2C_DELAY_US);   /* data setup */
      scl_high();                /* latch on rising edge */
      scl_low();
    }

  /* ACK clock */

  sda_high();
  up_udelay(I2C_DELAY_US);
  scl_high();
  ack = !pin_read(ACCEL_SDA_PIN);   /* 0 = ACK */
  scl_low();
  return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
  uint8_t b = 0;
  int i;

  sda_high();   /* release for slave to drive */
  for (i = 7; i >= 0; i--)
    {
      scl_high();                                  /* rising edge + settle */
      if (pin_read(ACCEL_SDA_PIN)) b |= (1u << i); /* sample while SCL high */
      scl_low();
    }

  if (ack) sda_low();
  else     sda_high();
  up_udelay(I2C_DELAY_US);
  scl_high();
  scl_low();
  sda_high();
  return b;
}

/****************************************************************************
 * Register access
 ****************************************************************************/

static int sc7_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
  i2c_start();
  if (!i2c_write_byte((addr << 1) | 0)) { i2c_stop(); return -1; }
  if (!i2c_write_byte(reg))             { i2c_stop(); return -1; }
  if (!i2c_write_byte(val))             { i2c_stop(); return -1; }
  i2c_stop();
  return 0;
}

static int sc7_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, int n)
{
  int i;

  i2c_start();
  if (!i2c_write_byte((addr << 1) | 0)) { i2c_stop(); return -1; }
  if (!i2c_write_byte(reg))             { i2c_stop(); return -1; }

  i2c_start();  /* repeated start */
  if (!i2c_write_byte((addr << 1) | 1)) { i2c_stop(); return -1; }

  for (i = 0; i < n; i++)
    {
      buf[i] = i2c_read_byte(i < (n - 1));   /* ACK all but last */
    }

  i2c_stop();
  return 0;
}

/****************************************************************************
 * Name: accel_gesture_demo
 *   Continuous loop: hardware single-click "tap" + gravity-vector "pickup".
 *   Runs until Enter.  Foundation for velapet gesture->emotion hookup.
 ****************************************************************************/

static int accel_gesture_demo(uint8_t addr)
{
  int stdin_flags;
  bool was_flat = true;

  /* 400 Hz (reliable click), XYZ enable; ±2g high-res */

  sc7_write_reg(addr, SC7_CTRL_REG1, 0x77);
  sc7_write_reg(addr, SC7_CTRL_REG4, 0x08);

  /* Click engine: single-click on X/Y/Z, latched, threshold + time window.
   * CLICK_THS bit7=LIR (latch until CLICK_SRC read).  Tune 0x20 if needed.
   */

  sc7_write_reg(addr, SC7_CLICK_CFG, 0x15);        /* single-click XYZ */
  sc7_write_reg(addr, SC7_CLICK_THS, 0x80 | 0x0c); /* latch + ~190mg, 隔壳更灵敏 */
  sc7_write_reg(addr, SC7_TIME_LIMIT, 0x18);       /* 放宽冲击时间窗 ~60ms */
  up_mdelay(20);

  stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);

  syslog(LOG_INFO,
         "[accel] gesture demo: TAP the board / PICK it up; press Enter to stop\n");

  while (true)
    {
      uint8_t buf[6];
      uint8_t src = 0;
      char c;

      if (read(STDIN_FILENO, &c, 1) == 1)
        {
          break;
        }

      /* --- Tap: hardware click engine (CLICK_SRC bit6 = IA) --- */

      if (sc7_read_regs(addr, SC7_CLICK_SRC, &src, 1) == 0 && (src & 0x40))
        {
          const char *ax = (src & 0x01) ? "X" :
                           (src & 0x02) ? "Y" :
                           (src & 0x04) ? "Z" : "?";
          syslog(LOG_INFO, "[accel] TAP! axis=%s src=0x%02x\n", ax, src);
        }

      /* --- Pickup: gravity vector leaves the flat pose --- */

      if (sc7_read_regs(addr, SC7_OUT_X_L | SC7_AUTO_INC, buf, 6) == 0)
        {
          int x = (int16_t)((buf[1] << 8) | buf[0]) >> 4;
          int y = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
          int z = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
          bool flat = (abs(z) > 800 && abs(x) < 350 && abs(y) < 350);

          if (was_flat && !flat)
            {
              syslog(LOG_INFO, "[accel] PICKUP  (x=%d y=%d z=%d)\n", x, y, z);
            }
          else if (!was_flat && flat)
            {
              syslog(LOG_INFO, "[accel] PUT DOWN (flat)\n");
            }

          was_flat = flat;
        }

      up_mdelay(30);
    }

  fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
  syslog(LOG_INFO, "[accel] gesture demo done\n");
  return 0;
}

/****************************************************************************
 * Embedded API (used by velapet gesture hookup) — persistent address
 ****************************************************************************/

static uint8_t g_accel_addr;

int bk7258_accel_probe(void)
{
  uint8_t who = 0;
  uint8_t cand[2] = { SC7_ADDR_HI, SC7_ADDR_LO };
  int i;

  g_accel_addr = 0;

  accel_pin_gpio_mode(ACCEL_SCL_PIN);
  accel_pin_gpio_mode(ACCEL_SDA_PIN);
  pin_drive_high(ACCEL_SCL_PIN);
  pin_release(ACCEL_SDA_PIN);
  up_mdelay(5);

  for (i = 0; i < 2; i++)
    {
      if (sc7_read_regs(cand[i], SC7_WHO_AM_I, &who, 1) == 0)
        {
          g_accel_addr = cand[i];
          break;
        }
    }

  if (g_accel_addr == 0)
    {
      return -1;
    }

  /* 400 Hz + ±2g HR; single-click XYZ, latched, ~310mg, ~60ms window */

  sc7_write_reg(g_accel_addr, SC7_CTRL_REG1,  0x77);
  sc7_write_reg(g_accel_addr, SC7_CTRL_REG4,  0x08);
  sc7_write_reg(g_accel_addr, SC7_CLICK_CFG,  0x15);
  sc7_write_reg(g_accel_addr, SC7_CLICK_THS,  0x80 | 0x0c);   /* ~190mg, 隔壳更灵敏 */
  sc7_write_reg(g_accel_addr, SC7_TIME_LIMIT, 0x18);
  up_mdelay(20);

  syslog(LOG_INFO, "[accel] probe ok @0x%02x WHO=0x%02x\n", g_accel_addr, who);
  return 0;
}

void bk7258_accel_sample(bool *tap, bool *flat)
{
  uint8_t src = 0;
  uint8_t buf[6];

  if (tap)  *tap  = false;
  if (flat) *flat = true;
  if (g_accel_addr == 0) return;

  /* Tap: hardware click latch (CLICK_SRC bit6 = IA) */

  if (sc7_read_regs(g_accel_addr, SC7_CLICK_SRC, &src, 1) == 0 && (src & 0x40))
    {
      if (tap) *tap = true;
    }

  /* Pose: gravity vector → flat or not */

  if (sc7_read_regs(g_accel_addr, SC7_OUT_X_L | SC7_AUTO_INC, buf, 6) == 0)
    {
      int x = (int16_t)((buf[1] << 8) | buf[0]) >> 4;
      int y = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
      int z = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
      if (flat) *flat = (abs(z) > 800 && abs(x) < 350 && abs(y) < 350);
    }
}

/****************************************************************************
 * Name: bk7258_accel_main
 *   NSH: lcdtest accel [n]  — probe SC7A20H, read WHO_AM_I, stream XYZ.
 ****************************************************************************/

int bk7258_accel_main(int argc, char *argv[])
{
  bool gesture = (argc > 1 && strcmp(argv[1], "g") == 0);
  int n = (!gesture && argc > 1) ? atoi(argv[1]) : 20;
  uint8_t addr = 0;
  uint8_t buf[6];
  uint8_t who = 0;
  int i;

  if (n < 1)   n = 1;
  if (n > 500) n = 500;

  /* Bus idle: both lines released (pulled high) */

  accel_pin_gpio_mode(ACCEL_SCL_PIN);
  accel_pin_gpio_mode(ACCEL_SDA_PIN);
  pin_drive_high(ACCEL_SCL_PIN);   /* SCL push-pull, idle high */
  pin_release(ACCEL_SDA_PIN);      /* SDA open-drain, idle high */
  up_mdelay(5);

  /* Address scan: try 0x19 then 0x18 */

  {
    uint8_t cand[2] = { SC7_ADDR_HI, SC7_ADDR_LO };

    for (i = 0; i < 2; i++)
      {
        if (sc7_read_regs(cand[i], SC7_WHO_AM_I, &who, 1) == 0)
          {
            addr = cand[i];
            break;
          }
      }
  }

  if (addr == 0)
    {
      syslog(LOG_ERR, "[accel] no ACK at 0x18/0x19 — check SCL=P20/SDA=P21 wiring\n");
      return -1;
    }

  syslog(LOG_INFO, "[accel] found at 0x%02x, WHO_AM_I=0x%02x (SC7A20 expect ~0x11)\n",
         addr, who);

  if (gesture)
    {
      return accel_gesture_demo(addr);
    }

  /* Wake: 100 Hz, XYZ enable, normal mode; ±2g, high-res 12-bit */

  sc7_write_reg(addr, SC7_CTRL_REG1, 0x57);
  sc7_write_reg(addr, SC7_CTRL_REG4, 0x08);
  up_mdelay(20);

  for (i = 0; i < n; i++)
    {
      int16_t x;
      int16_t y;
      int16_t z;

      if (sc7_read_regs(addr, SC7_OUT_X_L | SC7_AUTO_INC, buf, 6) != 0)
        {
          syslog(LOG_ERR, "[accel] read failed at sample %d\n", i);
          break;
        }

      x = (int16_t)((buf[1] << 8) | buf[0]) >> 4;   /* 12-bit, 1 mg/LSB @±2g */
      y = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
      z = (int16_t)((buf[5] << 8) | buf[4]) >> 4;

      syslog(LOG_INFO, "[accel] X=%5d  Y=%5d  Z=%5d (mg)\n", x, y, z);
      up_mdelay(100);
    }

  syslog(LOG_INFO, "[accel] done\n");
  return 0;
}
