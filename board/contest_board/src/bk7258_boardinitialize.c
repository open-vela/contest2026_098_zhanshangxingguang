/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_boardinitialize.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/board.h>

#include "arm_internal.h"
#include "bk7258-devkit.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_boardinitialize
 *
 * Description:
 *   Called early by __start (bk7258_start.c) for low-level board init.
 *   Nothing extra is needed here (clock/console are already set up before
 *   __start).
 *
 ****************************************************************************/

void arm_boardinitialize(void)
{
  /* M2/M3: initialize board GPIO, LCD backlight/reset pins, etc. here
   * (board_late_initialize is defined in bk7258_appinit.c).
   */
}
