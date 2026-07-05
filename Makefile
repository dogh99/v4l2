KERNELDIR := /home/megumi/rk3568_sdk/kernel
CURRENT_PATH := $(shell pwd)
obj-m := my_v4l2.o
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-none-linux-gnu-
build: kernel_modules
kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean