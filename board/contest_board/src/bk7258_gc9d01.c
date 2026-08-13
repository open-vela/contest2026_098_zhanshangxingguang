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
#include "bk7258_audio.h"

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

/****************************************************************************
 * Cached GPIO pin state for fast bit-bang
 *
 * Instead of read-modify-write on every gpio_write() call, we cache the
 * CFG base value (with OUTPUT=0) and register address at setup time.
 * Setting a pin HIGH is then a single putreg32(base | OUTPUT_BIT).
 ****************************************************************************/

typedef struct
{
  uintptr_t addr;   /* BK7258_GPIO_CFG(pin) address */
  uint32_t base_lo; /* CFG with OUTPUT bit cleared */
  uint32_t base_hi; /* CFG with OUTPUT bit set */
} gpio_cache_t;

static gpio_cache_t g_cache_sclk;
static gpio_cache_t g_cache_mosi;
static gpio_cache_t g_cache_cs;
static gpio_cache_t g_cache_dc;

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
  cfg &= ~GPIO_CFG_INPUT_EN;     /* input off (bit2: 0=off) */
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
 * Name: gpio_set_output_cached
 *
 * Description:
 *   Configure pin as GPIO output and cache the CFG register values
 *   for fast putreg32-only writes (no getreg32 read needed later).
 *
 ****************************************************************************/

static void gpio_set_output_cached(int pin, gpio_cache_t *c)
{
  uint32_t cfg;

  c->addr = BK7258_GPIO_CFG(pin);
  cfg = getreg32(c->addr);
  cfg &= ~GPIO_CFG_SECOND_FUNC;
  cfg &= ~GPIO_CFG_OUTPUT_EN;
  cfg &= ~GPIO_CFG_INPUT_EN;
  cfg &= ~GPIO_CFG_OUTPUT;   /* output = 0 */
  putreg32(cfg, c->addr);
  c->base_lo = cfg;            /* OUTPUT=0 */
  c->base_hi = cfg | GPIO_CFG_OUTPUT; /* OUTPUT=1 */
}

/****************************************************************************
 * Name: gpio_write_fast
 *
 * Description:
 *   Set pin via cached values — no read-modify-write, just a single
 *   putreg32.  ~2x faster than gpio_write().
 *
 ****************************************************************************/

static inline void gpio_write_fast(const gpio_cache_t *c, int val)
{
  putreg32(val ? c->base_hi : c->base_lo, c->addr);
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
  gpio_set_output(pins->rst);

  /* Cache SCLK/MOSI/CS/DC for fast bit-bang */

  gpio_set_output_cached(pins->sclk, &g_cache_sclk);
  gpio_set_output_cached(pins->mosi, &g_cache_mosi);
  gpio_set_output_cached(pins->cs,   &g_cache_cs);
  gpio_set_output_cached(pins->dc,   &g_cache_dc);

  /* Initial idle state: CS=HIGH, DC=HIGH, SCLK=LOW, MOSI=LOW */

  gpio_write_fast(&g_cache_cs,   1);
  gpio_write_fast(&g_cache_dc,   1);
  gpio_write_fast(&g_cache_sclk, 0);
  gpio_write_fast(&g_cache_mosi, 0);
}

/****************************************************************************
 * Name: spi_write_byte
 *
 * Description:
 *   Bit-bang one byte, MSB-first, SPI mode 0.
 *   Uses cached GPIO values (no getreg32 read) and no NOP delay.
 *   GC9D01 can handle fast SPI clock; add back one spi_delay() if
 *   display shows corruption.
 *
 ****************************************************************************/

static void spi_write_byte(uint8_t byte)
{
  int bit;

  for (bit = 7; bit >= 0; bit--)
    {
      gpio_write_fast(&g_cache_mosi, (byte >> bit) & 1);
      gpio_write_fast(&g_cache_sclk, 1);
      gpio_write_fast(&g_cache_sclk, 0);
    }
}

/****************************************************************************
 * Name: lcd_send_cmd / lcd_send_data / lcd_send_cmd_data
 ****************************************************************************/

static void lcd_send_cmd(uint8_t cmd)
{
  gpio_write_fast(&g_cache_dc, 0);
  gpio_write_fast(&g_cache_cs, 0);
  spi_write_byte(cmd);
  gpio_write_fast(&g_cache_cs, 1);
}

