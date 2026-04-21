#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

int main(void)
{
    int fd;
    char ch;

    fd = open("/dev/lcd", O_RDONLY);
    if (fd < 0) {
        perror("Error! Could not open /dev/lcd!");
        return fd;
    }

    while (read(fd, &ch, 1) > 0) {
        printf("%c", ch);
        if (lseek(fd, 1, SEEK_CUR) == -1)
            break;
    }

    close(fd);
    return 0;
}
