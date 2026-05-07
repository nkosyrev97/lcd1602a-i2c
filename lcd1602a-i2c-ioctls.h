#ifndef LCD1602A_I2C_IOCTLS_H
#define LCD1602A_I2C_IOCTLS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define LCD_MAGIC_IOCTL                0x4C /* ASCII 'L' */
#define LCD_CURSOR_GET_SEQ             0x01
#define LCD_CURSOR_SET_SEQ             0x02
#define LCD_IOC_CURSOR_GET             _IOR(LCD_MAGIC_IOCTL, LCD_CURSOR_GET_SEQ, unsigned int)
#define LCD_IOC_CURSOR_SET             _IOW(LCD_MAGIC_IOCTL, LCD_CURSOR_SET_SEQ, unsigned int)

#endif /* LCD1602A_I2C_IOCTLS_H */
