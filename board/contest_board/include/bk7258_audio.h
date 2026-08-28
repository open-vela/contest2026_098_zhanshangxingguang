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

#endif /* __BOARD_CONTEST_BOARD_INCLUDE_BK7258_AUDIO_H */
