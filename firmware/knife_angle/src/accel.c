/*
 * LIS2DH12 по SPI.
 *
 * Датчик питается от P0.08 (LVCC). В ВЫКЛ питание снимается полностью, так что
 * в System OFF датчик ничего не потребляет.
 *
 * Отсчёты копятся во встроенном FIFO (32 уровня) в режиме stream, а прошивка
 * забирает их пачкой раз в 200 мс (при подключении) или раз в 1 с (без него).
 * Так процессор просыпается редко, а сглаживание всё равно идёт по всем 25 отсчётам в секунду.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_gpio.h>

#include "accel.h"

#define REG_WHO_AM_I      0x0F
#define REG_CTRL_REG1     0x20
#define REG_CTRL_REG4     0x23
#define REG_CTRL_REG5     0x24
#define REG_OUT_X_L       0x28
#define REG_FIFO_CTRL_REG 0x2E
#define REG_FIFO_SRC_REG  0x2F

#define WHO_AM_I_VAL 0x33

#define SPI_READ  0x80
#define SPI_MULTI 0x40

/* Линии SPI по схеме (boards/holyiot/holyiot_21011/holyiot_21011-pinctrl.dtsi). */
#define PIN_SCK  5
#define PIN_MOSI 2
#define PIN_MISO 3

#define ACCEL_NODE DT_NODELABEL(lis2dh12)

/* LIS2DH12: SPI режим 3 (SCK в покое высокий), старший бит первым. */
static const struct spi_dt_spec spi = SPI_DT_SPEC_GET(
	ACCEL_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA);

static const struct gpio_dt_spec pwr = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), accel_power_gpios);

static int reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
	uint8_t cmd = SPI_READ | (len > 1 ? SPI_MULTI : 0) | reg;
	const struct spi_buf tx_buf = { .buf = &cmd, .len = 1 };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf rx_bufs[] = {
		{ .buf = NULL, .len = 1 }, /* байт во время передачи команды */
		{ .buf = buf, .len = len },
	};
	const struct spi_buf_set rx = { .buffers = rx_bufs, .count = ARRAY_SIZE(rx_bufs) };

	return spi_transceive_dt(&spi, &tx, &rx);
}

static int reg_write(uint8_t reg, uint8_t val)
{
	uint8_t data[2] = { reg, val };
	const struct spi_buf tx_buf = { .buf = data, .len = sizeof(data) };
	const struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };

	return spi_write_dt(&spi, &tx);
}

int accel_on(void)
{
	uint8_t who = 0;
	int err;

	if (!spi_is_ready_dt(&spi) || !gpio_is_ready_dt(&pwr)) {
		return -ENODEV;
	}

	gpio_pin_configure_dt(&pwr, GPIO_OUTPUT_ACTIVE);
	k_msleep(10); /* загрузка после подачи питания: 5 мс по даташиту */

	err = reg_read(REG_WHO_AM_I, &who, 1);
	if (err) {
		return err;
	}
	if (who != WHO_AM_I_VAL) {
		return -ENODEV;
	}

	/* Порядок: сначала FIFO в bypass (сброс), потом настройка, потом stream. */
	err = reg_write(REG_FIFO_CTRL_REG, 0x00);
	err |= reg_write(REG_CTRL_REG4, 0x88); /* BDU=1, ±2 g, HR=1 (12 бит, 1 mg/LSB) */
	err |= reg_write(REG_CTRL_REG5, 0x40); /* FIFO_EN */
	err |= reg_write(REG_FIFO_CTRL_REG, 0x80); /* stream: при переполнении старые отсчёты затираются */
	err |= reg_write(REG_CTRL_REG1, 0x37); /* ODR 25 Гц, X/Y/Z включены */

	return err ? -EIO : 0;
}

int accel_read_fifo(int16_t mg[][3], int max)
{
	uint8_t src;
	uint8_t raw[6];
	int n;

	if (reg_read(REG_FIFO_SRC_REG, &src, 1)) {
		return -EIO;
	}

	/* FSS: число непрочитанных отсчётов; OVRN: FIFO полон (32). */
	n = (src & BIT(6)) ? ACCEL_FIFO_DEPTH : (src & 0x1F);
	n = MIN(n, max);

	for (int i = 0; i < n; i++) {
		/* Чтение 6 байт с 0x28 забирает из FIFO один отсчёт X/Y/Z. */
		if (reg_read(REG_OUT_X_L, raw, sizeof(raw))) {
			return -EIO;
		}
		for (int a = 0; a < 3; a++) {
			/* Данные 12-битные, выровнены влево. */
			mg[i][a] = (int16_t)sys_get_le16(&raw[2 * a]) >> 4;
		}
	}

	return n;
}

void accel_off(void)
{
	(void)reg_write(REG_CTRL_REG1, 0x00); /* power-down */

	/* Снять питание и отпустить все линии датчика: высокий уровень на CS или SCK
	 * при обесточенном датчике подпитывал бы его через защитные диоды.
	 */
	gpio_pin_configure_dt(&pwr, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&spi.config.cs.gpio, GPIO_DISCONNECTED);
	nrf_gpio_cfg_default(PIN_SCK);
	nrf_gpio_cfg_default(PIN_MOSI);
	nrf_gpio_cfg_default(PIN_MISO);
}
