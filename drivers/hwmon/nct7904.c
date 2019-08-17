// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nct7904.c - driver for Nuvoton NCT7904D.
 *
 * Copyright (c) 2015 Kontron
 * Author: Vadim V. Vlasov <vvlasov@dev.rtsoft.ru>
 *
 * Copyright (c) 2019 Advantech
 * Author: Amy.Shih <amy.shih@advantech.com.tw>
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/hwmon.h>

#define VENDOR_ID_REG		0x7A	/* Any bank */
#define NUVOTON_ID		0x50
#define CHIP_ID_REG		0x7B	/* Any bank */
#define NCT7904_ID		0xC5
#define DEVICE_ID_REG		0x7C	/* Any bank */

#define BANK_SEL_REG		0xFF
#define BANK_0			0x00
#define BANK_1			0x01
#define BANK_2			0x02
#define BANK_3			0x03
#define BANK_4			0x04
#define BANK_MAX		0x04

#define FANIN_MAX		12	/* Counted from 1 */
#define VSEN_MAX		21	/* VSEN1..14, 3VDD, VBAT, V3VSB,
					   LTD (not a voltage), VSEN17..19 */
#define FANCTL_MAX		4	/* Counted from 1 */
#define TCPU_MAX		8	/* Counted from 1 */
#define TEMP_MAX		4	/* Counted from 1 */

#define VT_ADC_CTRL0_REG	0x20	/* Bank 0 */
#define VT_ADC_CTRL1_REG	0x21	/* Bank 0 */
#define VT_ADC_CTRL2_REG	0x22	/* Bank 0 */
#define FANIN_CTRL0_REG		0x24
#define FANIN_CTRL1_REG		0x25
#define DTS_T_CTRL0_REG		0x26
#define DTS_T_CTRL1_REG		0x27
#define VT_ADC_MD_REG		0x2E

#define VSEN1_HV_REG		0x40	/* Bank 0; 2 regs (HV/LV) per sensor */
#define TEMP_CH1_HV_REG		0x42	/* Bank 0; same as VSEN2_HV */
#define LTD_HV_REG		0x62	/* Bank 0; 2 regs in VSEN range */
#define FANIN1_HV_REG		0x80	/* Bank 0; 2 regs (HV/LV) per sensor */
#define T_CPU1_HV_REG		0xA0	/* Bank 0; 2 regs (HV/LV) per sensor */

#define PRTS_REG		0x03	/* Bank 2 */
#define PFE_REG			0x00	/* Bank 2; PECI Function Enable */
#define TSI_CTRL_REG		0x50	/* Bank 2; TSI Control Register */
#define FANCTL1_FMR_REG		0x00	/* Bank 3; 1 reg per channel */
#define FANCTL1_OUT_REG		0x10	/* Bank 3; 1 reg per channel */

static const unsigned short normal_i2c[] = {
	0x2d, 0x2e, I2C_CLIENT_END
};

struct nct7904_data {
	struct i2c_client *client;
	struct mutex bank_lock;
	int bank_sel;
	u32 fanin_mask;
	u32 vsen_mask;
	u32 tcpu_mask;
	u8 fan_mode[FANCTL_MAX];
	u8 enable_dts;
	u8 has_dts;
};

/* Access functions */
static int nct7904_bank_lock(struct nct7904_data *data, unsigned int bank)
{
	int ret;

	mutex_lock(&data->bank_lock);
	if (data->bank_sel == bank)
		return 0;
	ret = i2c_smbus_write_byte_data(data->client, BANK_SEL_REG, bank);
	if (ret == 0)
		data->bank_sel = bank;
	else
		data->bank_sel = -1;
	return ret;
}

static inline void nct7904_bank_release(struct nct7904_data *data)
{
	mutex_unlock(&data->bank_lock);
}

/* Read 1-byte register. Returns unsigned reg or -ERRNO on error. */
static int nct7904_read_reg(struct nct7904_data *data,
			    unsigned int bank, unsigned int reg)
{
	struct i2c_client *client = data->client;
	int ret;

	ret = nct7904_bank_lock(data, bank);
	if (ret == 0)
		ret = i2c_smbus_read_byte_data(client, reg);

	nct7904_bank_release(data);
	return ret;
}

