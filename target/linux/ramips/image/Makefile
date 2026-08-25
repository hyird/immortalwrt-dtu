# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

KERNEL_DTB = kernel-bin | append-dtb | lzma

define Device/Default
  PROFILES := Default
  BLOCKSIZE := 64k
  KERNEL := $(KERNEL_DTB) | uImage lzma
  KERNEL_LOADADDR := 0x80000000
  LZMA_TEXT_START := 0x81800000
  SOC := $(DEFAULT_SOC)
  DEVICE_DTS_DIR := ../dts
  DEVICE_DTS = $$(SOC)_$(1)
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | append-rootfs | pad-rootfs | \
	check-size | append-metadata
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))
