/*
 * Samsung Exynos5 SoC series Sensor driver
 *
 *
 * Copyright (c) 2021 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/videodev2.h>
#include <linux/videodev2_exynos_camera.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include <exynos-is-sensor.h>
#include "is-hw.h"
#include "is-core.h"
#include "is-param.h"
#include "is-device-sensor.h"
#include "is-device-sensor-peri.h"
#include "is-resourcemgr.h"
#include "is-dt.h"
#include "is-cis-gc08a3.h"
#include "is-cis-gc08a3-setA.h"
#include "is-cis-gc08a3-setB.h"

#include "is-helper-i2c.h"

#include "is-vender-specific.h"

#define SENSOR_NAME "S5KGC08A3"
/* #define DEBUG_GC08A3_PLL */

#define MULTIPLE_OF_2(val) ((val >> 1) << 1)

#if defined(CONFIG_VENDER_MCD_V2)
extern const struct is_vender_rom_addr *vender_rom_addr[SENSOR_POSITION_MAX];
#ifdef USE_DUALIZED_OTPROM_SENSOR
extern const struct is_vender_rom_addr *vender_rom_addr_dualized[SENSOR_POSITION_MAX];
#endif
#endif

#define POLL_TIME_MS (1)
#define POLL_TIME_US (1000)
#define STREAM_OFF_POLL_TIME_MS (500)

static const struct v4l2_subdev_ops subdev_ops;

static const u32 *sensor_gc08a3_global;
static u32 sensor_gc08a3_global_size;
static const u32 **sensor_gc08a3_setfiles;
static const u32 *sensor_gc08a3_setfile_sizes;
static const struct sensor_pll_info_compact **sensor_gc08a3_pllinfos;
static const u32 *sensor_gc08a3_dualsync_master;
static u32 sensor_gc08a3_dualsync_master_size;
static const u32 *sensor_gc08a3_dualsync_slave;
static u32 sensor_gc08a3_dualsync_slave_size;
static u32 sensor_gc08a3_max_setfile_num;

/*************************************************
 *  [GC08A3 Analog gain formular]
 *
 *  Analog Gain = (Reg value)/1024
 *
 *  Analog Gain Range = x1.0 to x16.0
 *
 *************************************************/

u32 sensor_gc08a3_cis_calc_again_code(u32 permile)
{
	return ((permile * 1024) / 1000);
}

u32 sensor_gc08a3_cis_calc_again_permile(u32 code)
{
	return ((code * 1000 / 1024));
}

#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
static const struct cam_mipi_sensor_mode *sensor_gc08a3_mipi_sensor_mode;
static u32 sensor_gc08a3_mipi_sensor_mode_size;
static const int *sensor_gc08a3_verify_sensor_mode;
static int sensor_gc08a3_verify_sensor_mode_size;

static int sensor_gc08a3_cis_set_mipi_clock(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	const struct cam_mipi_sensor_mode *cur_mipi_sensor_mode;
	int mode = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	mode = cis->cis_data->sens_config_index_cur;

	dbg_sensor(1, "%s : mipi_clock_index_cur(%d), new(%d)\n", __func__,
		cis->mipi_clock_index_cur, cis->mipi_clock_index_new);

	if (mode >= sensor_gc08a3_mipi_sensor_mode_size) {
		err("sensor mode is out of bound");
		return -1;
	}

	if (cis->mipi_clock_index_cur != cis->mipi_clock_index_new
		&& cis->mipi_clock_index_new >= 0) {
		cur_mipi_sensor_mode = &sensor_gc08a3_mipi_sensor_mode[mode];

		if (cur_mipi_sensor_mode->sensor_setting == NULL) {
			dbg_sensor(1, "no mipi setting for current sensor mode\n");
		} else if (cis->mipi_clock_index_new < cur_mipi_sensor_mode->sensor_setting_size) {
			info("%s: change mipi clock [%d %d]\n", __func__, mode, cis->mipi_clock_index_new);
			sensor_cis_set_registers(subdev,
				cur_mipi_sensor_mode->sensor_setting[cis->mipi_clock_index_new].setting,
				cur_mipi_sensor_mode->sensor_setting[cis->mipi_clock_index_new].setting_size);

			cis->mipi_clock_index_cur = cis->mipi_clock_index_new;
		} else {
			err("sensor setting index is out of bound %d %d",
				cis->mipi_clock_index_new, cur_mipi_sensor_mode->sensor_setting_size);
		}
	}

	return ret;
}
#endif

static void sensor_gc08a3_cis_data_calculation(const struct sensor_pll_info_compact *pll_info, cis_shared_data *cis_data)
{
	u32 vt_pix_clk_hz = 0;
	u32 frame_rate = 0, max_fps = 0, frame_valid_us = 0;

	BUG_ON(!pll_info);

	/* 1. get pclk value from pll info */
	vt_pix_clk_hz = pll_info->pclk;

	/* 2. the time of processing one frame calculation (us) */
	cis_data->min_frame_us_time = ((pll_info->frame_length_lines * pll_info->line_length_pck)
					/ (vt_pix_clk_hz / (1000 * 1000)));
	cis_data->cur_frame_us_time = cis_data->min_frame_us_time;

	/* 3. FPS calculation */
	frame_rate = vt_pix_clk_hz / (pll_info->frame_length_lines * pll_info->line_length_pck);
	dbg_sensor(1, "frame_rate (%d) = vt_pix_clk_hz(%d) / "
		KERN_CONT "(pll_info->frame_length_lines(%d) * pll_info->line_length_pck(%d))\n",
		frame_rate, vt_pix_clk_hz, pll_info->frame_length_lines, pll_info->line_length_pck);

	/* calculate max fps */
	max_fps = (vt_pix_clk_hz * 10) / (pll_info->frame_length_lines * pll_info->line_length_pck);
	max_fps = (max_fps % 10 >= 5 ? frame_rate + 1 : frame_rate);

	cis_data->pclk = vt_pix_clk_hz;
	cis_data->max_fps = max_fps;
	cis_data->frame_length_lines = pll_info->frame_length_lines;
	cis_data->line_length_pck = pll_info->line_length_pck;
	cis_data->line_readOut_time = sensor_cis_do_div64((u64)cis_data->line_length_pck * (u64)(1000 * 1000 * 1000), cis_data->pclk);
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;
	cis_data->stream_on = false;

	/* Frame valid time calcuration */
	frame_valid_us = sensor_cis_do_div64((u64)cis_data->cur_height * (u64)cis_data->line_length_pck * (u64)(1000 * 1000), cis_data->pclk);
	cis_data->frame_valid_us_time = (int)frame_valid_us;

	dbg_sensor(1, "%s\n", __func__);
	dbg_sensor(1, "Sensor size(%d x %d) setting: SUCCESS!\n",
					cis_data->cur_width, cis_data->cur_height);
	dbg_sensor(1, "Frame Valid(us): %d\n", frame_valid_us);
	dbg_sensor(1, "rolling_shutter_skew: %lld\n", cis_data->rolling_shutter_skew);

	dbg_sensor(1, "Fps: %d, max fps(%d)\n", frame_rate, cis_data->max_fps);
	dbg_sensor(1, "min_frame_time(%d us)\n", cis_data->min_frame_us_time);
	dbg_sensor(1, "Pixel rate(Mbps): %d\n", cis_data->pclk / 1000000);

	/* Frame period calculation */
	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;

	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n", __func__,
	cis_data->frame_time, cis_data->rolling_shutter_skew);

	/* Constant values */
	cis_data->min_fine_integration_time = SENSOR_GC08A3_FINE_INTEGRATION_TIME_MIN;
	cis_data->max_fine_integration_time = SENSOR_GC08A3_FINE_INTEGRATION_TIME_MAX;
	cis_data->min_coarse_integration_time = SENSOR_GC08A3_COARSE_INTEGRATION_TIME_MIN;
	cis_data->max_margin_coarse_integration_time = SENSOR_GC08A3_COARSE_INTEGRATION_TIME_MAX_MARGIN;
	info("%s: done", __func__);
}

