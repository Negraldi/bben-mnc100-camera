// SPDX-License-Identifier: GPL-2.0
/*
 * ar0330 driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 * V0.0X01.0X01 add enum_frame_interval function.
 *
 * Modified in 2026 by Negraldi for the BBEN MNC100 mini PC (Intel Cherry Trail
 * atomisp ISP), based on kitakar5525/surface3-atomisp-cameras:
 *  - ported to the Linux 6.12 V4L2 subdev and atomisp APIs;
 *  - atomisp-ar0330.c is generated from this file by gen_variant.py, which replaces
 *    the register tables and timing with the values the Windows driver
 *    (ar0330.sys 21.10586.6069.2007) programs: 19.2 MHz input clock, RAW10, and its
 *    sensor modes; adds runtime vertical blanking down to 16 lines, a per-mode pixel
 *    rate, and caching of controls set while the sensor is powered off.
 * See docs/REVERSE-ENGINEERING.md.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <linux/atomisp_gmin_platform.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x01)

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

/* 74.25Mhz */
#define AR0330_PIXEL_RATE		(135680000U)
#define AR0330_XVCLK_FREQ		19200000
#define AR0330_LINK_FREQ		329600000U

#define CHIP_ID				0x2604
#define AR0330_REG_CHIP_ID		0x3000

#define AR0330_REG_CTRL_MODE		0x301A
#define AR0330_MODE_SW_STANDBY		0x0058
#define AR0330_MODE_STREAMING		0x005C

#define AR0330_REG_EXPOSURE		0x3012
#define AR0330_EXPOSURE_MIN		4
#define AR0330_EXPOSURE_STEP		1
#define AR0330_VTS_MAX			0x0fff

#define AR0330_REG_ANALOG_GAIN		0x3060
#define ANALOG_GAIN_MIN			0x64
#define ANALOG_GAIN_MAX			0x320
#define ANALOG_GAIN_STEP		1
#define ANALOG_GAIN_DEFAULT		0xC8

#define AR0330_REG_VTS			0x300a

#define AR0330_REG_ORIENTATION		0x3040
#define AR0330_ORIENTATION_H		bit(14)
#define AR0330_ORIENTATION_V		bit(15)

#define AR0330_TEST_PATTERN_MODE	0x3070

#define REG_NULL			0xFFFF
#define REG_DELAY			0xFFFE

#define AR0330_REG_VALUE_08BIT		1
#define AR0330_REG_VALUE_16BIT		2
#define AR0330_REG_VALUE_24BIT		3

#define USE_HDR_MODE

/* h_offs 35 v_offs 14 */
#define PIX_FORMAT MEDIA_BUS_FMT_SGRBG10_1X10

#define AR0330_NAME			"ar0330"

static const char * const ar0330_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define AR0330_NUM_SUPPLIES ARRAY_SIZE(ar0330_supply_names)

struct regval {
	u16 addr;
	u16 val;
};

struct gain_range {
	u16 range_h;
	u16 val;
};

struct ar0330_mode {
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
	u32 pixel_rate;
};

struct ar0330 {
	struct i2c_client	*client;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;
	bool				streaming;
	bool				power_on;
	const struct ar0330_mode *cur_mode;

	struct v4l2_mbus_framefmt format;
	struct camera_sensor_platform_data *platform_data;
	int vt_pix_clk_freq_mhz;
	int fmt_idx;
	int run_mode;
	u8 res;
	u8 type;
};

#define to_ar0330(sd) container_of(sd, struct ar0330, subdev)

/*
 * Xclk 24Mhz
 * Pclk 98Mhz
 * linelength 0x9c0
 * framelength 0x51c
 * grabwindow_width 1920
 * grabwindow_height 1080
 * max_framerate 30fps
 * mipi rate 588Mbps 12it
 */

static const struct regval ar0330_global_regs[] = {
	{0x301a, 0x0059}, /* reset */
	{REG_DELAY, 0x0064},
	{0x301a, 0x0058},
	{REG_DELAY, 0x0032},
	{0x302a, 0x0005},
	{0x302c, 0x0002},
	{0x302e, 0x0004},
	{0x3030, 0x007d},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x31ac, 0x0a0a},
	{0x31ae, 0x0202},
	{0x31b0, 0x0032},
	{0x31b2, 0x0014},
	{0x31b4, 0x3d45},
	{0x31b6, 0x418f},
	{0x31b8, 0x208a},
	{0x31ba, 0x0208},
	{0x31bc, 0x8006},
	{0x3002, 0x0178},
	{0x3004, 0x01c6},
	{0x3006, 0x049f},
	{0x3008, 0x0755},
	{0x300a, 0x0638},
	{0x300c, 0x04e8},
	{0x3012, 0x0641},
	{0x3014, 0x0000},
	{0x30a2, 0x0001},
	{0x30a6, 0x0001},
	{0x308c, 0x0006},
	{0x308a, 0x0006},
	{0x3090, 0x0605},
	{0x308e, 0x0905},
	{0x30aa, 0x0642},
	{0x303e, 0x04e0},
	{0x3016, 0x0641},
	{0x3018, 0x0000},
	{0x30ae, 0x0001},
	{0x30a8, 0x0001},
	{0x3040, 0x0000},
	{0x3042, 0x0000},
	{0x30ba, 0x002c},
	{0x31e0, 0x0303},
	{0x3064, 0x1802},
	{0x3ed2, 0x0146},
	{0x3ed4, 0x8f6c},
	{0x3ed6, 0x66cc},
	{0x3ed8, 0x8c42},
	{0x3eda, 0x88bc},
	{0x3edc, 0xaa63},
	{0x305e, 0x00a0},
	{0x302e, 0x0003},
	{0x3030, 0x0067},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x300a, 0x06d4},
	{REG_NULL, 0x00},
};

