#include <linux/module.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>

#define LCD_MODULE_NAME                "lcd1602a-i2c"

#define LCD_MAJOR                      55
#define LCD_MINOR_BASE                 0
#define LCD_MINOR_COUNT                1

#define LCD_OPENED_FLAG                0
#define LCD_POWERED_FLAG               1
#define LCD_BACKLIGHT_FLAG             2
#define LCD_CURSOR_FLAG                3

/* PCF8574 GPIO pins to LCD1602A pins mapping */
#define RS_PIN                         BIT(0) /* 0 = CMD, 1 = DATA */
#define RW_PIN                         BIT(1) /* 0 = W, 1 = R */
#define E_PIN                          BIT(2) /* 1 -> 0 strobe bit */
#define K_PIN                          BIT(3) /* 1 = enable display light, 0 = disable */
#define D4_PIN                         BIT(4) /* DATA/CMD bit 0 */
#define D5_PIN                         BIT(5) /* DATA/CMD bit 1 */
#define D6_PIN                         BIT(6) /* DATA/CMD bit 2 */
#define D7_PIN                         BIT(7) /* DATA/CMD bit 3 */

/* List of HD44780 commands */
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

#define CMD_LCD_CLEAR                  CMD_GP_CLEAR_DISPLAY
#define CMD_RETURN_CURSOR              CMD_GP_RETURN_HOME
#define CMD_SHIFT_CURSOR_R             (CMD_GP_ENTRY_MODE_SET | CMD_CURSOR_INCREMENT)
#define CMD_DISPLAY_SHIFT              (CMD_GP_ENTRY_MODE_SET | CMD_CURSOR_INCREMENT | CMD_SHIFT_DISPLAY)
#define CMD_LCD_DISPLAY_OFF            CMD_GP_DISPLAY_ONOFF
#define CMD_LCD_DISPLAY_PLAIN          (CMD_GP_DISPLAY_ONOFF | CMD_DISPLAY_ON)
#define CMD_LCD_DISPLAY_CURSOR         (CMD_GP_DISPLAY_ONOFF | CMD_DISPLAY_ON | CMD_CURSOR_BLINK_ON)
#define CMD_4BIT_1ROW                  CMD_GP_FUNCTION_SET
#define CMD_4BIT_2ROWS                 (CMD_GP_FUNCTION_SET | CMD_2ROWS_MODE)
#define CMD_SET_POS_1ROW_BASE          (CMD_GP_SET_DDRAM_ADDR | DDRAM_1ROW_OFFSET)
#define CMD_SET_POS_2ROW_BASE          (CMD_GP_SET_DDRAM_ADDR | DDRAM_2ROW_OFFSET)

/* Sleep periods */
#define INIT_FIRST_SLEEP_MS            5
#define INIT_SECOND_SLEEP_US_MIN       150
#define INIT_SECOND_SLEEP_US_MAX       200
#define CLEAR_SLEEP_MS                 2
#define USUAL_SLEEP_US_MIN             50
#define USUAL_SLEEP_US_MAX             100

struct lcd1602a_data
{
    unsigned long state_flags;
    struct device *dev;
    struct i2c_client *client;
    struct cdev cdev;
    struct mutex lock;
    int irq;
    struct gpio_desc *btn;
};

static bool cursor_init;
module_param (cursor_init, bool, S_IRUGO);
MODULE_PARM_DESC (cursor_init, "Enable line cursor during initialization");

static void lcd1602a_error_recovery(struct lcd1602a_data *priv)
{
    u8 byte = 0;
    if (test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags))
        byte |= K_PIN;

    /* Yes, without any error-checks - we're in error situation already */
    i2c_smbus_write_byte(priv->client, byte);
}

static int lcd1602a_read_nibble(struct lcd1602a_data *priv, u8 ctrl_half)
{
    int err, nibble;
    u8 byte = 0xf0 | (ctrl_half & 0x0f);
    if (test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags))
        byte |= K_PIN;

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

