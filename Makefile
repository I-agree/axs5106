include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=axs5106
PKG_RELEASE:=1

# 如果源码不在根目录而在 src/ 目录下，需要指定 PKG_BUILD_DIR
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/axs5106
  SUBMENU:=Touchscreen modules
  TITLE:=ChipOne AXS5106 I2C Touchscreen Driver
  DEPENDS:=+kmod-input-core +kmod-i2c-core
  FILES:=$(PKG_BUILD_DIR)/src/axs5106.ko
  AUTOLOAD:=$(call AutoProbe,axs5106)
endef

define KernelPackage/axs5106/description
  Kernel module for ChipOne AXS5106 I2C Touchscreen controller.
  Adapted for OpenWrt 25.12 / Linux 6.12.
endef

# 准备源码：将 src 目录复制到构建目录
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/src/
endef

# 编译指令：调用内核的 Kbuild 系统
define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)/src" modules
endef

$(eval $(call KernelPackage,axs5106))
