/*
 * LIS2DH12 по SPI: питание через GPIO, FIFO в режиме stream.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KNIFE_ACCEL_H_
#define KNIFE_ACCEL_H_

#include <stdint.h>

/* Частота выборки датчика, Гц. */
#define ACCEL_ODR_HZ 25

/* Ёмкость FIFO LIS2DH12. */
#define ACCEL_FIFO_DEPTH 32

/* Подать питание, проверить WHO_AM_I и запустить: 25 Гц, ±2 g, HR, FIFO stream. */
int accel_on(void);

/* Забрать накопленные отсчёты (mg) из FIFO. Возвращает их число (0..32) или <0. */
int accel_read_fifo(int16_t mg[][3], int max);

/* Перевести датчик в power-down, снять питание и отпустить линии SPI. */
void accel_off(void);

#endif /* KNIFE_ACCEL_H_ */