static void lcd_send_data(const uint8_t *data, int len)
{
  int i;

  gpio_write_fast(&g_cache_dc, 1);
  gpio_write_fast(&g_cache_cs, 0);
  for (i = 0; i < len; i++)
    {
      spi_write_byte(data[i]);
    }

  gpio_write_fast(&g_cache_cs, 1);
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
  gpio_write_fast(&g_cache_dc, 1);
  gpio_write_fast(&g_cache_cs, 0);
  for (i = 0; i < npix; i++)
    {
      spi_write_byte(hi);
      spi_write_byte(lo);
    }

  gpio_write_fast(&g_cache_cs, 1);
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
 * Name: lcd_fill_round_rect
 *
 * Description:
 *   Fill a rounded rectangle centred at (cx,cy), width w, height h,
 *   corner radius r.  Built from one inner rect + four corner circles.
 *
 ****************************************************************************/

static void lcd_fill_round_rect(int16_t cx, int16_t cy,
                                int16_t w, int16_t h,
                                int16_t r, uint16_t color)
{
  int16_t x0 = cx - w / 2;
  int16_t y0 = cy - h / 2;
  int16_t x1 = cx + w / 2;
  int16_t y1 = cy + h / 2;

  /* Horizontal centre strip (full width, reduced height) */

  lcd_fill_rect(x0, y0 + r, x1, y1 - r, color);

  /* Vertical centre strip (reduced width, full height) */

  lcd_fill_rect(x0 + r, y0, x1 - r, y1, color);

  /* Four corner circles */

  lcd_fill_circle(x0 + r, y0 + r, r, color);
  lcd_fill_circle(x1 - r, y0 + r, r, color);
  lcd_fill_circle(x0 + r, y1 - r, r, color);
  lcd_fill_circle(x1 - r, y1 - r, r, color);
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
 * Name: eye_draw_full
 *
 * Description:
 *   Full draw of the animated eye model on the current panel.
 *   Call once per eye before starting animation.
 *
 *   Background: black
 *   Iris:       cyan 0x07FF, centre (80,80), r=58
 *   Pupil:      black 0x0000, r=22, centre (80+gaze_dx, 80)
 *   Highlight:  white 0xFFFF, r=8, centre (80+gaze_dx-8, 80-10)
 *
 ****************************************************************************/

#define EYE_CX        80
#define EYE_CY        80
#define EYE_IRIS_R    58
#define EYE_PUPIL_R   22
#define EYE_HIGHLIGHT_R 8

static void eye_draw_full(int gaze_dx)
{
  int pcx = EYE_CX + gaze_dx;

  syslog(LOG_INFO,
         "[eye] full draw gaze_dx=%d\n", gaze_dx);

  /* Black background */

  lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0x0000);

  /* Cyan iris */

  lcd_fill_circle(EYE_CX, EYE_CY, EYE_IRIS_R, 0x07ff);

  /* Black pupil */

  lcd_fill_circle(pcx, EYE_CY, EYE_PUPIL_R, 0x0000);

  /* White highlight (upper-left of pupil) */

  lcd_fill_circle(pcx - 8, EYE_CY - 10,
                  EYE_HIGHLIGHT_R, 0xffff);
}

/****************************************************************************
 * Name: eye_move_pupil
 *
 * Description:
 *   Local update: repaint old pupil position with iris colour, then
 *   draw pupil + highlight at the new position.  No full-screen fill.
 *
 ****************************************************************************/

static void eye_move_pupil(int old_dx, int new_dx)
{
  int old_cx = EYE_CX + old_dx;
  int new_cx = EYE_CX + new_dx;

  syslog(LOG_INFO,
         "[eye] move pupil %d -> %d\n", old_dx, new_dx);

  /* Erase old pupil (r23 to catch anti-aliased edge) */

  lcd_fill_circle(old_cx, EYE_CY, 23, 0x07ff);

  /* Erase old highlight (extends beyond pupil r22) */

  lcd_fill_circle(old_cx - 8, EYE_CY - 10, 10, 0x07ff);

  /* Black pupil at new position */

  lcd_fill_circle(new_cx, EYE_CY, EYE_PUPIL_R, 0x0000);

  /* White highlight */

  lcd_fill_circle(new_cx - 8, EYE_CY - 10,
                  EYE_HIGHLIGHT_R, 0xffff);
}

/****************************************************************************
 * Name: eye_blink
 *
 * Description:
 *   Quick blink: fill iris area black, pause, then redraw fully.
 *
 ****************************************************************************/

static void eye_blink(int gaze_dx)
{
  int pcx = EYE_CX + gaze_dx;

  syslog(LOG_INFO, "[eye] blink (gaze_dx=%d)\n", gaze_dx);

  /* Close: black over iris area */

  lcd_fill_circle(EYE_CX, EYE_CY, EYE_IRIS_R, 0x0000);
  up_mdelay(120);

  /* Open: iris -> pupil -> highlight (layered, no white dots) */

  lcd_fill_circle(EYE_CX, EYE_CY, EYE_IRIS_R, 0x07ff);
  lcd_fill_circle(pcx, EYE_CY, EYE_PUPIL_R, 0x0000);
  lcd_fill_circle(pcx - 8, EYE_CY - 10,
                  EYE_HIGHLIGHT_R, 0xffff);
}

/****************************************************************************
 * Horizontal human-like expression eyes
 *
 * Bounding box: x in [10,150], y in [22,138].
 * Each full-draw expression clears the box first (no ghosting).
 * eye_look() uses local pupil update for speed.
 ****************************************************************************/

#define EMO_X0  10
#define EMO_X1  150
#define EMO_Y0  22
#define EMO_Y1  138

#define EMO_EYE_W   124
#define EMO_EYE_H   86
#define EMO_EYE_R   42
#define EMO_PUPIL_R 26
#define EMO_HL_R    10

/* Current pupil gaze offset (tracked for local eye_look updates) */

static int g_emo_gaze = 0;

static void emo_clear(void)
{
  lcd_fill_rect(EMO_X0, EMO_Y0, EMO_X1, EMO_Y1, 0x0000);
}

/* Draw eye shape + pupil + highlight at gaze offset dx.
 * Used by both neutral (after emo_clear) and look (overdraw).
 */

static void draw_eye_shape(int dx)
{
  lcd_fill_round_rect(80, 80, EMO_EYE_W, EMO_EYE_H,
                      EMO_EYE_R, 0x07ff);
  lcd_fill_circle(80 + dx, 80, EMO_PUPIL_R, 0x0000);
  lcd_fill_circle(80 + dx - 10, 68, EMO_HL_R, 0xffff);
}

static void eye_neutral(void)
{
  emo_clear();
  draw_eye_shape(0);
  g_emo_gaze = 0;
}

static void eye_look(int dx)
{
  /* Redraw eye shape over current image — covers old pupil/highlight
   * with iris colour, preserves rounded contour perfectly.
   */

  draw_eye_shape(dx);
  g_emo_gaze = dx;
}

static void eye_happy(void)
{
  emo_clear();

  /* Cyan filled circle */

  lcd_fill_circle(80, 80, 54, 0x07ff);

  /* Black over upper half (y < 80) to leave a smile */

  lcd_fill_rect(EMO_X0, EMO_Y0, EMO_X1, 79, 0x0000);

  g_emo_gaze = 0;
}

static void eye_emo_blink(void)
{
  emo_clear();
  lcd_fill_round_rect(80, 80, EMO_EYE_W, 14, 7, 0x07ff);
  g_emo_gaze = 0;
}

/****************************************************************************
 * Official-website-style eye: white bg + blue iris + black eyelids
 *
 * Color palette (RGB565):
 *   0xFFFF  white  (background)
 *   0x02DF  sky-blue (iris) — try 0x001F for deeper blue
 *   0x0000  black  (pupil, eyelids)
 *   0xFFFF  white  (highlight)
 ****************************************************************************/

#define OEYE_BG        0xFFFF  /* white */
#define OEYE_IRIS      0x02DF  /* sky-blue */
#define OEYE_PUPIL     0x0000  /* black */
#define OEYE_HL        0xFFFF  /* white highlight */
#define OEYE_LID       0x0000  /* black eyelid */

#define OEYE_IRIS_R    56     /* blue iris with visible white border */
#define OEYE_PUPIL_R   28     /* black pupil */
#define OEYE_HL1_R     10     /* primary highlight upper-left */
#define OEYE_HL1_X     68     /* primary highlight centre */
#define OEYE_HL1_Y     68
#define OEYE_HL2_R     4      /* secondary highlight lower-right */
#define OEYE_HL2_X     92     /* secondary highlight centre */
#define OEYE_HL2_Y     92

/* Iris bounding box — the only region we redraw per frame.
 * iris at (80,80) r56 → x,y ∈ [24,136].
 */

#define OEYE_BOX_L     24
#define OEYE_BOX_R     136
#define OEYE_BOX_T     24
#define OEYE_BOX_B     136

/* Eyelid Y boundaries for each expression */

#define OEYE_NEUTRAL_TOP  16   /* neutral: upper lid covers y 0..16 */
#define OEYE_NEUTRAL_BOT  143  /* neutral: lower lid covers y 143..159 */
#define OEYE_HALF_TOP     50   /* half: upper lid covers y 0..50 */
#define OEYE_HALF_BOT     110  /* half: lower lid covers y 110..159 */
#define OEYE_BLINK_TOP    70   /* blink: black arc top edge */
#define OEYE_BLINK_BOT    90   /* blink: black arc bottom edge */
#define OEYE_HAPPY_BOT    115  /* happy: lower lid pushed up */

/****************************************************************************
 * Name: draw_oeye_bg
 *
 * Description:
 *   Full-screen white fill.  Call ONCE per panel before the
 *   expression loop.  Never called again during the loop.
 *
 ****************************************************************************/

static void draw_oeye_bg(void)
{
  lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, OEYE_BG);
}

