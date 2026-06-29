include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=axs5106
PKG_RELEASE:=1

# 强制每次重新编译，避免 OpenWrt 缓存导致修改不生效
PKG_BUILD_FLAGS:=no-mips16

include $(INCLUDE_DIR)/package.mk

define KernelPackage/axs5106
  SUBMENU:=Other modules
  TITLE:=ChipOne AXS5106 I2C Touchscreen Driver
  DEPENDS:=+kmod-input-core
  FILES:=$(PKG_BUILD_DIR)/src/axs5106.ko
  AUTOLOAD:=$(call AutoProbe,axs5106)
endef

define KernelPackage/axs5106/description
  Kernel module for ChipOne AXS5106 I2C Touchscreen controller.
  Adapted for OpenWrt 25.12 / Linux 6.12.
endef

# 覆盖默认的 Compile，直接调用内核 Kbuild 编译 src 目录
define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)/src" modules
endef

$(eval $(call KernelPackage,axs5106))
