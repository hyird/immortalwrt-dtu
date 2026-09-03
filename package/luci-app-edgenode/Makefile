#
# Copyright (C) 2026
#
# This is free software, licensed under the Apache License, Version 2.0.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-edgenode
PKG_VERSION:=1.1
PKG_RELEASE:=2
PKG_LICENSE:=Apache-2.0
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-edgenode
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI support for EdgeNode
  DEPENDS:=+luci-base +edgenode
endef

define Package/luci-app-edgenode/description
 LuCI configuration and runtime status interface for EdgeNode.
endef

define Build/Compile
endef

define Package/luci-app-edgenode/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/edgenode.js \
		$(1)/www/luci-static/resources/view/edgenode.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-edgenode.json \
		$(1)/usr/share/luci/menu.d/luci-app-edgenode.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-edgenode.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-edgenode.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode
	$(INSTALL_BIN) ./root/usr/share/rpcd/ucode/luci.edgenode \
		$(1)/usr/share/rpcd/ucode/luci.edgenode
endef

$(eval $(call BuildPackage,luci-app-edgenode))