/****************************************************************************
 * Name: draw_oeye_content
 *
 * Description:
 *   Redraw only the iris bounding box: clear box to white, then
 *   draw blue iris + black pupil + two highlights.  This is the
 *   fast per-frame content update — no full-screen fill.
 *
 ****************************************************************************/

static void draw_oeye_content(void)
{
  /* Clear iris box to white (erases previous pupil/highlight/lid) */

  lcd_fill_rect(OEYE_BOX_L, OEYE_BOX_T, OEYE_BOX_R, OEYE_BOX_B,
                OEYE_BG);

  /* Blue iris */

  lcd_fill_circle(80, 80, OEYE_IRIS_R, OEYE_IRIS);

  /* Black pupil */

  lcd_fill_circle(80, 80, OEYE_PUPIL_R, OEYE_PUPIL);

  /* Two white highlights */

  lcd_fill_circle(OEYE_HL1_X, OEYE_HL1_Y, OEYE_HL1_R, OEYE_HL);
  lcd_fill_circle(OEYE_HL2_X, OEYE_HL2_Y, OEYE_HL2_R, OEYE_HL);
}

/****************************************************************************
 * Name: eye_o_neutral
 *
 * Description:
 *   Neutral / fully open: redraw iris content + thin black eyelid
 *   strips top and bottom.  The strips are outside the iris box
 *   (y 0..16 and 143..159) so they overwrite the white bg there.
 *
 ****************************************************************************/

static void eye_o_neutral(void)
{
  draw_oeye_content();

  /* Upper eyelid: thin black strip at top */

  lcd_fill_rect(0, 0, LCD_WIDTH - 1, OEYE_NEUTRAL_TOP, OEYE_LID);

  /* Lower eyelid: thin black strip at bottom */

  lcd_fill_rect(0, OEYE_NEUTRAL_BOT, LCD_WIDTH - 1, LCD_HEIGHT - 1,
                OEYE_LID);
}

/****************************************************************************
 * Name: eye_o_half
 *
 * Description:
 *   Half-closed: redraw iris content + wider black eyelid strips.
 *   Upper lid extends into the iris box (y 0..50), lower lid
 *   extends into the iris box (y 110..159).
 *
 ****************************************************************************/

