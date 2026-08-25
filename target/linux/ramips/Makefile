# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2008-2011 OpenWrt.org

include $(TOPDIR)/rules.mk

ARCH:=mipsel
BOARD:=ramips
BOARDNAME:=Tastek TAS-682F
SUBTARGETS:=mt76x8
FEATURES:=squashfs gpio

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for the Tastek TAS-682F MT7628AN DTU.
endef

include $(INCLUDE_DIR)/target.mk
DEFAULT_PACKAGES += kmod-leds-gpio kmod-gpio-button-hotplug

$(eval $(call BuildTarget))
