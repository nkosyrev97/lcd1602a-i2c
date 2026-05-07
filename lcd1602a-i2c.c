#include <linux/module.h>

#define LCD_MODULE_NAME                "lcd1602a-i2c"

static int __init lcd1602a_i2c_init(void)
{
    
}

static void __exit lcd1602a_i2c_exit(void)
{
    
}

module_init(lcd1602a_i2c_init);
module_exit(lcd1602a_i2c_exit);

MODULE_DESCRIPTION("Driver for I2C-connected LCD1602A (4 bit mode)");
MODULE_AUTHOR("Nikita Kosyrev");
MODULE_LICENSE("GPL");
