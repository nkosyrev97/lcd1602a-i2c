obj-m += lcd1602a-i2c.o

KSRC ?= /path/to/the/kernel/build/directory
PWD = $(shell pwd)

all:
	make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -C $(KSRC) M=$(PWD) modules

clean:
	make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -C $(KSRC) M=$(PWD) clean