static void eye_o_half(void)
{
  draw_oeye_content();

  /* Upper eyelid: covers top portion of iris (extends into box) */

  lcd_fill_rect(0, 0, LCD_WIDTH - 1, OEYE_HALF_TOP, OEYE_LID);

  /* Lower eyelid: covers bottom portion (extends into box) */

  lcd_fill_rect(0, OEYE_HALF_BOT, LCD_WIDTH - 1, LCD_HEIGHT - 1,
                OEYE_LID);
}

/****************************************************************************
 * Name: eye_o_blink
 *
 * Description:
 *   Fully closed: clear iris box to white + draw thick black band
 *   y70..90.  Also restore any previous eyelid strips outside the
 *   box (top/bottom) back to white so only the black arc remains.
 *
 ****************************************************************************/

static void eye_o_blink(void)
{
  /* Clear iris box to white */

  lcd_fill_rect(OEYE_BOX_L, OEYE_BOX_T, OEYE_BOX_R, OEYE_BOX_B,
                OEYE_BG);

  /* Restore top/bottom strips outside box to white
   * (removes leftover black eyelid from previous frame)
   */

  lcd_fill_rect(0, 0, LCD_WIDTH - 1, OEYE_BOX_T - 1, OEYE_BG);
  lcd_fill_rect(0, OEYE_BOX_B + 1, LCD_WIDTH - 1, LCD_HEIGHT - 1,
                OEYE_BG);

  /* Black arc: thick band across the middle of the iris box */

  lcd_fill_rect(OEYE_BOX_L, OEYE_BLINK_TOP, OEYE_BOX_R, OEYE_BLINK_BOT,
                OEYE_LID);
}

/****************************************************************************
 * Name: eye_o_happy
 *
 * Description:
 *   Happy: redraw iris content + thin top lid + lower lid pushed
 *   up to y=115 (smile arc).  Lower lid extends into the iris box.
 *
 ****************************************************************************/

static void eye_o_happy(void)
{
  draw_oeye_content();

  /* Upper eyelid: thin strip (same as neutral) */

  lcd_fill_rect(0, 0, LCD_WIDTH - 1, OEYE_NEUTRAL_TOP, OEYE_LID);

  /* Lower eyelid: push up to carve a smile arc */

  lcd_fill_rect(0, OEYE_HAPPY_BOT, LCD_WIDTH - 1, LCD_HEIGHT - 1,
                OEYE_LID);
}

/****************************************************************************
 * Smooth blink animation — incremental eyelid sweep
 *
 * Instead of jumping neutral → half → blink in 3 big frames, the eyelids
 * sweep in ~10-12 small steps.  Each step only draws a narrow black band
 * (closing) or redraws the revealed eye content (opening), so the per-step
 * pixel count is tiny — even bit-bang can do it smoothly.
 *
 *   Closing: top lid descends from OEYE_NEUTRAL_TOP → OEYE_BLINK_TOP,
 *            bottom lid ascends from OEYE_NEUTRAL_BOT → OEYE_BLINK_BOT.
 *   Opening: reverse.
 ****************************************************************************/

#define OEYE_BLINK_STEPS  10  /* number of animation sub-frames */

/****************************************************************************
 * Name: oeye_pixel_color
 *
 * Description:
 *   Return the RGB565 color for pixel (px, py) in the eye content.
 *   Checks highlight, pupil, iris, then falls back to background.
 *
 ****************************************************************************/

static inline uint16_t oeye_pixel_color(int16_t px, int16_t py)
{
  int16_t ddx;
  int16_t ddy;
  int32_t d2;

  /* Highlight 1 */

  ddx = px - OEYE_HL1_X;
  ddy = py - OEYE_HL1_Y;

  if ((int32_t)ddx * ddx + (int32_t)ddy * ddy <=
      (int32_t)OEYE_HL1_R * OEYE_HL1_R)
    {
      return OEYE_HL;
    }

  /* Highlight 2 */

  ddx = px - OEYE_HL2_X;
  ddy = py - OEYE_HL2_Y;

  if ((int32_t)ddx * ddx + (int32_t)ddy * ddy <=
      (int32_t)OEYE_HL2_R * OEYE_HL2_R)
    {
      return OEYE_HL;
    }

  /* Pupil */

  ddx = px - 80;
  ddy = py - 80;
  d2 = (int32_t)ddx * ddx + (int32_t)ddy * ddy;

  if (d2 <= (int32_t)OEYE_PUPIL_R * OEYE_PUPIL_R)
    {
      return OEYE_PUPIL;
    }

  /* Iris */

  if (d2 <= (int32_t)OEYE_IRIS_R * OEYE_IRIS_R)
    {
      return OEYE_IRIS;
    }

  return OEYE_BG;
}

/****************************************************************************
 * Name: oeye_draw_row_band
 *
 * Description:
 *   Draw eye content (iris/pupil/highlights/bg) for rows y0..y1,
 *   clipped to the iris bounding box X range.  Uses CASET/RASET once
 *   and streams pixels row by row — much faster than lcd_fill_circle
 *   per row.
 *
 ****************************************************************************/

