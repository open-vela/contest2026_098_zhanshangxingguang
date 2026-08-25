/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_audio.c
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
 * BK7258 analog audio ADC dual-channel (L=M1, R=M2) polling capture.
 *
 * Register-level driver — no DMA, no interrupts.
 * Source: Armino aud_common_driver.c + aud_adc_driver.c + sys_ll.h
 *
 * CRITICAL: analog registers (ana_reg*) are NOT normal MMIO.  Writes go
 * through a serial SPI bus and must poll for completion before the next
 * write.  Without this the analog front-end never configures and the
 * ADC FIFO stays empty.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <debug.h>

#include <nuttx/arch.h>

#include "bk7258_gpio.h"   /* getreg32/putreg32 + GPIO cfg macros for PA control */
#include <nuttx/nuttx.h>

/****************************************************************************
 * Inline Register Access (self-contained, no arch dependency)
 ****************************************************************************/

static inline uint32_t aud_getreg(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static inline void aud_putreg(uint32_t val, uintptr_t addr)
{
  *(volatile uint32_t *)addr = val;
}

/****************************************************************************
 * Pre-processor Definitions — Base Addresses
 ****************************************************************************/

#define AUD_BASE           0x47800000
#define SYS_BASE           0x44010000

/* SYS audio clock registers */

#define SYS_CPU_POWER_SLEEP_WAKEUP (SYS_BASE + 0x40) /* reg0x10 */
#define SYS_CPU_DEVICE_CLK_ENABLE  (SYS_BASE + 0x30) /* reg0x0C */
#define SYS_CPU_CLK_DIV_MODE1     (SYS_BASE + 0x20) /* reg0x08 */
#define SYS_PWD_AUDP              (1u << 6)
#define SYS_AUD_CKEN              (1u << 30)
#define SYS_CKSEL_AUD             (1u << 25)

/* Analog register SPI completion status.
 * Each bit N corresponds to analog register N (index from ANA_REG0).
 * Bit = 1 means SPI transfer still in progress.
 */

#define ANA_BASE           (SYS_BASE + 0x100) /* 0x44010100 */
#define ANA_REG5           (SYS_BASE + 0x45 * 4) /* 0x44010114, pwdaudpll=bit13 */
#define ANA_REG18          (SYS_BASE + 0x52 * 4) /* 0x44010148 */
#define ANA_REG19          (SYS_BASE + 0x53 * 4) /* 0x4401014C */
#define ANA_REG20          (SYS_BASE + 0x54 * 4) /* 0x44010150 */
#define ANA_REG21          (SYS_BASE + 0x55 * 4) /* 0x44010154 */
#define ANA_REG25          (SYS_BASE + 0x59 * 4) /* 0x44010164, spi_trigger=bit18 */
#define ANA_REG26          (SYS_BASE + 0x5a * 4) /* 0x44010168 */
#define ANA_REG27          (SYS_BASE + 0x5b * 4) /* 0x4401016C */
#define ANA_SPI_STATE_REG  (SYS_BASE + 0x3a * 4) /* 0x440100E8 */

/****************************************************************************
 * Audio Digital Register Offsets (from Armino aud_reg.h)
 ****************************************************************************/

#define AUD_CLK_CONTROL    (AUD_BASE + 0x08)
#define AUD_ADC_CONFIG_0   (AUD_BASE + 0x10)
#define AUD_FIFO_CONFIG    (AUD_BASE + 0x28)
#define AUD_FIFO_STATUS    (AUD_BASE + 0x38)
#define AUD_ADC_FIFO_PORT  (AUD_BASE + 0x44)
#define AUD_EXTEND_CFG     (AUD_BASE + 0x60)
#define AUD_ADC_FRACMOD    (AUD_BASE + 0x68)
#define AUD_CONFIG         (AUD_BASE + 0xc0)

/* AUD_CLK_CONTROL bits */

#define AUD_ADC_SOFT_RESET (1u << 0)
#define AUD_ADC_CLK_GATE   (1u << 1)

/* AUD_ADC_CONFIG_0 bits */

#define AUD_ADC_HPF2_BYP   (1u << 16)
#define AUD_ADC_HPF1_BYP   (1u << 17)
#define AUD_ADC_GAIN_MASK  (0x3fu << 18)
#define AUD_ADC_GAIN_0DB   (0x2du << 18)
#define AUD_ADC_EDGE       (1u << 24)

/* AUD_FIFO_STATUS bits */

#define AUD_ADC_FIFO_EMPTY (1u << 14)

/* AUD_CONFIG bits */

#define AUD_RATE_ADC_MASK  (0x3u << 0)
#define AUD_RATE_ADC_16K   (0x1u << 0)
#define AUD_RATE_ADC_48K   (0x3u << 0)
#define AUD_ADC_ENABLE     (1u << 3)
#define AUD_LINEIN_ENABLE  (1u << 5)
#define AUD_APLL_SEL       (1u << 8)

/* AUD_EXTEND_CFG bits */

#define AUD_ADC_FRAC_MANUAL (1u << 1)

/* Analog register bit positions (from sys_reg.h) */

#define ANA_ENAUDBIAS_BIT  3
#define ANA_ENADCBIAS_BIT  4
#define ANA_ENMICBIAS_BIT  5
#define ANA_MICEN_BIT      28
#define ANA_MIC_RST_BIT    29

/****************************************************************************
 * DAC playback additions (M2c speaker bring-up)
 *   Digital regs share AUD_BASE; analog enables share ANA_REG20/21 and the
 *   ana_write/ana_setbit SPI mechanism used by capture.
 *   Port of Armino bk_aud_dac_init() (aud_dac_driver.c) + sys_hal.c mapping.
 ****************************************************************************/

#define AUD_DAC_CONFIG_0   (AUD_BASE + 0x1c)  /* REG_0x07 */
#define AUD_DAC_FIFO_PORT  (AUD_BASE + 0x48)  /* REG_0x12, dacl_port[15:0] */

/* AUD_DAC_CONFIG_0 bits */

#define AUD_DAC_HPF2_BYP   (1u << 16)
#define AUD_DAC_HPF1_BYP   (1u << 17)
#define AUD_DAC_GAIN_MASK  (0x3fu << 18)
#define AUD_DAC_GAIN_0DB   (0x2du << 18)   /* 0x2d = 0 dB */
#define AUD_DAC_CLK_INVERT (1u << 24)

/* AUD_CONFIG DAC bits (dac_enable=bit2, samp_rate_dac=[7:6]) */

#define AUD_DAC_ENABLE     (1u << 2)
#define AUD_RATE_DAC_MASK  (0x3u << 6)
#define AUD_RATE_DAC_8K    (0x0u << 6)
#define AUD_RATE_DAC_16K   (0x1u << 6)
#define AUD_RATE_DAC_48K   (0x3u << 6)

/* AUD_FIFO_STATUS: DAC-left FIFO full (bit9) */

#define AUD_DACL_FIFO_FULL (1u << 9)

/* Analog DAC enable bit positions (sys_hal.c → ana_reg20/21) */

#define ANA_DAC_LENDCOC_BIT  16   /* reg20: dcoc_en      */
#define ANA_DAC_DRVEN_BIT    19   /* reg20: dac_drv_en   */
#define ANA_DAC_LEN_BIT      21   /* reg20: dacl_en      */
#define ANA_DAC_ENIDACL_BIT  18   /* reg21: idac_en      */
#define ANA_DAC_ENBS_BIT     23   /* reg21: dac_bias_en  */

/****************************************************************************
 * Analog register base values.
 *
 * Computed from Armino *_DEFAULT_VAL macros in aud_common_driver.c.
 * These are the power-on defaults with trim fields — NOT zero.
 * The driver then layers enable bits (audbias, micen, etc.) on top.
 ****************************************************************************/

#define ANA_REG18_BASE     0x0bec0085u
#define ANA_REG19_BASE     0x81800006u
#define ANA_REG20_BASE     0xfbc02213u
#define ANA_REG21_BASE     0x00800400u

/* MIC2 (ana_reg27) uses the same base as reg19 but with mic2en
 * set in a second step.  Direct full-word write like Armino.
 */

#define ANA_REG27_BASE     0x81800006u

/****************************************************************************
 * Sample counts / timeouts
 ****************************************************************************/

#define MIC_N_SAMPLES      4096

#define FIFO_SPIN_LIMIT    1000
#define TOTAL_SPIN_CAP     (4u * 1024u * 1024u)

/* Cross-correlation parameters for sound localization */

#define CORR_LAG_MAX       32   /* lag search range [-32, +32] samples
                                 * 21 cm mic → ~29 samp max @48k */
#define TAU_DEAD_Q8        77   /* ~0.3 sample in Q8 (0.3 * 256 ≈ 77) */
#define RMS_GATE           50   /* silence gate: both channels below this → centered */
#define DX_LEFT           (-22)
#define DX_CENTER           0
#define DX_RIGHT            22

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int16_t g_mic_left[MIC_N_SAMPLES];
static int16_t g_mic_right[MIC_N_SAMPLES];

/* When true, audio_capture suppresses per-capture syslog to avoid
 * flooding the console in tight loops (e.g. lcdtest hear).
 */

static bool g_capture_quiet;

/****************************************************************************
 * Name: bk7258_mic_set_quiet
 *
 * Description:
 *   Suppress (true) or restore (false) audio_capture syslog messages.
 *
 ****************************************************************************/

void bk7258_mic_set_quiet(bool q)
{
  g_capture_quiet = q;
}

/****************************************************************************
 * Private Functions — Analog SPI
 ****************************************************************************/

/****************************************************************************
 * Name: ana_write
 *
 * Description:
 *   Write a value to an analog register and wait for the SPI serial
 *   transfer to complete.  Without this the value never takes effect.
 *
 ****************************************************************************/

static void ana_write(uintptr_t addr, uint32_t val)
{
  uint32_t idx = (addr - ANA_BASE) >> 2;

  aud_putreg(val, addr);

  while (aud_getreg(ANA_SPI_STATE_REG) & (1u << idx))
    {
    }
}

/****************************************************************************
 * Name: ana_setbit
 *
 * Description:
 *   Read-modify-write a single bitfield in an analog register via
 *   ana_write (with SPI completion poll).
 *
 ****************************************************************************/

static void ana_setbit(uintptr_t addr, int pos, uint32_t val)
{
  uint32_t r = aud_getreg(addr);

  r &= ~(1u << pos);
  r |= ((val & 1u) << pos);
  ana_write(addr, r);
}

/****************************************************************************
 * Private Functions — Audio Init / Capture
 ****************************************************************************/

/****************************************************************************
 * Name: audio_init
 *
 * Description:
 *   Initialise BK7258 audio ADC for dual-channel differential capture
 *   at 48 kHz.  Sequence from Armino:
 *     aud_common_driver.c: bk_aud_driver_init()
 *     aud_adc_driver.c:    bk_aud_adc_init()
 *     sys_ll.h:            analog SPI write helpers
 *
 ****************************************************************************/

void audio_init(void)
{
  uint32_t val;

  /* ---- Step -1: Audio power domain ----
   * cpu_power_sleep_wakeup bit6 pwd_audp: 0 = power on.
   * Must be first — without this the audio rail is off and
   * nothing else takes effect.
   */

  val = aud_getreg(SYS_CPU_POWER_SLEEP_WAKEUP);
  val &= ~SYS_PWD_AUDP;
  aud_putreg(val, SYS_CPU_POWER_SLEEP_WAKEUP);
  up_mdelay(2);

  syslog(LOG_INFO,
         "[mic] PWR: POWER_SLEEP_WAKEUP=0x%08lx\n",
         (unsigned long)aud_getreg(SYS_CPU_POWER_SLEEP_WAKEUP));

  /* ---- Step 0: SYS audio clock ----
   * Must be done BEFORE any AUD 0x478xxxxx register access.
   * cksel_aud = 1 (APLL), aud_cken = 1.
   * Analog ADC requires APLL; XTAL does not produce sample clocks.
   */

  val = aud_getreg(SYS_CPU_CLK_DIV_MODE1);
  val |= SYS_CKSEL_AUD;
  aud_putreg(val, SYS_CPU_CLK_DIV_MODE1);

  /* APLL power-on and configuration (from Armino BK7236XX APLL branch) */

  ana_setbit(ANA_REG5, 13, 0);          /* pwdaudpll=0: APLL power up */
  ana_write(ANA_REG26, 0x8973CA6F);     /* cal_val (16k/48k) */
  ana_write(ANA_REG25, 0xC2A0AE86);     /* config */
  ana_setbit(ANA_REG25, 18, 1);         /* spi_trigger pulse */
  up_mdelay(1);
  ana_setbit(ANA_REG25, 18, 0);
  up_mdelay(2);                         /* wait for APLL lock */

  val = aud_getreg(SYS_CPU_DEVICE_CLK_ENABLE);
  val |= SYS_AUD_CKEN;
  aud_putreg(val, SYS_CPU_DEVICE_CLK_ENABLE);

  syslog(LOG_INFO,
         "[mic] SYS aud clock: DIV=0x%08lx CLK=0x%08lx\n",
         (unsigned long)aud_getreg(SYS_CPU_CLK_DIV_MODE1),
         (unsigned long)aud_getreg(SYS_CPU_DEVICE_CLK_ENABLE));

  /* ---- Step 1: AUD digital soft reset ---- */

  val = aud_getreg(AUD_CLK_CONTROL);
  val &= ~AUD_ADC_CLK_GATE;
  aud_putreg(val, AUD_CLK_CONTROL);

  val = aud_getreg(AUD_CLK_CONTROL);
  val |= AUD_ADC_SOFT_RESET;
  aud_putreg(val, AUD_CLK_CONTROL);

  /* ---- Step 2: Analog register base values ----
   * Must go through analog-SPI (ana_write), NOT raw putreg32.
   * These carry trim fields from Armino *_DEFAULT_VAL computation.
   */

  syslog(LOG_INFO, "[mic] ana: writing base values\n");

  ana_write(ANA_REG18, ANA_REG18_BASE);
  ana_write(ANA_REG19, ANA_REG19_BASE);
  ana_write(ANA_REG20, ANA_REG20_BASE);
  ana_write(ANA_REG21, ANA_REG21_BASE);
  ana_write(ANA_REG27, ANA_REG27_BASE);

  /* ---- Step 3: Enable analog bias + mic enables ----
   * Each setbit goes through ana_write with SPI poll.
   * Armino: aud_adc_driver.c lines 101-106.
   */

  syslog(LOG_INFO, "[mic] ana: enable bias + mic1 + mic2\n");

  /* audbias / adcbias / micbias on ana_reg18 */

  ana_setbit(ANA_REG18, ANA_ENAUDBIAS_BIT, 1);
  ana_setbit(ANA_REG18, ANA_ENADCBIAS_BIT, 1);
  ana_setbit(ANA_REG18, ANA_ENMICBIAS_BIT, 1);

  /* mic1 enable on ana_reg19 bit28 */

  ana_setbit(ANA_REG19, ANA_MICEN_BIT, 1);

  /* mic2 enable on ana_reg27 bit28.
   * (sys_hal_aud_mic2_en is "not support" — write full word.)
   */

  ana_setbit(ANA_REG27, ANA_MICEN_BIT, 1);

  /* Differential mode: ana_reg19 micsingleen=0 (already 0 in base),
   * ana_reg20 diffen=1 (already 1 in base).
   * No extra writes needed.
   */

  /* ---- Step 4: MIC reset pulse ---- */

  syslog(LOG_INFO, "[mic] ana: mic reset\n");

  ana_setbit(ANA_REG19, ANA_MIC_RST_BIT, 1);
  up_mdelay(10);
  ana_setbit(ANA_REG19, ANA_MIC_RST_BIT, 0);

  /* ---- Step 5: ADC digital config ---- */

  syslog(LOG_INFO, "[mic] digital: ADC config\n");

  val = aud_getreg(AUD_ADC_CONFIG_0);
  val &= ~(AUD_ADC_GAIN_MASK | AUD_ADC_EDGE);
  val |= AUD_ADC_GAIN_0DB;
  val |= AUD_ADC_HPF1_BYP | AUD_ADC_HPF2_BYP;
  aud_putreg(val, AUD_ADC_CONFIG_0);

  /* ---- Step 6: Sample rate 48kHz, APLL ---- */

  val = aud_getreg(AUD_CONFIG);
  val &= ~AUD_RATE_ADC_MASK;
  val |= AUD_RATE_ADC_48K | AUD_APLL_SEL;
  aud_putreg(val, AUD_CONFIG);

  val = aud_getreg(AUD_EXTEND_CFG);
  val &= ~AUD_ADC_FRAC_MANUAL;
  aud_putreg(val, AUD_EXTEND_CFG);

  /* ---- Step 7: Enable ADC + analog input path ----
   * Armino bk_aud_adc_start: adc_enable and line_enable
   * are set together.  line_enable is the analog ADC input
   * path enable, NOT an external line-in selector.
   */

  syslog(LOG_INFO, "[mic] digital: ADC enable\n");

  val = aud_getreg(AUD_CONFIG);
  val |= AUD_ADC_ENABLE | AUD_LINEIN_ENABLE;
  aud_putreg(val, AUD_CONFIG);

  up_mdelay(50);

  /* Diagnostic readback */

  syslog(LOG_INFO,
         "[mic] init done. ana_reg readback:\n"
         "  reg5=0x%08lx reg25=0x%08lx reg26=0x%08lx\n"
         "  reg18=0x%08lx reg19=0x%08lx\n"
         "  reg20=0x%08lx reg27=0x%08lx\n"
         "  AUD_CONFIG=0x%08lx\n",
         (unsigned long)aud_getreg(ANA_REG5),
         (unsigned long)aud_getreg(ANA_REG25),
         (unsigned long)aud_getreg(ANA_REG26),
         (unsigned long)aud_getreg(ANA_REG18),
         (unsigned long)aud_getreg(ANA_REG19),
         (unsigned long)aud_getreg(ANA_REG20),
         (unsigned long)aud_getreg(ANA_REG27),
         (unsigned long)aud_getreg(AUD_CONFIG));
}

/****************************************************************************
 * Name: audio_deinit
 *
 * Description:
 *   Disable ADC, power down analog front-end.
 *
 ****************************************************************************/

void audio_deinit(void)
{
  uint32_t val;

  syslog(LOG_INFO, "[mic] deinit\n");

  val = aud_getreg(AUD_CONFIG);
  val &= ~(AUD_ADC_ENABLE | AUD_LINEIN_ENABLE);
  aud_putreg(val, AUD_CONFIG);

  /* Zero the analog registers via SPI */

  ana_write(ANA_REG18, 0);
  ana_write(ANA_REG19, 0);
  ana_write(ANA_REG20, 0);
  ana_write(ANA_REG21, 0);
  ana_write(ANA_REG27, 0);

  val = aud_getreg(AUD_CLK_CONTROL);
  val |= AUD_ADC_CLK_GATE;
  aud_putreg(val, AUD_CLK_CONTROL);
}

/****************************************************************************
 * Name: audio_dac_init
 *
 * Description:
 *   Initialise BK7258 internal audio DAC (left channel, differential) for
 *   speaker playback.  Reuses the same power/APLL clock and analog-SPI
 *   sequence as audio_init(); does NOT enable the ADC/mic.
 *   samp_rate: 8000 | 16000 | 48000.
 *   Port of Armino bk_aud_dac_init() (aud_dac_driver.c).
 ****************************************************************************/

void audio_dac_init(int samp_rate)
{
  uint32_t val;
  uint32_t rate_bits;

  /* ---- Power domain on ---- */

  val = aud_getreg(SYS_CPU_POWER_SLEEP_WAKEUP);
  val &= ~SYS_PWD_AUDP;
  aud_putreg(val, SYS_CPU_POWER_SLEEP_WAKEUP);
  up_mdelay(2);

  /* ---- SYS audio clock + APLL (identical to capture) ---- */

  val = aud_getreg(SYS_CPU_CLK_DIV_MODE1);
  val |= SYS_CKSEL_AUD;
  aud_putreg(val, SYS_CPU_CLK_DIV_MODE1);

  ana_setbit(ANA_REG5, 13, 0);          /* APLL power up */
  ana_write(ANA_REG26, 0x8973CA6F);     /* cal */
  ana_write(ANA_REG25, 0xC2A0AE86);     /* config */
  ana_setbit(ANA_REG25, 18, 1);         /* spi_trigger pulse */
  up_mdelay(1);
  ana_setbit(ANA_REG25, 18, 0);
  up_mdelay(2);

  val = aud_getreg(SYS_CPU_DEVICE_CLK_ENABLE);
  val |= SYS_AUD_CKEN;
  aud_putreg(val, SYS_CPU_DEVICE_CLK_ENABLE);

  /* ---- AUD digital block soft reset (global reset/gate, shared) ---- */

  val = aud_getreg(AUD_CLK_CONTROL);
  val &= ~AUD_ADC_CLK_GATE;
  aud_putreg(val, AUD_CLK_CONTROL);

  val = aud_getreg(AUD_CLK_CONTROL);
  val |= AUD_ADC_SOFT_RESET;
  aud_putreg(val, AUD_CLK_CONTROL);

  /* ---- Analog base values (carry trim; diffen + dac bias preset) ---- */

  ana_write(ANA_REG18, ANA_REG18_BASE);
  ana_write(ANA_REG20, ANA_REG20_BASE);
  ana_write(ANA_REG21, ANA_REG21_BASE);

  /* ---- DAC analog enables (order per Armino bk_aud_dac_init) ---- */

  ana_setbit(ANA_REG21, ANA_DAC_ENBS_BIT,    1);  /* dac bias   */
  ana_setbit(ANA_REG20, ANA_DAC_DRVEN_BIT,   1);  /* driver     */
  ana_setbit(ANA_REG20, ANA_DAC_LENDCOC_BIT, 1);  /* dcoc       */
  ana_setbit(ANA_REG21, ANA_DAC_ENIDACL_BIT, 1);  /* idac       */
  ana_setbit(ANA_REG20, ANA_DAC_LEN_BIT,     1);  /* dac left   */
  /* diffen (reg20 bit13) already 1 in base → differential output */

  /* ---- DAC digital: 0 dB gain, HPF bypass, normal clk edge ---- */

  val = aud_getreg(AUD_DAC_CONFIG_0);
  val &= ~(AUD_DAC_GAIN_MASK | AUD_DAC_CLK_INVERT);
  val |= AUD_DAC_GAIN_0DB | AUD_DAC_HPF1_BYP | AUD_DAC_HPF2_BYP;
  aud_putreg(val, AUD_DAC_CONFIG_0);

  /* ---- Sample rate (APLL) ---- */

  switch (samp_rate)
    {
      case 8000:  rate_bits = AUD_RATE_DAC_8K;  break;
      case 48000: rate_bits = AUD_RATE_DAC_48K; break;
      default:    rate_bits = AUD_RATE_DAC_16K; break;   /* 16k default */
    }

  val = aud_getreg(AUD_CONFIG);
  val &= ~AUD_RATE_DAC_MASK;
  val |= rate_bits | AUD_APLL_SEL;
  aud_putreg(val, AUD_CONFIG);

  /* ---- Enable DAC ---- */

  val = aud_getreg(AUD_CONFIG);
  val |= AUD_DAC_ENABLE;
  aud_putreg(val, AUD_CONFIG);

  up_mdelay(5);

  syslog(LOG_INFO,
         "[spk] dac init: rate=%d AUD_CONFIG=0x%08lx reg20=0x%08lx reg21=0x%08lx\n",
         samp_rate,
         (unsigned long)aud_getreg(AUD_CONFIG),
         (unsigned long)aud_getreg(ANA_REG20),
         (unsigned long)aud_getreg(ANA_REG21));
}

/****************************************************************************
 * Name: audio_dac_deinit
 ****************************************************************************/

void audio_dac_deinit(void)
{
  uint32_t val;

  syslog(LOG_INFO, "[spk] dac deinit\n");

  val = aud_getreg(AUD_CONFIG);
  val &= ~AUD_DAC_ENABLE;
  aud_putreg(val, AUD_CONFIG);

  ana_write(ANA_REG18, 0);
  ana_write(ANA_REG20, 0);
  ana_write(ANA_REG21, 0);

  val = aud_getreg(AUD_CLK_CONTROL);
  val |= AUD_ADC_CLK_GATE;
  aud_putreg(val, AUD_CLK_CONTROL);
}

/****************************************************************************
 * Name: pa_gpio_set
 *   Drive an arbitrary pin as GPIO output at the given level.  Used to
 *   enable/disable the external HT6873 Class-D speaker amp (PA_SD).
 *   Selects GPIO (first) function + output-enable, robust for any pin.
 ****************************************************************************/

static void pa_gpio_set(int pin, int level)
{
  uintptr_t cfg_addr = BK7258_GPIO_CFG(pin);
  uintptr_t fn_addr  = BK7258_SYS_GPIO_FUNC(pin);
  uint32_t v;

  /* SYS func-select nibble → 0 (GPIO / first function) */

  v = getreg32(fn_addr);
  v &= ~(BK7258_GPIO_FUNC_MASK << BK7258_GPIO_FUNC_SHIFT(pin));
  putreg32(v, fn_addr);

  /* AON cfg: GPIO mode, output enabled, drive level */

  v = getreg32(cfg_addr);
  v &= ~GPIO_CFG_SECOND_FUNC;   /* bit6=0: GPIO mode          */
  v &= ~GPIO_CFG_OUTPUT_EN;     /* bit3=0: output enabled (active-low) */
  if (level)
    {
      v |= GPIO_CFG_OUTPUT;     /* bit1=1: HIGH */
    }
  else
    {
      v &= ~GPIO_CFG_OUTPUT;    /* bit1=0: LOW  */
    }
  putreg32(v, cfg_addr);
}

/****************************************************************************
 * Name: audio_dac_write
 *   Poll DAC-left FIFO; write one 16-bit sample when not full.
 ****************************************************************************/

static void audio_dac_write(int16_t sample)
{
  uint32_t spin = 0;

  while (aud_getreg(AUD_FIFO_STATUS) & AUD_DACL_FIFO_FULL)
    {
      if (++spin >= TOTAL_SPIN_CAP)
        {
          break;   /* safety: never hang the shell */
        }
    }

  aud_putreg((uint32_t)(uint16_t)sample, AUD_DAC_FIFO_PORT);
}

/****************************************************************************
 * Name: audio_beep
 *   Play a sine tone through the speaker (polling data pump).
 *   freq_hz: 50..4000, ms: 10..5000.  Bring-up test for M2c.
 ****************************************************************************/

int audio_beep(int freq_hz, int ms, int pa_gpio)
{
  const int fs = 16000;
  const int16_t amp = 8000;          /* ~ -12 dBFS, safe level */
  static int16_t period[320];        /* fs/50 = 320 max */
  int plen;
  long total;
  long i;

  if (freq_hz < 50)   freq_hz = 50;
  if (freq_hz > 4000) freq_hz = 4000;
  if (ms < 10)   ms = 10;
  if (ms > 5000) ms = 5000;

  plen = fs / freq_hz;
  if (plen < 4)   plen = 4;
  if (plen > 320) plen = 320;

  for (i = 0; i < plen; i++)
    {
      float ph = 2.0f * 3.1415926f * (float)i / (float)plen;
      period[i] = (int16_t)((float)amp * sinf(ph));
    }

  audio_dac_init(fs);

  /* Enable external Class-D PA (HT6873) — active high.  pa_gpio < 0 skips. */

  if (pa_gpio >= 0)
    {
      pa_gpio_set(pa_gpio, 1);
      up_mdelay(10);   /* PA turn-on settling */
      syslog(LOG_INFO, "[spk] PA enable: P%d = HIGH\n", pa_gpio);
    }

  total = (long)fs * ms / 1000;

  syslog(LOG_INFO, "[spk] beep %d Hz, %d ms (%ld samples, period=%d)\n",
         freq_hz, ms, total, plen);

  for (i = 0; i < total; i++)
    {
      audio_dac_write(period[i % plen]);
    }

  for (i = 0; i < 256; i++)
    {
      audio_dac_write(0);
    }

  if (pa_gpio >= 0)
    {
      pa_gpio_set(pa_gpio, 0);   /* PA shutdown */
    }

  audio_dac_deinit();

  syslog(LOG_INFO, "[spk] beep done\n");
  return 0;
}

/****************************************************************************
 * Name: audio_capture
 *
 * Description:
 *   Poll ADC FIFO and capture N sample pairs (L+R interleaved).
 *   Each AUD_ADC_FIFO_PORT read returns L in [15:0], R in [31:16].
 *   Times out with diagnostics if FIFO stays empty.
 *
 ****************************************************************************/

int audio_capture(int n)
{
  int count = 0;
  uint32_t status;
  uint32_t sample;
  uint32_t spin_sample;
  uint32_t spin_total = 0;

  if (!g_capture_quiet)
    {
      syslog(LOG_INFO, "[mic] capturing %d samples ...\n", n);
    }

  while (count < n)
    {
      spin_sample = 0;

      for (;;)
        {
          status = aud_getreg(AUD_FIFO_STATUS);

          if (!(status & AUD_ADC_FIFO_EMPTY))
            {
              break;
            }

          if (++spin_sample >= FIFO_SPIN_LIMIT)
            {
              goto timeout;
            }
        }

      sample = aud_getreg(AUD_ADC_FIFO_PORT);
      g_mic_left[count]  = (int16_t)(sample & 0xffff);
      g_mic_right[count] = (int16_t)((sample >> 16) & 0xffff);
      count++;

      spin_total += spin_sample;

      if (spin_total >= TOTAL_SPIN_CAP)
        {
          goto timeout;
        }
    }

  if (!g_capture_quiet)
    {
      syslog(LOG_INFO, "[mic] capture done: %d pairs\n", count);
    }

  return count;

timeout:
  syslog(LOG_ERR,
         "[mic] TIMEOUT after %d pairs\n"
         "  AUD_FIFO_STATUS = 0x%08lx\n"
         "  AUD_CONFIG      = 0x%08lx\n"
         "  ana_reg18       = 0x%08lx\n"
         "  ana_reg19       = 0x%08lx\n"
         "  ana_reg27       = 0x%08lx\n"
         "  SPI_STATE       = 0x%08lx\n"
         "  PWR_SLEEP_WAKE  = 0x%08lx\n",
         count,
         (unsigned long)aud_getreg(AUD_FIFO_STATUS),
         (unsigned long)aud_getreg(AUD_CONFIG),
         (unsigned long)aud_getreg(ANA_REG18),
         (unsigned long)aud_getreg(ANA_REG19),
         (unsigned long)aud_getreg(ANA_REG27),
         (unsigned long)aud_getreg(ANA_SPI_STATE_REG),
         (unsigned long)aud_getreg(SYS_CPU_POWER_SLEEP_WAKEUP));
  return count;
}

/****************************************************************************
 * Name: compute_rms
 *
 * Description:
 *   Compute RMS of an int16_t buffer.
 *
 ****************************************************************************/

static float compute_rms(const int16_t *buf, int n)
{
  double sum = 0.0;
  int i;

  for (i = 0; i < n; i++)
    {
      sum += (double)buf[i] * (double)buf[i];
    }

  return (float)sqrt(sum / n);
}

/****************************************************************************
 * Name: mic_test
 *
 * Description:
 *   Full mic test: init, capture, print RMS + 8 samples, deinit.
 *
 ****************************************************************************/

static int mic_test(void)
{
  int n;
  int i;
  float rms_l;
  float rms_r;

  audio_init();

  n = audio_capture(MIC_N_SAMPLES);

  rms_l = compute_rms(g_mic_left, n);
  rms_r = compute_rms(g_mic_right, n);

  syslog(LOG_INFO,
         "[mic] RMS: left=%.1f  right=%.1f\n",
         rms_l, rms_r);

  syslog(LOG_INFO, "[mic] first %d samples (L, R):\n",
         n < 8 ? n : 8);

  for (i = 0; i < 8 && i < n; i++)
    {
      syslog(LOG_INFO, "  [%d] L=%6d  R=%6d\n",
             i, g_mic_left[i], g_mic_right[i]);
    }

  audio_deinit();
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mic_locate_process
 *
 * Description:
 *   Process captured g_mic_left/right[0..n-1] to estimate sound direction.
 *   Called by bk7258_mic_locate() after capture.
 *
 *   Algorithm:
 *     a) Remove DC offset (subtract per-channel mean).
 *     b) Compute RMS on original signal for energy gate.
 *     c) Pre-emphasis whitening: Ld[i] = L[i] - L[i-1] (first-order HP).
 *     d) Cross-correlation on whitened signal, lag d ∈ [-D, +D].
 *     e) Parabolic interpolation around peak for sub-sample τ.
 *     f) Map: |τ| < TAU_DEAD → center; else ±22 by sign.
 *
 ****************************************************************************/

