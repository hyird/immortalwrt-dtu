#
# Tastek TAS-682F MT76x8 profile
#

DEFAULT_SOC := mt7628an

define Device/tastek_it692
  IMAGE_SIZE := 7872k
  DEVICE_VENDOR := Tastek
  DEVICE_MODEL := TAS-682F
  DEVICE_VARIANT := IT692 / SUMMIT DTU IoT
  DEVICE_PACKAGES := -apk-openssl kmod-usb2 kmod-usb-ohci \
	kmod-usb-net-rndis kmod-usb-serial-option
  SUPPORTED_DEVICES += tastek,it692 tas-682
endef
TARGET_DEVICES += tastek_it692
