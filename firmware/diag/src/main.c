/*
 * Этап 0: диагностическая прошивка для HOLYIOT-21011.
 *
 * Пины берутся со схемы HOLYIOT-21014 V1.0, вслепую ничего не перебирается.
 * Прошивка проверяет то, на что опирается основная прошивка:
 *   1. LIS2DH12: WHO_AM_I по SPI (CS=P0.04, SCK=P0.05, MOSI=P0.02, MISO=P0.03),
 *      если не ответил, то по I2C на тех же пинах (адреса 0x19 и 0x18);
 *   2. кварц 32.768 кГц (P0.00/P0.01): запуск и измерение частоты;
 *   3. кнопку P0.31, светодиод P0.30, напряжение батареи;
 *   4. уход в System OFF и пробуждение кнопкой (удержание ≥1.5 с).
 * Результат передаётся в BLE-рекламе (имя и manufacturer data), а коротко дублируется
 * вспышками светодиода. Описание полей в README.md, раздел «Этап 0».
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/poweroff.h>
#include <hal/nrf_gpio.h>

#define DIAG_VERSION 1

/* Пины LIS2DH12 по схеме. */
#define PIN_SDI  2 /* SPI MOSI / I2C SDA */
#define PIN_SDO  3 /* SPI MISO / I2C SA0 */
#define PIN_CS   4 /* 0 = SPI, 1 = I2C */
#define PIN_SCK  5 /* SPI SCK / I2C SCL */
#define PIN_INT1 6
#define PIN_INT2 7
#define PIN_LVCC 8 /* питание датчика */

/* Регистры LIS2DH12/LIS3DH. */
#define REG_WHO_AM_I  0x0F
#define REG_CTRL_REG1 0x20
#define REG_CTRL_REG4 0x23
#define REG_OUT_X_L   0x28
#define WHO_AM_I_VAL  0x33

#define LONG_PRESS_MS      1500
#define LOOP_PERIOD_MS     200
#define AUTO_OFF_MINUTES   30

enum bus {
	BUS_NONE = 0,
	BUS_SPI = 1,
	BUS_I2C_19 = 2,
	BUS_I2C_18 = 3,
};

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct adc_dt_spec vdd_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

static enum bus accel_bus;
static uint8_t whoami_spi;
static uint8_t whoami_i2c;
static uint8_t lfxo_ok;
static uint16_t lfxo_ticks; /* тиков LFCLK за 100 мс, ожидается ~3277 */
static uint32_t resetreas;

/* ------------------------------------------------------------------------ */
/* Проверка кварца 32.768 кГц. Выполняется до инициализации драйвера часов,  */
/* потом LFCLK останавливается и Zephyr запускает его как обычно (от RC).    */
/* ------------------------------------------------------------------------ */

static int lfxo_probe(void)
{
	NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
	NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_Xtal << CLOCK_LFCLKSRC_SRC_Pos;
	NRF_CLOCK->TASKS_LFCLKSTART = 1;

	/* Кварц обычно стартует за ~0.25 с; ждём до 2 с. */
	for (int i = 0; i < 200 && !NRF_CLOCK->EVENTS_LFCLKSTARTED; i++) {
		k_busy_wait(10000);
	}

	if (NRF_CLOCK->EVENTS_LFCLKSTARTED) {
		/* Частоту сверяем с HFCLK по RTC0: 100 мс должно дать ~3277 тиков. */
		NRF_RTC0->PRESCALER = 0;
		NRF_RTC0->TASKS_CLEAR = 1;
		NRF_RTC0->TASKS_START = 1;
		k_busy_wait(100000);
		lfxo_ticks = (uint16_t)NRF_RTC0->COUNTER;
		NRF_RTC0->TASKS_STOP = 1;
		NRF_RTC0->TASKS_CLEAR = 1;
		lfxo_ok = (lfxo_ticks > 3113 && lfxo_ticks < 3441); /* ±5% */
	}

	NRF_CLOCK->TASKS_LFCLKSTOP = 1;
	for (int i = 0; i < 100 && (NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk); i++) {
		k_busy_wait(100);
	}
	NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
	NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos;

	return 0;
}

/* Приоритет 0: раньше драйвера часов (PRE_KERNEL_1, приоритет 30) и системного таймера. */
SYS_INIT(lfxo_probe, PRE_KERNEL_1, 0);

/* ------------------------------------------------------------------------ */
/* Программный SPI (режим 3) и I2C к LIS2DH12.                              */
/* ------------------------------------------------------------------------ */

static inline void bb_delay(void)
{
	k_busy_wait(5);
}

