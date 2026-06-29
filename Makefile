include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=axs5106
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/axs5106
  SUBMENU:=Touchscreen modules
  TITLE:=AXS5106L I2C Capacitive Touchscreen Driver
  DEPENDS:=+kmod-input-core
  FILES:=$(PKG_BUILD_DIR)/axs5106.ko
  AUTOLOAD:=$(call AutoProbe,axs5106)
endef

define KernelPackage/axs5106/description
  Kernel module for the ChipOne AXS5106L I2C capacitive touchscreen controller.
  Adapted for HINLINK H29K (RK3528) / OpenWrt 25.12 / Kernel 6.12.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,axs5106))
