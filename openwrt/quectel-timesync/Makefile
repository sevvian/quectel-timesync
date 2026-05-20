include $(TOPDIR)/rules.mk

PKG_NAME:=quectel-timesync
PKG_LICENSE:=GPL-2.0-only
PKG_VERSION:=$(shell git show -s --format=%cd --date=short)
PKG_RELEASE:=1

PKG_FILE_DEPENDS:=$(CURDIR)/../..

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	ln -s $(CURDIR)/../../.git $(PKG_BUILD_DIR)/.git
	cd $(PKG_BUILD_DIR) && git checkout .
endef

define Package/quectel-timesync
  SECTION:=net
  CATEGORY:=Network
  TITLE:=TOol for acquiring time from Quectel cellular modems
endef

define Package/quectel-timesync/conffiles
/etc/config/quectel-timesync
endef

define Package/quectel-timesync/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/quectel-timesync $(1)/etc/config/quectel-timesync
	$(INSTALL_BIN) ./files/etc/init.d/quectel-timesync $(1)/etc/init.d/quectel-timesync
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/quectel-timesync $(1)/usr/sbin/quectel-timesync
endef

$(eval $(call BuildPackage,quectel-timesync))
