/****************************************************************************
 * app/camera/camera_main.c
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

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <arch/board/bk7258_camera.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * camera_main
 ****************************************************************************/

int main(int argc, char *argv[])
{
  if (argc < 2)
    {
      printf("Usage: camera <command>\n");
      printf("Commands:\n");
      printf("  id     — read GC2145 chip ID\n");
      printf("  io     — DVP pin mux config/deconfig/readback\n");
      printf("  init   — GC2145 RGB565 640x480 init (MCLK stays on)\n");
      printf("  stop   — MCLK off, power off, restore pins\n");
      printf("  buf    — PSRAM framebuf alloc + test pattern verify\n");
      return 1;
    }

  if (strcmp(argv[1], "id") == 0)
    {
      return bk7258_camera_id();
    }
  else if (strcmp(argv[1], "io") == 0)
    {
      return bk7258_camera_io();
    }
  else if (strcmp(argv[1], "init") == 0)
    {
      return bk7258_camera_init();
    }
  else if (strcmp(argv[1], "stop") == 0)
    {
      return bk7258_camera_stop();
    }
  else if (strcmp(argv[1], "buf") == 0)
    {
      return bk7258_camera_buf();
    }
  else
    {
      printf("Unknown command: %s\n", argv[1]);
      return 1;
    }
}