/*
 * Xclk 24Mhz
 * Pclk 98Mhz
 * linelength 0x9c0
 * framelength 0x51c
 * grabwindow_width 1920
 * grabwindow_height 1080
 * max_framerate 30fps
 * mipi_datarate per lane 588Mbps
 */
static const struct regval ar0330_1424x808_regs[] = {
	{0x301a, 0x0058},
	{0x302a, 0x0005},
	{0x302c, 0x0002},
	{0x302e, 0x0004},
	{0x3030, 0x007d},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x31ac, 0x0a0a},
	{0x31ae, 0x0202},
	{0x31b0, 0x0032},
	{0x31b2, 0x0014},
	{0x31b4, 0x3d45},
	{0x31b6, 0x418f},
	{0x31b8, 0x208a},
	{0x31ba, 0x0208},
	{0x31bc, 0x8006},
	{0x3002, 0x0178},
	{0x3004, 0x01c6},
	{0x3006, 0x049f},
	{0x3008, 0x0755},
	{0x300a, 0x0638},
	{0x300c, 0x04e8},
	{0x3012, 0x0641},
	{0x3014, 0x0000},
	{0x30a2, 0x0001},
	{0x30a6, 0x0001},
	{0x308c, 0x0006},
	{0x308a, 0x0006},
	{0x3090, 0x0605},
	{0x308e, 0x0905},
	{0x30aa, 0x0642},
	{0x303e, 0x04e0},
	{0x3016, 0x0641},
	{0x3018, 0x0000},
	{0x30ae, 0x0001},
	{0x30a8, 0x0001},
	{0x3040, 0x0000},
	{0x3042, 0x0000},
	{0x30ba, 0x002c},
	{0x31e0, 0x0303},
	{0x3064, 0x1802},
	{0x3ed2, 0x0146},
	{0x3ed4, 0x8f6c},
	{0x3ed6, 0x66cc},
	{0x3ed8, 0x8c42},
	{0x3eda, 0x88bc},
	{0x3edc, 0xaa63},
	{0x305e, 0x00a0},
	{0x302e, 0x0003},
	{0x3030, 0x0067},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x300a, 0x06d4},
	{REG_DELAY, 0x000a}, /* PLL lock after the table rewrote it */
	{REG_NULL, 0x00},
};

static const struct regval ar0330_1936x1096_regs[] = {
	{0x301a, 0x0058},
	{0x302a, 0x0005},
	{0x302c, 0x0002},
	{0x302e, 0x0004},
	{0x3030, 0x007d},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x31ac, 0x0a0a},
	{0x31ae, 0x0202},
	{0x31b0, 0x0032},
	{0x31b2, 0x0014},
	{0x31b4, 0x3d45},
	{0x31b6, 0x418f},
	{0x31b8, 0x208a},
	{0x31ba, 0x0208},
	{0x31bc, 0x8006},
	{0x3002, 0x00e8},
	{0x3004, 0x00c6},
	{0x3006, 0x052f},
	{0x3008, 0x0855},
	{0x300a, 0x0638},
	{0x300c, 0x04e8},
	{0x3012, 0x0641},
	{0x3014, 0x0000},
	{0x30a2, 0x0001},
	{0x30a6, 0x0001},
	{0x308c, 0x0006},
	{0x308a, 0x0006},
	{0x3090, 0x0605},
	{0x308e, 0x0905},
	{0x30aa, 0x0642},
	{0x303e, 0x04e0},
	{0x3016, 0x0641},
	{0x3018, 0x0000},
	{0x30ae, 0x0001},
	{0x30a8, 0x0001},
	{0x3040, 0x0000},
	{0x3042, 0x0000},
	{0x30ba, 0x002c},
	{0x31e0, 0x0303},
	{0x3064, 0x1802},
	{0x3ed2, 0x0146},
	{0x3ed4, 0x8f6c},
	{0x3ed6, 0x66cc},
	{0x3ed8, 0x8c42},
	{0x3eda, 0x88bc},
	{0x3edc, 0xaa63},
	{0x305e, 0x00a0},
	{0x302e, 0x0003},
	{0x3030, 0x006a},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x300a, 0x0708},
	{REG_DELAY, 0x000a}, /* PLL lock after the table rewrote it */
	{REG_NULL, 0x00},
};

