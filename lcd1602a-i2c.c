#include <linux/module.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/string.h>

#define LCD_MODULE_NAME                "lcd1602a-i2c"

struct lcd1602a_data
{
    struct device *dev;
    struct i2c_client *client;
    struct mutex lock;
};

/* "Linux Device Model" (I2C) section */

static int lcd1602a_probe(struct i2c_client *client)
{
    struct lcd1602a_data *priv;

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
