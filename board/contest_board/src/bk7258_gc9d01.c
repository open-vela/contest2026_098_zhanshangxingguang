/****************************************************************************
 * arch/arm/src/bk7258/bk7258_gc9d01.c
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
 * Bit-bang SPI driver for the GC9D01 160x160 RGB565 LCD on BK7258 DevKit.
 *
 * Pin mapping (from schematic, confirmed against ARMINO gpio_map.h):
 *   SCLK = GPIO_2  (QSPI1_CLK, func6 — unused for bit-bang)
 *   CS   = GPIO_3  (QSPI1_CSN, func6 — unused for bit-bang)
 *   MOSI = GPIO_4  (QSPI1_IO0, func6 — unused for bit-bang)
 *   DC   = GPIO_5  (QSPI1_IO1, func6 — unused for bit-bang)
 *   RST  = GPIO_29 (LCD_RST on schematic, pin 65)
 *   BL   = GPIO_25 (LCD_BL_PWM via Q3)
 *
 * The GC9D01 uses a separate DC pin (not 9-bit SPI).  DC=LOW for command,
 * DC=HIGH for data.  SPI mode 0, MSB first.
 *
 * Staged test via NSH command "lcdtest":
 *   lcdtest          — staged bring-up (A=backlight, B=init, C=red sq)
 *   lcdtest go       — one-step reliable LCD bring-up (production flow)
 *   lcdtest pwr lo hi — power-on GPIO range for binary-search
 *   lcdtest scan     — GPIO pin scan for LCD power enable
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/nuttx.h>

#include "bk7258_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Shared pin */

#define LCD_PIN_BL     25

/* Per-panel pin descriptor */

typedef struct
{
  int sclk;
  int cs;
  int mosi;
  int dc;
  int rst;
} lcd_pins_t;

static const lcd_pins_t g_lcd_left =
{
  .sclk = 2, .cs = 3, .mosi = 4, .dc = 5, .rst = 29
};

static const lcd_pins_t g_lcd_right =
{
  .sclk = 22, .cs = 23, .mosi = 24, .dc = 7, .rst = 6
};

/* Currently active panel — all SPI functions use this */

static const lcd_pins_t *g_active_pins = &g_lcd_left;

/* Display geometry */

#define LCD_WIDTH      160
#define LCD_HEIGHT     160
#define LCD_BPP        16  /* RGB565 */

/* Test square size (Stage C) */

#define SQ_SIZE        40

/* SPI bit-bang delay: a few NOPs for timing margin. */

#define spi_delay() \
  do { \
    __asm__ volatile("nop; nop; nop; nop;"); \
  } while (0)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gpio_set_output
 ****************************************************************************/

static void gpio_set_output(int pin)
{
  uint32_t cfg = getreg32(BK7258_GPIO_CFG(pin));

  cfg &= ~GPIO_CFG_SECOND_FUNC;  /* GPIO mode (bit6 = 0) */
  cfg &= ~GPIO_CFG_OUTPUT_EN;    /* output enable (bit3 active-low: 0 = on) */
  cfg &= ~GPIO_CFG_INPUT_EN;     /* input disable (bit2 active-high: 0 = off) */
  putreg32(cfg, BK7258_GPIO_CFG(pin));
}

/****************************************************************************
 * Name: gpio_write
 ****************************************************************************/

static void gpio_write(int pin, int val)
{
  uint32_t cfg = getreg32(BK7258_GPIO_CFG(pin));

  if (val)
    {
      cfg |= GPIO_CFG_OUTPUT;
    }
  else
    {
      cfg &= ~GPIO_CFG_OUTPUT;
    }

  putreg32(cfg, BK7258_GPIO_CFG(pin));
}

/****************************************************************************
 * Name: lcd_set_pins / lcd_setup_pins
 ****************************************************************************/

static void lcd_set_pins(const lcd_pins_t *pins)
{
  g_active_pins = pins;
}