static void spi_pins_init(void)
{
	nrf_gpio_pin_set(PIN_CS);
	nrf_gpio_cfg_output(PIN_CS);
	nrf_gpio_pin_set(PIN_SCK);
	nrf_gpio_cfg_output(PIN_SCK);
	nrf_gpio_pin_clear(PIN_SDI);
	nrf_gpio_cfg_output(PIN_SDI);
	nrf_gpio_cfg_input(PIN_SDO, NRF_GPIO_PIN_NOPULL);
}

static uint8_t spi_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		nrf_gpio_pin_clear(PIN_SCK);
		nrf_gpio_pin_write(PIN_SDI, (out >> i) & 1);
		bb_delay();
		nrf_gpio_pin_set(PIN_SCK);
		bb_delay();
		in = (in << 1) | nrf_gpio_pin_read(PIN_SDO);
	}
	return in;
}

static void spi_read(uint8_t reg, uint8_t *buf, size_t len)
{
	nrf_gpio_pin_clear(PIN_CS);
	bb_delay();
	spi_xfer(0x80 | (len > 1 ? 0x40 : 0) | reg);
	for (size_t i = 0; i < len; i++) {
		buf[i] = spi_xfer(0);
	}
	nrf_gpio_pin_set(PIN_CS);
	bb_delay();
}

static void spi_write(uint8_t reg, uint8_t val)
{
	nrf_gpio_pin_clear(PIN_CS);
	bb_delay();
	spi_xfer(reg);
	spi_xfer(val);
	nrf_gpio_pin_set(PIN_CS);
	bb_delay();
}

/* I2C: открытый сток (S0D1) с внутренней подтяжкой ~13 кОм, ~100 кГц. */
static void i2c_pin_od(uint32_t pin)
{
	nrf_gpio_pin_set(pin);
	nrf_gpio_cfg(pin, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT,
		     NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_S0D1, NRF_GPIO_PIN_NOSENSE);
}

static void i2c_pins_init(bool sa0)
{
	nrf_gpio_pin_set(PIN_CS); /* CS=1: интерфейс I2C */
	nrf_gpio_cfg_output(PIN_CS);
	nrf_gpio_pin_write(PIN_SDO, sa0);
	nrf_gpio_cfg_output(PIN_SDO);
	i2c_pin_od(PIN_SDI);
	i2c_pin_od(PIN_SCK);
}

#define SDA(v) nrf_gpio_pin_write(PIN_SDI, (v))
#define SCL(v) nrf_gpio_pin_write(PIN_SCK, (v))

static void i2c_start(void)
{
	SDA(1); SCL(1); bb_delay();
	SDA(0); bb_delay();
	SCL(0); bb_delay();
}

static void i2c_stop(void)
{
	SDA(0); bb_delay();
	SCL(1); bb_delay();
	SDA(1); bb_delay();
}

static bool i2c_wbyte(uint8_t b)
{
	bool ack;

	for (int i = 7; i >= 0; i--) {
		SDA((b >> i) & 1); bb_delay();
		SCL(1); bb_delay();
		SCL(0);
	}
	SDA(1); bb_delay();
	SCL(1); bb_delay();
	ack = !nrf_gpio_pin_read(PIN_SDI);
	SCL(0); bb_delay();
	return ack;
}

static uint8_t i2c_rbyte(bool ack)
{
	uint8_t v = 0;

	SDA(1);
	for (int i = 0; i < 8; i++) {
		bb_delay();
		SCL(1); bb_delay();
		v = (v << 1) | nrf_gpio_pin_read(PIN_SDI);
		SCL(0);
	}
	SDA(ack ? 0 : 1); bb_delay();
	SCL(1); bb_delay();
	SCL(0); SDA(1); bb_delay();
	return v;
}

static uint8_t i2c_addr;

static int i2c_read(uint8_t reg, uint8_t *buf, size_t len)
{
	i2c_start();
	if (!i2c_wbyte(i2c_addr << 1) || !i2c_wbyte(reg | (len > 1 ? 0x80 : 0))) {
		i2c_stop();
		return -EIO;
	}
	i2c_start();
	if (!i2c_wbyte((i2c_addr << 1) | 1)) {
		i2c_stop();
		return -EIO;
	}
	for (size_t i = 0; i < len; i++) {
		buf[i] = i2c_rbyte(i + 1 < len);
	}
	i2c_stop();
	return 0;
}

static int i2c_write(uint8_t reg, uint8_t val)
{
	int ret = 0;

	i2c_start();
	if (!i2c_wbyte(i2c_addr << 1) || !i2c_wbyte(reg) || !i2c_wbyte(val)) {
		ret = -EIO;
	}
	i2c_stop();
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Акселерометр                                                             */
/* ------------------------------------------------------------------------ */

static void accel_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
	if (accel_bus == BUS_SPI) {
		spi_read(reg, buf, len);
	} else {
		(void)i2c_read(reg, buf, len);
	}
}

