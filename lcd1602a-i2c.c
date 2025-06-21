#include <linux/module.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/uaccess.h>

/* PCF8574 8-bit I2C GPIO-expander */
#define GPIO0 (1 << 0)
#define GPIO1 (1 << 1)
#define GPIO2 (1 << 2)
#define GPIO3 (1 << 3)
#define GPIO4 (1 << 4)
#define GPIO5 (1 << 5)
#define GPIO6 (1 << 6)
#define GPIO7 (1 << 7)

/* PCF8574 GPIO to LCD1602A mapping */
#define RS_PIN GPIO0 /* 0 = CMD, 1 = DATA */
#define RW_PIN GPIO1 /* 0 = W, 1 = R */
#define E_PIN  GPIO2 /* 1 -> 0 strobe bit */
#define K_PIN  GPIO3 /* 1 = enable display light, 0 = disable */
#define D4_PIN GPIO4 /* DATA/CMD bit 0 */
#define D5_PIN GPIO5 /* DATA/CMD bit 1 */
#define D6_PIN GPIO6 /* DATA/CMD bit 2 */
#define D7_PIN GPIO7 /* DATA/CMD bit 3 */

/* List of commands */
#define CMD_LCD_CLEAR               0x01
#define CMD_RETURN_CURSOR           0x02
#define CMD_SHIFT_CURSOR_L          0x04
#define CMD_SHIFT_CURSOR_R          0x06
#define CMD_LCD_POWEROFF            0x08
#define CMD_REMOVE_CURSOR           0x0c
#define CMD_SQUARE_CURSOR           0x0e
#define CMD_LINE_CURSOR             0x0e
#define CMD_4BIT_1ROW               0x20
#define CMD_4BIT_2ROWS              0x28
#define CMD_GET_RAM_BASE            0x40 /* 0x40-0x7f */
#define CMD_SET_POS_0ROW_BASE       0x80 /* 0x80-0x8f */  
#define CMD_SET_POS_1ROW_BASE       0xc0 /* 0xc0-0xcf */ 

/* uDelays */
#define CMD_INIT_UDELAY     2500
#define CMD_USUAL_UDELAY    500
#define NO_CMD_UDELAY       50

struct lcd1602a_t
{
    struct device *dev;
    struct i2c_client *client;
    struct miscdevice miscdev;
    struct mutex lock;
    bool power;
    bool light;
};

static int lcd1602a_write_byte(struct i2c_client *client, u8 data)
{
    return i2c_smbus_write_byte(client, data);
}

static int lcd1602a_read_byte(struct i2c_client *client)
{
    return i2c_smbus_read_byte(client);
}

static void lcd1602a_send_command(struct lcd1602a_t *priv, u8 cmd)
{
    /* send upper 4 bits of cmd */
    u8 data = 0;
    if (priv->light)
        data |= K_PIN;
    data |= (cmd & 0xf0);
    data |= E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(NO_CMD_UDELAY);
    data &= ~E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(CMD_USUAL_UDELAY);

    /* send lower 4 bits of cmd */
    data = 0;
    if (priv->light)
        data |= K_PIN;
    data |= (cmd & 0x0f) << 4;
    data |= E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(NO_CMD_UDELAY);
    data &= ~E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(CMD_USUAL_UDELAY);
}


static void lcd1602a_poweron(struct lcd1602a_t *priv)
{
    priv->power = 1;
    priv->light = 1;
    lcd1602a_send_command(priv, CMD_LCD_CLEAR);
    udelay(CMD_INIT_UDELAY);
    lcd1602a_send_command(priv, CMD_RETURN_CURSOR);
    udelay(CMD_INIT_UDELAY);
    lcd1602a_send_command(priv, CMD_4BIT_2ROWS);
    lcd1602a_send_command(priv, CMD_LINE_CURSOR);
    lcd1602a_send_command(priv, CMD_REMOVE_CURSOR);
    lcd1602a_send_command(priv, CMD_SHIFT_CURSOR_R);
    lcd1602a_send_command(priv, CMD_LCD_CLEAR);
    udelay(CMD_INIT_UDELAY);
}

static void lcd1602a_poweroff(struct lcd1602a_t *priv)
{
    priv->power = 1;
    priv->light = 1;
    lcd1602a_send_command(priv, CMD_LCD_CLEAR);
    udelay(CMD_INIT_UDELAY);
    lcd1602a_send_command(priv, CMD_RETURN_CURSOR);
    udelay(CMD_INIT_UDELAY);
    priv->light = 0;
    lcd1602a_send_command(priv, CMD_LCD_POWEROFF);
    udelay(CMD_INIT_UDELAY);
    priv->power = 0;
}

static void lcd1602a_clear(struct lcd1602a_t *priv)
{
    lcd1602a_send_command(priv, CMD_RETURN_CURSOR);
    udelay(CMD_INIT_UDELAY);
    lcd1602a_send_command(priv, CMD_LCD_CLEAR);
    udelay(CMD_INIT_UDELAY);
    lcd1602a_send_command(priv, CMD_SHIFT_CURSOR_R);
}

