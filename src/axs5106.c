// SPDX-License-Identifier: GPL-2.0-only
/*
 * AXS5106L Capacitive Touchscreen Driver
 *
 * Copyright (c) 2025 natheihei
 *
 * Adapted from CircuitPython axs5106.py driver for Linux kernel 6.12 / OpenWrt 25.12
 * Target: HINLINK H29K (RK3528)
 *
 * DTS compatible: "chipone,axs5106"
 * I2C address: 0x63
 * Interface: I2C
 * Max touch points: 5
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>

#define AXS5106_NAME            "axs5106"
#define AXS5106_I2C_ADDR        0x63

/* Register addresses */
#define AXS5106_REG_TOUCH_DATA  0x01
#define AXS5106_REG_CHIP_ID     0x08

/* Touch data constants */
#define AXS5106_MAX_TOUCH_POINTS    5
#define AXS5106_TOUCH_BUFFER_SIZE   14
#define AXS5106_TOUCH_DATA_OFFSET   2   /* Touch points start at byte 2 */
#define AXS5106_BYTES_PER_POINT     6   /* Each touch point = 6 bytes */

/* Reset timing (from Python driver) */
#define AXS5106_RESET_LOW_MS    200
#define AXS5106_RESET_HIGH_MS   300

struct axs5106_data {
    struct i2c_client       *client;
    struct input_dev        *input;
    struct gpio_desc        *reset_gpio;
    struct gpio_desc        *irq_gpio;
    u32                     max_x;
    u32                     max_y;
    bool                    invert_x;
    bool                    invert_y;
    bool                    swap_xy;
};

/**
 * axs5106_i2c_read - Read data from AXS5106 register
 * @data: driver data
 * @reg: register address
 * @buf: buffer to store read data
 * @len: number of bytes to read
 *
 * Protocol (from Python driver _read method):
 *   1. Write register address byte
 *   2. Read back 'len' bytes
 */
static int axs5106_i2c_read(struct axs5106_data *data, u8 reg,
                            u8 *buf, u16 len)
{
    struct i2c_client *client = data->client;
    struct i2c_msg msgs[2];
    int ret;

    /* Write register address */
    msgs[0].addr  = client->addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    /* Read data */
    msgs[1].addr  = client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret != 2) {
        dev_err(&client->dev, "I2C read failed: reg=0x%02x ret=%d\n",
                reg, ret);
        return ret < 0 ? ret : -EIO;
    }

    return 0;
}

/**
 * axs5106_hw_reset - Perform hardware reset sequence
 * @data: driver data
 *
 * Sequence (from Python driver __init__):
 *   1. Assert reset (low) for 200ms
 *   2. Deassert reset (high) and wait 300ms
 */
static void axs5106_hw_reset(struct axs5106_data *data)
{
    if (!data->reset_gpio)
        return;

    gpiod_set_value_cansleep(data->reset_gpio, 0);
    msleep(AXS5106_RESET_LOW_MS);
    gpiod_set_value_cansleep(data->reset_gpio, 1);
    msleep(AXS5106_RESET_HIGH_MS);
}

/**
 * axs5106_read_chip_id - Verify communication by reading chip ID
 * @data: driver data
 */
static int axs5106_read_chip_id(struct axs5106_data *data)
{
    u8 id_buf[3];
    int ret;

    ret = axs5106_i2c_read(data, AXS5106_REG_CHIP_ID, id_buf, 3);
    if (ret)
        return ret;

    dev_info(&data->client->dev,
             "Chip ID: %02X %02X %02X\n",
             id_buf[0], id_buf[1], id_buf[2]);

    if (id_buf[0] == 0 && id_buf[1] == 0 && id_buf[2] == 0)
        dev_warn(&data->client->dev,
                 "Warning: All zeros read from ID register\n");

    return 0;
}

/**
 * axs5106_transform_coord - Apply inversion/swap transformations
 * @data: driver data
 * @x: pointer to X coordinate (modified in place)
 * @y: pointer to Y coordinate (modified in place)
 *
 * DTS properties touchscreen-inverted-x, touchscreen-inverted-y,
 * touchscreen-swapped-x-y are applied here.
 */
static void axs5106_transform_coord(struct axs5106_data *data,
                                    u32 *x, u32 *y)
{
    u32 tx = *x;
    u32 ty = *y;

    if (data->invert_x)
        tx = data->max_x - 1 - tx;

    if (data->invert_y)
        ty = data->max_y - 1 - ty;

    if (data->swap_xy)
        swap(tx, ty);

    *x = tx;
    *y = ty;
}

