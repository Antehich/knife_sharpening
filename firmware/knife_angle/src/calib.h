/*
 * Калибровка: опорный вектор гравитации «нож плашмя на камне», хранится в NVS.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KNIFE_CALIB_H_
#define KNIFE_CALIB_H_

#include <stdbool.h>

/* Подключить NVS и прочитать сохранённый вектор. true, если калибровка есть. */
bool calib_load(float ref[3]);

/* Сохранить вектор. 0 при успехе. */
int calib_save(const float ref[3]);

#endif /* KNIFE_CALIB_H_ */