static void oeye_draw_row_band(int16_t y0, int16_t y1)
{
  uint8_t ca[4];
  uint8_t ra[4];
  uint8_t cmd[2];
  int16_t x;
  int16_t y;
  int16_t x0 = OEYE_BOX_L;
  int16_t x1 = OEYE_BOX_R;

  /* CASET + RASET for the band */

  ca[0] = (x0 >> 8) & 0xff;
  ca[1] = x0 & 0xff;
  ca[2] = (x1 >> 8) & 0xff;
  ca[3] = x1 & 0xff;
  lcd_send_cmd_data(0x2a, ca, 4);

  ra[0] = (y0 >> 8) & 0xff;
  ra[1] = y0 & 0xff;
  ra[2] = (y1 >> 8) & 0xff;
  ra[3] = y1 & 0xff;
  lcd_send_cmd_data(0x2b, ra, 4);

  /* RAMWR — stream pixels row by row */

  lcd_send_cmd(0x2c);
  gpio_write_fast(&g_cache_dc, 1);
  gpio_write_fast(&g_cache_cs, 0);

  for (y = y0; y <= y1; y++)
    {
      for (x = x0; x <= x1; x++)
        {
          uint16_t c = oeye_pixel_color(x, y);

          cmd[0] = (c >> 8) & 0xff;
          cmd[1] = c & 0xff;
          spi_write_byte(cmd[0]);
          spi_write_byte(cmd[1]);
        }
    }

  gpio_write_fast(&g_cache_cs, 1);
}

/****************************************************************************
 * Name: eye_o_blink_anim
 *
 * Description:
 *   Smooth eyelid animation: close → brief hold → open.
 *   Each step draws only the narrow newly-occluded/revealed strip,
 *   so the per-step pixel count is small (160×~6px for 10 steps).
 *
 ****************************************************************************/

static void eye_o_blink_anim(void)
{
  int step;
  int16_t top_prev;
  int16_t top_cur;
  int16_t bot_prev;
  int16_t bot_cur;

  /* --- Phase 1: close (top lid descends, bottom lid ascends) --- */

  top_prev = OEYE_NEUTRAL_TOP;
  bot_prev = OEYE_NEUTRAL_BOT;

  for (step = 1; step <= OEYE_BLINK_STEPS; step++)
    {
      /* Interpolate lid positions */

      top_cur = OEYE_NEUTRAL_TOP +
                (int16_t)((int32_t)(OEYE_BLINK_TOP - OEYE_NEUTRAL_TOP) *
                          step / OEYE_BLINK_STEPS);
      bot_cur = OEYE_NEUTRAL_BOT +
                (int16_t)((int32_t)(OEYE_BLINK_BOT - OEYE_NEUTRAL_BOT) *
                          step / OEYE_BLINK_STEPS);

      /* Draw newly covered rows as black (only the delta strip) */

      if (top_cur > top_prev)
        {
          lcd_fill_rect(OEYE_BOX_L, top_prev + 1,
                        OEYE_BOX_R, top_cur, OEYE_LID);
        }

      if (bot_cur < bot_prev)
        {
          lcd_fill_rect(OEYE_BOX_L, bot_cur,
                        OEYE_BOX_R, bot_prev - 1, OEYE_LID);
        }

      top_prev = top_cur;
      bot_prev = bot_cur;
    }

  /* --- Phase 2: brief hold at fully closed --- */

  up_mdelay(60);

  /* --- Phase 3: open (top lid ascends, bottom lid descends) --- */

  for (step = OEYE_BLINK_STEPS - 1; step >= 0; step--)
    {
      top_cur = OEYE_NEUTRAL_TOP +
                (int16_t)((int32_t)(OEYE_BLINK_TOP - OEYE_NEUTRAL_TOP) *
                          step / OEYE_BLINK_STEPS);
      bot_cur = OEYE_NEUTRAL_BOT +
                (int16_t)((int32_t)(OEYE_BLINK_BOT - OEYE_NEUTRAL_BOT) *
                          step / OEYE_BLINK_STEPS);

      /* Redraw revealed rows with eye content (only the delta strip) */

      if (top_cur < top_prev)
        {
          oeye_draw_row_band(top_cur + 1, top_prev);
        }

      if (bot_cur > bot_prev)
        {
          oeye_draw_row_band(bot_prev, bot_cur - 1);
        }

      top_prev = top_cur;
      bot_prev = bot_cur;
    }

  /* Restore the thin neutral eyelid strips (outside iris box) */

  lcd_fill_rect(0, 0, LCD_WIDTH - 1, OEYE_NEUTRAL_TOP, OEYE_LID);
  lcd_fill_rect(0, OEYE_NEUTRAL_BOT, LCD_WIDTH - 1, LCD_HEIGHT - 1,
                OEYE_LID);
}

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
 * Name: lcdtest_anim
 *
 * Description:
 *   Eye animation demo: pupils track left/centre/right, with blinking.
 *   Reuses go's power-on + init sequence, then runs 3 animation rounds.
 *
 ****************************************************************************/

