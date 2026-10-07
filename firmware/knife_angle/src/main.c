/*
 * KnifeAngle: BLE-датчик угла заточки (HOLYIOT-21011, nRF52810 + LIS2DH12).
 *
 * Состояния:
 *   ВЫКЛ: System OFF, датчик обесточен, радио выключено, пробуждение по кнопке.
 *         Пробуждение из System OFF это сброс чипа, поэтому после старта проверяется,
 *         что кнопку держат ≥1.5 с; если нет, чип сразу засыпает обратно.
 *         Любой другой сброс (установка батарейки, перепрошивка, watchdog) тоже ведёт в ВЫКЛ.
 *   ВКЛ:  датчик 25 Гц, реклама «KnifeAngle», при подключении notify угла 5 раз в секунду.
 *
 * Кнопка в ВКЛ: короткое нажатие (<0.8 с) = калибровка, длинное (≥1.5 с) = ВЫКЛ.
 * Светодиод: ВКЛ = одна вспышка 0.5 с, ВЫКЛ = две по 0.15 с,
 *            калибровка = короткая 60 мс, отказ калибровки = три быстрые.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_power.h>

#include "accel.h"
#include "app.h"
#include "ble.h"
#include "calib.h"

/* Сглаживание: экспоненциальное скользящее среднее по вектору гравитации.
 * α = 1 − exp(−Δt/τ) при Δt = 40 мс (25 Гц) и τ = 80 мс: 95 % отклика за 3τ ≈ 0.24 с.
 */
#define EMA_ALPHA 0.39f

/* Допустимый модуль вектора при калибровке, mg: датчик должен лежать спокойно. */
#define CALIB_MIN_MG 700.0f
#define CALIB_MAX_MG 1300.0f

#define WDT_TIMEOUT_MS 5000

#define RAD_TO_DEG 57.29577951f

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct adc_dt_spec vdd_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));

K_MSGQ_DEFINE(evq, sizeof(uint8_t), 16, 1);

void app_post(enum app_event ev)
{
	uint8_t e = ev;

	(void)k_msgq_put(&evq, &e, K_NO_WAIT);
}

/* ------------------------------------------------------------------------ */
/* Светодиод, сторожевой таймер, батарея                                    */
/* ------------------------------------------------------------------------ */

static void led_flash(int n, int on_ms, int off_ms)
{
	for (int i = 0; i < n; i++) {
		gpio_pin_set_dt(&led, 1);
		k_msleep(on_ms);
		gpio_pin_set_dt(&led, 0);
		if (i + 1 < n) {
			k_msleep(off_ms);
		}
	}
}

static int wdt_channel = -1;

static void wdt_start(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = { .min = 0, .max = WDT_TIMEOUT_MS },
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt)) {
		return;
	}
	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel >= 0) {
		wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	}
}

static void wdt_kick(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt, wdt_channel);
	}
}

/* Грубая кривая разряда CR2032 под малой нагрузкой: мВ → %. */
static uint8_t battery_percent(int32_t mv)
{
	static const struct {
		int16_t mv;
		uint8_t pct;
	} curve[] = {
		{ 3000, 100 }, { 2900, 80 }, { 2800, 60 }, { 2700, 40 },
		{ 2600, 20 },  { 2500, 10 }, { 2400, 5 },  { 2000, 0 },
	};

	if (mv >= curve[0].mv) {
		return 100;
	}
	for (size_t i = 1; i < ARRAY_SIZE(curve); i++) {
		if (mv >= curve[i].mv) {
			int32_t span = curve[i - 1].mv - curve[i].mv;

			return curve[i].pct + (mv - curve[i].mv) *
				(curve[i - 1].pct - curve[i].pct) / span;
		}
	}
	return 0;
}

static void battery_update(void)
{
	int16_t sample;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t mv;

	adc_sequence_init_dt(&vdd_adc, &seq);
	if (adc_read_dt(&vdd_adc, &seq) != 0) {
		return;
	}
	mv = sample;
	adc_raw_to_millivolts_dt(&vdd_adc, &mv);
	ble_set_battery(battery_percent(mv));
}

