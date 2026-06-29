include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=axs5106
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/axs5106
  SUBMENU:=Touchscreen
  TITLE:=AXS5106 I2C Touchscreen Driver
  DEPENDS:=+kmod-input-core +kmod-i2c-core
  FILES:=$(PKG_BUILD_DIR)/axs5106.ko
  AUTOLOAD:=$(call AutoProbe,axs5106)
endef

define KernelPackage/axs5106/description
  Kernel module for AXS5106 I2C Capacitive Touchscreen Controller.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,axs5106))
