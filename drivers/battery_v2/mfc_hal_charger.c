/*
 *  mfc_hal_charger.c
 *  Samsung MFC IC Charger Driver
 *
 *  Copyright (C) 2019 Samsung Electronics
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "include/charger/mfc_hal_charger.h"
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/ctype.h>

#define SENDSZ 16

unsigned int mfc_chip_id_now;

int mfc_reg_read(struct i2c_client *client, u16 reg, u8 *val)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret;
	struct i2c_msg msg[2];
	u8 wbuf[2];
	u8 rbuf[2];

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = 2;
	msg[0].buf = wbuf;

	wbuf[0] = (reg & 0xFF00) >> 8;
	wbuf[1] = (reg & 0xFF);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = rbuf;

	mutex_lock(&charger->io_lock);
	ret = i2c_transfer(client->adapter, msg, 2);
	mutex_unlock(&charger->io_lock);
	if (ret < 0)
	{
		pr_err("%s: i2c read error, reg: 0x%x, ret: %d (called by %ps)\n",
			__func__, reg, ret, __builtin_return_address(0));
		return ret;
	}
	*val = rbuf[0];

	return ret;
}

int mfc_reg_write(struct i2c_client *client, u16 reg, u8 val)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret;
	unsigned char data[3] = { reg >> 8, reg & 0xff, val };

	//pr_info("%s reg=0x%x, val=0x%x", __func__, reg, val);

	mutex_lock(&charger->io_lock);
	ret = i2c_master_send(client, data, 3);
	mutex_unlock(&charger->io_lock);
	if (ret < 3) {
		pr_err("%s: i2c write error, reg: 0x%x, ret: %d (called by %ps)\n",
				__func__, reg, ret, __builtin_return_address(0));
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

int mfc_reg_multi_write(struct i2c_client *client, u16 reg, const u8 * val, int size)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret = 0;
	unsigned char data[SENDSZ+2];
	int cnt = 0;

	pr_err("%s: size: 0x%x\n",
				__func__, size);
	while(size > SENDSZ) {
		data[0] = (reg+cnt) >>8;
		data[1] = (reg+cnt) & 0xff;
		memcpy(data+2, val+cnt, SENDSZ);
		mutex_lock(&charger->io_lock);
		ret = i2c_master_send(client, data, SENDSZ+2);
		mutex_unlock(&charger->io_lock);
		if (ret < SENDSZ+2) {
			pr_err("%s: i2c write error, reg: 0x%x\n",
					__func__, reg);
			return ret < 0 ? ret : -EIO;
		}
		cnt = cnt + SENDSZ;
		size = size - SENDSZ;
	}
	if (size > 0) {
		data[0] = (reg+cnt) >>8;
		data[1] = (reg+cnt) & 0xff;
		memcpy(data+2, val+cnt, size);
		mutex_lock(&charger->io_lock);
		ret = i2c_master_send(client, data, size+2);
		mutex_unlock(&charger->io_lock);
		if (ret < size+2) {
			dev_err(&client->dev, "%s: i2c write error, reg: 0x%x\n",
					__func__, reg);
			return ret < 0 ? ret : -EIO;
		}
	}

	return ret;
}

int mfc_reg_update(struct i2c_client *client, u16 reg, u8 val, u8 mask)
{
	//val = 0b 0000 0001
	//ms = 0b 1111 1110
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	unsigned char data[3] = { reg >> 8, reg & 0xff, val };
	u8 data2;
	int ret;

	ret = mfc_reg_read(client, reg, &data2);
	if (ret >= 0) {
		u8 old_val = data2 & 0xff;
		u8 new_val = (val & mask) | (old_val & (~mask));
		data[2] = new_val;

		mutex_lock(&charger->io_lock);
		ret = i2c_master_send(client, data, 3);
		mutex_unlock(&charger->io_lock);
		if (ret < 3) {
			pr_err("%s: i2c write error, reg: 0x%x, ret: %d\n",
				__func__, reg, ret);
			return ret < 0 ? ret : -EIO;
		}
	}
	mfc_reg_read(client, reg, &data2);

	return ret;
}

void mfc_set_cmd_l_reg(struct mfc_charger_data *charger, u8 val, u8 mask)
{
	u8 temp = 0;
	int ret = 0, i = 0;

	do {
		pr_info("%s\n", __func__);
		ret = mfc_reg_update(charger->client, MFC_HAL_AP2MFC_CMD_L_REG, val, mask); // command
		if (ret >= 0) {
			msleep(10);
			ret = mfc_reg_read(charger->client, MFC_HAL_AP2MFC_CMD_L_REG, &temp); // check out set bit exists
			if(temp != 0)
				pr_info("%s CMD is not clear yet, cnt = %d\n", __func__, i);
			if (ret < 0 || i > 3)
				break;
		}
		i++;
	} while ((temp != 0) && (i < 3));
}

int mfc_get_firmware_version(struct mfc_charger_data *charger, int firm_mode)
{
	int version = -1;
	int ret;
	u8 fw_major[2] = {0,};
	u8 fw_minor[2] = {0,};

	pr_info("%s: called by (%ps)\n", __func__, __builtin_return_address(0));
	switch (firm_mode) {
	case MFC_RX_FIRMWARE:
		ret = mfc_reg_read(charger->client, MFC_HAL_FW_MAJOR_REV_L_REG, &fw_major[0]);
		ret = mfc_reg_read(charger->client, MFC_HAL_FW_MAJOR_REV_H_REG, &fw_major[1]);
		if (ret >= 0) {
			version =  fw_major[0] | (fw_major[1] << 8);
		}
		pr_info("%s rx major firmware version 0x%x\n", __func__, version);

		ret = mfc_reg_read(charger->client, MFC_HAL_FW_MINOR_REV_L_REG, &fw_minor[0]);
		ret = mfc_reg_read(charger->client, MFC_HAL_FW_MINOR_REV_H_REG, &fw_minor[1]);
		if (ret >= 0) {
			version = fw_minor[0] | (fw_minor[1] << 8);
		}
		pr_info("%s rx minor firmware version 0x%x\n", __func__, version);
		break;
	default:
		pr_err("%s Wrong firmware mode\n", __func__);
		version = -1;
		break;
	}

	return version;
}

static int mfc_get_ic_id(struct mfc_charger_data *charger)
{
	u8 temp;
	int ret, i;

	pr_info("%s: called by (%ps)\n", __func__, __builtin_return_address(0));

	for (i = 0; i < 3; i++) {
		ret = mfc_reg_read(charger->client, MFC_HAL_CHIP_ID_L_REG, &temp);
		if (ret >= 0) {
			pr_info("%s ic id = 0x%x\n", __func__, temp);
			ret =  temp;
			break;
		}
	}
	if (i == 3)
		ret = -1;

	return ret;
}

static void mfc_uno_on(struct mfc_charger_data *charger, bool on)
{
	union power_supply_propval value = {0, };

	if (on) { /* UNO ON */
		value.intval = 1;
		psy_do_property(charger->pdata->wired_charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL, value);
		pr_info("%s: ENABLE\n", __func__);
	} else { /* UNO OFF */
		value.intval = 0;
		psy_do_property(charger->pdata->wired_charger_name, set,
			POWER_SUPPLY_PROP_CHARGE_UNO_CONTROL, value);
		pr_info("%s: DISABLE\n", __func__);
	}
}