static void lcd_setup_pins(const lcd_pins_t *pins)
{
  lcd_set_pins(pins);
  gpio_set_output(pins->sclk);
  gpio_set_output(pins->cs);
  gpio_set_output(pins->mosi);
  gpio_set_output(pins->dc);
  gpio_set_output(pins->rst);
  gpio_write(pins->cs, 1);
  gpio_write(pins->dc, 1);
  gpio_write(pins->sclk, 0);
  gpio_write(pins->mosi, 0);
}

/****************************************************************************
 * Name: spi_write_byte
 ****************************************************************************/

static void spi_write_byte(uint8_t byte)
{
  int bit;

  for (bit = 7; bit >= 0; bit--)
    {
      gpio_write(g_active_pins->mosi, (byte >> bit) & 1);
      spi_delay();
      gpio_write(g_active_pins->sclk, 1);
      spi_delay();
      gpio_write(g_active_pins->sclk, 0);
    }
}

/****************************************************************************
 * Name: lcd_send_cmd / lcd_send_data / lcd_send_cmd_data
 ****************************************************************************/

static void lcd_send_cmd(uint8_t cmd)
{
  gpio_write(g_active_pins->dc, 0);
  gpio_write(g_active_pins->cs, 0);
  spi_write_byte(cmd);
  gpio_write(g_active_pins->cs, 1);
}

static void lcd_send_data(const uint8_t *data, int len)
{
  int i;

  gpio_write(g_active_pins->dc, 1);
  gpio_write(g_active_pins->cs, 0);
  for (i = 0; i < len; i++)
    {
      spi_write_byte(data[i]);
    }

  gpio_write(g_active_pins->cs, 1);
}

static void lcd_send_cmd_data(uint8_t cmd, const uint8_t *data, int len)
{
  lcd_send_cmd(cmd);
  if (len > 0)
    {
      lcd_send_data(data, len);
    }
}

/****************************************************************************
 * Name: lcd_init_sequence
 *
 * Description:
 *   Full GC9D01 init sequence from ARMINO lcd_spi_gc9d01.c.
 *   If display_on is true, sends 0x29 (display on) at the end;
 *   otherwise leaves the display off so the caller can fill the
 *   framebuffer first (avoids flash of uninitialised GRAM).
 *
 ****************************************************************************/

static void lcd_init_sequence(bool display_on)
{
  lcd_send_cmd(0xfe);
  lcd_send_cmd(0xef);

  lcd_send_cmd_data(0x80, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x81, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x82, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x83, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x84, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x85, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x86, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x87, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x88, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x89, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x8a, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x8b, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x8c, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x8d, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x8e, (const uint8_t *)"\xff", 1);
  lcd_send_cmd_data(0x8f, (const uint8_t *)"\xff", 1);

  lcd_send_cmd_data(0x3a, (const uint8_t *)"\x05", 1);
  lcd_send_cmd_data(0xec, (const uint8_t *)"\x01", 1);
  lcd_send_cmd_data(0x74,
    (const uint8_t *)"\x02\x0e\x00\x00\x00\x00\x00", 7);
  lcd_send_cmd_data(0x98, (const uint8_t *)"\x3e\x99\x3e", 3);
  lcd_send_cmd_data(0xb5, (const uint8_t *)"\x0d\x0d", 2);
  lcd_send_cmd_data(0x60,
    (const uint8_t *)"\x38\x0f\x79\x67", 4);
  lcd_send_cmd_data(0x61,
    (const uint8_t *)"\x38\x11\x79\x67", 4);
  lcd_send_cmd_data(0x64,
    (const uint8_t *)"\x38\x17\x71\x5f\x79\x67", 6);
  lcd_send_cmd_data(0x65,
    (const uint8_t *)"\x38\x13\x71\x5b\x79\x67", 6);
  lcd_send_cmd_data(0x6a, (const uint8_t *)"\x00\x00", 2);
  lcd_send_cmd_data(0x6c,
    (const uint8_t *)"\x22\x02\x22\x02\x22\x22\x50", 7);
  lcd_send_cmd_data(0x6e,
    (const uint8_t *)"\x03\x03\x01\x01\x00\x00"
                     "\x0f\x0f\x0d\x0d\x0b\x0b"
                     "\x09\x09\x00\x00\x00\x00"
                     "\x0a\x0a\x0c\x0c\x0e\x0e"
                     "\x10\x10\x00\x00\x02\x02"
                     "\x04\x04", 32);
  lcd_send_cmd_data(0xbf, (const uint8_t *)"\x01", 1);
  lcd_send_cmd_data(0xf9, (const uint8_t *)"\x40", 1);
  lcd_send_cmd_data(0x9b, (const uint8_t *)"\x3b", 1);
  lcd_send_cmd_data(0x93, (const uint8_t *)"\x33\x7f\x00", 3);
  lcd_send_cmd_data(0x7e, (const uint8_t *)"\x30", 1);
  lcd_send_cmd_data(0x70,
    (const uint8_t *)"\x0d\x02\x08\x0d\x02\x08", 6);
  lcd_send_cmd_data(0x71,
    (const uint8_t *)"\x0d\x02\x08", 3);
  lcd_send_cmd_data(0x91, (const uint8_t *)"\x0e\x09", 2);
  lcd_send_cmd_data(0xc3, (const uint8_t *)"\x18", 1);
  lcd_send_cmd_data(0xc4, (const uint8_t *)"\x18", 1);
  lcd_send_cmd_data(0xc9, (const uint8_t *)"\x3c", 1);
  lcd_send_cmd_data(0xf0,
    (const uint8_t *)"\x13\x15\x04\x05\x01\x38", 6);
  lcd_send_cmd_data(0xf2,
    (const uint8_t *)"\x13\x15\x04\x05\x01\x34", 6);
  lcd_send_cmd_data(0xf1,
    (const uint8_t *)"\x4b\xb8\x7b\x34\x35\xef", 6);
  lcd_send_cmd_data(0xf3,
    (const uint8_t *)"\x47\xb4\x72\x34\x35\xda", 6);
  lcd_send_cmd_data(0x36, (const uint8_t *)"\x00", 1);

  lcd_send_cmd(0x34);  /* tearing effect off */
  lcd_send_cmd(0x11);  /* sleep out */
  up_mdelay(120);

  if (display_on)
    {
      lcd_send_cmd(0x29);  /* display on */
    }
}

