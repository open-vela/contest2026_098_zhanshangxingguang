/****************************************************************************
 * vendor/beken/boards/bk7258/bk7258-devkit/src/bk7258-devkit.h
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

#ifndef __BOARDS_BK7258_DEVKIT_SRC_BK7258_DEVKIT_H
#define __BOARDS_BK7258_DEVKIT_SRC_BK7258_DEVKIT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bringup
 *
 * Description:
 *   Board peripheral initialization entry (see bk7258_bringup.c).
 *
 ****************************************************************************/

int bk7258_bringup(void);

/****************************************************************************
 * Name: bk7258_lcd_initialize (M3)
 *
 * Description:
 *   TODO(M3): initialize the QSPI LCD and register the framebuffer.
 *
 ****************************************************************************/

/* int bk7258_lcd_initialize(void); */

#endif /* __BOARDS_BK7258_DEVKIT_SRC_BK7258_DEVKIT_H */
