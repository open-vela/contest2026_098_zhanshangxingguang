/****************************************************************************
 * BK7258 battery voltage via internal SARADC (channel 0 = VBAT, 1/5 divider).
 * Single-shot polled read.  Register-level hand-port from ARMino saradc.
 *   SARADC base 0x45890000; SYS analog-SPI bias in ana_reg2/5/9 @0x44010000.
 * NOTE: absolute mV needs one-time empirical calibration (no eFuse codes).
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <debug.h>
#include <nuttx/arch.h>

#include "bk7258_gpio.h"   /* getreg32 / putreg32 */

/* SYS analog block (same as audio driver) */
#define SYSB              0x44010000u
#define ANA_BASE          (SYSB + 0x100u)            /* ana_reg0 @0x44010100 */
#define ANA_SPI_STATE     (SYSB + (0x3au << 2))      /* 0x440100E8 */
#define ANA_REG2          (SYSB + (0x42u << 2))      /* 0x44010108 GADC bias */
#define ANA_REG5          (SYSB + (0x45u << 2))      /* 0x44010114 adc_div */
#define ANA_REG9          (SYSB + (0x49u << 2))      /* 0x44010124 spi_latch1v */
#define SYS_DEV_CLK_EN    (SYSB + (0x0cu << 2))      /* 0x44010030 */
#define SADC_CKEN         (1u << 5)

/* SARADC controller @0x45890000 */
#define SADC_BASE         0x45890000u
#define SADC_GLOBAL_CTRL  (SADC_BASE + 0x08u)  /* bit0 soft_reset */
#define SADC_CTRL         (SADC_BASE + 0x10u)
#define SADC_RAW_DATA     (SADC_BASE + 0x14u)
#define SADC_STEADY_CTRL  (SADC_BASE + 0x18u)
#define SADC_SAT_CTRL     (SADC_BASE + 0x1cu)
#define SADC_ADC_DATA     (SADC_BASE + 0x20u)  /* bits[15:0] saturated result */

/* One-point calibration: measured raw 6533 == 4200 mV (full, multimeter) */
#define BAT_CAL_RAW    6533
#define BAT_CAL_MV     4200
#define BAT_FULL_MV    4200   /* Li-ion 100% */
#define BAT_EMPTY_MV   3300   /* Li-ion ~0%  */

/* analog-SPI write with completion poll (same scheme as bk7258_audio.c) */
static void ana_write(uintptr_t addr, uint32_t val)
{
  uint32_t idx = (uint32_t)((addr - ANA_BASE) >> 2);
  putreg32(val, addr);
  while (getreg32(ANA_SPI_STATE) & (1u << idx))
    {
    }
}

static void ana_field(uintptr_t addr, int pos, uint32_t mask, uint32_t val)
{
  uint32_t r = getreg32(addr);
  r &= ~(mask << pos);
  r |= (val & mask) << pos;
  ana_write(addr, r);
}

static void battery_adc_init(void)
{
  uint32_t v;

  /* SADC clock on (plain MMIO) */
  v = getreg32(SYS_DEV_CLK_EN);
  v |= SADC_CKEN;
  putreg32(v, SYS_DEV_CLK_EN);

  /* GADC analog bias/enable (ana_reg2, via analog-SPI) */
  ana_field(ANA_REG2, 15, 0x1,  1);   /* gadc_nobuf_enable */
  ana_field(ANA_REG2, 25, 0x7f, 0x3); /* sp_nt_ctrl */
  ana_field(ANA_REG2, 13, 0x3,  2);   /* gadc_refbuf_ictrl */
  ana_field(ANA_REG2, 11, 0x3,  2);   /* gadc_inbuf_ictrl */
  ana_field(ANA_REG2,  9, 0x3,  2);   /* gadc_cmp_ictrl */

  /* VBAT (ch0) input divider = 1/5: latch, set adc_div=1, unlatch */
  ana_field(ANA_REG9, 9,  0x1, 1);    /* spi_latch1v = 1 */
  ana_field(ANA_REG5, 10, 0x3, 1);    /* ch0 adc_div=1 => 1/5 */
  ana_field(ANA_REG9, 9,  0x1, 0);    /* spi_latch1v = 0 */

  up_mdelay(2);
}

static uint16_t battery_sample_once(void)
{
  uint32_t v;
  int spin;

  putreg32(getreg32(SADC_GLOBAL_CTRL) | 0x1u, SADC_GLOBAL_CTRL); /* soft reset */
  putreg32(0, SADC_CTRL);

  putreg32((7u << 5) | (1u << 10), SADC_STEADY_CTRL); /* steady=7, bypass calib */
  putreg32((1u << 2) | (0u << 0),  SADC_SAT_CTRL);    /* sat_enable, 12-bit */

  /* ctrl: single-step mode, ch0, 4-cyc, adc_div=12 (26M/2/13 ~= 1MHz) */
  v = (1u << 0) | (0u << 3) | (0u << 7) | (12u << 9);
  putreg32(v, SADC_CTRL);
  putreg32(v | (1u << 2), SADC_CTRL);  /* adc_en */

  spin = 200000;
  while ((getreg32(SADC_CTRL) & 0x3u) && --spin)   /* single-step auto-clears mode */
    {
    }
  up_udelay(50);

  {
    uint16_t data = (uint16_t)(getreg32(SADC_ADC_DATA) & 0xffffu);
    putreg32(getreg32(SADC_CTRL) & ~(1u << 2), SADC_CTRL); /* adc_en off */
    return data;
  }
}

static uint16_t g_bat_raw;

int bk7258_battery_read_mv(void)
{
  uint32_t sum = 0;
  uint32_t raw;
  int i;

  battery_adc_init();
  (void)battery_sample_once();          /* discard first */

  for (i = 0; i < 8; i++)
    {
      sum += battery_sample_once();
      up_mdelay(3);
    }

  raw = sum / 8;
  g_bat_raw = (uint16_t)raw;

  return (int)(raw * BAT_CAL_MV / BAT_CAL_RAW);   /* mV */
}

static int battery_pct(int mv)
{
  if (mv <= BAT_EMPTY_MV) return 0;
  if (mv >= BAT_FULL_MV)  return 100;
  return (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
}

int bk7258_battery_main(int argc, char *argv[])
{
  int mv  = bk7258_battery_read_mv();
  int pct = battery_pct(mv);

  syslog(LOG_INFO, "[bat] raw=%u  %d mV  ~%d%%\n",
         (unsigned)g_bat_raw, mv, pct);
  return 0;
}
