#include <linux/module.h>

#define LCD_MODULE_NAME                "lcd1602a-i2c"

#define pr_fmt(fmt)                    LCD_MODULE_NAME ": " fmt

static int num;
module_param(num, int, 0664);
MODULE_PARM_DESC(num, "Some integer number as parameter");

static int __init lcd1602a_i2c_init(void)
{
    pr_info("Hello, World! Parameter num is %d.\n", num);
    return 0;
}

static void __exit lcd1602a_i2c_exit(void)
{
    pr_info("Good bye, World! Parameter num was %d.\n", num);
}

module_init(lcd1602a_i2c_init);
module_exit(lcd1602a_i2c_exit);

MODULE_DESCRIPTION("Driver for I2C-connected LCD1602A (4 bit mode)");
MODULE_AUTHOR("Nikita Kosyrev");
MODULE_LICENSE("GPL");