static const struct regval ar0330_1160x774_regs[] = {
	{0x301a, 0x0058},
	{0x302a, 0x0005},
	{0x302c, 0x0002},
	{0x302e, 0x0004},
	{0x3030, 0x007d},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x31ac, 0x0a0a},
	{0x31ae, 0x0202},
	{0x31b0, 0x0032},
	{0x31b2, 0x0014},
	{0x31b4, 0x3d45},
	{0x31b6, 0x418f},
	{0x31b8, 0x208a},
	{0x31ba, 0x0208},
	{0x31bc, 0x8006},
	{0x3002, 0x0006},
	{0x3004, 0x0006},
	{0x3006, 0x0611},
	{0x3008, 0x0915},
	{0x300a, 0x0638},
	{0x300c, 0x04e8},
	{0x3012, 0x0641},
	{0x3014, 0x0000},
	{0x30a2, 0x0003},
	{0x30a6, 0x0003},
	{0x308c, 0x0006},
	{0x308a, 0x0006},
	{0x3090, 0x0605},
	{0x308e, 0x0905},
	{0x30aa, 0x0642},
	{0x303e, 0x04e0},
	{0x3016, 0x0641},
	{0x3018, 0x0000},
	{0x30ae, 0x0001},
	{0x30a8, 0x0001},
	{0x3040, 0x3000},
	{0x3042, 0x0000},
	{0x30ba, 0x002c},
	{0x31e0, 0x0303},
	{0x3064, 0x1802},
	{0x3ed2, 0x0146},
	{0x3ed4, 0x8f6c},
	{0x3ed6, 0x66cc},
	{0x3ed8, 0x8c42},
	{0x3eda, 0x88bc},
	{0x3edc, 0xaa63},
	{0x305e, 0x00a0},
	{0x302e, 0x0003},
	{0x3030, 0x006a},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x300a, 0x0708},
	{REG_DELAY, 0x000a}, /* PLL lock after the table rewrote it */
	{REG_NULL, 0x00},
};

static const struct regval ar0330_720x412_regs[] = {
	{0x301a, 0x0058},
	{0x302a, 0x0005},
	{0x302c, 0x0002},
	{0x302e, 0x0002},
	{0x3030, 0x0028},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x31ac, 0x0a0a},
	{0x31ae, 0x0202},
	{0x31b0, 0x0027},
	{0x31b2, 0x0010},
	{0x31b4, 0x2b34},
	{0x31b6, 0x310a},
	{0x31b8, 0x1089},
	{0x31ba, 0x0185},
	{0x31bc, 0x8004},
	{0x3002, 0x009c},
	{0x3004, 0x004e},
	{0x3006, 0x056f},
	{0x3008, 0x08bd},
	{0x300a, 0x0406},
	{0x300c, 0x04da},
	{0x3012, 0x00ce},
	{0x3014, 0x0000},
	{0x30a2, 0x0005},
	{0x30a6, 0x0005},
	{0x308c, 0x0006},
	{0x308a, 0x0006},
	{0x3090, 0x0605},
	{0x308e, 0x0905},
	{0x30aa, 0x060c},
	{0x303e, 0x04e0},
	{0x3016, 0x060b},
	{0x3018, 0x0000},
	{0x30ae, 0x0001},
	{0x30a8, 0x0001},
	{0x3040, 0x3000},
	{0x3042, 0x0000},
	{0x30ba, 0x002c},
	{0x31e0, 0x0303},
	{0x3064, 0x1802},
	{0x3ed2, 0x0146},
	{0x3ed4, 0x8f6c},
	{0x3ed6, 0x66cc},
	{0x3ed8, 0x8c42},
	{0x3eda, 0x88bc},
	{0x3edc, 0xaa63},
	{0x305e, 0x00a0},
	{REG_DELAY, 0x000a}, /* PLL lock after the table rewrote it */
	{REG_NULL, 0x00},
};