static int lcd1602a_get_current_address(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_rcv_byte_common(priv, 0);
    if (ret < 0) {
        dev_err(priv->dev, "Failed to get LCD's current address! (code = %d)\n", ret);
        return ret;
    }

    return (ret & LCD_CURRENT_ADDR);
}

static int lcd1602a_get_data_byte(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_rcv_byte_common(priv, 1);
    if (ret < 0)
        dev_err(priv->dev, "Failed to get LCD's data byte! (code = %d)\n", ret);

    return ret;
}

static int lcd1602a_write_nibble(struct lcd1602a_data *priv, u8 data_half, u8 ctrl_half)
{
    int ret;
    u8 byte = (data_half & 0xf0) | (ctrl_half & 0x0f);
    if (test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags))
        byte |= K_PIN;

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

static int lcd1602a_putchar(struct lcd1602a_data *priv, u8 ch)
{
    int ret = lcd1602a_send_data(priv, ch);
    if (ret)
        goto lcd_putchar_err;

    return ret;

lcd_putchar_err:
    dev_err(priv->dev, "Failed to send data byte to LCD! (code = %d)\n", ret);
    return ret;
}

static int lcd1602a_set_current_address(struct lcd1602a_data *priv, unsigned int pos)
{
    int ret;
    u8 cmd = 0;

    if (pos <= DDRAM_ROW_LENGTH) {
        cmd |= CMD_SET_POS_1ROW_BASE + pos;
    } else if ((pos > DDRAM_ROW_LENGTH) && (pos <= 2 * DDRAM_ROW_LENGTH + 1)) {
        cmd |= CMD_SET_POS_2ROW_BASE + (pos - DDRAM_ROW_LENGTH - 1);
    } else {
        ret = -ENOSPC;
    }

    ret = lcd1602a_send_cmd(priv, cmd);
    if (ret)
        goto lcd_set_addr_err;

    return ret;

lcd_set_addr_err:
    dev_err(priv->dev, "Failed to set current address for LCD! (code = %d)\n", ret);
    return ret;
}

static int lcd1602a_clear(struct lcd1602a_data *priv)
{
    int ret = lcd1602a_send_cmd(priv, CMD_LCD_CLEAR);
    if (ret)
        goto lcd_clear_err;

    msleep(CLEAR_SLEEP_MS);
    return ret;

lcd_clear_err:
    dev_err(priv->dev, "Failed to clear LCD! (code = %d)\n", ret);
    return ret;
}

static int lcd1602a_backlight_op(struct lcd1602a_data *priv, bool on)
{
    int ret;
    u8 byte = (on) ? K_PIN : 0;

    ret = i2c_smbus_write_byte(priv->client, byte);
    if (ret)
       goto lcd_bl_err;

    if (on)
        set_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags);
    else
        clear_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags);

    return ret;

lcd_bl_err:
    dev_err(priv->dev, "Failed to change LCD's backlight! (code = %d)\n", ret);
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

    set_bit(LCD_POWERED_FLAG, &priv->state_flags);

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

    clear_bit(LCD_POWERED_FLAG, &priv->state_flags);

    return ret;

lcd_exit_err:
    dev_err(priv->dev, "Failed during LCD's exit routine! (code = %d)\n", ret);
    return ret;
}

static irqreturn_t lcd1602a_threaded_isr(int irq, void *dev_id)
{
    struct lcd1602a_data *priv = dev_id;
    dev_info(priv->dev, "Hello from the ISR!\n");
    return IRQ_HANDLED;
}

static inline loff_t lcd1602_llseek(struct file *file, loff_t offset, int orig)
{
    return fixed_size_llseek(file, offset, orig, 2 * (DDRAM_ROW_LENGTH + 1));
}

