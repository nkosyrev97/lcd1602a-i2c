#include <linux/module.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/string.h>

/***** PCF8574 to LCD1602A pin-mapping *****/

#define RS_PIN                         BIT(0) /* 0 = CMD, 1 = DATA */
#define RW_PIN                         BIT(1) /* 0 = W, 1 = R */
#define E_PIN                          BIT(2) /* 1 -> 0 strobe bit */
#define BL_PIN                         BIT(3) /* 1 = enable backlight, 0 = disable */
#define D4_PIN                         BIT(4) /* DATA/CMD bit 0 */
#define D5_PIN                         BIT(5) /* DATA/CMD bit 1 */
#define D6_PIN                         BIT(6) /* DATA/CMD bit 2 */
#define D7_PIN                         BIT(7) /* DATA/CMD bit 3 */

/***** Sleep periods for HD44870 *****/

#define USUAL_SLEEP_US_MIN             50
#define USUAL_SLEEP_US_MAX             100

/***** Driver's data *****/

#define LCD_MODULE_NAME                "lcd1602a-i2c"

#define LCD_BACKLIGHT_FLAG             0

struct lcd1602a_data
{
    struct device *dev;
    struct i2c_client *client;
    struct mutex lock;
    unsigned long state_flags;
};

/***** Low-level I/O methods *****/

static void lcd1602a_error_recovery(struct lcd1602a_data *priv)
{
    u8 byte = 0;
    if (test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags))
        byte |= BL_PIN;

    /* Yes, without any error-checks - we're in error situation already */
    i2c_smbus_write_byte(priv->client, byte);
}

static int lcd1602a_read_nibble(struct lcd1602a_data *priv, u8 ctrl_half)
{
    int err, nibble;
    u8 byte = 0xf0 | (ctrl_half & 0x0f);
    if (test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags))
        byte |= BL_PIN;

    err = i2c_smbus_write_byte(priv->client, byte & ~E_PIN);
    if (err)
        goto i2c_r_err2;

    nibble = i2c_smbus_read_byte_data(priv->client, byte | E_PIN);
    if (nibble < 0)
        goto i2c_r_err1;

    err = i2c_smbus_write_byte(priv->client, byte & ~E_PIN);
    if (err)
        goto i2c_r_err2;

    nibble = (nibble >> 4) & 0x0f;
    usleep_range(USUAL_SLEEP_US_MIN, USUAL_SLEEP_US_MAX);
    return nibble;

i2c_r_err1:
    dev_err(priv->dev, "I2C read error (code = %d)!\n", nibble);
    lcd1602a_error_recovery(priv);
    return nibble;
i2c_r_err2:
    dev_err(priv->dev, "I2C write error (code = %d)!\n", err);
    lcd1602a_error_recovery(priv);
    return err;
}

static int lcd1602a_rcv_byte_common(struct lcd1602a_data *priv, bool get_char)
{
    int nibble, byte;
    u8 ctrl_flags = RW_PIN;

    if (get_char)
        ctrl_flags |= RS_PIN;

    /* rcv upper nibble (4 bits) */
    nibble = lcd1602a_read_nibble(priv, ctrl_flags);
    if (nibble < 0)
        return nibble;

    byte = (nibble & 0x0f) << 4;

    /* rcv lower nibble (4 bits) */
    nibble = lcd1602a_read_nibble(priv, ctrl_flags);
    if (nibble < 0)
        return nibble;

    byte |= nibble & 0x0f;
    return byte;
}

static int lcd1602a_write_nibble(struct lcd1602a_data *priv, u8 data_half, u8 ctrl_half)
{
    int ret;
    u8 byte = (data_half & 0xf0) | (ctrl_half & 0x0f);
    if (test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags))
        byte |= BL_PIN;

    ret = i2c_smbus_write_byte(priv->client, byte | E_PIN);
    if (ret)
        goto i2c_w_err;
    ret = i2c_smbus_write_byte(priv->client, byte & ~E_PIN);
    if (ret)
        goto i2c_w_err;

    usleep_range(USUAL_SLEEP_US_MIN, USUAL_SLEEP_US_MAX);
    return 0;

i2c_w_err:
    dev_err(priv->dev, "I2C write error (code = %d)!\n", ret);
    lcd1602a_error_recovery(priv);
    return ret;
}

static int lcd1602a_send_byte_common(struct lcd1602a_data *priv, u8 byte, bool not_cmd)
{
    int ret;
    u8 ctrl_flags = 0;

    if (not_cmd)
        ctrl_flags |= RS_PIN;

    /* send upper nibble (4 bits) */
    ret = lcd1602a_write_nibble(priv, (byte & 0xf0), ctrl_flags);
    if (ret)
        return ret;

    /* send lower 4 bits of cmd */
    ret = lcd1602a_write_nibble(priv, (byte << 4), ctrl_flags);
    return ret;
}

static inline int lcd1602a_send_cmd(struct lcd1602a_data *priv, u8 cmd)
{
    return lcd1602a_send_byte_common(priv, cmd, 0);
}

static inline int lcd1602a_send_data(struct lcd1602a_data *priv, u8 data)
{
    return lcd1602a_send_byte_common(priv, data, 1);
}

/* "Linux Device Model" (I2C) section */

static int lcd1602a_probe(struct i2c_client *client)
{
    struct lcd1602a_data *priv;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA)) {
        dev_err(&client->dev, "Required functionality is not supported by I2C-adapter!\n");
        return -EIO;
    }

    priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        dev_err(&client->dev, "Error! Could not allocate priv data!\n");
        return -ENOMEM;
    }

    priv->dev = &client->dev;
    priv->client = client;

    dev_set_drvdata(priv->dev, priv);

    mutex_init(&priv->lock);

    dev_info(priv->dev, "lcd1602a-i2c driver is probed!\n");
    return 0;
}

static void lcd1602a_remove(struct i2c_client *client)
{
    struct lcd1602a_data *priv = dev_get_drvdata(&client->dev);

    dev_info(priv->dev, "lcd1602a-i2c driver is removed!\n");
}

static const struct of_device_id lcd1602a_of_ids[] = {
    { .compatible = "nkosyrev,lcd1602a-i2c", },
    { }
};
MODULE_DEVICE_TABLE(of, lcd1602a_of_ids);

static const struct i2c_device_id lcd1602a_i2c_ids[] = {
    { .name = LCD_MODULE_NAME, },
    { }
};
MODULE_DEVICE_TABLE(i2c, lcd1602a_i2c_ids);

static struct i2c_driver lcd1602a_driver = {
    .driver = {
        .name = LCD_MODULE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = lcd1602a_of_ids,
    },
    .probe = lcd1602a_probe,
    .remove = lcd1602a_remove,
    .id_table = lcd1602a_i2c_ids,
};

/* "Linux Loadable Kernel Module" section */

static int __init lcd1602a_i2c_init(void)
{
    return i2c_add_driver(&lcd1602a_driver);
}

static void __exit lcd1602a_i2c_exit(void)
{
    i2c_del_driver(&lcd1602a_driver);
}

module_init(lcd1602a_i2c_init);
module_exit(lcd1602a_i2c_exit);

MODULE_DESCRIPTION("Driver for I2C-connected LCD1602A (4 bit mode)");
MODULE_AUTHOR("Nikita Kosyrev");
MODULE_LICENSE("GPL");