static void accel_write_reg(uint8_t reg, uint8_t val)
{
	if (accel_bus == BUS_SPI) {
		spi_write(reg, val);
	} else {
		(void)i2c_write(reg, val);
	}
}

static void accel_probe(void)
{
	nrf_gpio_pin_set(PIN_LVCC);
	nrf_gpio_cfg_output(PIN_LVCC);
	nrf_gpio_cfg_input(PIN_INT1, NRF_GPIO_PIN_NOPULL);
	nrf_gpio_cfg_input(PIN_INT2, NRF_GPIO_PIN_NOPULL);
	k_msleep(20); /* загрузка LIS2DH12 после подачи питания: 5 мс */

	spi_pins_init();
	spi_read(REG_WHO_AM_I, &whoami_spi, 1);
	if (whoami_spi == WHO_AM_I_VAL) {
		accel_bus = BUS_SPI;
		return;
	}

	static const struct {
		uint8_t addr;
		bool sa0;
		enum bus bus;
	} i2c_try[] = {
		{ 0x19, true, BUS_I2C_19 },
		{ 0x18, false, BUS_I2C_18 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(i2c_try); i++) {
		i2c_pins_init(i2c_try[i].sa0);
		i2c_addr = i2c_try[i].addr;
		k_msleep(2);
		if (i2c_read(REG_WHO_AM_I, &whoami_i2c, 1) == 0 &&
		    whoami_i2c == WHO_AM_I_VAL) {
			accel_bus = i2c_try[i].bus;
			return;
		}
	}
	accel_bus = BUS_NONE;
}

static void accel_start(void)
{
	accel_write_reg(REG_CTRL_REG4, 0x88); /* BDU=1, ±2 g, HR=1 */
	accel_write_reg(REG_CTRL_REG1, 0x37); /* 25 Гц, X/Y/Z включены */
}

/* Ускорение в mg: в режиме HR при ±2 g данные 12-битные, 1 mg на единицу. */
static void accel_read_mg(int16_t mg[3])
{
	uint8_t raw[6];

	accel_read_regs(REG_OUT_X_L, raw, sizeof(raw));
	for (int i = 0; i < 3; i++) {
		mg[i] = (int16_t)sys_get_le16(&raw[2 * i]) >> 4;
	}
}

static void accel_power_off(void)
{
	if (accel_bus != BUS_NONE) {
		accel_write_reg(REG_CTRL_REG1, 0x00); /* power-down */
	}
	/* Снять питание и не оставлять на линиях высокий уровень,
	 * иначе датчик подпитается через защитные диоды.
	 */
	nrf_gpio_pin_clear(PIN_LVCC);
	nrf_gpio_cfg_default(PIN_CS);
	nrf_gpio_cfg_default(PIN_SCK);
	nrf_gpio_cfg_default(PIN_SDI);
	nrf_gpio_cfg_default(PIN_SDO);
	nrf_gpio_cfg_default(PIN_INT1);
	nrf_gpio_cfg_default(PIN_INT2);
}

/* ------------------------------------------------------------------------ */
/* Светодиод, напряжение                                                    */
/* ------------------------------------------------------------------------ */

static void blink(int n, int on_ms, int off_ms)
{
	for (int i = 0; i < n; i++) {
		gpio_pin_set_dt(&led, 1);
		k_msleep(on_ms);
		gpio_pin_set_dt(&led, 0);
		k_msleep(off_ms);
	}
}

static int32_t vdd_read_mv(void)
{
	int16_t sample;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t mv;

	adc_sequence_init_dt(&vdd_adc, &seq);
	if (adc_read_dt(&vdd_adc, &seq) != 0) {
		return 0;
	}
	mv = sample;
	adc_raw_to_millivolts_dt(&vdd_adc, &mv);
	return mv;
}

/* ------------------------------------------------------------------------ */
/* BLE                                                                      */
/* ------------------------------------------------------------------------ */

static char adv_name[20];
static uint8_t mfg[21];

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, 0),
};

static struct bt_data sd[] = {
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg, sizeof(mfg)),
};

/* Интервал 500..520 мс (единицы 0.625 мс). */
static const struct bt_le_adv_param adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_SCANNABLE | BT_LE_ADV_OPT_USE_IDENTITY,
			     800, 832, NULL);