/* ------------------------------------------------------------------------ */
/* Кнопка                                                                   */
/* ------------------------------------------------------------------------ */

static int btn_level; /* после антидребезга; меняется только в debounce_handler */
static struct gpio_callback btn_cb;

static void debounce_handler(struct k_work *work)
{
	int level = gpio_pin_get_dt(&btn);

	if (level >= 0 && level != btn_level) {
		btn_level = level;
		app_post(level ? EV_BTN_DOWN : EV_BTN_UP);
	}
}

static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_handler);

static void btn_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&debounce_work, K_MSEC(20));
}

static void long_press_expiry(struct k_timer *t)
{
	app_post(EV_BTN_LONG);
}

static K_TIMER_DEFINE(long_press_timer, long_press_expiry, NULL);

static void tick_expiry(struct k_timer *t)
{
	app_post(EV_TICK);
}

static K_TIMER_DEFINE(tick_timer, tick_expiry, NULL);

static bool button_pressed(void)
{
	return gpio_pin_get_dt(&btn) > 0;
}

/* ------------------------------------------------------------------------ */
/* ВЫКЛ                                                                     */
/* ------------------------------------------------------------------------ */

static bool ble_started;

static FUNC_NORETURN void power_off(bool confirm)
{
	k_timer_stop(&tick_timer);
	k_timer_stop(&long_press_timer);
	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_DISABLE);

	if (ble_started) {
		ble_stop();
	}
	accel_off();

	if (confirm) {
		led_flash(2, 150, 150);
	}

	/* System OFF просыпается по уровню, поэтому сначала ждём отпускания кнопки. */
	while (button_pressed()) {
		wdt_kick();
		k_msleep(20);
	}
	k_msleep(50);

	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_LEVEL_ACTIVE);
	sys_poweroff();
}

/* ------------------------------------------------------------------------ */
/* Угол                                                                     */
/* ------------------------------------------------------------------------ */

static float filt[3];      /* сглаженный вектор гравитации, mg */
static bool have_sample;
static float ref[3] = { 0.0f, 0.0f, 1.0f }; /* без калибровки: ось Z датчика */
static bool calibrated;
static bool sensor_error;

static void process_samples(void)
{
	int16_t buf[ACCEL_FIFO_DEPTH][3];
	int n;

	if (sensor_error) {
		return;
	}
	n = accel_read_fifo(buf, ACCEL_FIFO_DEPTH);
	if (n < 0) {
		sensor_error = true;
		return;
	}
	for (int i = 0; i < n; i++) {
		for (int a = 0; a < 3; a++) {
			if (!have_sample) {
				filt[a] = buf[i][a];
			} else {
				filt[a] += EMA_ALPHA * ((float)buf[i][a] - filt[a]);
			}
		}
		have_sample = true;
	}
}

/* Угол между текущим вектором гравитации и опорным, 0..180°, в единицах 0.1°.
 * atan2(|r×g|, r·g) точнее, чем acos, при углах около 0° и 180°.
 */
static int16_t angle_x10(void)
{
	float cx = ref[1] * filt[2] - ref[2] * filt[1];
	float cy = ref[2] * filt[0] - ref[0] * filt[2];
	float cz = ref[0] * filt[1] - ref[1] * filt[0];
	float cross = sqrtf(cx * cx + cy * cy + cz * cz);
	float dot = ref[0] * filt[0] + ref[1] * filt[1] + ref[2] * filt[2];

	return (int16_t)lroundf(atan2f(cross, dot) * RAD_TO_DEG * 10.0f);
}

static void publish_angle(void)
{
	uint8_t flags = (calibrated ? ANGLE_FLAG_CALIBRATED : 0) |
			(sensor_error ? ANGLE_FLAG_SENSOR_ERROR : 0);

	ble_set_angle(have_sample ? angle_x10() : 0, flags);
}

static void calibrate(void)
{
	float m;
	float v[3];

	process_samples(); /* взять самые свежие отсчёты из FIFO */

	m = sqrtf(filt[0] * filt[0] + filt[1] * filt[1] + filt[2] * filt[2]);
	if (!have_sample || sensor_error || m < CALIB_MIN_MG || m > CALIB_MAX_MG) {
		led_flash(3, 40, 80);
		return;
	}
	for (int a = 0; a < 3; a++) {
		v[a] = filt[a] / m;
	}
	if (calib_save(v) != 0) {
		led_flash(3, 40, 80);
		return;
	}

	memcpy(ref, v, sizeof(ref));
	calibrated = true;
	led_flash(1, 60, 0);
	publish_angle();
}

