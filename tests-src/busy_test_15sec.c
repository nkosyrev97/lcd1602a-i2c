#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int main()
{
    int fd = open("/dev/lcd", O_RDWR);
    if (fd < 0) {
        perror("Error! Could not open /dev/lcd!");
        return fd;
    }

    sleep(15);

    close(fd);
    return 0;
}