static const struct regval ar0330_676x512_regs[] = {
	{0x301a, 0x0058},
	{0x302a, 0x0005},
	{0x302c, 0x0002},
	{0x302e, 0x0002},
	{0x3030, 0x0028},
	{0x3036, 0x000a},
	{0x3038, 0x0001},
	{0x31ac, 0x0a0a},
	{0x31ae, 0x0202},
	{0x31b0, 0x0027},
	{0x31b2, 0x0010},
	{0x31b4, 0x2b34},
	{0x31b6, 0x310a},
	{0x31b8, 0x1089},
	{0x31ba, 0x0185},
	{0x31bc, 0x8004},
	{0x3002, 0x0006},
	{0x3004, 0x0090},
	{0x3006, 0x0605},
	{0x3008, 0x087b},
	{0x300a, 0x0406},
	{0x300c, 0x04da},
	{0x3012, 0x00ce},
	{0x3014, 0x0000},
	{0x30a2, 0x0005},
	{0x30a6, 0x0005},
	{0x308c, 0x0006},
	{0x308a, 0x0006},
	{0x3090, 0x0605},
	{0x308e, 0x0905},
	{0x30aa, 0x060c},
	{0x303e, 0x04e0},
	{0x3016, 0x060b},
	{0x3018, 0x0000},
	{0x30ae, 0x0001},
	{0x30a8, 0x0001},
	{0x3040, 0x3000},
	{0x3042, 0x0000},
	{0x30ba, 0x002c},
	{0x31e0, 0x0303},
	{0x3064, 0x1802},
	{0x3ed2, 0x0146},
	{0x3ed4, 0x8f6c},
	{0x3ed6, 0x66cc},
	{0x3ed8, 0x8c42},
	{0x3eda, 0x88bc},
	{0x3edc, 0xaa63},
	{0x305e, 0x00a0},
	{REG_DELAY, 0x000a}, /* PLL lock after the table rewrote it */
	{REG_NULL, 0x00},
};

#define HTS_DEF 2512
#define VTS_DEF 1748
#define MAX_FPS 30
static const struct ar0330_mode supported_modes[] = {
	{
		.width = 1424,
		.height = 808,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x018c,
		.hts_def = 2512,
		.vts_def = 1748,
		.reg_list = ar0330_1424x808_regs,
		.pixel_rate = 131840000,
	},
	{
		.width = 1936,
		.height = 1096,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x018c,
		.hts_def = 2512,
		.vts_def = 1800,
		.reg_list = ar0330_1936x1096_regs,
		.pixel_rate = 135680000,
	},
	{
		.width = 1160,
		.height = 774,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x018c,
		.hts_def = 2512,
		.vts_def = 1800,
		.reg_list = ar0330_1160x774_regs,
		.pixel_rate = 135680000,
	},
	{
		.width = 720,
		.height = 412,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x018c,
		.hts_def = 2484,
		.vts_def = 1030,
		.reg_list = ar0330_720x412_regs,
		.pixel_rate = 76800000,
	},
	{
		.width = 676,
		.height = 512,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x018c,
		.hts_def = 2484,
		.vts_def = 1030,
		.reg_list = ar0330_676x512_regs,
		.pixel_rate = 76800000,
	},
};

static const s64 link_freq_menu_items[] = {
	AR0330_LINK_FREQ
};

static const char * const ar0330_test_pattern_menu[] = {
	"Disabled",
	"Solid Color",
	"Full Color Bar",
	"Fade-to-gray Color Bar",
	"Walking 1s",
};

/* sensor register write */
static int ar0330_write(struct i2c_client *client, u16 reg, u16 val)
{
	struct i2c_msg msg;
	u8 buf[4];
	int ret;

	dev_dbg(&client->dev, "write reg(0x%x val:0x%x)!\n", reg, val);

	buf[0] = reg >> 8;
	buf[1] = reg & 0xFF;
	buf[2] = val >> 8 & 0xff;
	buf[3] = val & 0xff;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret >= 0)
		return 0;

	dev_err(&client->dev,
		"ov5640 write reg(0x%x val:0x%x) failed !\n", reg, val);

	return ret;
}

static int ar0330_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	int i, delay_ms, ret = 0;

	i = 0;
	while (regs[i].addr != REG_NULL) {
		if (regs[i].addr == REG_DELAY) {
			delay_ms = regs[i].val;
			dev_info(&client->dev, "delay(%d) ms !\n", delay_ms);
			usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
			i++;
			continue;
		}
		ret = ar0330_write(client, regs[i].addr, regs[i].val);
		if (ret) {
			dev_err(&client->dev, "%s failed !\n", __func__);
			break;
		}
		i++;
	}

	return ret;
}

/* Read registers up to 4 at a time */
/* sensor register read */
static int ar0330_read_reg(struct i2c_client *client, u16 reg, u16 *val)
{
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	buf[0] = (reg >> 8) & 0xff;
	buf[1] = reg & 0xFF;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = 2;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret >= 0) {
		*val = (buf[0] << 8) | buf[1];
		return 0;
	}

	dev_err(&client->dev,
		"ar0330 read reg:0x%x failed !\n", reg);

	return ret;
}

static int ar0330_get_reso_dist(const struct ar0330_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct ar0330_mode *
ar0330_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = ar0330_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int ar0330_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct ar0330 *ar0330 = to_ar0330(sd);
	const struct ar0330_mode *mode;
	s64 h_blank, vblank_def;

	pr_info("%s() called\n", __func__);

	mutex_lock(&ar0330->mutex);

	mode = ar0330_find_best_fit(fmt);
	fmt->format.code = PIX_FORMAT;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;
	} else {
		ar0330->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(ar0330->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(ar0330->vblank, 16,
					 AR0330_VTS_MAX - mode->height,
					 1, vblank_def);
		__v4l2_ctrl_s_ctrl(ar0330->vblank, vblank_def);
		__v4l2_ctrl_s_ctrl_int64(ar0330->pixel_rate, mode->pixel_rate);
	}

	mutex_unlock(&ar0330->mutex);

	return 0;
}