static int lcdtest_anim(void)
{
  int pin;
  int count = 0;
  int round;
  int gaze = 0;

  /* --- Power-on (same as go step1-3) --- */

  syslog(LOG_INFO,
         "[anim] step1: GPIO 0-52 high (skip reserved)\n");

  for (pin = 0; pin <= 52; pin++)
    {
      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      if (pin == 10 || pin == 11 || pin == 8 ||
          pin == 20 || pin == 21 ||
          pin == 38 || pin == 39)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      count++;
    }

  syslog(LOG_INFO, "[anim] step1 done: %d pins\n", count);

  syslog(LOG_INFO, "[anim] step2: wait 500 ms\n");
  up_mdelay(500);

  syslog(LOG_INFO, "[anim] step3: backlight P25 high\n");
  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* --- Init left eye --- */

  syslog(LOG_INFO, "[anim] init left eye\n");
  lcd_setup_pins(&g_lcd_left);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  eye_draw_full(0);
  lcd_display_on();

  /* --- Init right eye --- */

  syslog(LOG_INFO, "[anim] init right eye\n");
  lcd_setup_pins(&g_lcd_right);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  eye_draw_full(0);
  lcd_display_on();

  /* --- Animation loop: 3 rounds --- */

  for (round = 0; round < 3; round++)
    {
      syslog(LOG_INFO, "[anim] round %d/3\n", round + 1);

      /* Look left: 0 -> -24 */

      syslog(LOG_INFO, "[anim] look left\n");
      lcd_set_pins(&g_lcd_left);
      eye_move_pupil(gaze, -24);
      lcd_set_pins(&g_lcd_right);
      eye_move_pupil(gaze, -24);
      gaze = -24;
      up_mdelay(500);

      /* Look centre: -24 -> 0 */

      syslog(LOG_INFO, "[anim] look centre\n");
      lcd_set_pins(&g_lcd_left);
      eye_move_pupil(gaze, 0);
      lcd_set_pins(&g_lcd_right);
      eye_move_pupil(gaze, 0);
      gaze = 0;
      up_mdelay(500);

      /* Look right: 0 -> +24 */

      syslog(LOG_INFO, "[anim] look right\n");
      lcd_set_pins(&g_lcd_left);
      eye_move_pupil(gaze, 24);
      lcd_set_pins(&g_lcd_right);
      eye_move_pupil(gaze, 24);
      gaze = 24;
      up_mdelay(500);

      /* Look centre: +24 -> 0 */

      syslog(LOG_INFO, "[anim] look centre\n");
      lcd_set_pins(&g_lcd_left);
      eye_move_pupil(gaze, 0);
      lcd_set_pins(&g_lcd_right);
      eye_move_pupil(gaze, 0);
      gaze = 0;
      up_mdelay(500);

      /* TODO: eye_blink disabled — bit-bang redraw too slow,
       * enable after switching to QSPI with smooth refresh.
       */
    }

  syslog(LOG_INFO, "[anim] done\n");
  return OK;
}

/****************************************************************************
 * Name: lcdtest_emo
 *
 * Description:
 *   Cozmo/Vector-style expression eye animation.
 *   Two rounds of: neutral -> look L/C/R/C -> happy -> blink -> neutral.
 *
 ****************************************************************************/

static int lcdtest_emo(void)
{
  int pin;
  int count = 0;
  int round;

  /* --- Power-on (same as go) --- */

  syslog(LOG_INFO,
         "[emo] step1: GPIO 0-52 high (skip reserved)\n");

  for (pin = 0; pin <= 52; pin++)
    {
      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      if (pin == 10 || pin == 11 || pin == 8 ||
          pin == 20 || pin == 21 ||
          pin == 38 || pin == 39)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      count++;
    }

  syslog(LOG_INFO, "[emo] step1 done: %d pins\n", count);

  syslog(LOG_INFO, "[emo] step2: wait 500 ms\n");
  up_mdelay(500);

  syslog(LOG_INFO, "[emo] step3: backlight P25 high\n");
  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* --- Init left eye --- */

  syslog(LOG_INFO, "[emo] init left eye\n");
  lcd_setup_pins(&g_lcd_left);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0x0000);
  eye_neutral();
  lcd_display_on();

  /* --- Init right eye --- */

  syslog(LOG_INFO, "[emo] init right eye\n");
  lcd_setup_pins(&g_lcd_right);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0x0000);
  eye_neutral();
  lcd_display_on();

  /* --- Expression loop: 2 rounds --- */

  for (round = 0; round < 2; round++)
    {
      syslog(LOG_INFO, "[emo] round %d/2\n", round + 1);

      /* neutral */

      syslog(LOG_INFO, "[emo] neutral\n");
      lcd_set_pins(&g_lcd_left);
      eye_neutral();
      lcd_set_pins(&g_lcd_right);
      eye_neutral();
      up_mdelay(800);

      /* look left */

      syslog(LOG_INFO, "[emo] look left\n");
      lcd_set_pins(&g_lcd_left);
      eye_look(-22);
      lcd_set_pins(&g_lcd_right);
      eye_look(-22);
      up_mdelay(600);

      /* look centre */

      syslog(LOG_INFO, "[emo] look centre\n");
      lcd_set_pins(&g_lcd_left);
      eye_look(0);
      lcd_set_pins(&g_lcd_right);
      eye_look(0);
      up_mdelay(600);

      /* look right */

      syslog(LOG_INFO, "[emo] look right\n");
      lcd_set_pins(&g_lcd_left);
      eye_look(22);
      lcd_set_pins(&g_lcd_right);
      eye_look(22);
      up_mdelay(600);

      /* happy */

      syslog(LOG_INFO, "[emo] happy\n");
      lcd_set_pins(&g_lcd_left);
      eye_happy();
      lcd_set_pins(&g_lcd_right);
      eye_happy();
      up_mdelay(1000);

      /* TODO: eye_emo_blink disabled — bit-bang scanline blink
       * looks blocky; enable after switching to QSPI.
       */

      /* neutral */

      syslog(LOG_INFO, "[emo] neutral\n");
      lcd_set_pins(&g_lcd_left);
      eye_neutral();
      lcd_set_pins(&g_lcd_right);
      eye_neutral();
      up_mdelay(600);
    }

  syslog(LOG_INFO, "[emo] done\n");
  return OK;
}