/**
 * axs5106_parse_touch_data - Parse touch buffer and report to input subsystem
 * @data: driver data
 * @buf: raw touch data buffer (AXS5106_TOUCH_BUFFER_SIZE bytes)
 *
 * Data format (from Python driver):
 *   buf[0]   : reserved
 *   buf[1]   : touch count
 *   buf[2+i*6]   : X high nibble (bits 11:8 in low 4 bits)
 *   buf[2+i*6+1] : X low byte (bits 7:0)
 *   buf[2+i*6+2] : Y high nibble (bits 11:8 in low 4 bits)
 *   buf[2+i*6+3] : Y low byte (bits 7:0)
 *   buf[2+i*6+4] : reserved
 *   buf[2+i*6+5] : reserved
 */
static void axs5106_parse_touch_data(struct axs5106_data *data,
                                     const u8 *buf)
{
    struct input_dev *input = data->input;
    u8 touch_count;
    int i;

    touch_count = buf[1];

    dev_dbg(&data->client->dev, "Touch count: %d\n", touch_count);

    for (i = 0; i < AXS5106_MAX_TOUCH_POINTS; i++) {
        int offset;
        u32 raw_x, raw_y, x, y;

        input_mt_slot(input, i);

        if (i >= touch_count) {
            /* No touch at this slot - report inactive */
            input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
            continue;
        }

        offset = AXS5106_TOUCH_DATA_OFFSET + i * AXS5106_BYTES_PER_POINT;

        /* Parse coordinates (from Python driver) */
        raw_x = ((buf[offset] & 0x0F) << 8) | buf[offset + 1];
        raw_y = ((buf[offset + 2] & 0x0F) << 8) | buf[offset + 3];

        /* Apply coordinate transformation */
        x = raw_x;
        y = raw_y;
        axs5106_transform_coord(data, &x, &y);

        /* Clamp to valid range */
        x = min_t(u32, x, data->max_x - 1);
        y = min_t(u32, y, data->max_y - 1);

        dev_dbg(&data->client->dev,
                "Touch %d: raw(%u, %u) -> transformed(%u, %u)\n",
                i, raw_x, raw_y, x, y);

        /* Report active touch */
        input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
        input_event(input, EV_ABS, ABS_MT_POSITION_X, x);
        input_event(input, EV_ABS, ABS_MT_POSITION_Y, y);
    }

    input_mt_sync_frame(input);
    input_sync(input);
}

/**
 * axs5106_irq_handler - IRQ handler for touch events
 *
 * Called on falling edge / level-low of IRQ pin.
 * Reads touch data buffer and reports to input subsystem.
 */
static irqreturn_t axs5106_irq_handler(int irq, void *dev_id)
{
    struct axs5106_data *data = dev_id;
    u8 buf[AXS5106_TOUCH_BUFFER_SIZE];
    int ret;

    ret = axs5106_i2c_read(data, AXS5106_REG_TOUCH_DATA,
                           buf, AXS5106_TOUCH_BUFFER_SIZE);
    if (ret) {
        dev_err(&data->client->dev,
                "Failed to read touch data: %d\n", ret);
        return IRQ_HANDLED;
    }

    axs5106_parse_touch_data(data, buf);

    return IRQ_HANDLED;
}

/**
 * axs5106_parse_dt - Parse device tree properties
 * @data: driver data
 *
 * Parses standard touchscreen properties:
 *   - touchscreen-size-x
 *   - touchscreen-size-y
 *   - touchscreen-inverted-x
 *   - touchscreen-inverted-y
 *   - touchscreen-swapped-x-y
 */
static void axs5106_parse_dt(struct axs5106_data *data)
{
    struct device *dev = &data->client->dev;
    struct device_node *np = dev->of_node;
    u32 val;

    if (!np)
        return;

    if (of_property_read_u32(np, "touchscreen-size-x", &val) == 0)
        data->max_x = val;
    else
        data->max_x = 172;  /* Default from DTS: 0xac = 172 */

    if (of_property_read_u32(np, "touchscreen-size-y", &val) == 0)
        data->max_y = val;
    else
        data->max_y = 320;  /* Default from DTS: 0x140 = 320 */

    data->invert_x = of_property_read_bool(np, "touchscreen-inverted-x");
    data->invert_y = of_property_read_bool(np, "touchscreen-inverted-y");
    data->swap_xy  = of_property_read_bool(np, "touchscreen-swapped-x-y");

    dev_info(dev, "Touchscreen: %ux%u, invert_x=%d, invert_y=%d, swap_xy=%d\n",
             data->max_x, data->max_y,
             data->invert_x, data->invert_y, data->swap_xy);
}