static int ar0330_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct ar0330 *ar0330 = to_ar0330(sd);
	const struct ar0330_mode *mode = ar0330->cur_mode;

	pr_info("%s() called\n", __func__);

	mutex_lock(&ar0330->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
							  fmt->pad);
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = PIX_FORMAT;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&ar0330->mutex);

	return 0;
}

static int ar0330_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	pr_info("%s() called\n", __func__);

	if (code->index != 0)
		return -EINVAL;
	code->code = PIX_FORMAT;

	return 0;
}

static int ar0330_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	pr_info("%s() called\n", __func__);

	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != PIX_FORMAT)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int ar0330_enable_test_pattern(struct ar0330 *ar0330, u32 pattern)
{
	static const u16 test_pattern[] = { 0, 1, 2, 3, 256, };

	return ar0330_write(ar0330->client, AR0330_TEST_PATTERN_MODE,
			    test_pattern[pattern]);
}

static int __ar0330_start_stream(struct ar0330 *ar0330)
{
	int ret;

	ret = ar0330_write_array(ar0330->client, ar0330->cur_mode->reg_list);
	if (ret)
		return ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&ar0330->mutex);
	ret = v4l2_ctrl_handler_setup(&ar0330->ctrl_handler);
	mutex_lock(&ar0330->mutex);
	if (ret)
		return ret;

	return ar0330_write(ar0330->client, AR0330_REG_CTRL_MODE,
						AR0330_MODE_STREAMING);
}

static int __ar0330_stop_stream(struct ar0330 *ar0330)
{
	return ar0330_write(ar0330->client, AR0330_REG_CTRL_MODE,
						AR0330_MODE_SW_STANDBY);
}

static int ar0330_s_stream(struct v4l2_subdev *sd, int on)
{
	struct ar0330 *ar0330 = to_ar0330(sd);
	int ret = 0;

	mutex_lock(&ar0330->mutex);
	on = !!on;
	if (on == ar0330->streaming)
		goto unlock_and_return;

	if (on) {
		ret = __ar0330_start_stream(ar0330);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			goto unlock_and_return;
		}
	} else {
		__ar0330_stop_stream(ar0330);
	}

	ar0330->streaming = on;
unlock_and_return:
	mutex_unlock(&ar0330->mutex);

	return ret;
}

static int ar0330_get_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_frame_interval *fi)
{
	struct ar0330 *ar0330 = to_ar0330(sd);
	const struct ar0330_mode *mode = ar0330->cur_mode;

	pr_info("%s() called\n", __func__);

	/*
	 * FIXME: Implement support for V4L2_SUBDEV_FORMAT_TRY, using the V4L2
	 * subdev active state API.
	 */
	if (fi->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	mutex_lock(&ar0330->mutex);
	fi->interval = mode->max_fps;
	mutex_unlock(&ar0330->mutex);

	return 0;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 ar0330_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, AR0330_XVCLK_FREQ / 1000 / 1000);
}

