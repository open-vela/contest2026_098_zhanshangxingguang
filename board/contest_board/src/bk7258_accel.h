/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_accel.h
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

#ifndef __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_ACCEL_H
#define __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_ACCEL_H

#include <nuttx/config.h>
#include <stdbool.h>

int bk7258_accel_main(int argc, char *argv[]);

int  bk7258_accel_probe(void);              /* init + find addr + config click */
void bk7258_accel_sample(bool *tap, bool *flat);  /* one poll: tap event + flat pose */

#endif /* __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_ACCEL_H */
