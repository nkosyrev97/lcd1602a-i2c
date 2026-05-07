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

#define INIT_FIRST_SLEEP_MS            5
#define INIT_SECOND_SLEEP_US_MIN       150
#define INIT_SECOND_SLEEP_US_MAX       200
#define CLEAR_SLEEP_MS                 2
#define USUAL_SLEEP_US_MIN             50
#define USUAL_SLEEP_US_MAX             100

/***** List of all HD44780 commands *****/

/* Clear Display group */
#define CMD_GP_CLEAR_DISPLAY           BIT(0)
/* Return Home group */
#define CMD_GP_RETURN_HOME             BIT(1)
/* Entry Mode Set group */
#define CMD_GP_ENTRY_MODE_SET          BIT(2)
#define CMD_CURSOR_INCREMENT           BIT(1) /* 0 = CMD_SHIFT_CURSOR_DECREMENT */
#define CMD_SHIFT_DISPLAY              BIT(0)
/* Display ON/OFF group */
#define CMD_GP_DISPLAY_ONOFF           BIT(3)
#define CMD_DISPLAY_ON                 BIT(2)
#define CMD_CURSOR_ON                  BIT(1)
#define CMD_CURSOR_BLINK_ON            BIT(0)
/* Cursor or Display Shift group */
#define CMD_GP_CURSOR_DISLAY_SHIFT     BIT(4)
#define CMD_DISPLAY_OR_CURSOR_SHIFT    BIT(3) /* 1 = Display shift, 0 = Cursor Shift */
#define CMD_SHIFT_R                    BIT(2) /* 0 = Shift left */
/* Function Set */
#define CMD_GP_FUNCTION_SET            BIT(5)
#define CMD_8BIT_DATA_MODE             BIT(4) /* 0 = 4-bit */
#define CMD_2ROWS_MODE                 BIT(3) /* 0 = 1 row */
#define CMD_BIG_FONT                   BIT(2) /* 1 = 5x10, 0 = 5x8 */
/* Set CGRAM address (Character Generation RAM) */
#define CMD_GP_SET_CGRAM_ADDR          BIT(6)
#define CGRAM_LOCATION_SHIFT           3
#define CGRAM_LOCATION_MASK            GENMASK(5,3) /* b00111000 */
/* Set DDRAM address (Display Data RAM) */
#define CMD_GP_SET_DDRAM_ADDR          BIT(7)
#define DDRAM_ADDR                     GENMASK(6,0)
#define DDRAM_1ROW_OFFSET              0
#define DDRAM_2ROW_OFFSET              0x40
#define DDRAM_ROW_LENGTH               16
/* For Read Busy Flags and Current Address */
#define LCD_IS_BUSY                    BIT(7)
#define LCD_CURRENT_ADDR               GENMASK(6,0)

/***** Selected command helpers *****/

#define CMD_LCD_CLEAR                  CMD_GP_CLEAR_DISPLAY
#define CMD_SHIFT_CURSOR_R             (CMD_GP_ENTRY_MODE_SET | CMD_CURSOR_INCREMENT)
#define CMD_LCD_DISPLAY_OFF            CMD_GP_DISPLAY_ONOFF
#define CMD_LCD_DISPLAY_PLAIN          (CMD_GP_DISPLAY_ONOFF | CMD_DISPLAY_ON)
#define CMD_LCD_DISPLAY_CURSOR         (CMD_GP_DISPLAY_ONOFF | CMD_DISPLAY_ON | CMD_CURSOR_BLINK_ON)
#define CMD_4BIT_2ROWS                 (CMD_GP_FUNCTION_SET | CMD_2ROWS_MODE)
#define CMD_SET_POS_1ROW_BASE          (CMD_GP_SET_DDRAM_ADDR | DDRAM_1ROW_OFFSET)
#define CMD_SET_POS_2ROW_BASE          (CMD_GP_SET_DDRAM_ADDR | DDRAM_2ROW_OFFSET)

/***** Driver's data *****/

#define LCD_MODULE_NAME                "lcd1602a-i2c"

#define LCD_BACKLIGHT_FLAG             0
#define LCD_CURSOR_FLAG                1

struct lcd1602a_data
{
    struct device *dev;
    struct i2c_client *client;
    struct mutex lock;
    unsigned long state_flags;
};

static bool cursor_init;
module_param (cursor_init, bool, S_IRUGO);
MODULE_PARM_DESC (cursor_init, "Enable blink cursor during initialization");

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

/***** Basic LCD communication methods *****/

static int lcd1602a_get_current_address(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_rcv_byte_common(priv, 0);
    if (ret < 0) {
        dev_err(priv->dev, "Failed to get LCD's current address! (code = %d)\n", ret);
        return ret;
    }

    return (ret & LCD_CURRENT_ADDR);
}

