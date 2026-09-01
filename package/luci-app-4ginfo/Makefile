#
# Copyright (C) 2026
#
# This is free software, licensed under the Apache License, Version 2.0.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-4ginfo
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-4ginfo
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI support for 4G modem information
  DEPENDS:=+luci-base +edgenode
endef

define Package/luci-app-4ginfo/description
 LuCI page for displaying EdgeNode 4G modem information.
endef

define Build/Compile
endef

define Package/luci-app-4ginfo/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/4ginfo.js \
		$(1)/www/luci-static/resources/view/4ginfo.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-4ginfo.json \
		$(1)/usr/share/luci/menu.d/luci-app-4ginfo.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-4ginfo.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-4ginfo.json
endef

$(eval $(call BuildPackage,luci-app-4ginfo))