/****************************************************************************
 * Name: lcd_display_on
 *
 * Description:
 *   Send 0x29 (display on) — call after framebuffer is ready.
 *
 ****************************************************************************/

static void lcd_display_on(void)
{
  lcd_send_cmd(0x29);
}

/****************************************************************************
 * Name: lcd_fill_rect
 *
 * Description:
 *   Fill a rectangle (x0,y0)-(x1,y1) with a single RGB565 color.
 *   Much smaller than full-screen fill — avoids millions of bit-bangs.
 *
 ****************************************************************************/

static void lcd_fill_rect(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          uint16_t color)
{
  uint8_t ca[4];
  uint8_t ra[4];
  uint8_t hi = (color >> 8) & 0xff;
  uint8_t lo = color & 0xff;
  int npix = (int)(x1 - x0 + 1) * (int)(y1 - y0 + 1);
  int i;

  /* CASET */

  ca[0] = (x0 >> 8) & 0xff;
  ca[1] = x0 & 0xff;
  ca[2] = (x1 >> 8) & 0xff;
  ca[3] = x1 & 0xff;
  lcd_send_cmd_data(0x2a, ca, 4);

  /* RASET */

  ra[0] = (y0 >> 8) & 0xff;
  ra[1] = y0 & 0xff;
  ra[2] = (y1 >> 8) & 0xff;
  ra[3] = y1 & 0xff;
  lcd_send_cmd_data(0x2b, ra, 4);

  /* RAMWR + pixel data */

  lcd_send_cmd(0x2c);
  gpio_write(g_active_pins->dc, 1);
  gpio_write(g_active_pins->cs, 0);
  for (i = 0; i < npix; i++)
    {
      spi_write_byte(hi);
      spi_write_byte(lo);
    }

  gpio_write(g_active_pins->cs, 1);
}

/****************************************************************************
 * Name: sqrt_int — integer square root (Newton's method)
 ****************************************************************************/

