/****************************************************************************
 * apps/examples/kws/kws_main.c
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
 * Offline keyword spotting — CLI.
 *
 *   kws enroll <name>   record ~1 s, store as a template for <name>
 *   kws run             record ~1 s, match against all templates
 *   kws list            list enrolled templates
 *   kws clear           erase all templates
 *   kws thresh [v]      show / set the DTW rejection threshold
 *
 * Speaker-dependent: you enrol your own voice, then it recognises it.
 * Fully offline, self-authored MFCC + DTW (Apache-2.0, no external libs).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <arch/board/bk7258_audio.h>

#include "kws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Discard the first 100 ms of every capture (analog start-up transient). */

#define KWS_DISCARD_MS   100

/* Default DTW rejection threshold (path-normalised squared-Euclid).
 * Calibrate on-device with "kws run" + "kws thresh <v>": read the printed
 * distances for correct words vs. wrong words and set the cut in between.
 */

#define KWS_DEFAULT_THRESH 11.0f

/* Accept only if the best distance also beats the runner-up by this ratio
 * (guards against "sounds a bit like everything").  1.0 disables the test.
 */

#define KWS_MARGIN_RATIO   0.88f

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct kws_template_s g_db[KWS_MAX_TEMPLATES];
static int                   g_ntpl;
static struct kws_feat_s     g_query;
static float                 g_thresh = KWS_DEFAULT_THRESH;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: record_utterance
 *
 * Description:
 *   Record ~1 s of 16 kHz mono PCM and extract its MFCC feature sequence
 *   into g_query.  Returns the number of frames, or 0 on no-speech / error.
 *
 ****************************************************************************/

static int record_utterance(void)
{
  const int16_t *pcm;
  int npcm;
  int nf;

  printf("[kws] speak the command now ...\n");
  fflush(stdout);

  npcm = bk7258_mic_record_16k(KWS_DISCARD_MS);
  pcm  = bk7258_mic_pcm();

  if (npcm <= 0)
    {
      printf("[kws] capture failed (mic?)\n");
      return 0;
    }

  nf = kws_extract(pcm, npcm, &g_query);

  if (nf == 0)
    {
      printf("[kws] no speech detected — speak louder / closer, retry\n");
      return 0;
    }

  printf("[kws] captured %d samples, %d MFCC frames\n", npcm, nf);
  return nf;
}

/****************************************************************************
 * Name: kws_enroll
 ****************************************************************************/

static int kws_enroll(const char *name)
{
  struct kws_template_s *t;
  int nf;
  int f;
  int k;

  if (g_ntpl >= KWS_MAX_TEMPLATES)
    {
      printf("[kws] template store full (%d/%d) — 'kws clear' first\n",
             g_ntpl, KWS_MAX_TEMPLATES);
      return -1;
    }

  nf = record_utterance();
  if (nf == 0)
    {
      return -1;
    }

  t = &g_db[g_ntpl];
  strncpy(t->name, name, KWS_NAME_LEN - 1);
  t->name[KWS_NAME_LEN - 1] = '\0';
  t->nframes = nf;

  /* Quantise float MFCC -> int16 (scaled) for compact storage */

  for (f = 0; f < nf; f++)
    {
      for (k = 0; k < KWS_NCEP; k++)
        {
          float v = g_query.mfcc[f][k] * KWS_MFCC_SCALE;
          int   q = (int)lroundf(v);

          if (q > 32767)
            {
              q = 32767;
            }

          if (q < -32768)
            {
              q = -32768;
            }

          t->mfcc[f][k] = (int16_t)q;
        }
    }

  g_ntpl++;
  printf("[kws] enrolled \"%s\" (slot %d, %d frames). total templates: %d\n",
         t->name, g_ntpl - 1, nf, g_ntpl);
  return 0;
}

/****************************************************************************
 * Name: kws_run
 ****************************************************************************/

