obj-m += lcd1602a-i2c.o

KDIR ?= /home/nkosyrev/Desktop/VisionFive2/Kernels/linux-JH7110_VF2_6.12_v6.0.0
PWD = $(shell pwd)

ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

IP_ADDR = 10.42.0.34
DEPLOY_PATH = /home/user/lcd1602a/

TESTS_SRC_DIR = tests-src
TESTS_BIN_DIR = tests-bin

TESTS_SRCS = $(wildcard $(TESTS_SRC_DIR)/*.c)
TESTS_BINS = $(patsubst $(TESTS_SRC_DIR)/%.c, $(TESTS_BIN_DIR)/%, $(TESTS_SRCS))

DTS_INC = -I $(KDIR)/include \
          -I $(KDIR)/arch/$(ARCH)/boot/dts/starfive \
          -I $(KDIR)/scripts/dtc/include-prefixes

module:
	# module building
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

$(TESTS_BIN_DIR):
	mkdir -p $(TESTS_BIN_DIR)

$(TESTS_BIN_DIR)/%: $(TESTS_SRC_DIR)/%.c
	$(CROSS_COMPILE)gcc -Wall $< -o $@

tests: $(TESTS_BIN_DIR) $(TESTS_BINS)

dtbo:
	# dtso preprocessing
	$(CROSS_COMPILE)gcc -E -nostdinc $(DTS_INC) -undef -D__DTS__ -x assembler-with-cpp vf2-lcd1602a-i2c.dtso > vf2-lcd1602a-i2c.dtso.tmp
	# dtbo compiling
	dtc -@ -I dts -O dtb -o vf2-lcd1602a-i2c.dtbo vf2-lcd1602a-i2c.dtso.tmp
	rm vf2-lcd1602a-i2c.dtso.tmp

all: module tests dtbo

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) clean
	rm -rf $(TESTS_BIN_DIR)
	rm -f *.dtbo *.dtso.tmp

deploy:
	scp lcd1602a-i2c.ko $(TESTS_BIN_DIR)/* vf2-lcd1602a-i2c.dtbo attach_dtbo.sh user@$(IP_ADDR):$(DEPLOY_PATH)
