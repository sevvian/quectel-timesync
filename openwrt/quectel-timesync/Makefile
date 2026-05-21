include $(TOPDIR)/rules.mk

PKG_NAME:=quectel-timesync
PKG_VERSION:=1.0.0
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/quectel-timesync
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Tool for acquiring time from Quectel cellular modems
endef

define Package/quectel-timesync/conffiles
/etc/config/quectel-timesync
endef

# --- Override Prepare to copy source WITHOUT git ---
# CMakeLists.txt is at repo root; we copy both src/ and CMakeLists.txt
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./CMakeLists.txt $(PKG_BUILD_DIR)/
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/quectel-timesync/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/quectel-timesync $(1)/etc/config/quectel-timesync
	$(INSTALL_BIN) ./files/etc/init.d/quectel-timesync $(1)/etc/init.d/quectel-timesync
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/quectel-timesync $(1)/usr/sbin/quectel-timesync
endef

$(eval $(call BuildPackage,quectel-timesync))
