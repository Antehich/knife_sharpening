/*
 * Оценка вектора гравитации по отсчётам акселерометра, устойчивая к штрихам.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KNIFE_FILTER_H_
#define KNIFE_FILTER_H_

#include <stdbool.h>
#include <stdint.h>

/* Добавить отсчёт (mg). Вызывать для каждого отсчёта с частотой ACCEL_ODR_HZ. */
void filter_add(const int16_t mg[3]);

/* Текущая оценка вектора гравитации, mg. false, если отсчётов ещё не было. */
bool filter_get(float g[3]);

#endif /* KNIFE_FILTER_H_ */
