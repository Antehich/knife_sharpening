# SPDX-License-Identifier: Apache-2.0
# Прошивка обычно идёт через ESP32_nRF52_SWD (.bin со смещением 0),
# но jlink/pyocd тоже работают, если есть нормальный отладчик.
board_runner_args(jlink "--device=nRF52810_xxAA" "--speed=4000")
board_runner_args(pyocd "--target=nrf52810")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