/****************************************************************************
 * Name: lcdtest_hear
 *
 * Description:
 *   Continuous sound localization loop.
 *   LCD + audio_init once at start.  Then loop up to 300 iterations:
 *     capture N=2048 → onset gate → locate → eye_look → print.
 *   Exits on iteration cap or serial keypress.
 *
 ****************************************************************************/

#define HEAR_LOOP_MAX    80    /* ~4-5 s at ~60 ms per iteration */
#define HEAR_N_SAMPLES   2048
#define HEAR_ONSET_GATE  80   /* RMS threshold to trigger locate */

static int lcdtest_hear(void)
{
  int tau_q8;
  int dx;
  int iter;
  int n;
  int energy;
  int pin;
  int count = 0;

  /* ---- Hardware bring-up (same as lcdtest_go steps 1-5,7,9-10,12
   *       but WITHOUT draw_eye — we paint emo eyes directly).
   * ---- Step 1: GPIO 0-52 high, skip reserved pins ---- */

  for (pin = 0; pin <= 52; pin++)
    {
      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      if (pin == 10 || pin == 11 || pin == 8 ||
          pin == 20 || pin == 21 ||
          pin == 38 || pin == 39)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      count++;
    }

  /* Step 2: wait for power rails */

  up_mdelay(500);

  /* Step 3: backlight on */

  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* ---- Left eye: SPI pins + RST + init + display on ---- */

  lcd_setup_pins(&g_lcd_left);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_display_on();

  /* ---- Right eye: SPI pins + RST + init + display on ---- */

  lcd_setup_pins(&g_lcd_right);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_display_on();

  /* ---- Draw emo eyes (no round-eye flash) ---- */

  lcd_set_pins(&g_lcd_left);
  eye_neutral();
  lcd_set_pins(&g_lcd_right);
  eye_neutral();
  lcd_set_pins(&g_lcd_left);

  /* Initialize audio ADC (once) */

  audio_init();

  /* Suppress per-capture syslog during tight loop */

  bk7258_mic_set_quiet(true);

  syslog(LOG_INFO,
         "[hear] === sound localization loop ===\n"
         "[hear] loop_max=%d  N=%d  onset_gate=%d\n",
         HEAR_LOOP_MAX, HEAR_N_SAMPLES, HEAR_ONSET_GATE);

  for (iter = 0; iter < HEAR_LOOP_MAX; iter++)
    {
      /* Capture a chunk */

      n = audio_capture(HEAR_N_SAMPLES);
      if (n < 64)
        {
          continue;
        }

      /* Onset gate: check energy before running full locate */

      energy = bk7258_mic_energy(n);
      if (energy < HEAR_ONSET_GATE)
        {
          continue;
        }

      /* Sound detected — run full locate on this capture */

      dx = mic_locate_process(n, &tau_q8);

      syslog(LOG_INFO,
             "[hear] iter=%d  energy=%d  dx=%d  "
             "tau_q8=%d (%.2f samp)\n",
             iter, energy, dx,
             tau_q8, (double)tau_q8 / 256.0);

      /* Drive eye toward detected direction (both panels) */

      lcd_set_pins(&g_lcd_left);
      eye_look(dx);
      lcd_set_pins(&g_lcd_right);
      eye_look(dx);
      lcd_set_pins(&g_lcd_left);
    }

  /* Cleanup */

  bk7258_mic_set_quiet(false);
  audio_deinit();

  syslog(LOG_INFO,
         "[hear] loop ended after %d iterations\n", iter);

  return OK;
}

/****************************************************************************
 * Name: lcdtest_oeye
 *
 * Description:
 *   Preview official-website-style eyes on both panels.
 *   Cycles: neutral → half → blink → neutral → happy → neutral.
 *
 ****************************************************************************/

static int lcdtest_oeye(void)
{
  /* Hardware bring-up (same as lcdtest_go but skip draw_eye) */

  int pin;
  int count = 0;

  for (pin = 0; pin <= 52; pin++)
    {
      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      if (pin == 10 || pin == 11 || pin == 8 ||
          pin == 20 || pin == 21 ||
          pin == 38 || pin == 39)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
      count++;
    }

  up_mdelay(500);

  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* Left panel init */

  lcd_setup_pins(&g_lcd_left);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_display_on();

  /* Right panel init */

  lcd_setup_pins(&g_lcd_right);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_display_on();

  /* White background — once per panel, never again in the loop */

  lcd_set_pins(&g_lcd_left);
  draw_oeye_bg();
  lcd_set_pins(&g_lcd_right);
  draw_oeye_bg();
  lcd_set_pins(&g_lcd_left);

  syslog(LOG_INFO,
         "[oeye] === official eye preview (partial redraw) ===\n"
         "[oeye] iris_r=%d pupil_r=%d hl1_r=%d hl2_r=%d\n",
         OEYE_IRIS_R, OEYE_PUPIL_R, OEYE_HL1_R, OEYE_HL2_R);

  /* --- Frame 1: neutral (fully open) --- */

  syslog(LOG_INFO, "[oeye] frame: NEUTRAL\n");

  lcd_set_pins(&g_lcd_left);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_right);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_left);

  up_mdelay(2000);

  /* --- Frame 2: half-closed --- */

  syslog(LOG_INFO, "[oeye] frame: HALF\n");

  lcd_set_pins(&g_lcd_left);
  eye_o_half();
  lcd_set_pins(&g_lcd_right);
  eye_o_half();
  lcd_set_pins(&g_lcd_left);

  up_mdelay(1200);

  /* --- Frame 3: smooth blink (incremental eyelid sweep) --- */

  syslog(LOG_INFO, "[oeye] frame: SMOOTH BLINK\n");

  lcd_set_pins(&g_lcd_left);
  eye_o_blink_anim();
  lcd_set_pins(&g_lcd_right);
  eye_o_blink_anim();
  lcd_set_pins(&g_lcd_left);

  /* --- Frame 4: back to neutral --- */

  syslog(LOG_INFO, "[oeye] frame: NEUTRAL\n");

  lcd_set_pins(&g_lcd_left);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_right);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_left);

  up_mdelay(2000);

  /* --- Frame 5: happy --- */

  syslog(LOG_INFO, "[oeye] frame: HAPPY\n");

  lcd_set_pins(&g_lcd_left);
  eye_o_happy();
  lcd_set_pins(&g_lcd_right);
  eye_o_happy();
  lcd_set_pins(&g_lcd_left);

  up_mdelay(2000);

  /* --- Frame 6: back to neutral --- */

  syslog(LOG_INFO, "[oeye] frame: NEUTRAL\n");

  lcd_set_pins(&g_lcd_left);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_right);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_left);

  up_mdelay(2000);

  syslog(LOG_INFO, "[oeye] done\n");

  return OK;
}