int mic_locate_process(int n, int *out_tau_q8)
{
  int i;
  int d;
  int best_d = 0;
  int32_t mean_l = 0;
  int32_t mean_r = 0;
  int64_t energy_l = 0;
  int64_t energy_r = 0;
  int16_t rms_l;
  int16_t rms_r;
  int32_t best_corr;
  int32_t corr_prev;
  int32_t corr_best;
  int32_t corr_next;
  int32_t tau_q8;
  int dx;
  int nd; /* length after differentiation */

  /* Scratch buffers for whitened signal (on stack, 4096*2*2 = 16KB) */

  static int16_t g_ld[MIC_N_SAMPLES];
  static int16_t g_rd[MIC_N_SAMPLES];

  if (n < 64)
    {
      syslog(LOG_ERR, "[hear] capture too short: %d\n", n);
      if (out_tau_q8)
        {
          *out_tau_q8 = 0;
        }

      return DX_CENTER;
    }

  /* a) Remove DC offset — compute mean then subtract */

  for (i = 0; i < n; i++)
    {
      mean_l += g_mic_left[i];
      mean_r += g_mic_right[i];
    }

  mean_l /= n;
  mean_r /= n;

  for (i = 0; i < n; i++)
    {
      g_mic_left[i]  = (int16_t)(g_mic_left[i]  - mean_l);
      g_mic_right[i] = (int16_t)(g_mic_right[i] - mean_r);
    }

  /* b) Compute RMS on original (post-DC) signal for energy gate.
   *    Use int64_t to prevent overflow on loud signals.
   */

  for (i = 0; i < n; i++)
    {
      int32_t sl = g_mic_left[i];
      int32_t sr = g_mic_right[i];

      energy_l += (int64_t)sl * sl;
      energy_r += (int64_t)sr * sr;
    }

  /* Integer RMS: sqrt(energy/n).  Use shift-approx for speed. */

  {
    int32_t vl = (int32_t)(energy_l / n);
    int32_t vr = (int32_t)(energy_r / n);
    int16_t rl = 0;
    int16_t rr = 0;
    int bit;

    for (bit = 15; bit >= 0; bit--)
      {
        int32_t t;

        t = rl + (1 << bit);
        if (t * t <= vl)
          {
            rl = (int16_t)t;
          }

        t = rr + (1 << bit);
        if (t * t <= vr)
          {
            rr = (int16_t)t;
          }
      }

    rms_l = rl;
    rms_r = rr;
  }

  syslog(LOG_INFO,
         "[hear] RMS: L=%d R=%d (gate=%d)\n",
         rms_l, rms_r, RMS_GATE);

  /* Validity: BOTH channels must exceed threshold.
   * If either is too low the cross-correlation is meaningless
   * (e.g. one mic dead or signal off-axis) → report center.
   */

  if (rms_l < RMS_GATE || rms_r < RMS_GATE)
    {
      syslog(LOG_INFO,
             "[hear] signal insufficient (L=%d R=%d) → center\n",
             rms_l, rms_r);
      if (out_tau_q8)
        {
          *out_tau_q8 = 0;
        }

      return DX_CENTER;
    }

  /* c) Pre-emphasis whitening: first-order difference high-pass.
   *    Ld[i] = L[i] - L[i-1],  Rd[i] = R[i] - R[i-1]  (i from 1)
   *    Suppresses low-freq / common-mode, sharpens transient peaks.
   *    Cross-correlation uses whitened signal; RMS gate uses original.
   */

  nd = n - 1;
  for (i = 1; i < n; i++)
    {
      g_ld[i - 1] = (int16_t)(g_mic_left[i]  - g_mic_left[i  - 1]);
      g_rd[i - 1] = (int16_t)(g_mic_right[i] - g_mic_right[i - 1]);
    }

  /* d) Cross-correlation on whitened signal,
   *    lag d ∈ [-CORR_LAG_MAX, +CORR_LAG_MAX]
   *    acc is int64 to prevent overflow on loud signals.
   *    No per-frame energy normalisation (constant across lags,
   *    doesn't affect argmax, and was causing overflow).
   */

  best_corr = 0;
  best_d = 0;

  for (d = -CORR_LAG_MAX; d <= CORR_LAG_MAX; d++)
    {
      int64_t acc = 0;
      int start;
      int end;
      int idx;

      /* Valid overlap: i ∈ [max(0,d), min(nd, nd+d)) */

      start = (d > 0) ? d : 0;
      end   = (d > 0) ? nd : nd + d;

      for (idx = start; idx < end; idx++)
        {
          acc += (int64_t)g_ld[idx] *
                 (int64_t)g_rd[idx - d];
        }

      if (d == -CORR_LAG_MAX || acc > best_corr)
        {
          best_corr = acc;
          best_d = d;
        }
    }

  syslog(LOG_INFO,
         "[hear] peak lag d*=%d\n",
         best_d);

  /* e) Parabolic interpolation: fit parabola to (d*-1, d*, d*+1)
   *    τ_frac = (c[0] - c[2]) / (2 * (c[0] - 2*c[1] + c[2]))
   *    Clamp τ_frac to [-1, +1] to reject wild extrapolation.
   *    If denominator ≈ 0 or result out of range, fall back to d*.
   *    Result in Q8 fixed-point.
   */

  {
    int64_t c[3]; /* c[0]=corr(d*-1), c[1]=corr(d*), c[2]=corr(d*+1) */
    int64_t num;
    int64_t den;
    int64_t frac_q8;

    for (i = 0; i < 3; i++)
      {
        int dd = best_d - 1 + i;
        int64_t acc = 0;
        int start2;
        int end2;
        int idx2;

        start2 = (dd > 0) ? dd : 0;
        end2   = (dd > 0) ? nd : nd + dd;

        for (idx2 = start2; idx2 < end2; idx2++)
          {
            acc += (int64_t)g_ld[idx2] *
                   (int64_t)g_rd[idx2 - dd];
          }

        c[i] = acc;
      }

    corr_prev = (int32_t)c[0];
    corr_best = (int32_t)c[1];
    corr_next = (int32_t)c[2];

    /* Parabolic interpolation for sub-sample estimate */

    num = c[0] - c[2];               /* numerator */
    den = c[0] - 2 * c[1] + c[2];   /* denominator (×2 already in formula) */

    if (den != 0 && (den > 0 ? den : -den) > (num > 0 ? num : -num) / 4)
      {
        /* frac_q8 = (num << 8) / (2 * den), clamped to [-256, +256] */

        frac_q8 = (num << 8) / (2 * den);

        if (frac_q8 < -256)
          {
            frac_q8 = -256;
          }

        if (frac_q8 > 256)
          {
            frac_q8 = 256;
          }

        tau_q8 = (int32_t)best_d * 256 + (int32_t)frac_q8;
      }
    else
      {
        /* Flat top or degenerate — use integer lag directly */

        tau_q8 = (int32_t)best_d * 256;
      }
  }

  syslog(LOG_INFO,
         "[hear] tau_q8=%ld (%.2f samples)\n",
         (long)tau_q8, (double)tau_q8 / 256.0);

  /* f) Map to direction */

  if (tau_q8 < -TAU_DEAD_Q8)
    {
      dx = DX_LEFT;
    }
  else if (tau_q8 > TAU_DEAD_Q8)
    {
      dx = DX_RIGHT;
    }
  else
    {
      dx = DX_CENTER;
    }

  syslog(LOG_INFO,
         "[hear] direction: %s (dx=%d)\n",
         dx < 0 ? "LEFT" : dx > 0 ? "RIGHT" : "CENTER",
         dx);

  if (out_tau_q8)
    {
      *out_tau_q8 = (int)tau_q8;
    }

  return dx;
}