static int lcd1602a_open(struct inode *inode, struct file *filp)
{
    int ret = -EFAULT;
    struct lcd1602a_data *priv = container_of(inode->i_cdev, struct lcd1602a_data, cdev);

    if (test_and_set_bit(LCD_OPENED_FLAG, &priv->state_flags))
        return -EBUSY;

    filp->private_data = priv;
    filp->f_pos = 0;

    mutex_lock(&priv->lock);

    if (filp->f_flags & O_TRUNC) {
        ret = lcd1602a_clear(priv);
        if (ret)
            goto open_err;
    }

    if (filp->f_flags & O_APPEND) {
        ret = lcd1602a_get_current_address(priv);
        if (ret < 0)
            goto open_err;

        if (ret >= DDRAM_1ROW_OFFSET &&
            ret <= DDRAM_1ROW_OFFSET + DDRAM_ROW_LENGTH)
            filp->f_pos = ret - DDRAM_1ROW_OFFSET;
        else if (ret >= DDRAM_2ROW_OFFSET &&
                 ret <= DDRAM_2ROW_OFFSET + DDRAM_ROW_LENGTH)
            filp->f_pos = (ret - DDRAM_2ROW_OFFSET) + DDRAM_ROW_LENGTH + 1;
        else
            filp->f_pos = 2 * DDRAM_ROW_LENGTH + 2;
    }

    ret = 0;

open_err:
    mutex_unlock(&priv->lock);
    return ret;
}

static int lcd1602a_release(struct inode *inode, struct file *filp)
{
    struct lcd1602a_data *priv = filp->private_data;
    clear_bit(LCD_OPENED_FLAG, &priv->state_flags);
    return 0;
}

static ssize_t lcd1602a_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    int i = 0;
    ssize_t ret = -EFAULT;
    unsigned char *tmp = NULL;
    struct lcd1602a_data *priv = filp->private_data;

    /* We are going to read by rows which have 17 chars. The 17th char is always '\n'. */
    int virt_row_size = DDRAM_ROW_LENGTH + 1;
    loff_t virt_pos = *ppos;
    loff_t rel_virt_pos = virt_pos % virt_row_size;

    if (!test_bit(LCD_POWERED_FLAG, &priv->state_flags))
        return -EIO;

    if (!access_ok(buf, count)) {
        dev_err(priv->dev, "Error! Suspicious read access from user-space!\n");
        return -EFAULT;
    }

    /* Handle zero count or EOF */
    if (!count || (*ppos >= 2 * virt_row_size))
        return 0;

    if (count > virt_row_size - rel_virt_pos)
        count = virt_row_size - rel_virt_pos;

    tmp = kzalloc(count * sizeof(*tmp), GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;

    mutex_lock(&priv->lock);

    /* Sync cursor and file position */
    ret = lcd1602a_set_current_address(priv, *ppos);
    if (ret)
        goto read_err;

    for (i = 0; i < count; i++) {

        /* Check 'new line' position */
        if (rel_virt_pos == DDRAM_ROW_LENGTH) {
            lcd1602a_set_current_address(priv, virt_pos + 1);
            tmp[i] = '\n';
        } else {
            ret = lcd1602a_get_data_byte(priv);
            if (ret < 0)
                goto read_err;
            tmp[i] = ret;
        }

        virt_pos++;
        rel_virt_pos = virt_pos % virt_row_size;
    }

    mutex_unlock(&priv->lock);

    if (copy_to_user(buf, tmp, count)) {
        ret = -EFAULT;
    } else {
        *ppos = virt_pos;
        ret = count;
    }

    kfree(tmp);
    return ret;

read_err:
    mutex_unlock(&priv->lock);
    kfree(tmp);
    return ret;
}

