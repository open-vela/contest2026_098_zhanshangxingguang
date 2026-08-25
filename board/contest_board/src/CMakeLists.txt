# ##############################################################################
# vendor/beken/boards/bk7258/bk7258-devkit/src/CMakeLists.txt
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

set(SRCS bk7258_bringup.c bk7258_boardinitialize.c)

if(CONFIG_BOARDCTL)
  list(APPEND SRCS bk7258_appinit.c)
endif()

if(CONFIG_LCD_GC9D01)
  list(APPEND SRCS bk7258_gc9d01.c)
endif()

if(CONFIG_EXAMPLES_GC2145_ID)
  list(APPEND SRCS bk7258_camera.c)
endif()

list(APPEND SRCS bk7258_audio.c)
list(APPEND SRCS bk7258_accel.c)

target_sources(board PRIVATE ${SRCS})

# BK7258 CPU0 app linker script (FLASH XIP + RAM)
set_property(GLOBAL PROPERTY LD_SCRIPT "${NUTTX_BOARD_DIR}/scripts/ld.script")