/****************************************************************************
 * Name: lcdtest_blink
 *
 * Description:
 *   Preview smooth blink animation on both panels.
 *   Usage: lcdtest blink [count]   (default 5 blinks)
 *
 ****************************************************************************/

static int lcdtest_blink(int count)
{
  int pin;
  int i;

  /* Power-on non-LCD GPIOs */

  for (pin = 0; pin <= 52; pin++)
    {
      if (pin == 2 || pin == 3 || pin == 4 || pin == 5 ||
          pin == 6 || pin == 7 || pin == 22 || pin == 23 ||
          pin == 24 || pin == 25 || pin == 29)
        {
          continue;
        }

      if (pin == 10 || pin == 11 || pin == 8 ||
          pin == 20 || pin == 21 ||
          pin == 38 || pin == 39)
        {
          continue;
        }

      gpio_set_output(pin);
      gpio_write(pin, 1);
    }

  up_mdelay(500);

  gpio_set_output(LCD_PIN_BL);
  gpio_write(LCD_PIN_BL, 1);

  /* Left panel init */

  lcd_setup_pins(&g_lcd_left);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_display_on();

  /* Right panel init */

  lcd_setup_pins(&g_lcd_right);
  gpio_write(g_active_pins->rst, 0);
  up_mdelay(15);
  gpio_write(g_active_pins->rst, 1);
  up_mdelay(120);
  lcd_init_sequence(false);
  lcd_display_on();

  /* White background */

  lcd_set_pins(&g_lcd_left);
  draw_oeye_bg();
  lcd_set_pins(&g_lcd_right);
  draw_oeye_bg();

  syslog(LOG_INFO,
         "[blink] === smooth blink preview: %d blinks ===\n", count);

  /* Initial neutral on both panels */

  lcd_set_pins(&g_lcd_left);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_right);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_left);

  up_mdelay(1000);

  /* Blink loop */

  for (i = 0; i < count; i++)
    {
      syslog(LOG_INFO, "[blink] blink %d/%d\n", i + 1, count);

      lcd_set_pins(&g_lcd_left);
      eye_o_blink_anim();
      lcd_set_pins(&g_lcd_right);
      eye_o_blink_anim();
      lcd_set_pins(&g_lcd_left);

      /* Brief pause between blinks */

      up_mdelay(800 + (i % 3) * 400); /* vary rhythm slightly */
    }

  /* End neutral */

  lcd_set_pins(&g_lcd_left);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_right);
  eye_o_neutral();
  lcd_set_pins(&g_lcd_left);

  syslog(LOG_INFO, "[blink] done\n");
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
 *     lcdtest go       — one-step dual-eye LCD bring-up
 *     lcdtest anim     — pupil tracking animation (3 rounds)
 *     lcdtest emo      — Cozmo/Vector expression eyes (2 rounds)
 *     lcdtest oeye     — official-website-style eye preview
 *     lcdtest blink [N] — smooth blink preview (default 5 blinks)
 *     lcdtest hear     — sound localization → eye_look
 *     lcdtest mic      — raw ADC capture + RMS dump
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

  if (argc > 1 && strcmp(argv[1], "anim") == 0)
    {
      return lcdtest_anim();
    }

  if (argc > 1 && strcmp(argv[1], "emo") == 0)
    {
      return lcdtest_emo();
    }

  if (argc > 1 && strcmp(argv[1], "oeye") == 0)
    {
      return lcdtest_oeye();
    }

  if (argc > 1 && strcmp(argv[1], "blink") == 0)
    {
      int count = 5;

      if (argc > 2)
        {
          count = atoi(argv[2]);
        }

      if (count < 1)
        {
          count = 1;
        }

      return lcdtest_blink(count);
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

  if (argc > 1 && strcmp(argv[1], "hear") == 0)
    {
      return lcdtest_hear();
    }

  if (argc > 1 && strcmp(argv[1], "mic") == 0)
    {
      return bk7258_mic_main(argc - 1, &argv[1]);
    }

  return lcdtest_stages();
}
