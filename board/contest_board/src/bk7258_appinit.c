/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_appinit.c
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

#include <sys/types.h>
#include <syslog.h>

#include <nuttx/board.h>

#include "bk7258-devkit.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  Called by the system
 *   after the NuttX kernel has been booted and the C libraries are ready.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#ifndef CONFIG_BOARD_LATE_INITIALIZE
  return bk7258_bringup();
#else
  UNUSED(arg);
  return OK;
#endif
}

#ifdef CONFIG_BOARD_LATE_INITIALIZE

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called when CONFIG_BOARD_LATE_INITIALIZE is enabled to run board
 *   initialization.
 *
 ****************************************************************************/

void board_late_initialize(void)
{
  bk7258_bringup();
}
#endif
