/****************************************************************************
 * apps/examples/kws/kws_mfcc.c
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
 * MFCC frontend — fully self-authored (no CMSIS-DSP, no external libs).
 *
 *   pre-emphasis -> Hamming window -> radix-2 FFT -> power spectrum
 *   -> Mel filterbank -> log -> DCT-II -> 13 cepstra
 *   -> energy-based endpoint detection (VAD) -> per-utterance CMVN.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <string.h>
#include <stdbool.h>

#include "kws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#define KWS_PI          3.14159265358979f
#define KWS_LOG_EPS     1.0e-6f        /* floor inside log()              */
#define KWS_VAD_FRAC    0.30f          /* proportional part of threshold  */
#define KWS_VAD_ABS_MIN 2.0f           /* min log-energy above noise floor
                                        * (speech is ~6 above noise, so a
                                        * fixed floor gives SNR-robust,
                                        * consistent endpointing)          */
#define KWS_VAD_MARGIN  3              /* frames padded around speech     */

/****************************************************************************
 * Private Data — precomputed tables (built once)
 ****************************************************************************/

static bool  g_tables_ready;

static float g_hamming[KWS_FRAME_LEN];
static float g_tw_cos[KWS_FFT_HALF];   /* cos(2*pi*k/N)                   */
static float g_tw_sin[KWS_FFT_HALF];   /* -sin(2*pi*k/N) (forward FFT)    */
static int   g_mel_bin[KWS_NMEL + 2];  /* FFT bin edges of Mel filters    */
static float g_dct[KWS_NCEP][KWS_NMEL];

/* Work buffers (reused per call — not re-entrant, single-threaded use) */

static float g_re[KWS_FFT];
static float g_im[KWS_FFT];
static float g_logE[KWS_MAX_FRAMES];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mel_from_hz / hz_from_mel
 ****************************************************************************/

