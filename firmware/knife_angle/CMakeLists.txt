# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Плата holyiot_21011 описана в этом репозитории (boards/holyiot/holyiot_21011).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(knife_angle)

target_sources(app PRIVATE
  src/main.c
  src/accel.c
  src/ble.c
  src/calib.c
)
