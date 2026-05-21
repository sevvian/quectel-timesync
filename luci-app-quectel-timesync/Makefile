include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-quectel-timesync
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Your Name
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-quectel-timesync
  SECTION:=luci
  CATEGORY:=LuCI
  TITLE:=LuCI web interface for Quectel Time Sync
  DEPENDS:=+luci-base +quectel-timesync
  PKGARCH:=all
endef

define Package/luci-app-quectel-timesync/description
  LuCI configuration page for quectel-timesync.
endef

define Build/Prepare
	# nothing to compile, just install files
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-quectel-timesync/install
	# Install the JavaScript view
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/quectel-timesync
	$(INSTALL_DATA) ./root/usr/share/luci-static/resources/view/quectel-timesync/form.js \
		$(1)/www/luci-static/resources/view/quectel-timesync/form.js

	# Install ACL permissions
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-quectel-timesync.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-quectel-timesync.json

	# Install menu entry
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-quectel-timesync.json \
		$(1)/usr/share/luci/menu.d/luci-app-quectel-timesync.json
endef

$(eval $(call BuildPackage,luci-app-quectel-timesync))
