obj-m := axs5106.o

# 本地编译测试用 (在开发机上直接 make)
ifeq ($(KERNELRELEASE),)
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean
endif