/*
 * Read 2-byte register. Returns register in big-endian format or
 * -ERRNO on error.
 */
static int nct7904_read_reg16(struct nct7904_data *data,
			      unsigned int bank, unsigned int reg)
{
	struct i2c_client *client = data->client;
	int ret, hi;

	ret = nct7904_bank_lock(data, bank);
	if (ret == 0) {
		ret = i2c_smbus_read_byte_data(client, reg);
		if (ret >= 0) {
			hi = ret;
			ret = i2c_smbus_read_byte_data(client, reg + 1);
			if (ret >= 0)
				ret |= hi << 8;
		}
	}

	nct7904_bank_release(data);
	return ret;
}

/* Write 1-byte register. Returns 0 or -ERRNO on error. */
static int nct7904_write_reg(struct nct7904_data *data,
			     unsigned int bank, unsigned int reg, u8 val)
{
	struct i2c_client *client = data->client;
	int ret;

	ret = nct7904_bank_lock(data, bank);
	if (ret == 0)
		ret = i2c_smbus_write_byte_data(client, reg, val);

	nct7904_bank_release(data);
	return ret;
}

static int nct7904_read_fan(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct nct7904_data *data = dev_get_drvdata(dev);
	unsigned int cnt, rpm;
	int ret;

	switch (attr) {
	case hwmon_fan_input:
		ret = nct7904_read_reg16(data, BANK_0,
					 FANIN1_HV_REG + channel * 2);
		if (ret < 0)
			return ret;
		cnt = ((ret & 0xff00) >> 3) | (ret & 0x1f);
		if (cnt == 0x1fff)
			rpm = 0;
		else
			rpm = 1350000 / cnt;
		*val = rpm;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7904_fan_is_visible(const void *_data, u32 attr, int channel)
{
	const struct nct7904_data *data = _data;

	if (attr == hwmon_fan_input && data->fanin_mask & (1 << channel))
		return 0444;
	return 0;
}

static u8 nct7904_chan_to_index[] = {
	0,	/* Not used */
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	18, 19, 20, 16
};

static int nct7904_read_in(struct device *dev, u32 attr, int channel,
			   long *val)
{
	struct nct7904_data *data = dev_get_drvdata(dev);
	int ret, volt, index;

	index = nct7904_chan_to_index[channel];

	switch (attr) {
	case hwmon_in_input:
		ret = nct7904_read_reg16(data, BANK_0,
					 VSEN1_HV_REG + index * 2);
		if (ret < 0)
			return ret;
		volt = ((ret & 0xff00) >> 5) | (ret & 0x7);
		if (index < 14)
			volt *= 2; /* 0.002V scale */
		else
			volt *= 6; /* 0.006V scale */
		*val = volt;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7904_in_is_visible(const void *_data, u32 attr, int channel)
{
	const struct nct7904_data *data = _data;
	int index = nct7904_chan_to_index[channel];

	if (channel > 0 && attr == hwmon_in_input &&
	    (data->vsen_mask & BIT(index)))
		return 0444;

	return 0;
}

static int nct7904_read_temp(struct device *dev, u32 attr, int channel,
			     long *val)
{
	struct nct7904_data *data = dev_get_drvdata(dev);
	int ret, temp;

	switch (attr) {
	case hwmon_temp_input:
		if (channel == 4)
			ret = nct7904_read_reg16(data, BANK_0, LTD_HV_REG);
		else if (channel < 5)
			ret = nct7904_read_reg16(data, BANK_0,
						 TEMP_CH1_HV_REG + channel * 4);
		else
			ret = nct7904_read_reg16(data, BANK_0,
						 T_CPU1_HV_REG + (channel - 5)
						 * 2);
		if (ret < 0)
			return ret;
		temp = ((ret & 0xff00) >> 5) | (ret & 0x7);
		*val = sign_extend32(temp, 10) * 125;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7904_temp_is_visible(const void *_data, u32 attr, int channel)
{
	const struct nct7904_data *data = _data;

	if (attr == hwmon_temp_input) {
		if (channel < 5) {
			if (data->tcpu_mask & BIT(channel))
				return 0444;
		} else {
			if (data->has_dts & BIT(channel - 5))
				return 0444;
		}
	}

	return 0;
}

static int nct7904_read_pwm(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct nct7904_data *data = dev_get_drvdata(dev);
	int ret;

	switch (attr) {
	case hwmon_pwm_input:
		ret = nct7904_read_reg(data, BANK_3, FANCTL1_OUT_REG + channel);
		if (ret < 0)
			return ret;
		*val = ret;
		return 0;
	case hwmon_pwm_enable:
		ret = nct7904_read_reg(data, BANK_3, FANCTL1_FMR_REG + channel);
		if (ret < 0)
			return ret;

		*val = ret ? 2 : 1;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int nct7904_write_pwm(struct device *dev, u32 attr, int channel,
			     long val)
{
	struct nct7904_data *data = dev_get_drvdata(dev);
	int ret;

	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		ret = nct7904_write_reg(data, BANK_3, FANCTL1_OUT_REG + channel,
					val);
		return ret;
	case hwmon_pwm_enable:
		if (val < 1 || val > 2 ||
		    (val == 2 && !data->fan_mode[channel]))
			return -EINVAL;
		ret = nct7904_write_reg(data, BANK_3, FANCTL1_FMR_REG + channel,
					val == 2 ? data->fan_mode[channel] : 0);
		return ret;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7904_pwm_is_visible(const void *_data, u32 attr, int channel)
{
	switch (attr) {
	case hwmon_pwm_input:
	case hwmon_pwm_enable:
		return 0644;
	default:
		return 0;
	}
}

static int nct7904_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_in:
		return nct7904_read_in(dev, attr, channel, val);
	case hwmon_fan:
		return nct7904_read_fan(dev, attr, channel, val);
	case hwmon_pwm:
		return nct7904_read_pwm(dev, attr, channel, val);
	case hwmon_temp:
		return nct7904_read_temp(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int nct7904_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_pwm:
		return nct7904_write_pwm(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t nct7904_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		return nct7904_in_is_visible(data, attr, channel);
	case hwmon_fan:
		return nct7904_fan_is_visible(data, attr, channel);
	case hwmon_pwm:
		return nct7904_pwm_is_visible(data, attr, channel);
	case hwmon_temp:
		return nct7904_temp_is_visible(data, attr, channel);
	default:
		return 0;
	}
}

/* Return 0 if detection is successful, -ENODEV otherwise */
static int nct7904_detect(struct i2c_client *client,
			  struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if (!i2c_check_functionality(adapter,
				     I2C_FUNC_SMBUS_READ_BYTE |
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	/* Determine the chip type. */
	if (i2c_smbus_read_byte_data(client, VENDOR_ID_REG) != NUVOTON_ID ||
	    i2c_smbus_read_byte_data(client, CHIP_ID_REG) != NCT7904_ID ||
	    (i2c_smbus_read_byte_data(client, DEVICE_ID_REG) & 0xf0) != 0x50 ||
	    (i2c_smbus_read_byte_data(client, BANK_SEL_REG) & 0xf8) != 0x00)
		return -ENODEV;

	strlcpy(info->type, "nct7904", I2C_NAME_SIZE);

	return 0;
}

static const struct hwmon_channel_info *nct7904_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT, /* dummy, skipped in is_visible */
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops nct7904_hwmon_ops = {
	.is_visible = nct7904_is_visible,
	.read = nct7904_read,
	.write = nct7904_write,
};

static const struct hwmon_chip_info nct7904_chip_info = {
	.ops = &nct7904_hwmon_ops,
	.info = nct7904_info,
};

static int nct7904_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct nct7904_data *data;
	struct device *hwmon_dev;
	struct device *dev = &client->dev;
	int ret, i;
	u32 mask;
	u8 val, bit;

	data = devm_kzalloc(dev, sizeof(struct nct7904_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->bank_lock);
	data->bank_sel = -1;

	/* Setup sensor groups. */
	/* FANIN attributes */
	ret = nct7904_read_reg16(data, BANK_0, FANIN_CTRL0_REG);
	if (ret < 0)
		return ret;
	data->fanin_mask = (ret >> 8) | ((ret & 0xff) << 8);

	/*
	 * VSEN attributes
	 *
	 * Note: voltage sensors overlap with external temperature
	 * sensors. So, if we ever decide to support the latter
	 * we will have to adjust 'vsen_mask' accordingly.
	 */
	mask = 0;
	ret = nct7904_read_reg16(data, BANK_0, VT_ADC_CTRL0_REG);
	if (ret >= 0)
		mask = (ret >> 8) | ((ret & 0xff) << 8);
	ret = nct7904_read_reg(data, BANK_0, VT_ADC_CTRL2_REG);
	if (ret >= 0)
		mask |= (ret << 16);
	data->vsen_mask = mask;

	/* CPU_TEMP attributes */
	ret = nct7904_read_reg(data, BANK_0, VT_ADC_CTRL0_REG);
	if (ret < 0)
		return ret;

	if ((ret & 0x6) == 0x6)
		data->tcpu_mask |= 1; /* TR1 */
	if ((ret & 0x18) == 0x18)
		data->tcpu_mask |= 2; /* TR2 */
	if ((ret & 0x20) == 0x20)
		data->tcpu_mask |= 4; /* TR3 */
	if ((ret & 0x80) == 0x80)
		data->tcpu_mask |= 8; /* TR4 */

	/* LTD */
	ret = nct7904_read_reg(data, BANK_0, VT_ADC_CTRL2_REG);
	if (ret < 0)
		return ret;
	if ((ret & 0x02) == 0x02)
		data->tcpu_mask |= 0x10;

	/* Multi-Function detecting for Volt and TR/TD */
	ret = nct7904_read_reg(data, BANK_0, VT_ADC_MD_REG);
	if (ret < 0)
		return ret;

	for (i = 0; i < 4; i++) {
		val = (ret & (0x03 << i)) >> (i * 2);
		bit = (1 << i);
		if (val == 0)
			data->tcpu_mask &= ~bit;
	}

	/* PECI */
	ret = nct7904_read_reg(data, BANK_2, PFE_REG);
	if (ret < 0)
		return ret;
	if (ret & 0x80) {
		data->enable_dts = 1; /* Enable DTS & PECI */
	} else {
		ret = nct7904_read_reg(data, BANK_2, TSI_CTRL_REG);
		if (ret < 0)
			return ret;
		if (ret & 0x80)
			data->enable_dts = 0x3; /* Enable DTS & TSI */
	}

	/* Check DTS enable status */
	if (data->enable_dts) {
		ret = nct7904_read_reg(data, BANK_0, DTS_T_CTRL0_REG);
		if (ret < 0)
			return ret;
		data->has_dts = ret & 0xF;
		if (data->enable_dts & 0x2) {
			ret = nct7904_read_reg(data, BANK_0, DTS_T_CTRL1_REG);
			if (ret < 0)
				return ret;
			data->has_dts |= (ret & 0xF) << 4;
		}
	}

	for (i = 0; i < FANCTL_MAX; i++) {
		ret = nct7904_read_reg(data, BANK_3, FANCTL1_FMR_REG + i);
		if (ret < 0)
			return ret;
		data->fan_mode[i] = ret;
	}

	hwmon_dev =
		devm_hwmon_device_register_with_info(dev, client->name, data,
						     &nct7904_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id nct7904_id[] = {
	{"nct7904", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, nct7904_id);

static struct i2c_driver nct7904_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = "nct7904",
	},
	.probe = nct7904_probe,
	.id_table = nct7904_id,
	.detect = nct7904_detect,
	.address_list = normal_i2c,
};

module_i2c_driver(nct7904_driver);

MODULE_AUTHOR("Vadim V. Vlasov <vvlasov@dev.rtsoft.ru>");
MODULE_DESCRIPTION("Hwmon driver for NUVOTON NCT7904");
MODULE_LICENSE("GPL");