static int mfc_check_ic_info(struct mfc_charger_data *charger)
{
	int id = 0, ver = 0, wpc_det = 0;

	wpc_det = gpio_get_value(charger->pdata->wpc_det);
	pr_info("%s wpc_det = %d\n", __func__, wpc_det);

	if (!wpc_det) {
		mfc_uno_on(charger, 1);
		msleep(200);
	}

	id = mfc_get_ic_id(charger);
	if (id == MFC_CHIP_ID_S2MIW04)
		mfc_chip_id_now = MFC_CHIP_ID_S2MIW04;
	else
		mfc_chip_id_now = MFC_CHIP_ID_P9320; /* default is IDT */

	ver = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);

	if (!wpc_det)
		mfc_uno_on(charger, 0);

	pr_info("%s ic ver = 0x%x , firmware ver = 0x%x \n", __func__, id, ver);

	return id;
}

static int mfc_chg_parse_dt(struct device *dev, 
		mfc_hal_charger_platform_data_t *pdata)
{
	int ret = 0;
	struct device_node *np  = dev->of_node;
	enum of_gpio_flags irq_gpio_flags;

	if (!np) {
		pr_err("%s np NULL\n", __func__);
		return 1;
	} else {
		/* wpc_det */
		ret = pdata->wpc_det = of_get_named_gpio_flags(np, "battery,wpc_det",
				0, &irq_gpio_flags);
		if (ret < 0) {
			dev_err(dev, "%s : can't get wpc_det\r\n", __FUNCTION__);
		}
		
		ret = of_property_read_string(np,
			"battery,charger_name", (char const **)&pdata->wired_charger_name);
		if (ret < 0)
			pr_info("%s: Charger name is Empty\n", __func__);

		return 0;
	}
}

