obj-m += lcd1602a-i2c.o

KDIR ?= /home/nkosyrev/Desktop/VisionFive2/Kernels/linux-JH7110_VF2_6.12_v6.0.0
PWD = $(shell pwd)

ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

IP_ADDR = 10.42.0.34
DEPLOY_PATH = /home/user/lcd1602a/

module:
	# module building
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

all: module

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) clean

deploy:
	scp lcd1602a-i2c.ko user@$(IP_ADDR):$(DEPLOY_PATH)
