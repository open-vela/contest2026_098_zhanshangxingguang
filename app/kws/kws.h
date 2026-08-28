/****************************************************************************
 * apps/examples/kws/kws.h
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
 * Offline keyword spotting (KWS) — self-contained MFCC + DTW.
 *
 * Fully self-authored, zero third-party runtime: microphone PCM ->
 * MFCC feature sequence -> Dynamic Time Warping template matching.
 * No neural network, no proprietary blob — Apache-2.0 clean.
 *
 * Pipeline:
 *   16 kHz mono PCM (board mic) -> pre-emphasis -> framing + Hamming
 *   -> radix-2 FFT -> Mel filterbank -> log -> DCT -> 13-D MFCC
 *   -> per-utterance CMVN, with energy-based endpoint detection (VAD).
 *   Enrol templates per keyword; at run time pick the min-DTW-distance
 *   template and accept only if below a rejection threshold.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_KWS_KWS_H
#define __APPS_EXAMPLES_KWS_KWS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Audio / framing (must match bk7258_mic_record_16k output) */

#define KWS_SR          16000          /* sample rate (Hz)                */
#define KWS_FRAME_LEN   400            /* 25 ms analysis frame            */
#define KWS_HOP         160            /* 10 ms hop                       */
#define KWS_FFT         512            /* FFT size (>= KWS_FRAME_LEN, 2^n)*/
#define KWS_FFT_HALF    (KWS_FFT / 2)  /* usable spectrum bins + DC       */

/* Feature dimensions */

#define KWS_NMEL        26             /* Mel filterbank channels         */
#define KWS_NCEP        13             /* cepstral coefficients kept (c0..)*/
#define KWS_MEL_LOW_HZ  300.0f         /* filterbank low edge             */
#define KWS_MEL_HIGH_HZ 8000.0f        /* filterbank high edge (Nyquist)  */
#define KWS_PREEMPH     0.97f          /* pre-emphasis coefficient        */

/* Raw analysis frame limit.  1 s @ 10 ms hop ~= 98 frames; 100 covers it.
 * This bounds only the per-frame energy pass (endpoint detection); the
 * stored feature sequence is time-normalised to KWS_FIXED_FRAMES below.
 */

#define KWS_MAX_FRAMES  100
#define KWS_MIN_FRAMES  8              /* below this -> "no speech"       */

/* Time normalisation: every utterance's trimmed MFCC sequence is linearly
 * resampled to this fixed number of frames before storing / matching.
 * This removes speaking-rate / window-timing length differences, so DTW
 * compares phonetic content rather than duration (the dominant confusion
 * in a raw variable-length setup).  Kept small (also saves RAM).
 */

#define KWS_FIXED_FRAMES 48

/* Template database (RAM-resident; speaker-dependent enrolment).
 * Templates store MFCC as int16 (scaled by KWS_MFCC_SCALE) to save BSS.
 * 6 slots = e.g. 2-3 command words with a couple of takes each.
 */

#define KWS_MAX_TEMPLATES 6
#define KWS_NAME_LEN      16
#define KWS_MFCC_SCALE    64.0f        /* float MFCC (~z-scores) -> int16 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Feature sequence for one utterance (query side, float).
 * Always KWS_FIXED_FRAMES long after time normalisation.
 */

struct kws_feat_s
{
  int   nframes;
  float mfcc[KWS_FIXED_FRAMES][KWS_NCEP];
};

/* One enrolled template (int16-quantised MFCC to save memory).
 * Always KWS_FIXED_FRAMES long.
 */

struct kws_template_s
{
  char    name[KWS_NAME_LEN];
  int     nframes;
  int16_t mfcc[KWS_FIXED_FRAMES][KWS_NCEP];   /* scaled by KWS_MFCC_SCALE */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* kws_mfcc.c — extract an MFCC feature sequence from mono PCM.
 * Applies pre-emphasis, framing, FFT, Mel, log, DCT, endpoint trim, CMVN.
 * Returns the number of (trimmed) frames written to out->mfcc, or 0 if no
 * speech was detected / input too short.
 */

int kws_extract(const int16_t *pcm, int npcm, struct kws_feat_s *out);

/* kws_dtw.c — DTW distance between a float query sequence and an
 * int16-quantised template.  Returns the path-length-normalised distance
 * (smaller = closer).  tscale converts the int16 template back to float
 * (pass 1.0f / KWS_MFCC_SCALE).
 */

float kws_dtw(const float *query, int nq,
              const int16_t *tpl, int nt, float tscale);

#endif /* __APPS_EXAMPLES_KWS_KWS_H */
