/****************************************************************************
 * board/contest_board/include/bk7258_audio.h
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
 * Application-facing audio API, exposed as <arch/board/bk7258_audio.h>.
 * The full board-internal driver lives in src/bk7258_audio.c/.h; this
 * header publishes only what user-space apps (e.g. the KWS example) need:
 * a 16 kHz mono PCM recording primitive.
 ****************************************************************************/

#ifndef __BOARD_CONTEST_BOARD_INCLUDE_BK7258_AUDIO_H
#define __BOARD_CONTEST_BOARD_INCLUDE_BK7258_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_mic_record_16k
 *
 * Description:
 *   Record up to 1 s of 16 kHz mono (left-channel) PCM into an internal
 *   buffer.  Powers up the analog front-end, re-programs the ADC to
 *   16 kHz, discards `discard_ms` of start-up transient, poll-captures,
 *   then powers down.  Bounded spin — never hangs the shell.
 *
 * Returns:
 *   Number of samples captured (retrieve them with bk7258_mic_pcm()).
 *
 ****************************************************************************/

int bk7258_mic_record_16k(int discard_ms);

/****************************************************************************
 * Name: bk7258_mic_pcm
 *
 * Description:
 *   Return a pointer to the internal 16 kHz mono PCM buffer filled by the
 *   most recent bk7258_mic_record_16k() call.  Valid for the returned
 *   sample count.
 *
 ****************************************************************************/

const int16_t *bk7258_mic_pcm(void);

/****************************************************************************
 * Name: bk7258_play_pcm16k
 *
 * Description:
 *   Play a 16 kHz mono int16 PCM buffer through the speaker (internal DAC
 *   + HT6873 PA on P50).  Blocks until playback finishes.  For the offline
 *   voice reply (canned greeting played on wake-word recognition).
 *
 ****************************************************************************/

int bk7258_play_pcm16k(const int16_t *pcm, int n);

/****************************************************************************
 * Name: audio_play_melody
 *
 * Description:
 *   Play a short note sequence (freqs[i] Hz / durs_ms[i] ms; freqs[i]=0 =
 *   rest).  pa_gpio = HT6873 PA enable pin (50 here), <0 to skip.
 *   Used as the fallback "answer chime" when no greeting PCM is embedded.
 *
 ****************************************************************************/

int audio_play_melody(const unsigned short *freqs,
                      const unsigned short *durs_ms, int n, int pa_gpio);

#endif /* __BOARD_CONTEST_BOARD_INCLUDE_BK7258_AUDIO_H */
