/*
 * Synaptics TCM touchscreen driver
 *
 * Copyright (C) 2017-2018 Synaptics Incorporated. All rights reserved.
 *
 * Copyright (C) 2017-2018 Scott Lin <scott.lin@tw.synaptics.com>
 * Copyright (C) 2018-2019 Ian Su <ian.su@tw.synaptics.com>
 * Copyright (C) 2018-2019 Joey Zhou <joey.zhou@synaptics.com>
 * Copyright (C) 2018-2019 Yuehao Qiu <yuehao.qiu@synaptics.com>
 * Copyright (C) 2018-2019 Aaron Chen <aaron.chen@tw.synaptics.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * INFORMATION CONTAINED IN THIS DOCUMENT IS PROVIDED "AS-IS," AND SYNAPTICS
 * EXPRESSLY DISCLAIMS ALL EXPRESS AND IMPLIED WARRANTIES, INCLUDING ANY
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE,
 * AND ANY WARRANTIES OF NON-INFRINGEMENT OF ANY INTELLECTUAL PROPERTY RIGHTS.
 * IN NO EVENT SHALL SYNAPTICS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, PUNITIVE, OR CONSEQUENTIAL DAMAGES ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OF THE INFORMATION CONTAINED IN THIS DOCUMENT, HOWEVER CAUSED
 * AND BASED ON ANY THEORY OF LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, AND EVEN IF SYNAPTICS WAS ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE. IF A TRIBUNAL OF COMPETENT JURISDICTION DOES
 * NOT PERMIT THE DISCLAIMER OF DIRECT DAMAGES OR ANY OTHER DAMAGES, SYNAPTICS'
 * TOTAL CUMULATIVE LIABILITY TO ANY PARTY SHALL NOT EXCEED ONE HUNDRED U.S.
 * DOLLARS.
 */

#ifndef _OVT_TCM_H_
#define _OVT_TCM_H_

#if IS_ENABLED(CONFIG_SPI_MT65XX)
#include <linux/platform_data/spi-mt65xx.h>
#endif

#define I2C_MODULE_NAME "ovt_tcm_i2c"
#define SPI_MODULE_NAME "ovt_tcm_spi"

struct ovt_tcm_board_data {
	bool x_flip;
	bool y_flip;
	bool swap_axes;
	int irq_gpio;
	int irq_on_state;
	int cs_gpio;
	int power_gpio;
	int power_on_state;
	int reset_gpio;
	int reset_on_state;
	int tpio_reset_gpio;
	unsigned int spi_mode;
	unsigned int power_delay_ms;
	unsigned int reset_delay_ms;
	unsigned int reset_active_ms;
	unsigned int byte_delay_us;
	unsigned int block_delay_us;
	unsigned int ubl_i2c_addr;
	unsigned int ubl_max_freq;
	unsigned int ubl_byte_delay_us;
	unsigned long irq_flags;
	const char *pwr_reg_name;
	const char *bus_reg_name;
	const char *fw_name;
	const char *regulator_lcd_vdd;
	const char *regulator_lcd_reset;
	const char *regulator_lcd_bl;
	const char *regulator_lcd_vsp;
	const char *regulator_lcd_vsn;
	const char *regulator_tsp_reset;
	struct pinctrl *pinctrl;
	u32	area_indicator;
	u32	area_navigation;
	u32	area_edge;
	bool enable_settings_aot;
	bool support_ear_detect;
	bool prox_lp_scan_enabled;
	bool enable_sysinput_enabled;
	bool support_spay_gesture;
	unsigned int scrub_id;
	bool support_cs_gpio_control;
#if IS_ENABLED(CONFIG_SPI_MT65XX)
	struct mtk_chip_config spi_ctrl;
#endif
};

#endif
