/*
 * BLE: реклама, сервис KnifeAngle, служба батареи.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KNIFE_BLE_H_
#define KNIFE_BLE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Сервис KnifeAngle (UUID сгенерированы случайно):
 *   сервис  8c900001-4d19-4014-a48c-3f940a276aeb
 *   Angle   8c900002-4d19-4014-a48c-3f940a276aeb  read, notify: int16 LE (0.1°) + uint8 flags
 *   Command 8c900003-4d19-4014-a48c-3f940a276aeb  write: 0x01 = калибровка
 */
#define KA_UUID_SVC_VAL     BT_UUID_128_ENCODE(0x8c900001, 0x4d19, 0x4014, 0xa48c, 0x3f940a276aeb)
#define KA_UUID_ANGLE_VAL   BT_UUID_128_ENCODE(0x8c900002, 0x4d19, 0x4014, 0xa48c, 0x3f940a276aeb)
#define KA_UUID_COMMAND_VAL BT_UUID_128_ENCODE(0x8c900003, 0x4d19, 0x4014, 0xa48c, 0x3f940a276aeb)

#define KA_CMD_CALIBRATE 0x01

/* Реклама: интервал 852.5–875 мс (единицы 0.625 мс). 852.5 мс входит в список
 * интервалов, которые Apple рекомендует для быстрого обнаружения на iOS.
 */
#define ADV_INTERVAL_MIN 1364
#define ADV_INTERVAL_MAX 1400

int ble_init(void);
int ble_adv_start(void);
void ble_stop(void);
bool ble_is_connected(void);

/* Обновить значение Angle и, если клиент подписан, отправить notify. */
void ble_set_angle(int16_t angle_x10, uint8_t flags);

void ble_set_battery(uint8_t percent);

#endif /* KNIFE_BLE_H_ */
