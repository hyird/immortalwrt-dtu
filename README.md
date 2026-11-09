<img src="https://avatars.githubusercontent.com/u/53193414?s=200&v=4" alt="logo" width="200" height="200" align="right">

# ImmortalWrt DTU firmware

This device tree follows the official **ImmortalWrt openwrt-25.12** stable branch,
currently synchronized to `3682a14dd5ec6382af65d7e72b5b415b5f3173a8`
with Linux 6.12.112. The official packages and LuCI feeds are pinned to reviewed
`openwrt-25.12` commits in `feeds.conf.default`. Do not update this tree from master.

This repository maintains three devices:

| Device | Target | Release seed |
| --- | --- | --- |
| Tastek TAS-682F | ramips/mt76x8 | `configs/tastek-mt76x8.config` |
| Tastek IT-694_s3 | ramips/mt76x8 | `configs/tastek-mt76x8.config` |
| Ebyte T113-I ECM30 | sunxi/cortexa7 | `configs/ebyte-ecm30.config` |

Unrelated board targets, bootloaders and device-specific packages are removed.
Shared build tools, libraries and kernel infrastructure remain available.

The TAS-682F / IT-694_s3 release seed builds both images together.
The independent ECM30 seed is `configs/ebyte-ecm30.config`. Linux 6.12 NAND boot,
the factory 8 KiB / 512 KiB geometry, SquashFS with a UBIFS overlay, and
configuration-preserving sysupgrade have been verified on hardware. A RAM
recovery image is built alongside the NAND firmware.
See [AGENTS.md](AGENTS.md) for the release workflow.

For the first ECM30 installation on a board with the supported factory system,
boot the initramfs recovery image from U-Boot, open LuCI at `192.168.1.1`, and
upload the ECM30 sysupgrade image under **System → Backup / Flash Firmware**.
The standard sysupgrade path checks the factory UBI layout, creates the rootfs
and overlay volumes, verifies the written kernel and rootfs, and reboots.
Subsequent upgrades use the same page and image format. The factory bootloader,
environment and 8 KiB / 512 KiB NAND geometry are retained. The unused factory
`private` and `UDISK` volumes are removed and their space goes to the overlay.
The SquashFS volume occupies 16 eraseblocks (8 MiB of NAND, 7.875 MiB usable).
The remaining UBI capacity is assigned to the UBIFS overlay. Migrating the
earlier 32/64-eraseblock rootfs allocations also uses the RAM recovery image.