static int sensor_gc08a3_wait_stream_off_status(cis_shared_data *cis_data)
{
	int ret = 0;
	u32 timeout = 0;

	BUG_ON(!cis_data);

#define STREAM_OFF_WAIT_TIME 250
	while (timeout < STREAM_OFF_WAIT_TIME) {
		if (cis_data->is_active_area == false &&
				cis_data->stream_on == false) {
			pr_debug("actual stream off\n");
			break;
		}
		timeout++;
	}

	if (timeout == STREAM_OFF_WAIT_TIME) {
		pr_err("actual stream off wait timeout\n");
		ret = -1;
	}

	return ret;
}

/* CIS OPS */
int sensor_gc08a3_cis_init(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	u32 setfile_index = 0;
	cis_setting_info setinfo;

	setinfo.param = NULL;
	setinfo.return_value = 0;

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	BUG_ON(!cis->cis_data);
	memset(cis->cis_data, 0, sizeof(cis_shared_data));
	cis->rev_flag = false;

	cis->cis_data->cur_width = SENSOR_GC08A3_MAX_WIDTH;
	cis->cis_data->cur_height = SENSOR_GC08A3_MAX_HEIGHT;
	cis->cis_data->low_expo_start = 33000;
	cis->need_mode_change = false;
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
	cis->mipi_clock_index_new = CAM_MIPI_NOT_INITIALIZED;
#endif

	sensor_gc08a3_cis_data_calculation(sensor_gc08a3_pllinfos[setfile_index], cis->cis_data);

	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min dgain : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max dgain : %d\n", __func__, setinfo.return_value);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_log_status(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client = NULL;
	u8 data8 = 0;
	u16 data16 = 0;

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -ENODEV;
		goto p_err;
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -ENODEV;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	pr_info("[%s] *******************************\n", __func__);
	ret = is_sensor_read16(client, 0x0000, &data16);
	if (unlikely(!ret)) pr_info("model_id(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0002, &data8);
	if (unlikely(!ret)) pr_info("revision_number(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0005, &data8);
	if (unlikely(!ret)) pr_info("frame_count(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0100, &data8);
	if (unlikely(!ret)) pr_info("0x0100(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0136, &data16);
	if (unlikely(!ret)) pr_info("0x0136(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0202, &data16);
	if (unlikely(!ret)) pr_info("0x0202(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0204, &data16);
	if (unlikely(!ret)) pr_info("0x0204(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0340, &data16);
	if (unlikely(!ret)) pr_info("0x0340(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x3C02, &data8);
	if (unlikely(!ret)) pr_info("0x3C02(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x3C03, &data8);
	if (unlikely(!ret)) pr_info("0x3C03(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x3C05, &data8);
	if (unlikely(!ret)) pr_info("0x3C05(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x3500, &data8);
	if (unlikely(!ret)) pr_info("0x3500(0x%x)\n", data8);
	else goto i2c_err;
	pr_info("[%s] *******************************\n", __func__);

i2c_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);
p_err:
	return ret;
}

int sensor_gc08a3_cis_set_global_setting(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	struct is_core *core = NULL;

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		err("[GC08A3] in Global setting the core device is null");
		return -EINVAL;
	}

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	BUG_ON(!cis);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = sensor_cis_set_registers(subdev, sensor_gc08a3_global, sensor_gc08a3_global_size);

	if (ret < 0) {
		err("sensor_gc08a3_set_registers fail!!");
		goto p_err;
	}

#if 0 // A21s does not use dual mode
	/* Sensor Dual sync on/off */
	if(test_bit(IS_SENSOR_OPEN, &(core->sensor[1].state)))
	{
		info("[%s]dual sync slave mode\n", __func__);
		ret = sensor_cis_set_registers(subdev, sensor_gc08a3_dualsync_slave, sensor_gc08a3_dualsync_slave_size);
		if (ret < 0)
			err("[%s] sensor_gc08a3_dualsync_slave fail\n", __func__);
	}
	else
	{
		warn("%s dualsync mode master mode\n", __func__);
		ret = sensor_cis_set_registers(subdev, sensor_gc08a3_dualsync_master, sensor_gc08a3_dualsync_master_size);
		if (ret < 0)
			err("[%s] sensor_gc08a3_dualsync_master fail\n", __func__);
	}
#endif
	info("[%s] global setting done\n", __func__);

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

int sensor_gc08a3_cis_mode_change(struct v4l2_subdev *subdev, u32 mode)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	if (mode > sensor_gc08a3_max_setfile_num) {
		err("invalid mode(%d)!!", mode);
		ret = -EINVAL;
		goto p_err;
	}

	/* If check_rev fail when cis_init, one more check_rev in mode_change */
	if (cis->rev_flag == true) {
		cis->rev_flag = false;
		ret = sensor_cis_check_rev(cis);
		if (ret < 0) {
			err("sensor_gc08a3_check_rev is fail");
			goto p_err;
		}
	}

	sensor_gc08a3_cis_data_calculation(sensor_gc08a3_pllinfos[mode], cis->cis_data);

#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
#endif
	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = sensor_cis_set_registers(subdev, sensor_gc08a3_setfiles[mode], sensor_gc08a3_setfile_sizes[mode]);
	if (ret < 0) {
		err("sensor_gc08a3_set_registers fail!!");
		goto p_err;
	}

	info("[%s] mode changed(%d)\n", __func__, mode);

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

/* TODO: Sensor set size sequence(sensor done, sensor stop, 3AA done in FW case */
int sensor_gc08a3_cis_set_size(struct v4l2_subdev *subdev, cis_shared_data *cis_data)
{
	int ret = 0;
	bool binning = false;
	u32 ratio_w = 0, ratio_h = 0, start_x = 0, start_y = 0, end_x = 0, end_y = 0;
	struct i2c_client *client = NULL;
	struct is_cis *cis = NULL;
#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif
	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	BUG_ON(!cis);

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	if (unlikely(!cis_data)) {
		err("cis data is NULL");
		if (unlikely(!cis->cis_data)) {
			ret = -EINVAL;
			goto p_err;
		} else {
			cis_data = cis->cis_data;
		}
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	/* Wait actual stream off */
	ret = sensor_gc08a3_wait_stream_off_status(cis_data);
	if (ret) {
		err("Must stream off\n");
		ret = -EINVAL;
		goto p_err;
	}

	binning = cis_data->binning;
	if (binning) {
		ratio_w = (SENSOR_GC08A3_MAX_WIDTH / cis_data->cur_width);
		ratio_h = (SENSOR_GC08A3_MAX_HEIGHT / cis_data->cur_height);
	} else {
		ratio_w = 1;
		ratio_h = 1;
	}

	if (((cis_data->cur_width * ratio_w) > SENSOR_GC08A3_MAX_WIDTH) ||
		((cis_data->cur_height * ratio_h) > SENSOR_GC08A3_MAX_HEIGHT)) {
		err("Config max sensor size over~!!\n");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* 1. pixel address region setting */
	start_x = ((SENSOR_GC08A3_MAX_WIDTH - cis_data->cur_width * ratio_w) / 2) & (~0x1);
	start_y = ((SENSOR_GC08A3_MAX_HEIGHT - cis_data->cur_height * ratio_h) / 2) & (~0x1);
	end_x = start_x + (cis_data->cur_width * ratio_w - 1);
	end_y = start_y + (cis_data->cur_height * ratio_h - 1);

	if (!(end_x & (0x1)) || !(end_y & (0x1))) {
		err("Sensor pixel end address must odd\n");
		ret = -EINVAL;
		goto p_err;
	}
	/*2 byte address for writing the start_x*/
	ret = is_sensor_write8(client, 0x0344, ((start_x>>8) & 0xF));
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x0345, (start_x & 0xFE));
	if (ret < 0)
		goto p_err;

	/*2 byte address for writing the start_y*/
	ret = is_sensor_write8(client, 0x0346, ((start_y>>8) & 0xF));
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write8(client, 0x0347, (start_y & 0xFF));
	if (ret < 0)
		goto p_err;

	/*2 byte address for writing the end_x*/
	ret = is_sensor_write8(client, 0x0348, ((end_x>>8) & 0xF));
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x0349, (end_x & 0xFE));
	if (ret < 0)
		goto p_err;

	/*2 byte address for writing the end_y*/
	ret = is_sensor_write8(client, 0x034A, ((end_y>>8) & 0xF));
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x034B, (end_y & 0xFF));
	if (ret < 0)
		goto p_err;

	/* 3. output address setting width */
	ret = is_sensor_write8(client, 0x034C, ((cis_data->cur_width >> 8) & 0xF));
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x034D, (cis_data->cur_width & 0xFF));
	if (ret < 0)
		goto p_err;

	/* 4. output address setting height*/
	ret = is_sensor_write8(client, 0x034E, ((cis_data->cur_height >> 8) & 0xF));
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x034F, (cis_data->cur_height & 0xFF));
	if (ret < 0)
		goto p_err;

	/* If not use to binning, sensor image should set only crop */
	if (!binning) {
		dbg_sensor(1, "Sensor size set is not binning\n");
		goto p_err;
	}

	/* 5. binnig setting */
	ret = is_sensor_write8(client, 0x0900, binning);	/* 1:  binning enable, 0: disable */
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x0901, (ratio_w << 4) | ratio_h);
	if (ret < 0)
		goto p_err;

	/* 6. scaling setting: but not use */
	/* scaling_mode (0: No scaling, 1: Horizontal, 2: Full) */
	ret = is_sensor_write16(client, 0x0400, 0x0000);
	if (ret < 0)
		goto p_err;
	/* down_scale_m: 1 to 16 upwards (scale_n: 16(fixed)) */
	/* down scale factor = down_scale_m / down_scale_n */
	ret = is_sensor_write16(client, 0x0404, 0x0010);
	if (ret < 0)
		goto p_err;

	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis->cis_data->rolling_shutter_skew = (cis->cis_data->cur_height - 1) * cis->cis_data->line_readOut_time;
	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n",
		__func__, cis->cis_data->frame_time, cis->cis_data->rolling_shutter_skew);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec) * 1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

int sensor_gc08a3_cis_stream_on(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	sensor_gc08a3_cis_set_mipi_clock(subdev);
#endif

#ifdef DEBUG_GC08A3_PLL
	{
	u16 pll;
	is_sensor_read16(client, 0x0300, &pll);
	dbg_sensor(1, "______ vt_pix_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0302, &pll);
	dbg_sensor(1, "______ vt_sys_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0304, &pll);
	dbg_sensor(1, "______ pre_pll_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0306, &pll);
	dbg_sensor(1, "______ pll_multiplier(%x)\n", pll);
	is_sensor_read16(client, 0x0308, &pll);
	dbg_sensor(1, "______ op_pix_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x030a, &pll);
	dbg_sensor(1, "______ op_sys_clk_div(%x)\n", pll);

	is_sensor_read16(client, 0x030c, &pll);
	dbg_sensor(1, "______ secnd_pre_pll_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x030e, &pll);
	dbg_sensor(1, "______ secnd_pll_multiplier(%x)\n", pll);
	is_sensor_read16(client, 0x0340, &pll);
	dbg_sensor(1, "______ frame_length_lines(%x)\n", pll);
	is_sensor_read16(client, 0x0342, &pll);
	dbg_sensor(1, "______ line_length_pck(%x)\n", pll);
	}
#endif

	msleep(50); /* first frame should be delayed */

	/* Sensor stream on */
	I2C_MUTEX_LOCK(cis->i2c_lock);
	is_sensor_write8(client, 0x0100, 0x01);

	info("%s\n", __func__);

	cis_data->stream_on = true;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

int sensor_gc08a3_cis_stream_off(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	/* Sensor stream off */
	is_sensor_write8(client, 0x0100, 0x00);

	info("%s\n", __func__);

	cis_data->stream_on = false;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

int sensor_gc08a3_cis_wait_streamoff(struct v4l2_subdev *subdev)
{
	int ret = 0;
	u32 poll_time_ms = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	cis_data = cis->cis_data;
	FIMC_BUG(!cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		return ret;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);
	/* Checking stream off */
	do {
		u8 frame_counter_msb = 0;
		u8 frame_counter_lsb = 0;

		/* Sensor stream off */
		ret = is_sensor_read8(client, 0x146, &frame_counter_msb);
		if (ret < 0) {
			err("i2c transfer fail addr(%x) ret = %d\n", 0x146, ret);
			goto p_err;
		}

		ret = is_sensor_read8(client, 0x147, &frame_counter_lsb);
		if (ret < 0) {
			err("i2c transfer fail addr(%x) ret = %d\n", 0x147, ret);
			goto p_err;
		}

		/* frame count == 0 when stream off */
		if (frame_counter_msb == 0x00 && frame_counter_lsb == 0x00)
			break;

		usleep_range(POLL_TIME_US, POLL_TIME_US);
		poll_time_ms += POLL_TIME_MS;
	} while (poll_time_ms < STREAM_OFF_POLL_TIME_MS);

	if (poll_time_ms < STREAM_OFF_POLL_TIME_MS)
		info("%s: finished after %d ms\n", __func__, poll_time_ms);
	else
		warn("%s: finished : polling timeout occured after %d ms\n", __func__, poll_time_ms);

p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

int sensor_gc08a3_cis_set_exposure_time(struct v4l2_subdev *subdev, struct ae_param *target_exposure)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u32 vt_pic_clk_freq_mhz = 0;
	u16 long_coarse_int = 0;
	u16 short_coarse_int = 0;
	u32 line_length_pck = 0;
	u32 fine_int = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!target_exposure);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if ((target_exposure->long_val <= 0) || (target_exposure->short_val <= 0)) {
		err("[%s] invalid target exposure(%d, %d)\n", __func__,
				target_exposure->long_val, target_exposure->short_val);
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), target long(%d), short(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, target_exposure->long_val, target_exposure->short_val);

	vt_pic_clk_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;
	fine_int = cis_data->min_fine_integration_time;

	long_coarse_int = ((target_exposure->long_val * vt_pic_clk_freq_mhz) - fine_int) / line_length_pck;
	short_coarse_int = ((target_exposure->short_val * vt_pic_clk_freq_mhz) - fine_int) / line_length_pck;

	if (long_coarse_int > cis_data->max_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) max(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, long_coarse_int, cis_data->max_coarse_integration_time);
		long_coarse_int = cis_data->max_coarse_integration_time;
	}

	if (short_coarse_int > cis_data->max_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), short coarse(%d) max(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, short_coarse_int, cis_data->max_coarse_integration_time);
		short_coarse_int = cis_data->max_coarse_integration_time;
	}

	if (long_coarse_int < cis_data->min_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) min(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, long_coarse_int, cis_data->min_coarse_integration_time);
		long_coarse_int = cis_data->min_coarse_integration_time;
	}

	if (short_coarse_int < cis_data->min_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), short coarse(%d) min(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, short_coarse_int, cis_data->min_coarse_integration_time);
		short_coarse_int = cis_data->min_coarse_integration_time;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	short_coarse_int = (short_coarse_int / 2)*2;
	/* Short exposure */
	ret = is_sensor_write8(client, 0x202, (short_coarse_int >> 8) & 0xff);
	if (ret < 0)
		goto p_err_unlock;
	ret = is_sensor_write8(client, 0x203, (short_coarse_int & 0xff));
	if (ret < 0)
		goto p_err_unlock;

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), vt_pic_clk_freq_mhz (%d),"
		KERN_CONT "line_length_pck(%d), fine_int (%d)\n", cis->id, __func__,
		cis_data->sen_vsync_count, vt_pic_clk_freq_mhz, line_length_pck, fine_int);
	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), frame_length_lines(%#x),"
		KERN_CONT "long_coarse_int %#x, short_coarse_int %#x\n", cis->id, __func__,
		cis_data->sen_vsync_count, cis_data->frame_length_lines, long_coarse_int, short_coarse_int);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_min_exposure_time(struct v4l2_subdev *subdev, u32 *min_expo)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	cis_shared_data *cis_data = NULL;
	u32 min_integration_time = 0;
	u32 min_coarse = 0;
	u32 min_fine = 0;
	u32 vt_pic_clk_freq_mhz = 0;
	u32 line_length_pck = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!min_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_mhz = cis_data->pclk / (1000 * 1000);
	if (vt_pic_clk_freq_mhz == 0) {
		pr_err("[MOD:D:%d] %s, Invalid vt_pic_clk_freq_mhz(%d)\n", cis->id, __func__, vt_pic_clk_freq_mhz);
		goto p_err;
	}
	line_length_pck = cis_data->line_length_pck;
	min_coarse = cis_data->min_coarse_integration_time;
	min_fine = cis_data->min_fine_integration_time;

	min_integration_time = ((line_length_pck * min_coarse) + min_fine) / vt_pic_clk_freq_mhz;
	*min_expo = min_integration_time;

	dbg_sensor(1, "[%s] min integration time %d\n", __func__, min_integration_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_max_exposure_time(struct v4l2_subdev *subdev, u32 *max_expo)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u32 max_integration_time = 0;
	u32 max_coarse_margin = 0;
	u32 max_fine_margin = 0;
	u32 max_coarse = 0;
	u32 max_fine = 0;
	u32 vt_pic_clk_freq_mhz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!max_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_mhz = cis_data->pclk / (1000 * 1000);
	if (vt_pic_clk_freq_mhz == 0) {
		pr_err("[MOD:D:%d] %s, Invalid vt_pic_clk_freq_mhz(%d)\n", cis->id, __func__, vt_pic_clk_freq_mhz);
		goto p_err;
	}
	line_length_pck = cis_data->line_length_pck;
	frame_length_lines = cis_data->frame_length_lines;

	max_coarse_margin = cis_data->max_margin_coarse_integration_time;
	max_fine_margin = line_length_pck - cis_data->min_fine_integration_time;
	max_coarse = frame_length_lines - max_coarse_margin;
	max_fine = cis_data->max_fine_integration_time;

	max_integration_time = ((line_length_pck * max_coarse) + max_fine) / vt_pic_clk_freq_mhz;

	*max_expo = max_integration_time;

	/* TODO: Is this values update hear? */
	cis_data->max_margin_fine_integration_time = max_fine_margin;
	cis_data->max_coarse_integration_time = max_coarse;

	dbg_sensor(1, "[%s] max integration time %d, max margin fine integration %d, max coarse integration %d\n",
			__func__, max_integration_time, cis_data->max_margin_fine_integration_time, cis_data->max_coarse_integration_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_adjust_frame_duration(struct v4l2_subdev *subdev,
						u32 input_exposure_time,
						u32 *target_duration)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 vt_pic_clk_freq_mhz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;
	u32 frame_duration = 0;
	u32 max_frame_us_time = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!target_duration);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;
	if (input_exposure_time > SENSOR_GC08A3_EXPOSURE_TIME_MAX) {
		err("input_exposure_time is out of bound (%d -> %d)", input_exposure_time, SENSOR_GC08A3_EXPOSURE_TIME_MAX);
		input_exposure_time = SENSOR_GC08A3_EXPOSURE_TIME_MAX;
	}
	frame_length_lines = ((vt_pic_clk_freq_mhz * input_exposure_time) / line_length_pck);
	frame_length_lines += cis_data->max_margin_coarse_integration_time;
	max_frame_us_time = 1000000/cis->min_fps;

	frame_duration = (frame_length_lines * line_length_pck) / vt_pic_clk_freq_mhz;

	dbg_sensor(1, "[%s](vsync cnt = %d) input exp(%d), adj duration, frame duraion(%d), min_frame_us(%d)\n",
			__func__, cis_data->sen_vsync_count, input_exposure_time, frame_duration, cis_data->min_frame_us_time);
	dbg_sensor(1, "[%s](vsync cnt = %d) adj duration, frame duraion(%d), min_frame_us(%d)\n",
			__func__, cis_data->sen_vsync_count, frame_duration, cis_data->min_frame_us_time);

	*target_duration = MAX(frame_duration, cis_data->min_frame_us_time);
		if(cis->min_fps == cis->max_fps) {
				*target_duration = MIN(frame_duration, max_frame_us_time);
		}
#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gc08a3_cis_set_frame_duration(struct v4l2_subdev *subdev, u32 frame_duration)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u32 vt_pic_clk_freq_mhz = 0;
	u32 line_length_pck = 0;
	u16 frame_length_lines = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	if (frame_duration < cis_data->min_frame_us_time) {
		dbg_sensor(1, "frame duration is less than min(%d)\n", frame_duration);
		frame_duration = cis_data->min_frame_us_time;
	}

	vt_pic_clk_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;

	frame_length_lines = (u16)((vt_pic_clk_freq_mhz * frame_duration) / line_length_pck);
	/* Frame length lines should be a multiple of 2 */
	frame_length_lines = MULTIPLE_OF_2(frame_length_lines);

	dbg_sensor(1, "[MOD:D:%d] %s, vt_pic_clk_freq_mhz(%#x) frame_duration = %d us,"
		KERN_CONT "(line_length_pck%#x), frame_length_lines(%#x)\n",
		cis->id, __func__, vt_pic_clk_freq_mhz, frame_duration, line_length_pck, frame_length_lines);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_write8(client, 0x0340, ((frame_length_lines>>8) & 0xFF));
	if (ret < 0)
		goto p_err_unlock;

	ret = is_sensor_write8(client, 0x0341, (frame_length_lines & 0xFF));
	if (ret < 0)
		goto p_err_unlock;

	cis_data->cur_frame_us_time = frame_duration;
	cis_data->frame_length_lines = frame_length_lines;
	cis_data->max_coarse_integration_time = cis_data->frame_length_lines - cis_data->max_margin_coarse_integration_time;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gc08a3_cis_set_frame_rate(struct v4l2_subdev *subdev, u32 min_fps)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 frame_duration = 0;
	u32 cur_mode_min_duration = 0;
	const struct sensor_pll_info_compact *pll_info = NULL;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	cis_data = cis->cis_data;

	if (min_fps > cis_data->max_fps) {
		err("[MOD:D:%d] %s, request FPS is too high(%d), set to max(%d)\n",
			cis->id, __func__, min_fps, cis_data->max_fps);
		min_fps = cis_data->max_fps;
	}

	if (min_fps == 0) {
		err("[MOD:D:%d] %s, request FPS is 0, set to min FPS(1)\n",
			cis->id, __func__);
		min_fps = 1;
	}

	frame_duration = (1 * 1000 * 1000) / min_fps;

	dbg_sensor(1, "[MOD:D:%d] %s, set FPS(%d), frame duration(%d)\n",
			cis->id, __func__, min_fps, frame_duration);

	ret = sensor_gc08a3_cis_set_frame_duration(subdev, frame_duration);
	if (ret < 0) {
		err("[MOD:D:%d] %s, set frame duration is fail(%d)\n",
			cis->id, __func__, ret);
		goto p_err;
	}

	if (cis_data->sens_config_index_cur <= sensor_gc08a3_max_setfile_num) {
		pll_info = sensor_gc08a3_pllinfos[cis_data->sens_config_index_cur];
	} else {
		err("[MOD:D:%d] %s, current sensor mode is invalid(%d)\n",
			cis->id, __func__, cis_data->sens_config_index_cur);
	}

	if (pll_info != NULL) {
		cur_mode_min_duration = (pll_info->frame_length_lines * pll_info->line_length_pck
			/ (cis_data->pclk / (1000 * 1000)));
	}

	if (frame_duration < cur_mode_min_duration)
		cis_data->min_frame_us_time = cur_mode_min_duration;
	else
		cis_data->min_frame_us_time = frame_duration;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:

	return ret;
}

int sensor_gc08a3_cis_adjust_analog_gain(struct v4l2_subdev *subdev, u32 input_again, u32 *target_permile)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 again_code = 0;
	u32 again_permile = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!target_permile);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	cis_data = cis->cis_data;

	again_code = sensor_gc08a3_cis_calc_again_code(input_again);

	if (again_code > cis_data->max_analog_gain[0]) {
		again_code = cis_data->max_analog_gain[0];
	} else if (again_code < cis_data->min_analog_gain[0]) {
		again_code = cis_data->min_analog_gain[0];
	}

	again_permile = sensor_gc08a3_cis_calc_again_permile(again_code);

	dbg_sensor(1, "[%s] min again(%d), max(%d), input_again(%d), code(%d), permile(%d)\n", __func__,
			cis_data->max_analog_gain[0],
			cis_data->min_analog_gain[0],
			input_again,
			again_code,
			again_permile);

	*target_permile = again_permile;

	return ret;
}

int sensor_gc08a3_cis_set_analog_gain(struct v4l2_subdev *subdev, struct ae_param *again)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	analog_gain = (u16)sensor_gc08a3_cis_calc_again_code(again->val);

	if (analog_gain < cis->cis_data->min_analog_gain[0]) {
		analog_gain = cis->cis_data->min_analog_gain[0];
	}

	if (analog_gain > cis->cis_data->max_analog_gain[0]) {
		analog_gain = cis->cis_data->max_analog_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d), input_again = %d us, analog_gain(%#x)\n",
		cis->id, __func__, cis->cis_data->sen_vsync_count, again->val, analog_gain);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_write8(client, 0x0204, ((analog_gain>>8) & 0xFF));
	if (ret < 0)
		goto p_err_unlock;

	ret = is_sensor_write8(client, 0x0205, (analog_gain & 0xFF));
	if (ret < 0)
		goto p_err_unlock;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_analog_gain(struct v4l2_subdev *subdev, u32 *again)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;
	u8 analog_gain_low = 0,analog_gain_high = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_read8(client, 0x0205, &analog_gain_low);
	if (ret < 0)
		goto p_err_unlock;

	ret = is_sensor_read8(client, 0x0204, &analog_gain_high);
	if (ret < 0)
		goto p_err_unlock;

	analog_gain = (u16)analog_gain_high;
	analog_gain = ((analog_gain<<8) | analog_gain_low);

	*again = sensor_gc08a3_cis_calc_again_permile(analog_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_again = %d us, analog_gain(%#x)\n",
			cis->id, __func__, *again, analog_gain);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_min_analog_gain(struct v4l2_subdev *subdev, u32 *min_again)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!min_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	cis_data->min_analog_gain[0] = 0x0400; /* x1, gain=x/0x400 */
	cis_data->min_analog_gain[1] = sensor_gc08a3_cis_calc_again_permile(cis_data->min_analog_gain[0]);

	*min_again = cis_data->min_analog_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->min_analog_gain[0], cis_data->min_analog_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_max_analog_gain(struct v4l2_subdev *subdev, u32 *max_again)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!max_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;
	cis_data->max_analog_gain[0] = 0x4000; /* x16, gain=x/0x20 */
	cis_data->max_analog_gain[1] = sensor_gc08a3_cis_calc_again_permile(cis_data->max_analog_gain[0]);

	*max_again = cis_data->max_analog_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->max_analog_gain[0], cis_data->max_analog_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_set_digital_gain(struct v4l2_subdev *subdev, struct ae_param *dgain)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u16 long_gain = 0;
	u16 short_gain = 0;
	u16 dgains[2] = {0};

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	long_gain = (u16)sensor_cis_calc_dgain_code(dgain->long_val);
	short_gain = (u16)sensor_cis_calc_dgain_code(dgain->short_val);

	if (long_gain < cis->cis_data->min_digital_gain[0]) {
		long_gain = cis->cis_data->min_digital_gain[0];
	}
	if (long_gain > cis->cis_data->max_digital_gain[0]) {
		long_gain = cis->cis_data->max_digital_gain[0];
	}

	if (short_gain < cis->cis_data->min_digital_gain[0]) {
		short_gain = cis->cis_data->min_digital_gain[0];
	}
	if (short_gain > cis->cis_data->max_digital_gain[0]) {
		short_gain = cis->cis_data->max_digital_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d), input_dgain = %d/%d us, long_gain(%#x), short_gain(%#x)\n",
			cis->id, __func__, cis->cis_data->sen_vsync_count, dgain->long_val, dgain->short_val, long_gain, short_gain);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	dgains[0] = dgains[1] = short_gain;

	/* Short digital gain */
	ret = is_sensor_write8(client, 0x020E, (short_gain>>8) & 0x3F);
	if (ret < 0)
		goto p_err_unlock;

	ret = is_sensor_write8(client, 0x020F, short_gain & 0xFF);
	if (ret < 0)
		goto p_err_unlock;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_digital_gain(struct v4l2_subdev *subdev, u32 *dgain)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 digital_gain = 0;
	u8 digital_gain_low = 0, digital_gain_high = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_read8(client, 0x020E, &digital_gain_high);
	if (ret < 0)
		goto p_err_unlock;

	ret = is_sensor_read8(client, 0x020F, &digital_gain_low);
	if (ret < 0)
		goto p_err_unlock;

	digital_gain = (u16)digital_gain_high;
	digital_gain = (digital_gain<<8 | digital_gain_low);

	*dgain = sensor_cis_calc_dgain_permile(digital_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_dgain = %d us, digital_gain(%#x)\n",
			cis->id, __func__, *dgain, digital_gain);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_min_digital_gain(struct v4l2_subdev *subdev, u32 *min_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!min_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	cis_data->min_digital_gain[0] = 0x0400;
	cis_data->min_digital_gain[1] = sensor_cis_calc_dgain_permile(cis_data->min_digital_gain[0]);
	*min_dgain = cis_data->min_digital_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->min_digital_gain[0], cis_data->min_digital_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_get_max_digital_gain(struct v4l2_subdev *subdev, u32 *max_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	BUG_ON(!subdev);
	BUG_ON(!max_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	BUG_ON(!cis);
	BUG_ON(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	cis_data->max_digital_gain[0] = 0x3FFF;
	cis_data->max_digital_gain[1] = sensor_cis_calc_dgain_permile(cis_data->max_digital_gain[0]);
	*max_dgain = cis_data->max_digital_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->max_digital_gain[0], cis_data->max_digital_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gc08a3_cis_compensate_gain_for_extremely_br(struct v4l2_subdev *subdev, u32 expo, u32 *again, u32 *dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 vt_pic_clk_freq_mhz = 0;
	u32 line_length_pck = 0;
	u32 min_fine_int = 0;
	u16 coarse_int = 0;
	u32 compensated_again = 0;
	u32 integration_time = 0;

	u32 ratio = 0;
	static u32 pre_ratio = 0;
	static u32 pre_coarse_int = 0;

	BUG_ON(!subdev);
	BUG_ON(!again);
	BUG_ON(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -EINVAL;
		goto p_err;
	}
	cis_data = cis->cis_data;

	vt_pic_clk_freq_mhz = cis_data->pclk / (1000 * 1000);
	line_length_pck = cis_data->line_length_pck;
	min_fine_int = cis_data->min_fine_integration_time;

	if (line_length_pck <= 0) {
		err("[%s] invalid line_length_pck(%d)\n", __func__, line_length_pck);
		goto p_err;
	}

	coarse_int = ((expo * vt_pic_clk_freq_mhz) - (cis_data->line_length_pck - 0xf0)) / line_length_pck;
	if (coarse_int < cis_data->min_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) min(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, coarse_int, cis_data->min_coarse_integration_time);
		coarse_int = cis_data->min_coarse_integration_time;
	}

	integration_time = ((cis_data->line_length_pck * coarse_int + (cis_data->line_length_pck - 0xf0)) / vt_pic_clk_freq_mhz);
	ratio = ((expo << 8) / integration_time);

	if (pre_coarse_int <= 15) {
			compensated_again = (*again * (pre_ratio)) >> 8;

		if (compensated_again < cis_data->min_analog_gain[1]) {
			*again = cis_data->min_analog_gain[1];
		} else if (*again >= cis_data->max_analog_gain[1]) {
			*dgain = (*dgain * (pre_ratio));
		} else {
			*again = compensated_again;
		}

		dbg_sensor(1, "[%s] exp(%d), again(%d), dgain(%d), coarse_int(%d),"
			KERN_CONT "compensated_again(%d), integration_time : (%d)\n",
			__func__, expo, *again, *dgain, coarse_int, compensated_again, integration_time);
	}

	pre_ratio = ratio;
	pre_coarse_int = coarse_int;
p_err:
	return ret;
}

#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
static int sensor_gc08a3_cis_update_mipi_info(struct v4l2_subdev *subdev)
{
	struct is_cis *cis = NULL;
	struct is_device_sensor *device;
	const struct cam_mipi_sensor_mode *cur_mipi_sensor_mode;
	int found = -1;

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (device == NULL) {
		err("device is NULL");
		return -1;
	}

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (cis == NULL) {
		err("cis is NULL");
		return -1;
	}

	if (device->cfg->mode >= sensor_gc08a3_mipi_sensor_mode_size) {
		err("sensor mode is out of bound");
		return -1;
	}

	cur_mipi_sensor_mode = &sensor_gc08a3_mipi_sensor_mode[device->cfg->mode];

	if (cur_mipi_sensor_mode->mipi_channel_size == 0 ||
		cur_mipi_sensor_mode->mipi_channel == NULL) {
		dbg_sensor(1, "skip select mipi channel\n");
		return -1;
	}

	found = is_vendor_select_mipi_by_rf_channel(cur_mipi_sensor_mode->mipi_channel,
				cur_mipi_sensor_mode->mipi_channel_size);
	if (found != -1) {
		if (found < cur_mipi_sensor_mode->sensor_setting_size) {
			device->cfg->mipi_speed = cur_mipi_sensor_mode->sensor_setting[found].mipi_rate;
			cis->mipi_clock_index_new = found;
			info("%s - update mipi rate : %d\n", __func__, device->cfg->mipi_speed);
		} else {
			err("sensor setting size is out of bound");
		}
	}

	return 0;
}

static int sensor_gc08a3_cis_get_mipi_clock_string(struct v4l2_subdev *subdev, char *cur_mipi_str)
{
	struct is_cis *cis = NULL;
	struct is_device_sensor *device;
	const struct cam_mipi_sensor_mode *cur_mipi_sensor_mode;
	int mode = 0;

	cur_mipi_str[0] = '\0';

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (device == NULL) {
		err("device is NULL");
		return -1;
	}

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (cis == NULL) {
		err("cis is NULL");
		return -1;
	}

	if (cis->cis_data->stream_on) {
		mode = cis->cis_data->sens_config_index_cur;

		if (mode >= sensor_gc08a3_mipi_sensor_mode_size) {
			err("sensor mode is out of bound");
			return -1;
		}

		cur_mipi_sensor_mode = &sensor_gc08a3_mipi_sensor_mode[mode];

		if (cur_mipi_sensor_mode->sensor_setting_size == 0 ||
			cur_mipi_sensor_mode->sensor_setting == NULL) {
			err("sensor_setting is not available");
			return -1;
		}

		if (cis->mipi_clock_index_new < 0 ||
			cur_mipi_sensor_mode->sensor_setting[cis->mipi_clock_index_new].str_mipi_clk == NULL) {
			err("mipi_clock_index_new is not available");
			return -1;
		}

		sprintf(cur_mipi_str, "%s",
			cur_mipi_sensor_mode->sensor_setting[cis->mipi_clock_index_new].str_mipi_clk);
	}

	return 0;
}
#endif

int sensor_gc08a3_cis_recover_stream_on(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	info("%s start\n", __func__);

	ret = sensor_gc08a3_cis_set_global_setting(subdev);
	if (ret < 0) goto p_err;
	ret = sensor_gc08a3_cis_mode_change(subdev, cis->cis_data->sens_config_index_cur);
	if (ret < 0) goto p_err;
	ret = sensor_gc08a3_cis_stream_on(subdev);
	if (ret < 0) goto p_err;
	ret = sensor_cis_wait_streamon(subdev);
	if (ret < 0) goto p_err;

	info("%s end\n", __func__);
p_err:
	return ret;
}

static struct is_cis_ops cis_ops = {
	.cis_init = sensor_gc08a3_cis_init,
	.cis_log_status = sensor_gc08a3_cis_log_status,
	.cis_set_global_setting = sensor_gc08a3_cis_set_global_setting,
	.cis_mode_change = sensor_gc08a3_cis_mode_change,
	.cis_set_size = sensor_gc08a3_cis_set_size,
	.cis_stream_on = sensor_gc08a3_cis_stream_on,
	.cis_stream_off = sensor_gc08a3_cis_stream_off,
	.cis_set_exposure_time = sensor_gc08a3_cis_set_exposure_time,
	.cis_get_min_exposure_time = sensor_gc08a3_cis_get_min_exposure_time,
	.cis_get_max_exposure_time = sensor_gc08a3_cis_get_max_exposure_time,
	.cis_adjust_frame_duration = sensor_gc08a3_cis_adjust_frame_duration,
	.cis_set_frame_duration = sensor_gc08a3_cis_set_frame_duration,
	.cis_set_frame_rate = sensor_gc08a3_cis_set_frame_rate,
	.cis_adjust_analog_gain = sensor_gc08a3_cis_adjust_analog_gain,
	.cis_set_analog_gain = sensor_gc08a3_cis_set_analog_gain,
	.cis_get_analog_gain = sensor_gc08a3_cis_get_analog_gain,
	.cis_get_min_analog_gain = sensor_gc08a3_cis_get_min_analog_gain,
	.cis_get_max_analog_gain = sensor_gc08a3_cis_get_max_analog_gain,
	.cis_set_digital_gain = sensor_gc08a3_cis_set_digital_gain,
	.cis_get_digital_gain = sensor_gc08a3_cis_get_digital_gain,
	.cis_get_min_digital_gain = sensor_gc08a3_cis_get_min_digital_gain,
	.cis_get_max_digital_gain = sensor_gc08a3_cis_get_max_digital_gain,
	.cis_compensate_gain_for_extremely_br = sensor_gc08a3_cis_compensate_gain_for_extremely_br,
	.cis_wait_streamoff = sensor_gc08a3_cis_wait_streamoff,
	.cis_wait_streamon = sensor_cis_wait_streamon,
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	.cis_update_mipi_info = sensor_gc08a3_cis_update_mipi_info,
	.cis_get_mipi_clock_string = sensor_gc08a3_cis_get_mipi_clock_string,
#endif
	.cis_set_initial_exposure = sensor_cis_set_initial_exposure,
	.cis_recover_stream_on = sensor_gc08a3_cis_recover_stream_on,
	.cis_check_rev_on_init = sensor_cis_check_rev_on_init,
};

int cis_gc08a3_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret = 0;
	struct is_core *core = NULL;
	struct v4l2_subdev *subdev_cis = NULL;
	struct is_cis *cis = NULL;
	struct is_device_sensor *device = NULL;
	struct is_device_sensor_peri *sensor_peri = NULL;
	u32 sensor_id = 0;
	char const *setfile;
	struct device *dev;
	struct device_node *dnode;
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	int i;
	int index;
#endif

#if defined(CONFIG_VENDER_MCD_V2) || defined(CONFIG_CAMERA_OTPROM_SUPPORT_FRONT) || defined(CONFIG_CAMERA_OTPROM_SUPPORT_REAR)
	struct is_vender_specific *specific = NULL;
	u32 rom_position = 0;
#endif

	BUG_ON(!client);
	BUG_ON(!is_dev);

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		probe_info("core device is not yet probed");
		return -EPROBE_DEFER;
	}

	dev = &client->dev;
	dnode = dev->of_node;

	ret = of_property_read_u32(dnode, "id", &sensor_id);
	if (ret) {
		err("sensor_id read is fail(%d)", ret);
		goto p_err;
	}

	probe_info("%s sensor_id %d\n", __func__, sensor_id);

	device = &core->sensor[sensor_id];

	sensor_peri = find_peri_by_cis_id(device, SENSOR_NAME_GC08A3);
	if (!sensor_peri) {
		probe_info("sensor peri is not yet probed");
		return -EPROBE_DEFER;
	}

	cis = &sensor_peri->cis;
	if (!cis) {
		err("cis is NULL");
		ret = -ENOMEM;
		goto p_err;
	}

	subdev_cis = kzalloc(sizeof(struct v4l2_subdev), GFP_KERNEL);
	if (!subdev_cis) {
		probe_err("subdev_cis is NULL");
		ret = -ENOMEM;
		goto p_err;
	}
	sensor_peri->subdev_cis = subdev_cis;

	cis->id = SENSOR_NAME_GC08A3;
	cis->subdev = subdev_cis;
	cis->device = 0;
	cis->client = client;
	sensor_peri->module->client = cis->client;
	cis->ctrl_delay = N_PLUS_TWO_FRAME;
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
	cis->mipi_clock_index_new = CAM_MIPI_NOT_INITIALIZED;
#endif
	cis->cis_data = kzalloc(sizeof(cis_shared_data), GFP_KERNEL);
	if (!cis->cis_data) {
		err("cis_data is NULL");
		ret = -ENOMEM;
		goto p_err;
	}
	cis->cis_ops = &cis_ops;

#if defined(CONFIG_VENDER_MCD_V2)
	if (of_property_read_bool(dnode, "use_sensor_otp")) {
		ret = of_property_read_u32(dnode, "rom_position", &rom_position);
		if (ret) {
			err("rom_position read is fail(%d)", ret);
		} else {
			specific = core->vender.private_data;
			specific->rom_data[rom_position].rom_type = ROM_TYPE_OTPROM;
			specific->rom_data[rom_position].rom_valid = true;

			if (cis->id == specific->sensor_id[rom_position]) {
				specific->rom_client[rom_position] = cis->client;

				if (vender_rom_addr[rom_position]) {
					specific->rom_cal_map_addr[rom_position] = vender_rom_addr[rom_position];
					probe_info("%s: rom_id=%d, OTP Registered\n", __func__, rom_position);
				} else {
					probe_info("%s: S5KGC08A3 OTP address not defined!\n", __func__);
				}
			} 
#ifdef USE_DUALIZED_OTPROM_SENSOR
			else if (of_property_read_bool(dnode, "dualized_sensor")) {
				specific->dualized_rom_client[rom_position] = cis->client;
				specific->dualized_sensor_id[rom_position] = cis->id;

				if (vender_rom_addr_dualized[rom_position]) {
					specific->dualized_rom_cal_map_addr[rom_position] = vender_rom_addr_dualized[rom_position];
					probe_info("%s: [Dualization] rom_id=%d, OTP Registered\n", __func__, rom_position);
				} else {
					probe_info("%s: [Dualization] S5KGC08A3 OTP address not defined!\n", __func__);
				}
			}
#endif
			else {
				err("%s: sensor id does not match", __func__);
				goto p_err;
			}
		}
	}
#endif

	/* belows are depend on sensor cis. MUST check sensor spec */
	cis->bayer_order = OTF_INPUT_ORDER_BAYER_RG_GB;

	if (of_property_read_bool(dnode, "sensor_f_number")) {
		ret = of_property_read_u32(dnode, "sensor_f_number", &cis->aperture_num);
		if (ret) {
			warn("f-number read is fail(%d)",ret);
		}
	} else {
		cis->aperture_num = F2_2;
	}

	probe_info("%s f-number %d\n", __func__, cis->aperture_num);

	cis->use_dgain = true;
	cis->hdr_ctrl_by_again = false;

	cis->use_initial_ae = of_property_read_bool(dnode, "use_initial_ae");
	probe_info("%s use initial_ae(%d)\n", __func__, cis->use_initial_ae);

	ret = of_property_read_string(dnode, "setfile", &setfile);
	if (ret) {
		err("setfile index read fail(%d), take default setfile!!", ret);
		setfile = "default";
	}

	if (strcmp(setfile, "default") == 0 ||
			strcmp(setfile, "setA") == 0) {
		probe_info("%s setfile_A\n", __func__);
		sensor_gc08a3_global = sensor_gc08a3_setfile_A_Global;
		sensor_gc08a3_global_size = ARRAY_SIZE(sensor_gc08a3_setfile_A_Global);
		sensor_gc08a3_setfiles = sensor_gc08a3_setfiles_A;
		sensor_gc08a3_setfile_sizes = sensor_gc08a3_setfile_A_sizes;
		sensor_gc08a3_pllinfos = sensor_gc08a3_pllinfos_A;
		sensor_gc08a3_max_setfile_num = ARRAY_SIZE(sensor_gc08a3_setfiles_A);
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
		sensor_gc08a3_mipi_sensor_mode = sensor_gc08a3_setfile_A_mipi_sensor_mode;
		sensor_gc08a3_mipi_sensor_mode_size = ARRAY_SIZE(sensor_gc08a3_setfile_A_mipi_sensor_mode);
		sensor_gc08a3_verify_sensor_mode = sensor_gc08a3_setfile_A_verify_sensor_mode;
		sensor_gc08a3_verify_sensor_mode_size = ARRAY_SIZE(sensor_gc08a3_setfile_A_verify_sensor_mode);
#endif
		sensor_gc08a3_dualsync_master = sensor_gc08a3_setfile_A_dualsync_Master;
		sensor_gc08a3_dualsync_master_size = sizeof(sensor_gc08a3_setfile_A_dualsync_Master) / sizeof(sensor_gc08a3_setfile_A_dualsync_Master[0]);
		sensor_gc08a3_dualsync_slave = sensor_gc08a3_setfile_A_dualsync_Slave;
		sensor_gc08a3_dualsync_slave_size = sizeof(sensor_gc08a3_setfile_A_dualsync_Slave) / sizeof(sensor_gc08a3_setfile_A_dualsync_Slave[0]);
	} else if (strcmp(setfile, "setB") == 0) {
		probe_info("%s setfile_B\n", __func__);
		sensor_gc08a3_global = sensor_gc08a3_setfile_B_Global;
		sensor_gc08a3_global_size = ARRAY_SIZE(sensor_gc08a3_setfile_B_Global);
		sensor_gc08a3_setfiles = sensor_gc08a3_setfiles_B;
		sensor_gc08a3_setfile_sizes = sensor_gc08a3_setfile_B_sizes;
		sensor_gc08a3_pllinfos = sensor_gc08a3_pllinfos_B;
		sensor_gc08a3_max_setfile_num = ARRAY_SIZE(sensor_gc08a3_setfiles_B);
	} else {
		err("%s setfile index out of bound, take default (setfile_A)", __func__);
		sensor_gc08a3_global = sensor_gc08a3_setfile_A_Global;
		sensor_gc08a3_global_size = ARRAY_SIZE(sensor_gc08a3_setfile_A_Global);
		sensor_gc08a3_setfiles = sensor_gc08a3_setfiles_A;
		sensor_gc08a3_setfile_sizes = sensor_gc08a3_setfile_A_sizes;
		sensor_gc08a3_pllinfos = sensor_gc08a3_pllinfos_A;
		sensor_gc08a3_max_setfile_num = ARRAY_SIZE(sensor_gc08a3_setfiles_A);
#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
		sensor_gc08a3_mipi_sensor_mode = sensor_gc08a3_setfile_A_mipi_sensor_mode;
		sensor_gc08a3_mipi_sensor_mode_size = ARRAY_SIZE(sensor_gc08a3_setfile_A_mipi_sensor_mode);
		sensor_gc08a3_verify_sensor_mode = sensor_gc08a3_setfile_A_verify_sensor_mode;
		sensor_gc08a3_verify_sensor_mode_size = ARRAY_SIZE(sensor_gc08a3_setfile_A_verify_sensor_mode);
#endif
		sensor_gc08a3_dualsync_master = sensor_gc08a3_setfile_A_dualsync_Master;
		sensor_gc08a3_dualsync_master_size = sizeof(sensor_gc08a3_setfile_A_dualsync_Master) / sizeof(sensor_gc08a3_setfile_A_dualsync_Master[0]);
		sensor_gc08a3_dualsync_slave = sensor_gc08a3_setfile_A_dualsync_Slave;
		sensor_gc08a3_dualsync_slave_size = sizeof(sensor_gc08a3_setfile_A_dualsync_Slave) / sizeof(sensor_gc08a3_setfile_A_dualsync_Slave[0]);
	}

#ifdef USE_CAMERA_MIPI_CLOCK_VARIATION
	for (i = 0; i < sensor_gc08a3_verify_sensor_mode_size; i++) {
		index = sensor_gc08a3_verify_sensor_mode[i];
		if (is_vendor_verify_mipi_channel(sensor_gc08a3_mipi_sensor_mode[index].mipi_channel,
					sensor_gc08a3_mipi_sensor_mode[index].mipi_channel_size)) {
			panic("wrong mipi channel");
			break;
		}
	}
#endif

	v4l2_i2c_subdev_init(subdev_cis, client, &subdev_ops);
	v4l2_set_subdevdata(subdev_cis, cis);
	v4l2_set_subdev_hostdata(subdev_cis, device);
	snprintf(subdev_cis->name, V4L2_SUBDEV_NAME_SIZE, "cis-subdev.%d", cis->id);

	probe_info("%s done\n", __func__);

p_err:
	return ret;
}

static int cis_gc08a3_remove(struct i2c_client *client)
{
	int ret = 0;
	return ret;
}

static const struct of_device_id exynos_is_cis_gc08a3_match[] = {
	{
		.compatible = "samsung,exynos-is-cis-gc08a3",
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_is_cis_gc08a3_match);

static const struct i2c_device_id cis_gc08a3_idt[] = {
	{ SENSOR_NAME, 0 },
	{},
};

static struct i2c_driver cis_gc08a3_driver = {
	.driver = {
		.name	= SENSOR_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = exynos_is_cis_gc08a3_match
	},
	.probe	= cis_gc08a3_probe,
	.remove	= cis_gc08a3_remove,
	.id_table = cis_gc08a3_idt
};
module_i2c_driver(cis_gc08a3_driver);