static int axs5106_probe(struct i2c_client *client)
{
    struct axs5106_data *data;
    struct input_dev *input;
    int error;

    /* Verify I2C functionality */
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev, "I2C_FUNC_I2C not supported\n");
        return -ENODEV;
    }

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);

    /* Parse device tree */
    axs5106_parse_dt(data);

    /* Get reset GPIO (DTS: reset-gpios = <&gpio4 11 0>) */
    data->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
                                               GPIOD_OUT_HIGH);
    if (IS_ERR(data->reset_gpio)) {
        error = PTR_ERR(data->reset_gpio);
        dev_err(&client->dev, "Failed to get reset GPIO: %d\n", error);
        return error;
    }

    /* Get IRQ GPIO (DTS: irq-gpios = <&gpio4 10 0>) */
    data->irq_gpio = devm_gpiod_get_optional(&client->dev, "irq",
                                             GPIOD_IN);
    if (IS_ERR(data->irq_gpio)) {
        error = PTR_ERR(data->irq_gpio);
        dev_err(&client->dev, "Failed to get IRQ GPIO: %d\n", error);
        return error;
    }

    /* Perform hardware reset */
    axs5106_hw_reset(data);

    /* Read and verify chip ID */
    error = axs5106_read_chip_id(data);
    if (error)
        dev_warn(&client->dev,
                 "Failed to read chip ID, continuing anyway: %d\n",
                 error);

    /* Allocate input device */
    input = devm_input_allocate_device(&client->dev);
    if (!input)
        return -ENOMEM;

    data->input = input;

    input->name = "AXS5106L Touchscreen";
    input->phys = "input/axs5106";
    input->id.bustype = BUS_I2C;
    input->id.vendor  = 0x0001;
    input->id.product = 0x5106;
    input->id.version = 0x0100;

    /* Set absolute axis parameters */
    input_set_abs_params(input, ABS_MT_POSITION_X, 0, data->max_x - 1, 0, 0);
    input_set_abs_params(input, ABS_MT_POSITION_Y, 0, data->max_y - 1, 0, 0);

    /* Initialize multitouch slots */
    error = input_mt_init_slots(input, AXS5106_MAX_TOUCH_POINTS,
                                INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
    if (error) {
        dev_err(&client->dev,
                "Failed to init MT slots: %d\n", error);
        return error;
    }

    error = input_register_device(input);
    if (error) {
        dev_err(&client->dev,
                "Failed to register input device: %d\n", error);
        return error;
    }

    /* Request IRQ - DTS: interrupts = <10 8> => IRQ_TYPE_LEVEL_LOW */
    if (client->irq) {
        error = devm_request_threaded_irq(&client->dev, client->irq,
                                          NULL, axs5106_irq_handler,
                                          IRQF_ONESHOT |
                                          IRQF_TRIGGER_LOW,
                                          AXS5106_NAME, data);
        if (error) {
            dev_err(&client->dev,
                    "Failed to request IRQ %d: %d\n",
                    client->irq, error);
            return error;
        }
    } else {
        dev_warn(&client->dev, "No IRQ configured, touch will not work\n");
    }

    dev_info(&client->dev,
             "AXS5106L touchscreen probed successfully (%ux%u)\n",
             data->max_x, data->max_y);

    return 0;
}

static void axs5106_remove(struct i2c_client *client)
{
    struct axs5106_data *data = i2c_get_clientdata(client);

    /* Assert reset on removal */
    if (data->reset_gpio)
        gpiod_set_value_cansleep(data->reset_gpio, 0);

    dev_info(&client->dev, "AXS5106L touchscreen removed\n");
}

static const struct of_device_id axs5106_of_match[] = {
    { .compatible = "chipone,axs5106" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, axs5106_of_match);

static const struct i2c_device_id axs5106_i2c_id[] = {
    { AXS5106_NAME, 0 },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, axs5106_i2c_id);

static struct i2c_driver axs5106_driver = {
    .driver = {
        .name           = AXS5106_NAME,
        .of_match_table = axs5106_of_match,
    },
    .probe  = axs5106_probe,
    .remove = axs5106_remove,
    .id_table = axs5106_i2c_id,
};
module_i2c_driver(axs5106_driver);

MODULE_AUTHOR("natheihei");
MODULE_DESCRIPTION("AXS5106L Capacitive Touchscreen Driver for Linux 6.12");
MODULE_LICENSE("GPL");