ImmortalWrt is a fork of [OpenWrt](https://openwrt.org), with more packages ported, more devices supported, default optimized profiles and localization modifications for mainland China users.<br/>
Compared to upstream, we allow to use (non-upstreamable) modifications/hacks to provide better feature/performance/support.

Default login address: http://192.168.1.1 or http://immortalwrt.lan, username: __root__, password: _none_.

## Firmware images

Build the matching seed configuration from this repository. TAS-682F and
IT-694_s3 images are written to `bin/targets/ramips/mt76x8/`; ECM30 NAND and
RAM recovery images are written to `bin/targets/sunxi/cortexa7/`.

## Development
To build your own firmware you need a GNU/Linux, BSD or macOS system (case sensitive filesystem required). Cygwin is unsupported because of the lack of a case sensitive file system.<br/>

  ### Requirements
  To build with this project, Debian 11 is preferred. And you need use the CPU based on AMD64 architecture, with at least 4GB RAM and 25 GB available disk space. Make sure the __Internet__ is accessible.

  The following tools are needed to compile ImmortalWrt, the package names vary between distributions.

  - Here is an example for Debian/Ubuntu users:<br/>
    - Method 1:
      <details>
        <summary>Setup dependencies via APT</summary>

        ```bash
        sudo apt update -y
        sudo apt full-upgrade -y
        sudo apt install -y ack antlr3 asciidoc autoconf automake autopoint binutils bison build-essential \
          bzip2 ccache clang cmake cpio curl device-tree-compiler ecj fastjar flex gawk gettext gcc-multilib \
          g++-multilib git gnutls-dev gperf haveged help2man intltool lib32gcc-s1 libc6-dev-i386 libelf-dev \
          libglib2.0-dev libgmp3-dev libltdl-dev libmpc-dev libmpfr-dev libncurses-dev libpython3-dev \
          libreadline-dev libssl-dev libtool libyaml-dev libz-dev lld llvm lrzsz mkisofs msmtp nano \
          ninja-build p7zip p7zip-full patch pkgconf python3 python3-pip python3-ply python3-docutils \
          python3-pyelftools qemu-utils re2c rsync scons squashfs-tools subversion swig texinfo uglifyjs \
          upx-ucl unzip vim wget xmlto xxd zlib1g-dev zstd
        ```
      </details>
    - Method 2:
      ```bash
      sudo bash -c 'bash <(curl -s https://build-scripts.immortalwrt.org/init_build_environment.sh)'
      ```

  Note:
  - Builds may run as an unprivileged user or as root. For root builds, the top-level Makefile automatically exports `FORCE_UNSAFE_CONFIGURE=1` to host-tool configure scripts and recursive build commands. Other prerequisite checks remain enabled; `FORCE=1` is not required.
  - Use one account consistently for a build checkout so generated files remain writable by that account.
  - Using CPUs based on other architectures should be fine to compile ImmortalWrt, but more hacks are needed - No warranty at all.
  - You must __not__ have spaces or non-ascii characters in PATH or in the work folders on the drive.
  - If you're using Windows Subsystem for Linux (or WSL), removing Windows folders from PATH is required, please see [Build system setup WSL](https://openwrt.org/docs/guide-developer/build-system/wsl) documentation.
  - Using macOS as the host build OS is __not__ recommended. No warranty at all. You can get tips from [Build system setup macOS](https://openwrt.org/docs/guide-developer/build-system/buildroot.exigence.macosx) documentation.
  - For more details, please see [Build system setup](https://openwrt.org/docs/guide-developer/build-system/install-buildsystem) documentation.

  ### Quickstart
  1. Run `git clone -b main --single-branch --filter=blob:none https://github.com/hyird/immortalwrt-dtu.git` to clone this device tree.
  2. Run `cd immortalwrt-dtu` to enter the source directory.
  3. Run `./scripts/feeds update -a` to obtain the pinned official feeds.
  4. Run `./scripts/install-tas-feeds.sh` to install the selected packages and their dependencies.
  5. Run `cp configs/tastek-mt76x8.config .config && make defconfig` to select the TAS-682F and IT-694_s3 release profiles.
  6. Run `make` to build your firmware. This will download all sources, build the cross-compile toolchain and then cross-compile the GNU/Linux kernel & all chosen applications for your target system.

  For ECM30, use `cp configs/ebyte-ecm30.config .config && make defconfig` in step 5.

  ### Related Repositories
  The official package and LuCI feeds are pinned in `feeds.conf.default`.
  The release seeds use APK during image assembly and omit an on-device package manager.
  - [LuCI Web Interface](https://github.com/immortalwrt/luci): Modern and modular interface to control the device via a web browser.
  - [ImmortalWrt Packages](https://github.com/immortalwrt/packages): Community repository of ported packages.

## Support Information
The supported devices in this repository are listed above.
  ### Documentation
  - [Quick Start Guide](https://openwrt.org/docs/guide-quick-start/start)
  - [User Guide](https://openwrt.org/docs/guide-user/start)
  - [Developer Documentation](https://openwrt.org/docs/guide-developer/start)
  - [Technical Reference](https://openwrt.org/docs/techref/start)

  ### Support Community
  - Support Chat: group [@ctcgfw_openwrt_discuss](https://t.me/ctcgfw_openwrt_discuss) on [Telegram](https://telegram.org/).
  - Support Chat: group [#immortalwrt](https://matrix.to/#/#immortalwrt:matrix.org) on [Matrix](https://matrix.org/).

## License
ImmortalWrt is licensed under [GPL-2.0-only](https://spdx.org/licenses/GPL-2.0-only.html).

## Acknowledgements
<table>
  <tr>
    <td><a href="https://dlercloud.com/"><img src="https://user-images.githubusercontent.com/22235437/111103249-f9ec6e00-8588-11eb-9bfc-67cc55574555.png" width="183" height="52" border="0" alt="Dler Cloud"></a></td>
    <td><a href="https://www.jetbrains.com/"><img src="https://resources.jetbrains.com/storage/products/company/brand/logos/jb_square.png" width="120" height="120" border="0" alt="JetBrains Black Box Logo logo"></a></td>
    <td><a href="https://sourceforge.net/"><img src="https://sourceforge.net/sflogo.php?type=17&group_id=3663829" alt="SourceForge" width=200></a></td>
  </tr>
</table>
