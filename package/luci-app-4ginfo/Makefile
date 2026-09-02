#
# Copyright (C) 2026
#
# This is free software, licensed under the Apache License, Version 2.0.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-4ginfo
PKG_VERSION:=1.0
PKG_RELEASE:=9
PKG_LICENSE:=Apache-2.0

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-4ginfo
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI support for 4G information and configuration
  DEPENDS:=+luci-base +edgenode +libuci +rpcd-mod-file
endef

define Package/luci-app-4ginfo/description
 LuCI page for displaying and configuring EdgeNode 4G modem functions.
endef

define Build/Compile
  $(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
    -o $(PKG_BUILD_DIR)/4ginfo-modemctl ./src/modemctl.c \
    $(TARGET_LDFLAGS) -luci
endef

define Package/luci-app-4ginfo/conffiles
/etc/config/4ginfo
endef

define Package/luci-app-4ginfo/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./root/etc/config/4ginfo $(1)/etc/config/4ginfo
	chmod 0600 $(1)/etc/config/4ginfo
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/4ginfo-modemctl $(1)/usr/sbin/4ginfo-modemctl
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
