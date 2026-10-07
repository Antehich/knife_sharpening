/*
 * Калибровка в NVS (раздел storage_partition, последние 8 КБ flash).
 *
 * Переживает выключение и замену батарейки. Стирается только полным стиранием
 * чипа (Erase nRF) при перепрошивке.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#include "calib.h"

#define CALIB_NVS_ID 1
#define CALIB_MAGIC  0x4B414C31 /* "KAL1": формат записи, версия 1 */

struct calib_record {
	uint32_t magic;
	float ref[3];
};

static struct nvs_fs fs;
static bool mounted;

static int calib_mount(void)
{
	struct flash_pages_info info;
	int err;

	if (mounted) {
		return 0;
	}

	fs.flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs.flash_device)) {
		return -ENODEV;
	}
	fs.offset = PARTITION_OFFSET(storage_partition);
	err = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
	if (err) {
		return err;
	}
	fs.sector_size = info.size;
	fs.sector_count = PARTITION_SIZE(storage_partition) / info.size;

	err = nvs_mount(&fs);
	if (err == 0) {
		mounted = true;
	}
	return err;
}

bool calib_load(float ref[3])
{
	struct calib_record rec;

	if (calib_mount() != 0) {
		return false;
	}
	if (nvs_read(&fs, CALIB_NVS_ID, &rec, sizeof(rec)) != sizeof(rec) ||
	    rec.magic != CALIB_MAGIC) {
		return false;
	}
	memcpy(ref, rec.ref, sizeof(rec.ref));
	return true;
}

int calib_save(const float ref[3])
{
	struct calib_record rec = { .magic = CALIB_MAGIC };
	ssize_t ret;
	int err;

	err = calib_mount();
	if (err) {
		return err;
	}
	memcpy(rec.ref, ref, sizeof(rec.ref));

	/* NVS не пишет запись, если значение не изменилось, и сам распределяет износ. */
	ret = nvs_write(&fs, CALIB_NVS_ID, &rec, sizeof(rec));
	return ret < 0 ? (int)ret : 0;
}