static void lcd1602a_put_char(struct lcd1602a_t *priv, u8 ch)
{
    /* send upper 4 bits of char */
    u8 data = 0;
    if (priv->light)
        data |= K_PIN;
    data |= (ch & 0xf0);
    data |= RS_PIN;
    data |= E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(NO_CMD_UDELAY);
    data &= ~E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(CMD_USUAL_UDELAY);

    /* send lower 4 bits of cmd */
    data = 0;
    if (priv->light)
        data |= K_PIN;
    data |= (ch & 0x0f) << 4;
    data |= RS_PIN;
    data |= E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(NO_CMD_UDELAY);
    data &= ~E_PIN;
    lcd1602a_write_byte(priv->client, data);
    udelay(CMD_USUAL_UDELAY);
}

static ssize_t lcd1602a_read(struct file *fp, char __user *buf, size_t count, loff_t *ppos)
{
    struct lcd1602a_t *priv = (struct lcd1602a_t *) container_of(fp->private_data, struct lcd1602a_t, miscdev);
    dev_info(priv->dev, "The read op for lcd1602a is not implemented yet!\n");
    return 0;
}

static ssize_t lcd1602a_write(struct file *fp, const char __user *buf, size_t count, loff_t *ppos)
{
    unsigned char ch;
    unsigned int i = 0;

    struct lcd1602a_t *priv = (struct lcd1602a_t *) container_of(fp->private_data, struct lcd1602a_t, miscdev);

    if (!access_ok(buf, count))
    {
        dev_err(priv->dev, "Error! Sus write access from user-space!\n");
        return -EFAULT;
    }

    if (count > 16)
        count = 16;

    if (!priv->power)
        return 0;

    mutex_lock(&priv->lock);

    lcd1602a_poweron(priv);

    for (i = 0; i < count; i++)
    {
        if (get_user(ch, buf + i))
        {
            dev_err(priv->dev, "get_user() error!\n");
            return -EFAULT;
        }
        lcd1602a_put_char(priv, ch);
    }
    
    mutex_unlock(&priv->lock);

    return count;
}

static struct file_operations lcd1602a_fops = {
    .owner = THIS_MODULE,
    .read = lcd1602a_read,
    .write = lcd1602a_write,
};

static ssize_t lcd1602a_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct lcd1602a_t *priv = (struct lcd1602a_t *) dev_get_drvdata(dev);

    mutex_lock(&priv->lock);
    count = sprintf(buf, "%d\n", priv->power);
    mutex_unlock(&priv->lock);

    return count;
}

static ssize_t lcd1602a_power_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int code = 0;
    struct lcd1602a_t *priv = (struct lcd1602a_t *) dev_get_drvdata(dev);

    mutex_lock(&priv->lock);

    sscanf(buf, "%d", &code);
    if (!code)
        lcd1602a_poweroff(priv);
    else
        lcd1602a_poweron(priv);

    mutex_unlock(&priv->lock);

    return count;
}

static DEVICE_ATTR(lcd_power, S_IWUSR | S_IRUGO, lcd1602a_power_show, lcd1602a_power_store);

static int lcd1602a_probe(struct i2c_client *client)
{
    int ret;
    struct lcd1602a_t *priv;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE))
    {
        dev_err(&client->dev, "I2C_FUNC_SMBUS_BYTE is not supported by this adapter!\n");
        return -EIO;
    }

    priv = devm_kzalloc(&client->dev, sizeof(struct lcd1602a_t), GFP_KERNEL);
    if (!priv)
    {
        dev_err(&client->dev, "Error! Could not allocate priv data!\n");
        return -ENOMEM;
    }

    priv->dev = &client->dev;
    priv->client = client;

    dev_set_drvdata(priv->dev, priv);

    mutex_init(&priv->lock);

    priv->power = 0;
    priv->light = 0;

    priv->miscdev.name = "lcd1602a";
    priv->miscdev.minor = MISC_DYNAMIC_MINOR;
    priv->miscdev.parent = priv->dev;
    priv->miscdev.fops = &lcd1602a_fops;

    ret = misc_register(&priv->miscdev);
    if (ret)
    {
        dev_err(priv->dev, "Error! Could not register misc device! (%d)\n", ret);
        return ret;
    }

    lcd1602a_poweron(priv);

    dev_info(priv->dev, "lcd1602a-i2c driver is probed!\n");

    return ret;
}

static void lcd1602a_remove(struct i2c_client *client)
{
    struct lcd1602a_t *priv = (struct lcd1602a_t *) dev_get_drvdata(&client->dev);
    misc_deregister(&priv->miscdev);
    dev_info(priv->dev, "lcd1602a-i2c driver is removed!\n");
}

static const struct of_device_id lcd1602a_of_ids[] = {
    { .compatible = "nkosyrev,lcd1602a-i2c", },
    { }
};
MODULE_DEVICE_TABLE(of, lcd1602a_of_ids);

static const struct i2c_device_id lcd1602a_i2c_ids[] = {
    { .name = "lcd1602a-i2c", },
    { }
};
MODULE_DEVICE_TABLE(i2c, lcd1602a_i2c_ids);

static struct i2c_driver lcd1602a_driver = {
    .driver = {
        .name = "lcd1602a-i2c",
        .owner = THIS_MODULE,
        .of_match_table = lcd1602a_of_ids,
    },
    .probe = lcd1602a_probe,
    .remove = lcd1602a_remove,
    .id_table = lcd1602a_i2c_ids,
};

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