static int kws_run(void)
{
  float dist[KWS_MAX_TEMPLATES];
  int nf;
  int i;
  int best = -1;
  float best_d = 0.0f;
  float other_d = -1.0f;   /* closest template of a DIFFERENT keyword */

  if (g_ntpl == 0)
    {
      printf("[kws] no templates — 'kws enroll <name>' first\n");
      return -1;
    }

  nf = record_utterance();
  if (nf == 0)
    {
      return -1;
    }

  printf("[kws] distances (lower = closer):\n");

  for (i = 0; i < g_ntpl; i++)
    {
      dist[i] = kws_dtw(&g_query.mfcc[0][0], g_query.nframes,
                        &g_db[i].mfcc[0][0], g_db[i].nframes,
                        1.0f / KWS_MFCC_SCALE);

      printf("    [%d] %-14s %.2f\n", i, g_db[i].name, (double)dist[i]);

      if (best < 0 || dist[i] < best_d)
        {
          best   = i;
          best_d = dist[i];
        }
    }

  /* Confusion margin: compare the winner against the closest template of a
   * DIFFERENT keyword — not the immediate runner-up, which may be another
   * take of the same word (that is high confidence, not ambiguity).
   */

  for (i = 0; i < g_ntpl; i++)
    {
      if (strcmp(g_db[i].name, g_db[best].name) == 0)
        {
          continue;
        }

      if (other_d < 0.0f || dist[i] < other_d)
        {
          other_d = dist[i];
        }
    }

  /* Accept if below absolute threshold AND clearly ahead of the nearest
   * other keyword (or there is only one keyword enrolled).
   */

  if (best_d <= g_thresh &&
      (other_d < 0.0f || best_d <= KWS_MARGIN_RATIO * other_d))
    {
      printf("[kws] MATCH: \"%s\"  (dist=%.2f, other=%.2f, thresh=%.2f)\n",
             g_db[best].name, (double)best_d,
             (double)(other_d < 0.0f ? 0.0f : other_d), (double)g_thresh);
    }
  else
    {
      printf("[kws] REJECTED (best=\"%s\" dist=%.2f, other=%.2f, thresh=%.2f)\n",
             g_db[best].name, (double)best_d,
             (double)(other_d < 0.0f ? 0.0f : other_d), (double)g_thresh);
    }

  return 0;
}

/****************************************************************************
 * Name: kws_list
 ****************************************************************************/

static int kws_list(void)
{
  int i;

  if (g_ntpl == 0)
    {
      printf("[kws] no templates enrolled\n");
      return 0;
    }

  printf("[kws] %d template(s):\n", g_ntpl);
  for (i = 0; i < g_ntpl; i++)
    {
      printf("    [%d] %-14s %d frames\n",
             i, g_db[i].name, g_db[i].nframes);
    }

  return 0;
}

/****************************************************************************
 * Name: kws_clear
 ****************************************************************************/

static int kws_clear(void)
{
  g_ntpl = 0;
  memset(g_db, 0, sizeof(g_db));
  printf("[kws] all templates cleared\n");
  return 0;
}

/****************************************************************************
 * Name: usage
 ****************************************************************************/

static int usage(void)
{
  printf("Usage: kws <command>\n");
  printf("  enroll <name>  record ~1s and store a template for <name>\n");
  printf("  run            record ~1s and match against templates\n");
  printf("  list           list enrolled templates\n");
  printf("  clear          erase all templates\n");
  printf("  thresh [v]     show or set the DTW rejection threshold\n");
  return 1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      return usage();
    }

  if (strcmp(argv[1], "enroll") == 0)
    {
      if (argc < 3)
        {
          printf("[kws] usage: kws enroll <name>\n");
          return 1;
        }

      return kws_enroll(argv[2]);
    }
  else if (strcmp(argv[1], "run") == 0)
    {
      return kws_run();
    }
  else if (strcmp(argv[1], "list") == 0)
    {
      return kws_list();
    }
  else if (strcmp(argv[1], "clear") == 0)
    {
      return kws_clear();
    }
  else if (strcmp(argv[1], "thresh") == 0)
    {
      if (argc >= 3)
        {
          g_thresh = strtof(argv[2], NULL);
          printf("[kws] threshold set to %.2f\n", (double)g_thresh);
        }
      else
        {
          printf("[kws] threshold = %.2f\n", (double)g_thresh);
        }

      return 0;
    }

  return usage();
}
