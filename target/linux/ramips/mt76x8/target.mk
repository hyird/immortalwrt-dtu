#
# Copyright (C) 2009 OpenWrt.org
#

SUBTARGET:=mt76x8
BOARDNAME:=Tastek TAS-682F MT7628AN DTU
FEATURES+=usb ramdisk small_flash
CPU_TYPE:=24kc

DEFAULT_PACKAGES += swconfig

define Target/Description
	Build firmware images for the Tastek TAS-682F MT7628AN DTU.
endef