static ssize_t lcd1602a_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    int i = 0;
    ssize_t ret = -EFAULT;
    unsigned char *tmp = NULL;
    struct lcd1602a_data *priv = filp->private_data;

    /* We are going to write by rows which have 17 chars. The 17th char is always '\n'. */
    int virt_row_size = DDRAM_ROW_LENGTH + 1;
    /* 2 phys rows by 16 chars and 1 virtual '\n' between them */
    int max_virt_size = 2 * virt_row_size - 1;
    loff_t virt_pos = *ppos;
    int rel_virt_pos = virt_pos % virt_row_size;

    if (!test_bit(LCD_POWERED_FLAG, &priv->state_flags))
        return -EIO;

    if (!access_ok(buf, count)) {
        dev_err(priv->dev, "Error! Suspicious write access from user-space!\n");
        return -EFAULT;
    }

    /* Handle EOF and zero count */
    if (*ppos >= max_virt_size)
        return -ENOSPC;
    if (!count)
        return 0;

    if (count > max_virt_size - *ppos)
        count = max_virt_size - *ppos;

    tmp = kzalloc(count * sizeof(*tmp), GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;

    if (copy_from_user(tmp, buf, count)) {
        kfree(tmp);
        return -EFAULT;
    }

    mutex_lock(&priv->lock);

    /* Sync cursor and file position */
    ret = lcd1602a_set_current_address(priv, *ppos);
    if (ret)
        goto write_err;

    for (i = 0; i < count; i++) {

        /* '\n' as 17th char in the virtual row */
        if (rel_virt_pos == DDRAM_ROW_LENGTH) {
            virt_pos++;
            if (tmp[i] == '\n')
                continue;
            rel_virt_pos = virt_pos % virt_row_size;

            /* Move cursor to the next row */
            ret = lcd1602a_set_current_address(priv, *ppos + 1);
            if (ret)
                goto write_err;
        }

        /* '\n' in the virtual row before 17th char. Fill the rest of the
         * row with spaces and then move the cursor to the next row. */
        if (tmp[i] == '\n') {
            while (rel_virt_pos < DDRAM_ROW_LENGTH) {
                ret = lcd1602a_putchar(priv, ' ');
                if (ret)
                    goto write_err;

                (*ppos)++;
                virt_pos++;
                rel_virt_pos = virt_pos % virt_row_size;
            }

            virt_pos++;
            /* Move cursor to the next row */
            ret = lcd1602a_set_current_address(priv, *ppos + 1);
            if (ret)
                goto write_err;
        } else {
            /* Usual putchar case */
            ret = lcd1602a_putchar(priv, tmp[i]);
            if (ret)
                goto write_err;

            (*ppos)++;
            virt_pos++;
        }
        rel_virt_pos = virt_pos % virt_row_size;
    }

    ret = i;

write_err:
    kfree(tmp);
    mutex_unlock(&priv->lock);
    return ret;
}

static struct file_operations lcd1602a_fops = {
    .owner = THIS_MODULE,
    .llseek = lcd1602_llseek,
    .open = lcd1602a_open,
    .release = lcd1602a_release,
    .read = lcd1602a_read,
    .write = lcd1602a_write,
};

static ssize_t lcd1602a_backlight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct lcd1602a_data *priv = dev_get_drvdata(dev);

    mutex_lock(&priv->lock);
    count = sysfs_emit(buf, "%d\n", test_bit(LCD_BACKLIGHT_FLAG, &priv->state_flags));
    mutex_unlock(&priv->lock);

    return count;
}

static ssize_t lcd1602a_backlight_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    bool res;
    struct lcd1602a_data *priv = dev_get_drvdata(dev);

    if (kstrtobool(buf, &res))
        return -EFAULT;

    mutex_lock(&priv->lock);
    if (lcd1602a_backlight_op(priv, res)) {
        mutex_unlock(&priv->lock);
        return -EFAULT;
    }

    mutex_unlock(&priv->lock);
    return count;
}

static DEVICE_ATTR(backlight, S_IWUSR | S_IRUGO, lcd1602a_backlight_show, lcd1602a_backlight_store);