static float mel_from_hz(float hz)
{
  return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float hz_from_mel(float mel)
{
  return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

/****************************************************************************
 * Name: kws_build_tables
 *
 * Description:
 *   Precompute the Hamming window, FFT twiddle factors, Mel filterbank
 *   bin edges and the DCT-II basis.  Idempotent.
 *
 ****************************************************************************/

static void kws_build_tables(void)
{
  int i;
  int m;
  int n;
  float mlo;
  float mhi;
  float mstep;

  if (g_tables_ready)
    {
      return;
    }

  /* Hamming window over the analysis frame */

  for (i = 0; i < KWS_FRAME_LEN; i++)
    {
      g_hamming[i] = 0.54f - 0.46f *
                     cosf(2.0f * KWS_PI * (float)i / (float)(KWS_FRAME_LEN - 1));
    }

  /* Forward-FFT twiddles: W_N^k = exp(-j 2*pi k / N) */

  for (i = 0; i < KWS_FFT_HALF; i++)
    {
      float ang = 2.0f * KWS_PI * (float)i / (float)KWS_FFT;
      g_tw_cos[i] =  cosf(ang);
      g_tw_sin[i] = -sinf(ang);
    }

  /* Mel filterbank edges: NMEL+2 equally-spaced points in the Mel scale,
   * mapped back to Hz, then to FFT bins.
   */

  mlo = mel_from_hz(KWS_MEL_LOW_HZ);
  mhi = mel_from_hz(KWS_MEL_HIGH_HZ);
  mstep = (mhi - mlo) / (float)(KWS_NMEL + 1);

  for (i = 0; i < KWS_NMEL + 2; i++)
    {
      float hz = hz_from_mel(mlo + mstep * (float)i);
      int   bin = (int)floorf((float)(KWS_FFT + 1) * hz / (float)KWS_SR);

      if (bin < 0)
        {
          bin = 0;
        }

      if (bin > KWS_FFT_HALF)
        {
          bin = KWS_FFT_HALF;
        }

      g_mel_bin[i] = bin;
    }

  /* DCT-II basis: c[n] = sum_m logmel[m] * cos(pi*(m+0.5)*n / NMEL) */

  for (n = 0; n < KWS_NCEP; n++)
    {
      for (m = 0; m < KWS_NMEL; m++)
        {
          g_dct[n][m] = cosf(KWS_PI * ((float)m + 0.5f) *
                             (float)n / (float)KWS_NMEL);
        }
    }

  g_tables_ready = true;
}

/****************************************************************************
 * Name: fft_radix2
 *
 * Description:
 *   In-place iterative radix-2 decimation-in-time FFT of size KWS_FFT.
 *   re[]/im[] hold the complex input/output.  Uses the precomputed
 *   twiddle table (forward transform).
 *
 ****************************************************************************/

static void fft_radix2(float *re, float *im)
{
  const int n = KWS_FFT;
  int i;
  int j;
  int len;

  /* Bit-reversal permutation */

  j = 0;
  for (i = 1; i < n; i++)
    {
      int bit = n >> 1;

      for (; j & bit; bit >>= 1)
        {
          j ^= bit;
        }

      j ^= bit;

      if (i < j)
        {
          float tr = re[i]; re[i] = re[j]; re[j] = tr;
          float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

  /* Butterflies */

  for (len = 2; len <= n; len <<= 1)
    {
      int half = len >> 1;
      int step = n / len;      /* twiddle stride into g_tw_* */

      for (i = 0; i < n; i += len)
        {
          int k = 0;

          for (j = 0; j < half; j++, k += step)
            {
              float wr = g_tw_cos[k];
              float wi = g_tw_sin[k];
              float ar = re[i + j + half];
              float ai = im[i + j + half];
              float tr = ar * wr - ai * wi;
              float ti = ar * wi + ai * wr;

              re[i + j + half] = re[i + j] - tr;
              im[i + j + half] = im[i + j] - ti;
              re[i + j]       += tr;
              im[i + j]       += ti;
            }
        }
    }
}

/****************************************************************************
 * Name: frame_to_mfcc
 *
 * Description:
 *   Turn one windowed spectrum (already in g_re/g_im after FFT) into
 *   KWS_NCEP cepstral coefficients written to out[].
 *
 ****************************************************************************/

static void spectrum_to_mfcc(float *out)
{
  float power[KWS_FFT_HALF + 1];
  float logmel[KWS_NMEL];
  int k;
  int m;
  int n;

  /* Power spectrum (DC..Nyquist) */

  for (k = 0; k <= KWS_FFT_HALF; k++)
    {
      power[k] = g_re[k] * g_re[k] + g_im[k] * g_im[k];
    }

  /* Triangular Mel filterbank -> log energy */

  for (m = 0; m < KWS_NMEL; m++)
    {
      int left   = g_mel_bin[m];
      int center = g_mel_bin[m + 1];
      int right  = g_mel_bin[m + 2];
      float acc  = 0.0f;

      for (k = left; k < center; k++)
        {
          if (center > left)
            {
              acc += power[k] * ((float)(k - left) / (float)(center - left));
            }
        }

      for (k = center; k <= right; k++)
        {
          if (right > center)
            {
              acc += power[k] * ((float)(right - k) / (float)(right - center));
            }
        }

      logmel[m] = logf(acc + KWS_LOG_EPS);
    }

  /* DCT-II -> cepstra */

  for (n = 0; n < KWS_NCEP; n++)
    {
      float c = 0.0f;

      for (m = 0; m < KWS_NMEL; m++)
        {
          c += logmel[m] * g_dct[n][m];
        }

      out[n] = c;
    }
}

/****************************************************************************
 * Name: cmvn
 *
 * Description:
 *   Cepstral mean & variance normalisation over `nf` frames of `feat`,
 *   in place: each coefficient is zero-mean, unit-variance across the
 *   utterance.  Makes DTW robust to gain / channel offsets.
 *
 ****************************************************************************/

static void cmvn(float feat[][KWS_NCEP], int nf)
{
  int k;
  int i;

  for (k = 0; k < KWS_NCEP; k++)
    {
      float mean = 0.0f;
      float var  = 0.0f;
      float sd;

      for (i = 0; i < nf; i++)
        {
          mean += feat[i][k];
        }

      mean /= (float)nf;

      for (i = 0; i < nf; i++)
        {
          float d = feat[i][k] - mean;
          var += d * d;
        }

      var /= (float)nf;
      sd = sqrtf(var) + KWS_LOG_EPS;

      for (i = 0; i < nf; i++)
        {
          feat[i][k] = (feat[i][k] - mean) / sd;
        }
    }
}

/****************************************************************************
 * Name: window_frame
 *
 * Description:
 *   Load one analysis frame at sample offset `start` into g_re/g_im:
 *   pre-emphasis + Hamming window, zero-padded to KWS_FFT (imag = 0).
 *   Returns the frame energy (sum of squared windowed samples).
 *
 ****************************************************************************/

static float window_frame(const int16_t *pcm, int start)
{
  float e = 0.0f;
  int i;

  for (i = 0; i < KWS_FRAME_LEN; i++)
    {
      int   idx  = start + i;
      float prev = (idx > 0) ? (float)pcm[idx - 1] : (float)pcm[idx];
      float s    = (float)pcm[idx] - KWS_PREEMPH * prev;
      float w    = g_hamming[i] * s;

      g_re[i] = w;
      g_im[i] = 0.0f;
      e += w * w;
    }

  for (i = KWS_FRAME_LEN; i < KWS_FFT; i++)
    {
      g_re[i] = 0.0f;
      g_im[i] = 0.0f;
    }

  return e;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kws_extract
 *
 * Description:
 *   Two-pass extraction to keep RAM low (no all-frames MFCC buffer):
 *     Pass 1 - per-frame log-energy (windowing only, no FFT) for VAD.
 *     Pass 2 - MFCC (with FFT) computed only for the trimmed speech
 *              segment, written straight into out->mfcc, then CMVN.
 *
 ****************************************************************************/

int kws_extract(const int16_t *pcm, int npcm, struct kws_feat_s *out)
{
  int nf = 0;
  int f;
  float emin;
  float emax;
  float thr;
  int first;
  int last;
  int seg;

  if (pcm == NULL || out == NULL || npcm < KWS_FRAME_LEN)
    {
      return 0;
    }

  kws_build_tables();

  /* --- Pass 1: per-frame log-energy for endpoint detection --- */

  for (f = 0; f < KWS_MAX_FRAMES; f++)
    {
      int start = f * KWS_HOP;

      if (start + KWS_FRAME_LEN > npcm)
        {
          break;
        }

      g_logE[f] = logf(window_frame(pcm, start) + 1.0f);
      nf++;
    }

  if (nf < KWS_MIN_FRAMES)
    {
      return 0;
    }

  /* --- Energy-based endpoint detection --- */

  emin = g_logE[0];
  emax = g_logE[0];

  for (f = 1; f < nf; f++)
    {
      if (g_logE[f] < emin)
        {
          emin = g_logE[f];
        }

      if (g_logE[f] > emax)
        {
          emax = g_logE[f];
        }
    }

  {
    float margin = KWS_VAD_FRAC * (emax - emin);

    if (margin < KWS_VAD_ABS_MIN)
      {
        margin = KWS_VAD_ABS_MIN;
      }

    thr = emin + margin;
  }

  first = -1;
  last  = -1;

  for (f = 0; f < nf; f++)
    {
      if (g_logE[f] >= thr)
        {
          if (first < 0)
            {
              first = f;
            }

          last = f;
        }
    }

  if (first < 0 || (last - first + 1) < KWS_MIN_FRAMES)
    {
      return 0;   /* no coherent speech segment */
    }

  /* Pad the segment a little, then clamp */

  first -= KWS_VAD_MARGIN;
  last  += KWS_VAD_MARGIN;

  if (first < 0)
    {
      first = 0;
    }

  if (last > nf - 1)
    {
      last = nf - 1;
    }

  seg = last - first + 1;   /* trimmed segment length (frames) */

  /* --- Pass 2: MFCC time-normalised to KWS_FIXED_FRAMES ---
   * Linearly resample the trimmed segment [first..last] to a fixed number
   * of output frames: output frame k takes source frame
   *   first + round(k * (seg-1) / (FIXED-1)).
   * This makes every utterance the same length so DTW compares phonetic
   * content, not speaking rate / window timing.  Only KWS_FIXED_FRAMES
   * FFTs are computed, written straight into out->mfcc.
   */

  for (f = 0; f < KWS_FIXED_FRAMES; f++)
    {
      int src = first;

      if (KWS_FIXED_FRAMES > 1)
        {
          src += (f * (seg - 1) + (KWS_FIXED_FRAMES - 1) / 2) /
                 (KWS_FIXED_FRAMES - 1);
        }

      window_frame(pcm, src * KWS_HOP);
      fft_radix2(g_re, g_im);
      spectrum_to_mfcc(out->mfcc[f]);
    }

  out->nframes = KWS_FIXED_FRAMES;

  /* Per-utterance CMVN */

  cmvn(out->mfcc, KWS_FIXED_FRAMES);

  return KWS_FIXED_FRAMES;
}
