/****************************************************************************
 * apps/examples/kws/kws_dtw.c
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
 * Dynamic Time Warping (DTW) distance between two MFCC sequences.
 *
 *   Classic DP with a rolling two-row cost array (O(nt) memory).  The
 *   local cost is the squared Euclidean distance over the KWS_NCEP
 *   cepstral coefficients.  The accumulated cost is normalised by the
 *   path length (nq + nt) so utterances of different length compare
 *   fairly.  The query is float; the template is int16-quantised and is
 *   scaled back to float on the fly (tscale = 1 / KWS_MFCC_SCALE).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>

#include "kws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KWS_DTW_INF 1.0e30f

/* Sakoe-Chiba band half-width.  For time-normalised sequences (both nq and
 * nt = KWS_FIXED_FRAMES = 48), the optimal DTW path stays close to the
 * diagonal.  Limiting the search to ±W cells around the diagonal reduces
 * the DP grid from 48×48 = 2304 to 48×(2W+1) = 816 cells (W=8), saving
 * ~65 % compute.  The band is wide enough to absorb residual speaking-rate
 * variation after time normalisation without affecting recognition accuracy.
 */

#define KWS_DTW_BAND_W  8

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline float local_cost(const float *q, const int16_t *t,
                               float tscale)
{
  float acc = 0.0f;
  int k;

  for (k = 0; k < KWS_NCEP; k++)
    {
      float d = q[k] - (float)t[k] * tscale;
      acc += d * d;
    }

  return acc;
}

static inline float min3(float a, float b, float c)
{
  float m = a < b ? a : b;
  return m < c ? m : c;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kws_dtw
 ****************************************************************************/

float kws_dtw(const float *query, int nq,
              const int16_t *tpl, int nt, float tscale)
{
  static float row_a[KWS_MAX_FRAMES];
  static float row_b[KWS_MAX_FRAMES];
  float *prev = row_a;
  float *cur  = row_b;
  int i;
  int j;
  int jmin;
  int jmax;
  int last_jmax;   /* jmax of the previous row, for cur[j-1] boundary */

  if (query == NULL || tpl == NULL ||
      nq < 1 || nt < 1 ||
      nq > KWS_MAX_FRAMES || nt > KWS_MAX_FRAMES)
    {
      return KWS_DTW_INF;
    }

  /* Initialise both rows to INF so out-of-band cells read as INF. */

  for (j = 0; j < nt; j++)
    {
      prev[j] = KWS_DTW_INF;
      cur[j]  = KWS_DTW_INF;
    }

  /* Row i = 0: only the "left" predecessor exists (monotone path).
   * Band: j ∈ [0, min(nt-1, W)].
   */

  jmax = nt - 1;
  if (jmax > KWS_DTW_BAND_W)
    {
      jmax = KWS_DTW_BAND_W;
    }

  for (j = 0; j <= jmax; j++)
    {
      float d = local_cost(&query[0], &tpl[j * KWS_NCEP], tscale);

      prev[j] = (j == 0) ? d : prev[j - 1] + d;
    }

  last_jmax = jmax;

  /* Rows i = 1 .. nq-1, Sakoe-Chiba band ±W around diagonal.
   * For row i the valid column range is [max(0, i-W), min(nt-1, i+W)].
   * Cells outside the band stay at INF (set above) and are never written,
   * so the min3 predecessor look-ups that reach them naturally return INF.
   */

  for (i = 1; i < nq; i++)
    {
      jmin = i - KWS_DTW_BAND_W;
      if (jmin < 0)
        {
          jmin = 0;
        }

      jmax = i + KWS_DTW_BAND_W;
      if (jmax > nt - 1)
        {
          jmax = nt - 1;
        }

      /* Reset cur band region to INF before computing (prev band was
       * already reset on the swap or initialisation).
       */

      for (j = jmin; j <= jmax; j++)
        {
          cur[j] = KWS_DTW_INF;
        }

      for (j = jmin; j <= jmax; j++)
        {
          float d = local_cost(&query[i * KWS_NCEP],
                               &tpl[j * KWS_NCEP], tscale);
          float best;

          if (j == 0)
            {
              best = prev[0];                       /* insertion only */
            }
          else
            {
              best = min3(prev[j], prev[j - 1], cur[j - 1]);
            }

          cur[j] = d + best;
        }

      /* Swap rows */

      {
        float *tmp = prev;
        prev = cur;
        cur  = tmp;
      }

      last_jmax = jmax;
    }

  /* After the final swap the last computed row is in `prev`.
   * The destination cell (nq-1, nt-1) is within the last band.
   */

  return prev[nt - 1] / (float)(nq + nt);
}
