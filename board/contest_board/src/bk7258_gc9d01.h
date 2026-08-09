/****************************************************************************
 * arch/arm/src/bk7258/bk7258_gc9d01.h
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

#ifndef __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_GC9D01_H
#define __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_GC9D01_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_lcdtest_main
 *
 * Description:
 *   NSH command "lcdtest" — staged GC9D01 LCD bring-up.
 *   Stage A: backlight on (P25 high).
 *   Stage B: RST(P29) reset + GC9D01 init sequence.
 *   Stage C: fill a 40x40 red square at (60,60).
 *
 ****************************************************************************/

int bk7258_lcdtest_main(int argc, char *argv[]);

#endif /* __VENDOR_BEKEN_BK7258_DEVKIT_BK7258_GC9D01_H */
