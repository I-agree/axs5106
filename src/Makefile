# 告诉内核我们要编译 axs5106.c 为模块 (.ko)
obj-m := axs5106.o

# 以下部分仅供您在开发机（如 Ubuntu）上本地测试编译时使用
# OpenWrt 编译时会自动忽略这部分
ifeq ($(KERNELRELEASE),)
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
endif