/****************************************************************************
 * Name: bk7258_mic_locate
 *
 * Description:
 *   Full locate cycle: audio_init → capture → process → audio_deinit.
 *   For single-shot use.  For continuous loop, use audio_init/capture/
 *   mic_locate_process/audio_deinit separately.
 *
 ****************************************************************************/

int bk7258_mic_locate(int *out_tau_q8)
{
  int n;
  int dx;

  audio_init();
  n = audio_capture(MIC_N_SAMPLES);
  dx = mic_locate_process(n, out_tau_q8);
  audio_deinit();

  return dx;
}

/****************************************************************************
 * Name: bk7258_mic_energy
 *
 * Description:
 *   Quick RMS of current g_mic_left/right buffers (already captured).
 *   Returns max(L_rms, R_rms).  Used for onset detection before
 *   running full locate.
 *
 ****************************************************************************/

int bk7258_mic_energy(int n)
{
  int i;
  int64_t energy_l = 0;
  int64_t energy_r = 0;
  int32_t vl;
  int32_t vr;
  int16_t rms = 0;
  int bit;

  if (n < 16)
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      int32_t sl = g_mic_left[i];
      int32_t sr = g_mic_right[i];

      energy_l += (int64_t)sl * sl;
      energy_r += (int64_t)sr * sr;
    }

  vl = (int32_t)(energy_l / n);
  vr = (int32_t)(energy_r / n);

  /* Return max RMS */

  if (vr > vl)
    {
      vl = vr;
    }

  for (bit = 15; bit >= 0; bit--)
    {
      int16_t t = rms + (1 << bit);

      if ((int32_t)t * t <= vl)
        {
          rms = t;
        }
    }

  return rms;
}

/****************************************************************************
 * Name: bk7258_mic_main
 *
 * Description:
 *   NSH command: lcdtest mic — ADC dual-channel capture + RMS dump.
 *
 ****************************************************************************/

int bk7258_mic_main(int argc, char *argv[])
{
  return mic_test();
}