static int lcd1602a_getchar(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_rcv_byte_common(priv, 1);
    if (ret < 0)
        dev_err(priv->dev, "Failed to get LCD's data byte! (code = %d)\n", ret);

    return ret;
}

static int lcd1602a_putchar(struct lcd1602a_data *priv, u8 ch)
{
    int ret = lcd1602a_send_data(priv, ch);
    if (ret)
        dev_err(priv->dev, "Failed to send data byte to LCD! (code = %d)\n", ret);

    return ret;
}

static int lcd1602a_clear(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_send_cmd(priv, CMD_LCD_CLEAR);
    if (ret) {
        dev_err(priv->dev, "Failed to clear LCD! (code = %d)\n", ret);
        return ret;
    }

    msleep(CLEAR_SLEEP_MS);
    return ret;
}

static int lcd1602a_backlight_op(struct lcd1602a_data *priv, bool on)
{
    int ret;
    u8 byte = (on) ? BL_PIN : 0;

    ret = i2c_smbus_write_byte(priv->client, byte);
    if (ret) {
        dev_err(priv->dev, "Failed to change LCD's backlight! (code = %d)\n", ret);
        return ret;
    }

    if (on)
        set_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags);
    else
        clear_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags);

    return ret;
}

static int lcd1602a_cursor_op(struct lcd1602a_data *priv, bool on)
{
    int ret;

    if (on) {
        ret = lcd1602a_send_cmd(priv, CMD_LCD_DISPLAY_CURSOR);
        if (ret)
            goto lcd_cursor_err;
        set_bit(LCD_CURSOR_FLAG, &priv->state_flags);
    } else {
        ret = lcd1602a_send_cmd(priv, CMD_LCD_DISPLAY_PLAIN);
        if (ret)
            goto lcd_cursor_err;
        clear_bit(LCD_CURSOR_FLAG, &priv->state_flags);
    }

    return ret;

lcd_cursor_err:
    dev_err(priv->dev, "Failed to show/hide LCD's cursor! (code = %d)\n", ret);
    return ret;
}

static int lcd1602a_init(struct lcd1602a_data *priv)
{
    /* Sync LCD and force to 4-bit mode by magic sequence */
    int ret = lcd1602a_write_nibble(priv, CMD_GP_FUNCTION_SET | CMD_8BIT_DATA_MODE, 0);
    if (ret)
        goto lcd_init_err;

    msleep(INIT_FIRST_SLEEP_MS);

    ret = lcd1602a_write_nibble(priv, CMD_GP_FUNCTION_SET | CMD_8BIT_DATA_MODE, 0);
    if (ret)
        goto lcd_init_err;

    usleep_range(INIT_SECOND_SLEEP_US_MIN, INIT_SECOND_SLEEP_US_MAX);

    ret = lcd1602a_write_nibble(priv, CMD_GP_FUNCTION_SET | CMD_8BIT_DATA_MODE, 0);
    if (ret)
        goto lcd_init_err;

    ret = lcd1602a_write_nibble(priv, CMD_GP_FUNCTION_SET, 0);
    if (ret)
        goto lcd_init_err;

    /* Now we can use regular cmds */
    ret = lcd1602a_send_cmd(priv, CMD_4BIT_2ROWS);
    if (ret)
        goto lcd_init_err;

    ret = lcd1602a_send_cmd(priv, CMD_SHIFT_CURSOR_R);
    if (ret)
        goto lcd_init_err;

    ret = lcd1602a_cursor_op(priv, cursor_init);
    if (ret)
        goto lcd_init_err;

    ret = lcd1602a_clear(priv);
    if (ret)
        goto lcd_init_err;

    ret = lcd1602a_backlight_op(priv, 1);
    if (ret)
        goto lcd_init_err;

    return ret;

lcd_init_err:
    dev_err(priv->dev, "Failed to init LCD! (code = %d)\n", ret);
    return ret;
}

static int lcd1602a_exit(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_clear(priv);
    if (ret)
        goto lcd_exit_err;

    ret = lcd1602a_send_cmd(priv, CMD_LCD_DISPLAY_OFF);
    if (ret)
        goto lcd_exit_err;

    ret = lcd1602a_backlight_op(priv, 0);
    if (ret)
        goto lcd_exit_err;

    priv->state_flags = 0;

    return ret;

lcd_exit_err:
    dev_err(priv->dev, "Failed during LCD's exit routine! (code = %d)\n", ret);
    return ret;
}

/* "Linux Device Model" (I2C) section */

static int lcd1602a_probe(struct i2c_client *client)
{
    int ret;
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

    ret = lcd1602a_init(priv);
    if (ret)
        return ret;

    dev_info(priv->dev, "lcd1602a-i2c driver is probed!\n");
    return ret;
}

static void lcd1602a_remove(struct i2c_client *client)
{
    struct lcd1602a_data *priv = dev_get_drvdata(&client->dev);

    lcd1602a_exit(priv);

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
