#
# Tastek TAS-682F MT76x8 profile
#

DEFAULT_SOC := mt7628an

define Device/tastek_it692
  IMAGE_SIZE := 7872k
  DEVICE_VENDOR := Tastek
  DEVICE_MODEL := TAS-682F
  DEVICE_VARIANT := IT692 / SUMMIT DTU IoT
  DEVICE_PACKAGES := -apk-openssl -apk-mbedtls -opkg \
	kmod-usb2 kmod-usb-ohci \
	kmod-usb-net-rndis kmod-usb-serial-option
  # tastek,it692 is supplied by the image framework from DEVICE_NAME.
  SUPPORTED_DEVICES += tas-682
endef
TARGET_DEVICES += tastek_it692

define Device/tastek_it694
  IMAGE_SIZE := 7872k
  DEVICE_VENDOR := Tastek
  DEVICE_MODEL := IT-694_s3
  DEVICE_VARIANT := MT7628AN / Wi-Fi / 4G DTU
  DEVICE_PACKAGES := -apk-openssl -apk-mbedtls -opkg \
	kmod-mt7603 wpad-basic-mbedtls kmod-usb2 kmod-usb-ohci \
	kmod-usb-net-rndis kmod-usb-serial-option
  # tastek,it694 is supplied by the image framework from DEVICE_NAME.
  SUPPORTED_DEVICES += it-694-s3
endef
TARGET_DEVICES += tastek_it694
