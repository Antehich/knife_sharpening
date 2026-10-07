/*
 * KnifeAngle: общие определения.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KNIFE_APP_H_
#define KNIFE_APP_H_

#include <stdint.h>

/* Время без подключения до автоматического выключения. */
#define AUTO_OFF_NO_CONN_MS (10 * 60 * 1000)

/* Кнопка. */
#define LONG_PRESS_MS  1500 /* ВКЛ/ВЫКЛ */
#define SHORT_PRESS_MS 800  /* короче: калибровка */

/* Период обработки: при подключении это и период notify (5 Гц). */
#define TICK_CONNECTED_MS 200
#define TICK_IDLE_MS      1000 /* без подключения FIFO датчика (32 отсчёта = 1.28 с) читается реже */

/* Период измерения батареи. */
#define BATTERY_PERIOD_MS (60 * 1000)

/* Биты поля flags в характеристике Angle. */
#define ANGLE_FLAG_CALIBRATED   0x01
#define ANGLE_FLAG_SENSOR_ERROR 0x02

/* События главного цикла. */
enum app_event {
	EV_TICK,
	EV_BTN_DOWN,
	EV_BTN_UP,
	EV_BTN_LONG,
	EV_CALIBRATE,
	EV_CONNECTED,
	EV_DISCONNECTED,
	EV_ADV_RESTART,
};

void app_post(enum app_event ev);

#endif /* KNIFE_APP_H_ */