static uint8_t bus_letter(void)
{
	switch (accel_bus) {
	case BUS_SPI:
		return 'S';
	case BUS_I2C_19:
		return 'I';
	case BUS_I2C_18:
		return 'i';
	default:
		return 'N';
	}
}

static void fill_adv(bool pressed, const int16_t mg[3], uint16_t tilt_x10, int32_t vdd_mv,
		     uint8_t counter)
{
	uint8_t who = (accel_bus == BUS_SPI || accel_bus == BUS_NONE) ? whoami_spi : whoami_i2c;
	uint8_t flags = 0;

	snprintf(adv_name, sizeof(adv_name), "KAD %c%02X X%u B%u", bus_letter(), who,
		 lfxo_ok, pressed);
	ad[1].data_len = strlen(adv_name);

	flags |= pressed ? BIT(0) : 0;
	flags |= (resetreas & POWER_RESETREAS_OFF_Msk) ? BIT(1) : 0;
	flags |= (resetreas & POWER_RESETREAS_RESETPIN_Msk) ? BIT(2) : 0;
	flags |= (resetreas & POWER_RESETREAS_SREQ_Msk) ? BIT(3) : 0;
	flags |= (resetreas & POWER_RESETREAS_DOG_Msk) ? BIT(4) : 0;
	flags |= (resetreas & POWER_RESETREAS_LOCKUP_Msk) ? BIT(5) : 0;

	sys_put_le16(0xFFFF, &mfg[0]); /* тестовый Company ID */
	mfg[2] = DIAG_VERSION;
	mfg[3] = accel_bus;
	mfg[4] = whoami_spi;
	mfg[5] = whoami_i2c;
	mfg[6] = lfxo_ok;
	sys_put_le16(lfxo_ticks, &mfg[7]);
	mfg[9] = flags;
	sys_put_le16((uint16_t)mg[0], &mfg[10]);
	sys_put_le16((uint16_t)mg[1], &mfg[12]);
	sys_put_le16((uint16_t)mg[2], &mfg[14]);
	sys_put_le16(tilt_x10, &mfg[16]);
	sys_put_le16((uint16_t)vdd_mv, &mfg[18]);
	mfg[20] = counter;
}

/* ------------------------------------------------------------------------ */

static void power_off(void)
{
	bt_le_adv_stop();
	accel_power_off();

	/* Длинная вспышка: «выключаюсь». */
	blink(1, 600, 0);

	/* Ждём отпускания кнопки, иначе System OFF сразу проснётся по уровню. */
	while (gpio_pin_get_dt(&btn) > 0) {
		k_msleep(20);
	}
	k_msleep(50);

	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_LEVEL_ACTIVE);
	sys_poweroff();
}

int main(void)
{
	int16_t mg[3] = { 0 };
	uint16_t tilt_x10 = 0;
	int32_t vdd_mv = 0;
	uint8_t counter = 0;
	int64_t press_start = -1;
	int64_t started = k_uptime_get();

	resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = resetreas; /* сброс битов записью единиц */

	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&btn, GPIO_INPUT);
	adc_channel_setup_dt(&vdd_adc);

	/* 1 вспышка: прошивка запустилась. */
	blink(1, 80, 600);

	accel_probe();
	if (accel_bus != BUS_NONE) {
		accel_start();
		blink(2, 80, 250); /* 2 вспышки: акселерометр найден */
	} else {
		blink(5, 40, 120); /* 5 быстрых: не найден */
	}

	vdd_mv = vdd_read_mv();
	fill_adv(false, mg, tilt_x10, vdd_mv, counter);

	if (bt_enable(NULL) == 0) {
		bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	} else {
		blink(10, 40, 80);
	}

	while (true) {
		k_msleep(LOOP_PERIOD_MS);

		bool pressed = gpio_pin_get_dt(&btn) > 0;
		int64_t now = k_uptime_get();

		if (pressed) {
			if (press_start < 0) {
				press_start = now;
			} else if (now - press_start >= LONG_PRESS_MS) {
				power_off();
			}
		} else {
			press_start = -1;
		}

		if (now - started > AUTO_OFF_MINUTES * 60 * 1000LL) {
			power_off();
		}

		if (accel_bus != BUS_NONE) {
			accel_read_mg(mg);
			float xy = sqrtf((float)mg[0] * mg[0] + (float)mg[1] * mg[1]);
			float deg = atan2f(xy, (float)mg[2]) * (180.0f / 3.14159265f);

			tilt_x10 = (uint16_t)lroundf(deg * 10.0f);
		}
		if ((counter % 25) == 0) {
			vdd_mv = vdd_read_mv();
		}

		counter++;
		fill_adv(pressed, mg, tilt_x10, vdd_mv, counter);
		bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	}

	return 0;
}