static int16_t sqrt_int(int32_t val)
{
  int32_t guess;
  int32_t next;
  int i;

  if (val <= 0)
    {
      return 0;
    }

  guess = val;

  for (i = 0; i < 10; i++)
    {
      next = (guess + val / guess) / 2;

      if (next >= guess)
        {
          break;
        }

      guess = next;
    }

  return (int16_t)guess;
}

/****************************************************************************
 * Name: lcd_fill_circle
 *
 * Description:
 *   Fill a circle centred at (cx,cy) with radius r using scanline
 *   rendering.  Each scanline is a 1px-high lcd_fill_rect call.
 *
 ****************************************************************************/

static void lcd_fill_circle(int16_t cx, int16_t cy, int16_t r,
                            uint16_t color)
{
  int16_t y;
  int16_t dx;
  int16_t x0;
  int16_t x1;
  int32_t r2 = (int32_t)r * r;

  for (y = cy - r; y <= cy + r; y++)
    {
      int32_t dy  = (int32_t)(y - cy);
      int32_t rem = r2 - dy * dy;

      if (rem < 0)
        {
          continue;
        }

      dx = sqrt_int(rem);
      x0 = cx - dx;
      x1 = cx + dx;

      if (x0 < 0)
        {
          x0 = 0;
        }

      if (x1 >= LCD_WIDTH)
        {
          x1 = LCD_WIDTH - 1;
        }

      if (y < 0 || y >= LCD_HEIGHT)
        {
          continue;
        }

      lcd_fill_rect(x0, y, x1, y, color);
    }
}

/****************************************************************************
 * Name: draw_eye
 *
 * Description:
 *   Draw a cute robot eye on the currently active panel:
 *     black background → cyan iris → white highlight
 *
 ****************************************************************************/

static void draw_eye(void)
{
  syslog(LOG_INFO, "[eye] fill black\n");
  lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0x0000);

  syslog(LOG_INFO, "[eye] cyan iris r=60 at (80,80)\n");
  lcd_fill_circle(80, 80, 60, 0x07ff);

  syslog(LOG_INFO, "[eye] white highlight r=16 at (62,60)\n");
  lcd_fill_circle(62, 60, 16, 0xffff);
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: lcdtest_stages
 *
 * Description:
 *   Staged LCD bring-up: A=backlight, B=init, C=40x40 red square.
 *
 ****************************************************************************/

static int lcdtest_stages(void)
{
  /* --- Stage A: backlight --- */

  syslog(LOG_INFO,
         "[lcdtest] Stage A: backlight ON (P25 high)\n");

  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);
  up_mdelay(1000);

  syslog(LOG_INFO,
         "[lcdtest] Stage A done - screen should be lit white\n");

  /* --- Stage B: reset + init --- */

  syslog(LOG_INFO,
         "[lcdtest] Stage B: RST(P29) reset + GC9D01 init\n");

  lcd_setup_pins(&g_lcd_left);

  gpio_write(g_active_pins->rst, 0);
  up_mdelay(10);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);

  lcd_init_sequence(true);

  syslog(LOG_INFO,
         "[lcdtest] Stage B done - panel initialized\n");

  /* --- Stage C: 40x40 red square --- */

  syslog(LOG_INFO,
         "[lcdtest] Stage C: fill 40x40 red at (60,60)\n");

  lcd_fill_rect(60, 60,
                60 + SQ_SIZE - 1, 60 + SQ_SIZE - 1,
                0xf800);

  syslog(LOG_INFO,
         "[lcdtest] Stage C done - look for red square\n");

  return OK;
}

/****************************************************************************
 * Name: lcdtest_scan
 *
 * Description:
 *   Scan GPIO 0-47 for the LCD power enable pin.
 *   Strategy: cumulative — pull each pin high and leave it high, so that
 *   if multiple pins need to be high simultaneously the condition is met.
 *   P25 (backlight) is held high throughout.  UART console pins (10, 11)
 *   and LCD signal pins (2-5, 29) are skipped to keep the console and
 *   SPI bus alive.
 *
 ****************************************************************************/

