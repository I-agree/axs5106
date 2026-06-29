// SPDX-License-Identifier: GPL-2.0-only
/*
 * Touchscreen driver for ChipOne AXS5106
 *
 * Copyright (C) 2026
 *
 * Adapted for OpenWrt 25.12 / Linux 6.12
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/of.h>

#define AXS5106_NAME            "axs5106"

#define AXS5106_REG_TOUCH_DATA  0x01
#define AXS5106_REG_FW_VERSION  0x05
#define AXS5106_TOUCH_DATA_LEN  6

/* Status mask: (buf[2] & 0xF0) == 0x40 means release, != 0x40 means press */
#define AXS5106_STATUS_MASK     0xF0
#define AXS5106_STATUS_RELEASE  0x40

/* Sleep command: {0x39, 0x01} */
static const u8 axs5106_sleep_cmd[] = {0x39, 0x01};

struct axs5106_data {
    struct i2c_client *client;
    struct input_dev *input;
    struct gpio_desc *reset_gpio;
    struct touchscreen_properties prop;
};

static void axs5106_reset(struct axs5106_data *data)
{
    if (!data->reset_gpio)
        return;

    /* Assert reset (Active state) */
    gpiod_set_value_cansleep(data->reset_gpio, 1);
    msleep(200);
    
    /* Deassert reset (Inactive state) */
    gpiod_set_value_cansleep(data->reset_gpio, 0);
    msleep(300); /* Wait for chip to initialize */
}

static int axs5106_read_fw_version(struct i2c_client *client, u16 *version)
{
    u8 cmd = AXS5106_REG_FW_VERSION;
    u8 buf[2];
    struct i2c_msg msgs[2] = {
        {
            .addr = client->addr,
            .flags = 0,
            .len = 1,
            .buf = &cmd,
        },
        {
            .addr = client->addr,
            .flags = I2C_M_RD,
            .len = 2,
            .buf = buf,
        },
    };
    int ret;

    ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
    if (ret != ARRAY_SIZE(msgs)) {
        dev_err(&client->dev, "Failed to read FW version: %d\n", ret);
        return ret < 0 ? ret : -EIO;
    }

    *version = (buf[0] << 8) | buf[1];
    return 0;
}

static irqreturn_t axs5106_irq_handler(int irq, void *dev_id)
{
    struct axs5106_data *data = dev_id;
    struct i2c_client *client = data->client;
    u8 cmd = AXS5106_REG_TOUCH_DATA;
    u8 buf[AXS5106_TOUCH_DATA_LEN];
    struct i2c_msg msgs[2] = {
        {
            .addr = client->addr,
            .flags = 0,
            .len = 1,
            .buf = &cmd,
        },
        {
            .addr = client->addr,
            .flags = I2C_M_RD,
            .len = AXS5106_TOUCH_DATA_LEN,
            .buf = buf,
        },
    };
    int ret;
    u16 x, y;
    bool pressed;

    ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
    if (ret != ARRAY_SIZE(msgs)) {
        dev_err_ratelimited(&client->dev, "Failed to read touch data: %d\n", ret);
        return IRQ_HANDLED;
    }

    /* Parse status: (buf[2] & 0xF0) != 0x40 means pressed */
    pressed = (buf[2] & AXS5106_STATUS_MASK) != AXS5106_STATUS_RELEASE;

    /* Parse coordinates */
    x = buf[3] | ((buf[2] & 0x0F) << 8);
    y = buf[5] | ((buf[4] & 0x0F) << 8);

    /* Report to input subsystem */
    if (pressed) {
        touchscreen_report_pos(data->input, &data->prop, x, y, false);
    }
    
    input_report_key(data->input, BTN_TOUCH, pressed);
    input_sync(data->input);

    return IRQ_HANDLED;
}

static int axs5106_probe(struct i2c_client *client)
{
    struct device *dev = &client->dev;
    struct axs5106_data *data;
    struct input_dev *input;
    u16 fw_version = 0;
    int err;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(dev, "I2C functionality not supported\n");
        return -ENODEV;
    }

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);

    /* Get reset GPIO (Optional, but recommended) */
    data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(data->reset_gpio)) {
        err = PTR_ERR(data->reset_gpio);
        dev_err(dev, "Failed to get reset GPIO: %d\n", err);
        return err;
    }

    /* Allocate input device */
    input = devm_input_allocate_device(dev);
    if (!input)
        return -ENOMEM;

    data->input = input;
    input->name = AXS5106_NAME;
    input->phys = "i2c/axs5106";
    input->id.bustype = BUS_I2C;

    /* Setup single touch capabilities */
    input_set_capability(input, EV_KEY, BTN_TOUCH);
    input_set_abs_params(input, ABS_X, 0, 0xFFFF, 0, 0);
    input_set_abs_params(input, ABS_Y, 0, 0xFFFF, 0, 0);

    /* Parse DTS properties (resolution, inversion, swap) */
    touchscreen_parse_properties(input, false, &data->prop);

    if (!data->prop.max_x || !data->prop.max_y) {
        dev_err(dev, "Touchscreen size not specified in DTS\n");
        return -EINVAL;
    }

    /* Hardware Reset */
    axs5106_reset(data);

    /* Read FW Version */
    err = axs5106_read_fw_version(client, &fw_version);
    if (err)
        dev_warn(dev, "Failed to read FW version, chip might not be ready\n");
    else
        dev_info(dev, "AXS5106 FW Version: 0x%04x\n", fw_version);

    /* Request IRQ */
    if (client->irq <= 0) {
        dev_err(dev, "No IRQ defined\n");
        return -EINVAL;
    }

    err = devm_request_threaded_irq(dev, client->irq, NULL, axs5106_irq_handler,
                                    IRQF_ONESHOT | IRQF_TRIGGER_LOW,
                                    AXS5106_NAME, data);
    if (err) {
        dev_err(dev, "Failed to request IRQ %d: %d\n", client->irq, err);
        return err;
    }

    err = input_register_device(input);
    if (err) {
        dev_err(dev, "Failed to register input device: %d\n", err);
        return err;
    }

    return 0;
}

static void axs5106_remove(struct i2c_client *client)
{
    /* Devm handles freeing resources */
}

static int axs5106_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct axs5106_data *data = i2c_get_clientdata(client);

    disable_irq(client->irq);
    
    /* Send sleep command */
    i2c_master_send(client, axs5106_sleep_cmd, sizeof(axs5106_sleep_cmd));
    
    return 0;
}

static int axs5106_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct axs5106_data *data = i2c_get_clientdata(client);

    /* Wakeup via hardware reset */
    axs5106_reset(data);
    
    enable_irq(client->irq);
    
    return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(axs5106_pm_ops, axs5106_suspend, axs5106_resume);

static const struct of_device_id axs5106_of_match[] = {
    { .compatible = "chipone,axs5106" },
    { }
};
MODULE_DEVICE_TABLE(of, axs5106_of_match);

static struct i2c_driver axs5106_driver = {
    .driver = {
        .name = AXS5106_NAME,
        .of_match_table = axs5106_of_match,
        .pm = pm_sleep_ptr(&axs5106_pm_ops),
    },
    .probe = axs5106_probe,
    .remove = axs5106_remove,
};
module_i2c_driver(axs5106_driver);

MODULE_AUTHOR("OpenWrt Community");
MODULE_DESCRIPTION("ChipOne AXS5106 Touchscreen Driver");
MODULE_LICENSE("GPL");