static int power_ctrl(struct v4l2_subdev *sd, bool flag)
{
	int ret = 0;
	struct ar0330 *dev = to_ar0330(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (!dev || !dev->platform_data)
		return -ENODEV;

	dev_dbg(&client->dev, "%s: %s", __func__, flag ? "on" : "off");

	if (flag) {
		ret |= dev->platform_data->v1p8_ctrl(sd, 1);
		ret |= dev->platform_data->v2p8_ctrl(sd, 1);
		usleep_range(10000, 15000);
	}

	if (!flag || ret) {
		ret |= dev->platform_data->v1p8_ctrl(sd, 0);
		ret |= dev->platform_data->v2p8_ctrl(sd, 0);
	}

	return ret;
}

static int gpio_ctrl(struct v4l2_subdev *sd, bool flag)
{
	int ret;
	struct ar0330 *dev = to_ar0330(sd);

	if (!dev || !dev->platform_data)
		return -ENODEV;

	/* Surface 3 has only one GPIO pin for this sensor, but other device
	 * might have two, not sure. */
	if (flag) {
		ret = dev->platform_data->gpio0_ctrl(sd, 1);
		usleep_range(10000, 15000);
		/* Ignore return from second gpio, it may not be there */
		dev->platform_data->gpio1_ctrl(sd, 1);
		usleep_range(10000, 15000);
	} else {
		dev->platform_data->gpio1_ctrl(sd, 0);
		ret = dev->platform_data->gpio0_ctrl(sd, 0);
	}
	return ret;
}

static int power_up(struct ar0330 *ar0330)
{
	int ret;
	u32 delay_us;
	struct device *dev = &ar0330->client->dev;

	dev_info(dev, "%s(%d) enter\n", __func__, __LINE__);

	if (!ar0330->platform_data) {
		dev_err(dev,
			"no camera_sensor_platform_data");
		return -ENODEV;
	}

	/* TODO: original ar0330 driver turns gpio off here, really needed? */
	ret = gpio_ctrl(&ar0330->subdev, 0);
	if (ret)
		goto fail_power;

	/* power control */
	ret = power_ctrl(&ar0330->subdev, 1);
	if (ret)
		goto fail_power;

	/* Delay after power on. These values are from the original ar0330 */
	usleep_range(50000, 100000);

	/* gpio ctrl */
	ret = gpio_ctrl(&ar0330->subdev, 1);
	if (ret) {
		ret = gpio_ctrl(&ar0330->subdev, 1);
		if (ret)
			goto fail_power;
	}

	/* flis clock control */
	ret = ar0330->platform_data->flisclk_ctrl(&ar0330->subdev, 1);
	if (ret)
		goto fail_clk;

	/* 8192 cycles prior to first SCCB transaction */
	delay_us = ar0330_cal_delay(92000);
	usleep_range(delay_us, delay_us * 2);

	return 0;

fail_clk:
	gpio_ctrl(&ar0330->subdev, 0);
fail_power:
	power_ctrl(&ar0330->subdev, 0);
	dev_err(dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct ar0330 *ar0330)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ar0330->subdev);
	int ret;

	if (!ar0330->platform_data) {
		dev_err(&client->dev,
			"no camera_sensor_platform_data");
		return -ENODEV;
	}

	ret = ar0330->platform_data->flisclk_ctrl(&ar0330->subdev, 0);
	if (ret)
		dev_err(&client->dev, "flisclk failed\n");

	/* gpio ctrl */
	ret = gpio_ctrl(&ar0330->subdev, 0);
	if (ret) {
		ret = gpio_ctrl(&ar0330->subdev, 0);
		if (ret)
			dev_err(&client->dev, "gpio failed 2\n");
	}

	/* power control */
	ret = power_ctrl(&ar0330->subdev, 0);
	if (ret)
		dev_err(&client->dev, "vprog failed.\n");

	return ret;
}

static int ar0330_s_power(struct v4l2_subdev *sd, int on)
{
	struct ar0330 *ar0330 = to_ar0330(sd);
	int ret = 0;

	mutex_lock(&ar0330->mutex);

	if (on == 0) {
		ar0330->power_on = false;
		ret = power_down(ar0330);
	} else {
		ret = power_up(ar0330);
		if (ret)
			goto unlock_and_return;

		ret = ar0330_write_array(ar0330->client, ar0330_global_regs);
		if (ret) {
			v4l2_err(sd, "could not set init registers\n");
			goto unlock_and_return;
		}
		ar0330->power_on = true;
	}

unlock_and_return:
	mutex_unlock(&ar0330->mutex);

	return ret;
}

static int ar0330_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ar0330 *ar0330 = to_ar0330(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_state_get_format(fh->state, 0);
	const struct ar0330_mode *def_mode = &supported_modes[0];

	mutex_lock(&ar0330->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = PIX_FORMAT;
	try_fmt->field = V4L2_FIELD_NONE;
	mutex_unlock(&ar0330->mutex);
	/* No crop or compose */

	return 0;
}

static int ar0330_enum_frame_interval(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_frame_interval_enum *fie)
{
	pr_info("%s() called\n", __func__);

	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fie->code != PIX_FORMAT)
		return -EINVAL;

	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	return 0;
}

static int ar0330_set_gain(struct ar0330 *ar0330, int gain);

static long ar0330_set_exposure(struct v4l2_subdev *sd, u16 coarse_itg,
				u16 fine_itg, u16 a_gain, u16 d_gain)

{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ar0330 *ar0330 = to_ar0330(sd);
	int ret;

	mutex_lock(&ar0330->mutex);

	/* TODO: using coarse_itg appropriate? */
	ret = ar0330_write(ar0330->client,
			   AR0330_REG_EXPOSURE, coarse_itg);
	if (ret) {
		dev_err(&client->dev,
			"setting AR0330_REG_EXPOSURE failed\n");
	}

	ret = ar0330_set_gain(ar0330, a_gain);
	if (ret)
		dev_err(&client->dev, "ar0330_set_gain() failed\n");

	mutex_unlock(&ar0330->mutex);

	return ret;
}

static long ar0330_s_exposure(struct v4l2_subdev *sd,
			      struct atomisp_exposure *exposure)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 coarse_itg, fine_itg, analog_gain, digital_gain;

	coarse_itg = exposure->integration_time[0];
	fine_itg = exposure->integration_time[1];
	analog_gain = exposure->gain[0];
	digital_gain = exposure->gain[1];

	/* we should not accept the invalid value below */
	if (fine_itg == 0 || analog_gain == 0) {
		v4l2_err(client, "%s(): invalid value\n", __func__);
		return -EINVAL;
	}

	dev_dbg(&client->dev, "%s() analog %x digital %x\n",
		__func__, analog_gain, digital_gain);
	return ar0330_set_exposure(sd, coarse_itg, fine_itg, analog_gain,
				   digital_gain);
}