static int lcdtest_scan(void)
{
  int pin;

  /* Backlight on first */

  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  syslog(LOG_INFO,
         "[scan] backlight P25 high, scanning GPIO 0-47 ...\n");

  for (pin = 0; pin <= 47; pin++)
    {
      /* Skip UART0 console: RX=10, TX=11 */

      if (pin == 10 || pin == 11)
        {
          syslog(LOG_INFO,
                 "[scan] pin=%d skipped (UART0 console)\n", pin);
          continue;
        }

      /* Skip LCD signal pins (both eyes + backlight) */

      if (pin == g_lcd_left.sclk  || pin == g_lcd_left.cs  ||
          pin == g_lcd_left.mosi  || pin == g_lcd_left.dc  ||
          pin == g_lcd_left.rst   ||
          pin == g_lcd_right.sclk || pin == g_lcd_right.cs ||
          pin == g_lcd_right.mosi || pin == g_lcd_right.dc ||
          pin == g_lcd_right.rst  ||
          pin == LCD_PIN_BL)
        {
          syslog(LOG_INFO,
                 "[scan] pin=%d skipped (LCD signal)\n", pin);
          continue;
        }

      /* Skip SWD debug port: SWCLK=20, SWDIO=21 */

      if (pin == 20 || pin == 21)
        {
          syslog(LOG_INFO,
                 "[scan] pin=%d skipped (SWD debug)\n", pin);
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      up_mdelay(800);

      syslog(LOG_INFO, "[scan] pin=%d high\n", pin);
    }

  syslog(LOG_INFO,
         "[scan] done - check screen for any change\n");

  return OK;
}

/****************************************************************************
 * Name: lcdtest_pwr
 *
 * Description:
 *   Full power-on sequence with configurable GPIO range for binary-search
 *   identification of the LCD power enable pin.
 *
 *   Usage: lcdtest pwr [lo] [hi]
 *     lo..hi  — decimal GPIO range to drive high (default 0..52)
 *
 *   Always skips: 2,3,4,5,25,29 (LCD)  10,11 (UART)  7,8 (motor)
 *                 20,21 (SWD)  38,39 (LEDs)
 *
 ****************************************************************************/

static int lcdtest_pwr(int lo, int hi)
{
  int pin;
  int count = 0;
  int off = 0;
  char pinbuf[256];

  /* Step 1: drive GPIO lo..hi high, skip reserved pins.
   * Collect the actual pin list for the syslog dump.
   */

  syslog(LOG_INFO,
         "[pwr] step1: GPIO %d-%d high (skip reserved)\n",
         lo, hi);

  for (pin = lo; pin <= hi; pin++)
    {
      /* LCD signals (both eyes) */

      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      /* UART0 console */

      if (pin == 10 || pin == 11)
        {
          continue;
        }

      /* Vibration motor (P7 is right-eye DC, only skip P8) */

      if (pin == 8)
        {
          continue;
        }

      /* Indicator LEDs */

      if (pin == 38 || pin == 39)
        {
          continue;
        }

      /* SWD debug port: SWCLK=20, SWDIO=21 */

      if (pin == 20 || pin == 21)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      count++;

      if (off < (int)sizeof(pinbuf) - 8)
        {
          off += snprintf(pinbuf + off, sizeof(pinbuf) - off,
                          " %d", pin);
        }
    }

  pinbuf[off] = '\0';

  /* Print which pins were actually driven high */

  syslog(LOG_INFO,
         "[pwr] driven high (%d pins):%s\n", count, pinbuf);

  /* Step 2: wait for power rails to settle */

  syslog(LOG_INFO, "[pwr] step2: wait 50 ms ...\n");
  up_mdelay(50);

  /* Step 3: backlight on */

  syslog(LOG_INFO, "[pwr] step3: backlight P25 high\n");
  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* Step 4: setup left-eye SPI pins + hardware reset */

  syslog(LOG_INFO,
         "[pwr] step4: left SPI setup + RST low 15 ms then high\n");
  lcd_setup_pins(&g_lcd_left);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);

  /* Step 5: GC9D01 init sequence */

  syslog(LOG_INFO, "[pwr] step5: GC9D01 init sequence\n");
  lcd_init_sequence(true);

  /* Step 6: clear screen to black */

  syslog(LOG_INFO, "[pwr] step6: fill screen black\n");
  lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0x0000);

  /* Step 7: draw 40x40 red square at (60,60) */

  syslog(LOG_INFO,
         "[pwr] step7: fill 40x40 red at (60,60)\n");
  lcd_fill_rect(60, 60,
                60 + SQ_SIZE - 1, 60 + SQ_SIZE - 1,
                0xf800);

  syslog(LOG_INFO,
         "[pwr] done (%d pins driven) - "
         "screen should show red on black\n",
         count);
  return OK;
}

