/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258_bringup.c
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

#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/fs/fs.h>

#include "bk7258-devkit.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bringup
 *
 * Description:
 *   Board peripheral initialization.  Registers SPI/QSPI, LCD, framebuffer
 *   and other drivers here as they are implemented.
 *
 ****************************************************************************/

int bk7258_bringup(void)
{
  int ret = OK;

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

  /* GC9D01 dual-eye LCD is driven via the NSH command "lcdtest"
   * (CONFIG_EXAMPLES_LCDTEST).  It is intentionally not called here —
   * running the bit-bang bring-up at boot would block startup.
   */

  UNUSED(ret);
  return OK;
}
