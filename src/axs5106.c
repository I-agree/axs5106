// SPDX-License-Identifier: GPL-2.0-only
/*
 * Touchscreen driver for ChipOne AXS5106
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

#define AXS5106_STATUS_MASK     0xF0
#define AXS5106_STATUS_RELEASE  0x40

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

    gpiod_set_value_cansleep(data->reset_gpio, 1);
    msleep(200);
    gpiod_set_value_cansleep(data->reset_gpio, 0);
    msleep(300);
}

static int axs5106_read_fw_version(struct i2c_client *client, u16 *version)
{
    u8 cmd = AXS5106_REG_FW_VERSION;
    u8 buf[2];
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &cmd },
        { .addr = client->addr, .flags = I2C_M_RD, .len = 2, .buf = buf },
    };
    int ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
    if (ret != ARRAY_SIZE(msgs))
        return ret < 0 ? ret : -EIO;

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
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &cmd },
        { .addr = client->addr, .flags = I2C_M_RD, .len = AXS5106_TOUCH_DATA_LEN, .buf = buf },
    };
    int ret;
    u16 x, y;
    bool pressed;

    ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
    if (ret != ARRAY_SIZE(msgs))
        return IRQ_HANDLED;

    pressed = (buf[2] & AXS5106_STATUS_MASK) != AXS5106_STATUS_RELEASE;
    x = buf[3] | ((buf[2] & 0x0F) << 8);
    y = buf[5] | ((buf[4] & 0x0F) << 8);

    if (pressed)
        touchscreen_report_pos(data->input, &data->prop, x, y, false);
    
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

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
        return -ENODEV;

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);

    data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(data->reset_gpio))
        return PTR_ERR(data->reset_gpio);

    input = devm_input_allocate_device(dev);
    if (!input)
        return -ENOMEM;

    data->input = input;
    input->name = AXS5106_NAME;
    input->phys = "i2c/axs5106";
    input->id.bustype = BUS_I2C;

    input_set_capability(input, EV_KEY, BTN_TOUCH);
    input_set_abs_params(input, ABS_X, 0, 0xFFFF, 0, 0);
    input_set_abs_params(input, ABS_Y, 0, 0xFFFF, 0, 0);

    touchscreen_parse_properties(input, false, &data->prop);

    if (!data->prop.max_x || !data->prop.max_y) {
        dev_err(dev, "Touchscreen size not specified in DTS\n");
        return -EINVAL;
    }

    axs5106_reset(data);

    if (axs5106_read_fw_version(client, &fw_version) == 0)
        dev_info(dev, "AXS5106 FW Version: 0x%04x\n", fw_version);

    if (client->irq <= 0)
        return -EINVAL;

    err = devm_request_threaded_irq(dev, client->irq, NULL, axs5106_irq_handler,
                                    IRQF_ONESHOT | IRQF_TRIGGER_LOW,
                                    AXS5106_NAME, data);
    if (err)
        return err;

    return input_register_device(input);
}

static int axs5106_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    disable_irq(client->irq);
    i2c_master_send(client, axs5106_sleep_cmd, sizeof(axs5106_sleep_cmd));
    return 0;
}

static int axs5106_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct axs5106_data *data = i2c_get_clientdata(client);
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
};
module_i2c_driver(axs5106_driver);

MODULE_AUTHOR("OpenWrt Community");
MODULE_DESCRIPTION("ChipOne AXS5106 Touchscreen Driver");
MODULE_LICENSE("GPL");
