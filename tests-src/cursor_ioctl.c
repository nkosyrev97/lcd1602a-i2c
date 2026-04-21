#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "../lcd1602a-i2c-ioctls.h"

int main(int argc, char **argv)
{
    int fd = -1;
    int code = -1;
    int ret = -1;
    unsigned int status = 0;

    if ((argc == 3) && !strcmp(argv[1], "set") && !strcmp(argv[2], "0")) {
        code = 0;
    } else if ((argc == 3) && !strcmp(argv[1], "set") && !strcmp(argv[2], "1")) {
        code = 1;
    } else if ((argc == 2) && !strcmp(argv[1], "get")) {
        code = 2;
    } else {
        printf("Error! Command not specified!\n");
        return -1;
    }

    fd = open("/dev/lcd", O_RDWR | O_APPEND);
    if (fd < 0) {
        perror("Error! Failed to open /dev/lcd!");
        return fd;
    }

    switch (code) {
    case 0:
    case 1:
        status = (unsigned int) code;
        ret = ioctl(fd, LCD_IOC_CURSOR_SET, &status);
        if (ret < 0) {
            perror("Error: CURSOR_SET failed!");
            goto err;
        }
        break;

    case 2:
        ret = ioctl(fd, LCD_IOC_CURSOR_GET, &status);
        if (ret < 0) {
            perror("Error: CURSOR_GET failed!");
            goto err;
        } else {
            printf("cursor status = %u\n", status);
        }
        break;

    default:
        printf("Error! Unkown operation code!\n");
        ret = -1;
    }

err:
    close(fd);
    return ret;
}
