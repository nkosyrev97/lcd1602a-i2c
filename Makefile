obj-m += lcd1602a-i2c.o

KDIR ?= /home/nkosyrev/Desktop/VisionFive2/Kernels/linux-JH7110_VF2_6.12_v6.0.0
PWD = $(shell pwd)

ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

DTS_INC = -I $(KDIR)/include \
          -I $(KDIR)/arch/$(ARCH)/boot/dts/starfive \
          -I $(KDIR)/scripts/dtc/include-prefixes

all:
	# module building
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	
	# dtso preprocessing
	$(CROSS_COMPILE)gcc -E -nostdinc $(DTS_INC) -undef -D__DTS__ -x assembler-with-cpp vf2-lcd1602a-i2c.dtso > vf2-lcd1602a-i2c.dtso.tmp
	
	# dtbo compiling
	dtc -@ -I dts -O dtb -o vf2-lcd1602a-i2c.dtbo vf2-lcd1602a-i2c.dtso.tmp
	rm vf2-lcd1602a-i2c.dtso.tmp

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) clean
	rm -f *.dtbo *.dtso.tmp