/* ------------------------------------------------------------------------ */

static void set_tick(uint32_t period_ms)
{
	k_timer_start(&tick_timer, K_MSEC(period_ms), K_MSEC(period_ms));
}

int main(void)
{
	uint32_t resetreas = NRF_POWER->RESETREAS;
	int64_t press_start = 0;
	int64_t last_conn_activity;
	int64_t last_battery;
	bool ignore_release;
	uint8_t ev;

	NRF_POWER->RESETREAS = resetreas; /* сброс битов записью единиц */

	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&btn, GPIO_INPUT);

	/* Не пробуждение кнопкой (батарейку вставили, перепрошили, сработал watchdog):
	 * устройство начинает в ВЫКЛ.
	 */
	if (!(resetreas & POWER_RESETREAS_OFF_Msk)) {
		power_off(false);
	}

	/* Пробуждение кнопкой: включаемся, только если её держат ≥1.5 с с момента старта. */
	while (k_uptime_get() < LONG_PRESS_MS) {
		if (!button_pressed()) {
			power_off(false);
		}
		k_msleep(10);
	}

	/* ---- ВКЛ ---- */
	wdt_start();
	led_flash(1, 500, 0);

	/* Нажатие, которым включили, ещё может длиться: его отпускание игнорируем. */
	btn_level = button_pressed();
	ignore_release = btn_level;
	gpio_init_callback(&btn_cb, btn_isr, BIT(btn.pin));
	gpio_add_callback_dt(&btn, &btn_cb);
	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);
	/* Перечитать уровень: кнопку могли отпустить до включения прерывания. */
	k_work_reschedule(&debounce_work, K_MSEC(20));

	calibrated = calib_load(ref);
	if (!calibrated) {
		ref[0] = 0.0f;
		ref[1] = 0.0f;
		ref[2] = 1.0f;
	}

	if (accel_on() != 0) {
		sensor_error = true;
		led_flash(5, 40, 120);
	}

	adc_channel_setup_dt(&vdd_adc);

	if (ble_init() != 0) {
		led_flash(10, 40, 80);
		power_off(false);
	}
	ble_started = true;
	battery_update();
	last_battery = k_uptime_get();
	ble_adv_start();

	last_conn_activity = k_uptime_get();
	set_tick(TICK_IDLE_MS);

	while (true) {
		k_msgq_get(&evq, &ev, K_FOREVER);
		int64_t now = k_uptime_get();

		switch (ev) {
		case EV_TICK:
			wdt_kick();
			process_samples();
			publish_angle();
			if (now - last_battery >= BATTERY_PERIOD_MS) {
				battery_update();
				last_battery = now;
			}
			if (!ble_is_connected() && now - last_conn_activity >= AUTO_OFF_NO_CONN_MS) {
				power_off(true);
			}
			break;

		case EV_BTN_DOWN:
			press_start = now;
			k_timer_start(&long_press_timer, K_MSEC(LONG_PRESS_MS), K_NO_WAIT);
			break;

		case EV_BTN_UP:
			k_timer_stop(&long_press_timer);
			if (ignore_release) {
				ignore_release = false;
			} else if (now - press_start < SHORT_PRESS_MS) {
				calibrate();
			}
			break;

		case EV_BTN_LONG:
			if (btn_level && !ignore_release) {
				power_off(true);
			}
			break;

		case EV_CALIBRATE:
			calibrate();
			break;

		case EV_CONNECTED:
			last_conn_activity = now;
			set_tick(TICK_CONNECTED_MS);
			break;

		case EV_DISCONNECTED:
			last_conn_activity = now;
			set_tick(TICK_IDLE_MS);
			break;

		case EV_ADV_RESTART:
			if (!ble_is_connected()) {
				ble_adv_start();
			}
			break;
		}
	}

	return 0;
}
