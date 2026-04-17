obj-m += lcd1602a-i2c.o

KSRC ?= /home/nkosyrev/Desktop/VisionFive2/Kernels/linux-JH7110_VF2_6.12_v6.0.0
PWD = $(shell pwd)

all:
	make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -C $(KSRC) M=$(PWD) modules

clean:
	make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -C $(KSRC) M=$(PWD) clean

