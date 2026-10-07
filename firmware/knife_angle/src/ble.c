/*
 * BLE: реклама «KnifeAngle», сервис с характеристиками Angle и Command, Battery Service.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/sys/byteorder.h>

#include "app.h"
#include "ble.h"

static const struct bt_uuid_128 ka_svc_uuid = BT_UUID_INIT_128(KA_UUID_SVC_VAL);
static const struct bt_uuid_128 ka_angle_uuid = BT_UUID_INIT_128(KA_UUID_ANGLE_VAL);
static const struct bt_uuid_128 ka_cmd_uuid = BT_UUID_INIT_128(KA_UUID_COMMAND_VAL);

static uint8_t angle_val[3]; /* int16 LE угол ×10 + uint8 flags */
static bool notify_enabled;
static struct bt_conn *cur_conn;

static ssize_t angle_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, angle_val, sizeof(angle_val));
}

static void angle_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static ssize_t cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0 || len != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (((const uint8_t *)buf)[0]) {
	case KA_CMD_CALIBRATE:
		app_post(EV_CALIBRATE);
		return len;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
}

BT_GATT_SERVICE_DEFINE(ka_svc,
	BT_GATT_PRIMARY_SERVICE(&ka_svc_uuid),
	BT_GATT_CHARACTERISTIC(&ka_angle_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, angle_read, NULL, NULL),
	BT_GATT_CCC(angle_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD("Angle", BT_GATT_PERM_READ),
	BT_GATT_CHARACTERISTIC(&ka_cmd_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, cmd_write, NULL),
	BT_GATT_CUD("Command", BT_GATT_PERM_READ),
);

/* Индекс значения Angle в ka_svc.attrs: 0 сервис, 1 объявление, 2 значение. */
#define ANGLE_ATTR (&ka_svc.attrs[2])

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* UUID сервиса не помещается в рекламный пакет вместе с именем и идёт в ответе на скан. */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, KA_UUID_SVC_VAL),
};

static const struct bt_le_adv_param adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_INTERVAL_MIN, ADV_INTERVAL_MAX, NULL);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		app_post(EV_ADV_RESTART);
		return;
	}
	cur_conn = bt_conn_ref(conn);
	app_post(EV_CONNECTED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (cur_conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	notify_enabled = false;
	app_post(EV_DISCONNECTED);
}

/* Объект соединения освобождён: только теперь можно снова начать рекламу. */
static void recycled(void)
{
	app_post(EV_ADV_RESTART);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

int ble_init(void)
{
	return bt_enable(NULL);
}

int ble_adv_start(void)
{
	int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	return (err == -EALREADY) ? 0 : err;
}

void ble_stop(void)
{
	bt_le_adv_stop();
	if (cur_conn) {
		bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		/* Дать контроллеру отправить LL_TERMINATE_IND, чтобы страница сразу
		 * увидела отключение, а не ждала таймаута супервизии (4 с).
		 */
		for (int i = 0; i < 50 && cur_conn; i++) {
			k_msleep(10);
		}
	}
}

bool ble_is_connected(void)
{
	return cur_conn != NULL;
}

void ble_set_angle(int16_t angle_x10, uint8_t flags)
{
	sys_put_le16((uint16_t)angle_x10, angle_val);
	angle_val[2] = flags;

	if (cur_conn && notify_enabled) {
		(void)bt_gatt_notify(cur_conn, ANGLE_ATTR, angle_val, sizeof(angle_val));
	}
}

void ble_set_battery(uint8_t percent)
{
	(void)bt_bas_set_battery_level(percent);
}
