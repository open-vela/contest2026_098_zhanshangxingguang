/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_audio.h
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

#ifndef __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_AUDIO_H
#define __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int bk7258_mic_main(int argc, char *argv[]);
int bk7258_mic_locate(int *out_tau_q8);

/* Continuous-loop helpers — audio_init/capture/deinit managed by caller */

void bk7258_mic_set_quiet(bool q);

void audio_init(void);
int  audio_capture(int n);
void audio_deinit(void);
int  mic_locate_process(int n, int *out_tau_q8);
int  bk7258_mic_energy(int n);

/* DAC playback (M2c speaker) */

void audio_dac_init(int samp_rate);
void audio_dac_deinit(void);
int  audio_beep(int freq_hz, int ms, int pa_gpio);
int  audio_play_melody(const uint16_t *freqs, const uint16_t *durs_ms,
                       int n, int pa_gpio);

#endif /* __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_AUDIO_H */