static ssize_t lcd1602a_cursor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct lcd1602a_data *priv = dev_get_drvdata(dev);

    mutex_lock(&priv->lock);
    count = sysfs_emit(buf, "%d\n", test_bit(LCD_CURSOR_FLAG, &priv->state_flags));
    mutex_unlock(&priv->lock);

    return count;
}

static ssize_t lcd1602a_cursor_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    bool res;
    struct lcd1602a_data *priv = dev_get_drvdata(dev);

    if (kstrtobool(buf, &res))
        return -EFAULT;

    mutex_lock(&priv->lock);
    if (lcd1602a_cursor_op(priv, res)) {
        mutex_unlock(&priv->lock);
        return -EFAULT;
    }

    mutex_unlock(&priv->lock);
    return count;
}

static DEVICE_ATTR(cursor, S_IWUSR | S_IRUGO, lcd1602a_cursor_show, lcd1602a_cursor_store);

static int lcd1602a_probe(struct i2c_client *client)
{
    int ret;
    u32 debounce_ms;
    struct lcd1602a_data *priv;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA)) {
        dev_err(&client->dev, "I2C_FUNC_SMBUS_BYTE is not supported by this adapter!\n");
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

    priv->btn = devm_gpiod_get(priv->dev, "button", GPIOD_IN);
    if (IS_ERR(priv->btn))
        return PTR_ERR(priv->btn);

    if (!device_property_read_u32(priv->dev, "debounce-interval", &debounce_ms)) {
        ret = gpiod_set_debounce(priv->btn, debounce_ms * 1000);
        if (ret)
            dev_warn(priv->dev, "Warning! Could not set debounce! (code = %d)\n", ret);
    }

    priv->irq = gpiod_to_irq(priv->btn);
    if (priv->irq < 0)
        return priv->irq;

    ret = devm_request_threaded_irq(priv->dev, priv->irq, NULL, lcd1602a_threaded_isr,
                                    IRQF_ONESHOT | IRQF_TRIGGER_FALLING, LCD_MODULE_NAME, priv);
    if (ret) {
        dev_err(priv->dev, "Error! Could request IRQ handler! (code = %d)\n", ret);
        return ret;
    }

    ret = register_chrdev_region(MKDEV(LCD_MAJOR, LCD_MINOR_BASE), LCD_MINOR_COUNT, LCD_MODULE_NAME);
    if (ret) {
        dev_err(priv->dev, "Error! Could register major:minor numbers!\n");
        return ret;
    }

    priv->cdev.owner = THIS_MODULE;
    cdev_init(&priv->cdev, &lcd1602a_fops);
    ret = cdev_add(&priv->cdev, MKDEV(LCD_MAJOR, LCD_MINOR_BASE), LCD_MINOR_COUNT);
    if (ret) {
        dev_err(priv->dev, "Error! Could register cdev object!\n");
        goto probe_err1;
    }

    device_create_file(priv->dev, &dev_attr_cursor);
    device_create_file(priv->dev, &dev_attr_backlight);

    ret = lcd1602a_init(priv);
    if (ret)
        goto probe_err2;

    dev_info(priv->dev, "lcd1602a-i2c driver is probed!\n");
    return ret;

probe_err2:
    device_remove_file(priv->dev, &dev_attr_backlight);
    cdev_del(&priv->cdev);
probe_err1:
    unregister_chrdev_region(MKDEV(LCD_MAJOR, LCD_MINOR_BASE), LCD_MINOR_COUNT);
    return ret;
}

static void lcd1602a_remove(struct i2c_client *client)
{
    struct lcd1602a_data *priv = dev_get_drvdata(&client->dev);

    lcd1602a_exit(priv);

    device_remove_file(priv->dev, &dev_attr_backlight);
    device_remove_file(priv->dev, &dev_attr_cursor);

    cdev_del(&priv->cdev);
    unregister_chrdev_region(MKDEV(LCD_MAJOR, LCD_MINOR_BASE), LCD_MINOR_COUNT);

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
