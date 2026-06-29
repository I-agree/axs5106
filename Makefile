include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=axs5106
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/axs5106
  SUBMENU:=Touchscreen
  TITLE:=AXS5106 I2C Touchscreen Driver for Rockchip
  DEPENDS:=+kmod-input-core +kmod-i2c-core @TARGET_rockchip
  FILES:=$(PKG_BUILD_DIR)/src/axs5106.ko
  KCONFIG:=
  AUTOLOAD:=$(call AutoProbe,axs5106)
  DEFAULT:=y if TARGET_rockchip
endef

define KernelPackage/axs5106/description
  Kernel module for AXS5106 I2C Capacitive Touchscreen Controller.
  Adapted for HINLINK H29K / RK3528 platform.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)/src" modules
endef

$(eval $(call KernelPackage,axs5106))