static long ar0330_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	switch (cmd) {
	case ATOMISP_IOC_S_EXPOSURE:
		return ar0330_s_exposure(sd, (struct atomisp_exposure *)arg);
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct v4l2_subdev_internal_ops ar0330_internal_ops = {
	.open = ar0330_open,
};

static const struct v4l2_subdev_core_ops ar0330_core_ops = {
	.s_power = ar0330_s_power,
	.ioctl = ar0330_ioctl,
};

static const struct v4l2_subdev_video_ops ar0330_video_ops = {
	.s_stream = ar0330_s_stream,
};

static const struct v4l2_subdev_pad_ops ar0330_pad_ops = {
	.enum_mbus_code = ar0330_enum_mbus_code,
	.enum_frame_size = ar0330_enum_frame_sizes,
	.enum_frame_interval = ar0330_enum_frame_interval,
	.get_fmt = ar0330_get_fmt,
	.set_fmt = ar0330_set_fmt,
	.get_frame_interval = ar0330_get_frame_interval,
};

static const struct v4l2_subdev_ops ar0330_subdev_ops = {
	.core	= &ar0330_core_ops,
	.video	= &ar0330_video_ops,
	.pad	= &ar0330_pad_ops,
};

struct gain_range gain_level[] = {
	{101, 0x00}, {104, 0x01}, {108, 0x02}, {111, 0x03}, {115, 0x04},
	{120, 0x05}, {124, 0x06}, {129, 0x07}, {134, 0x08}, {140, 0x09},
	{146, 0x0a}, {153, 0x0b}, {161, 0x0c}, {169, 0x0d}, {179, 0x0e},
	{189, 0x0f}, {201, 0x10}, {214, 0x12}, {230, 0x14}, {247, 0x16},
	{268, 0x18}, {292, 0x1a}, {321, 0x1c}, {357, 0x1e}, {401, 0x20},
	{457, 0x24}, {533, 0x28}, {641, 0x2c}, {801, 0x30}
};

static int ar0330_set_gain(struct ar0330 *ar0330, int gain)
{
	int ret = 0, i = 0;
	int num_gains = ARRAY_SIZE(gain_level);
	u16 again = 0;

	for (i = 0; i < num_gains; i++) {
		if (gain_level[i].range_h >= gain) {
			again = gain_level[i].val;
			break;
		}
	}

	ret = ar0330_write(ar0330->client, AR0330_REG_ANALOG_GAIN, again);
	return ret;
}

static int ar0330_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ar0330 *ar0330 = container_of(ctrl->handler,
					     struct ar0330, ctrl_handler);
	struct i2c_client *client = ar0330->client;
	int ret = 0;

	if (!ar0330->power_on)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ar0330_write(ar0330->client,
						AR0330_REG_EXPOSURE, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ar0330_set_gain(ar0330, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = ar0330_write(ar0330->client, AR0330_REG_VTS,
				       ctrl->val + ar0330->cur_mode->height);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ar0330_enable_test_pattern(ar0330, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ar0330_ctrl_ops = {
	.s_ctrl = ar0330_set_ctrl,
};

static int ar0330_initialize_controls(struct ar0330 *ar0330)
{
	const struct ar0330_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &ar0330->ctrl_handler;
	mode = ar0330->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 7);
	if (ret)
		return ret;
	handler->lock = &ar0330->mutex;

	/* TODO: atomisp drivers use v4l2_ctrl_new_custom() instead. What's
	 * the difference? */
	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ar0330->pixel_rate = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, AR0330_PIXEL_RATE, 1, mode->pixel_rate);

	h_blank = mode->hts_def - mode->width;
	ar0330->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (ar0330->hblank)
		ar0330->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	ar0330->vblank = v4l2_ctrl_new_std(handler, &ar0330_ctrl_ops,
				V4L2_CID_VBLANK, 16,
				AR0330_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 4;
	ar0330->exposure = v4l2_ctrl_new_std(handler, &ar0330_ctrl_ops,
				V4L2_CID_EXPOSURE, AR0330_EXPOSURE_MIN,
				exposure_max, AR0330_EXPOSURE_STEP,
				mode->exp_def);

	ar0330->anal_gain = v4l2_ctrl_new_std(handler, &ar0330_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, ANALOG_GAIN_MIN,
				ANALOG_GAIN_MAX, ANALOG_GAIN_STEP,
				ANALOG_GAIN_DEFAULT);

	ar0330->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&ar0330_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(ar0330_test_pattern_menu) - 1,
				0, 0, ar0330_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(&ar0330->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	/* Use same lock for controls as for everything else. */
	ar0330->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int ar0330_check_sensor_id(struct ar0330 *ar0330,
				  struct i2c_client *client)
{
	struct device *dev = &ar0330->client->dev;
	u16 id = 0;
	int ret;

	ret = ar0330_read_reg(client, AR0330_REG_CHIP_ID, &id);
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%x), ret(%d)\n", id, ret);
		return -ENODEV;
	}

	dev_info(dev, "Detected AR0330 sensor\n");

	return 0;
}

static int ar0330_s_config(struct v4l2_subdev *sd,
			   int irq, void *platform_data)
{
	struct ar0330 *dev = to_ar0330(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (!platform_data)
		return -ENODEV;

	dev->platform_data =
	    (struct camera_sensor_platform_data *)platform_data;

	mutex_lock(&dev->mutex);
	/* power off the module, then power on it in future
	 * as first power on by board may not fulfill the
	 * power on sequqence needed by the module
	 */
	ret = power_down(dev);
	if (ret) {
		dev_err(&client->dev, "ar0330 power-off err.\n");
		goto fail_power_off;
	}

	ret = power_up(dev);
	if (ret) {
		dev_err(&client->dev, "ar0330 power-up err.\n");
		goto fail_power_on;
	}

	ret = dev->platform_data->csi_cfg(sd, 1);
	if (ret)
		goto fail_csi_cfg;

	/* config & detect sensor */
	/* NOTE: atomisp drivers use *_detect() function instead. */
	ret = ar0330_check_sensor_id(dev, client);
	if (ret) {
		dev_err(&client->dev, "ar0330_check_sensor_id err s_config.\n");
		goto fail_csi_cfg;
	}

	/* turn off sensor, after probed */
	ret = power_down(dev);
	if (ret) {
		dev_err(&client->dev, "ar0330 power-off err.\n");
		goto fail_csi_cfg;
	}
	mutex_unlock(&dev->mutex);

	return 0;

fail_csi_cfg:
	dev->platform_data->csi_cfg(sd, 0);
fail_power_on:
	power_down(dev);
	dev_err(&client->dev, "sensor power-gating failed\n");
fail_power_off:
	mutex_unlock(&dev->mutex);
	return ret;
}

static int ar0330_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ar0330 *ar0330;
	struct v4l2_subdev *sd;
	void *pdata;
	int ret;

	dev_info(dev, "wpzz driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	ar0330 = devm_kzalloc(dev, sizeof(*ar0330), GFP_KERNEL);
	if (!ar0330)
		return -ENOMEM;

	ar0330->client = client;
	ar0330->cur_mode = &supported_modes[0];

	mutex_init(&ar0330->mutex);

	sd = &ar0330->subdev;
	v4l2_i2c_subdev_init(sd, client, &ar0330_subdev_ops);

	pdata = gmin_camera_platform_data(sd,
					  ATOMISP_INPUT_FORMAT_RAW_10,
					  atomisp_bayer_order_grbg);
	if (!pdata) {
		ret = -EINVAL;
		goto out_free;
	}

	ret = ar0330_s_config(sd, client->irq, pdata);
	if (ret)
		goto out_free;

	ret = atomisp_register_i2c_module(sd, pdata);
	if (ret)
		goto out_free;

	ret = ar0330_initialize_controls(ar0330);
	if (ret)
		goto out_free;

	sd->internal_ops = &ar0330_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ar0330->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &ar0330->pad);
	if (ret < 0)
		goto err_free_handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ar0330->ctrl_handler);
out_free:
	v4l2_device_unregister_subdev(sd);
	atomisp_gmin_remove_subdev(sd);
	mutex_destroy(&ar0330->mutex);

	return ret;
}

static void ar0330_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar0330 *ar0330 = to_ar0330(sd);

	dev_info(&client->dev, "ar0330_remove...\n");

	ar0330->platform_data->csi_cfg(sd, 0);

	power_down(ar0330);

	/* TODO: atomisp sensor drivers use v4l2_device_unregister_subdev()
	 * instead. What's the difference? */
	// v4l2_async_unregister_subdev(sd);
	v4l2_device_unregister_subdev(sd);

	atomisp_gmin_remove_subdev(sd);

	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ar0330->ctrl_handler);
	mutex_destroy(&ar0330->mutex);
}

static const struct acpi_device_id ar0330_acpi_ids[] = {
       {"APTA0330"},
       {},
};
MODULE_DEVICE_TABLE(acpi, ar0330_acpi_ids);

static const struct i2c_device_id ar0330_match_id[] = {
	{ "Aptina,ar0330", 0 },
	{ },
};

static struct i2c_driver ar0330_i2c_driver = {
	.driver = {
		.name = AR0330_NAME,
		.acpi_match_table = ACPI_PTR(ar0330_acpi_ids),
	},
	.probe		= &ar0330_probe,
	.remove		= &ar0330_remove,
	.id_table	= ar0330_match_id,
};
module_i2c_driver(ar0330_i2c_driver);

MODULE_DESCRIPTION("Aptina ar0330 sensor driver");
MODULE_LICENSE("GPL v2");