/****************************************************************************
 * Name: lcdtest_go
 *
 * Description:
 *   One-step dual-eye LCD bring-up: left (red) + right (blue).
 *
 ****************************************************************************/

static int lcdtest_go(void)
{
  int pin;
  int count = 0;

  /* Step 1: drive GPIO 0-52 high, skip reserved pins */

  syslog(LOG_INFO,
         "[go] step1: GPIO 0-52 high (skip reserved)\n");

  for (pin = 0; pin <= 52; pin++)
    {
      /* LCD signals (both eyes + backlight) */

      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      /* UART0 console */

      if (pin == 10 || pin == 11)
        {
          continue;
        }

      /* Vibration motor (P7 is right-eye DC, only skip P8) */

      if (pin == 8)
        {
          continue;
        }

      /* Indicator LEDs */

      if (pin == 38 || pin == 39)
        {
          continue;
        }

      /* SWD debug port: SWCLK=20, SWDIO=21 */

      if (pin == 20 || pin == 21)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      count++;
    }

  syslog(LOG_INFO, "[go] step1 done: %d pins driven high\n", count);

  /* Step 2: wait for power rails to settle */

  syslog(LOG_INFO, "[go] step2: wait 500 ms for power stable\n");
  up_mdelay(500);

  /* Step 3: backlight on (shared) */

  syslog(LOG_INFO, "[go] step3: backlight P25 high\n");
  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* ---- Left eye ---- */

  syslog(LOG_INFO, "[go] step4 [left]: SPI pins setup\n");
  lcd_setup_pins(&g_lcd_left);

  syslog(LOG_INFO, "[go] step5 [left]: RST + init (display off)\n");
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);

  syslog(LOG_INFO, "[go] step6 [left]: draw eye\n");
  draw_eye();

  syslog(LOG_INFO, "[go] step7 [left]: display on\n");
  lcd_display_on();

  /* ---- Right eye ---- */

  syslog(LOG_INFO, "[go] step9 [right]: SPI pins setup\n");
  lcd_setup_pins(&g_lcd_right);

  syslog(LOG_INFO, "[go] step10 [right]: RST + init (display off)\n");
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);

  syslog(LOG_INFO, "[go] step11 [right]: draw eye\n");
  draw_eye();

  syslog(LOG_INFO, "[go] step12 [right]: display on\n");
  lcd_display_on();

  syslog(LOG_INFO,
         "[go] done - both eyes show robot eye\n");
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_lcdtest_main
 *
 * Description:
 *   NSH command "lcdtest":
 *     lcdtest          — staged bring-up (A/B/C, diagnostics)
 *     lcdtest go       — one-step reliable LCD bring-up
 *     lcdtest pwr lo hi — power-on GPIO range for binary-search
 *     lcdtest scan     — GPIO pin scan for LCD power enable
 *
 ****************************************************************************/

int bk7258_lcdtest_main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "go") == 0)
    {
      return lcdtest_go();
    }

  if (argc > 1 && strcmp(argv[1], "scan") == 0)
    {
      return lcdtest_scan();
    }

  if (argc > 1 && strcmp(argv[1], "pwr") == 0)
    {
      int lo = 0;
      int hi = 52;

      if (argc > 2)
        {
          lo = atoi(argv[2]);
        }

      if (argc > 3)
        {
          hi = atoi(argv[3]);
        }

      if (lo < 0)
        {
          lo = 0;
        }

      if (hi > 52)
        {
          hi = 52;
        }

      if (lo > hi)
        {
          syslog(LOG_ERR,
                 "[pwr] error: lo(%d) > hi(%d)\n", lo, hi);
          return -EINVAL;
        }

      return lcdtest_pwr(lo, hi);
    }

  return lcdtest_stages();
}