static int mfc_hal_charger_probe(
						struct i2c_client *client,
						const struct i2c_device_id *id)
{
	struct device_node *of_node = client->dev.of_node;
	struct mfc_charger_data *charger;
	mfc_hal_charger_platform_data_t *pdata = client->dev.platform_data;
	int ret = 0;
#if defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
	unsigned short origin_addr = client->addr;
#endif

	dev_info(&client->dev,
		"%s: MFC Hal Charger Driver Loading\n", __func__);

#if defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
	if (client->addr != 0x3b)
		client->addr = 0x3b;
#endif

	if (of_node) {
		pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}
		ret = mfc_chg_parse_dt(&client->dev, pdata);
		if (ret < 0)
			goto err_parse_dt;
	} else {
		pdata = client->dev.platform_data;
		return -ENOMEM;
	}

	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (charger == NULL) {
		dev_err(&client->dev, "Memory is not enough.\n");
		ret = -ENOMEM;
		goto err_wpc_nomem;
	}
	charger->dev = &client->dev;

	ret = i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
		I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_I2C_BLOCK);
	if (!ret) {
		ret = i2c_get_functionality(client->adapter);
		dev_err(charger->dev, "I2C functionality is not supported.\n");
		ret = -ENOSYS;
		goto err_i2cfunc_not_support;
	}

	charger->client = client;
	charger->pdata = pdata;

	i2c_set_clientdata(client, charger);

	mutex_init(&charger->io_lock);

#if defined(CONFIG_WIRELESS_IC_PARAM)
	pr_info("%s, wireless_chip_id_param : 0x%02X, wireless_fw_ver_param : 0x%04X "
		"wireless_fw_mode_param : (0x%01X)\n", __func__,
		wireless_chip_id_param, wireless_fw_ver_param, wireless_fw_mode_param);

	if (wireless_chip_id_param == MFC_CHIP_ID_P9320 || wireless_chip_id_param == MFC_CHIP_ID_S2MIW04) {
		mfc_chip_id_now = wireless_chip_id_param;
	} else {
		mfc_check_ic_info(charger);
	}
#else
	mfc_check_ic_info(charger);
#endif
	ret = -ENODEV;

#if defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
	client->addr = origin_addr;
#endif


err_i2cfunc_not_support:
	mutex_destroy(&charger->io_lock);
	kfree(charger);
err_wpc_nomem:
err_parse_dt:
	devm_kfree(&client->dev, pdata);

	return ret;
}

static int mfc_hal_charger_remove(struct i2c_client *client)
{
	return 0;
}

static void mfc_hal_charger_shutdown(struct i2c_client *client)
{
	pr_info("%s\n", __func__);
}

static const struct i2c_device_id mfc_hal_charger_id_table[] = {
	{ "mfc-hal-charger", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, mfc_hal_charger_id_table);

#ifdef CONFIG_OF
static struct of_device_id mfc_hal_charger_match_table[] = {
	{ .compatible = "sec,mfc-hal-charger",},
	{},
};
MODULE_DEVICE_TABLE(of, mfc_hal_charger_match_table);
#else
#define mfc_hal_charger_match_table NULL
#endif

static struct i2c_driver mfc_hal_charger_driver = {
	.driver = {
		.name	= "mfc-hal-charger",
		.owner	= THIS_MODULE,
		.of_match_table = mfc_hal_charger_match_table,
	},
	.shutdown	= mfc_hal_charger_shutdown,
	.probe	= mfc_hal_charger_probe,
	.remove	= mfc_hal_charger_remove,
	.id_table	= mfc_hal_charger_id_table,
};

static int __init mfc_hal_charger_init(void)
{
	mfc_chip_id_now = 0xFF;

	pr_info("%s\n", __func__);
	return i2c_add_driver(&mfc_hal_charger_driver);
}

static void __exit mfc_hal_charger_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&mfc_hal_charger_driver);
}

module_init(mfc_hal_charger_init);
module_exit(mfc_hal_charger_exit);

MODULE_DESCRIPTION("Samsung MFC Hal Charger Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");

