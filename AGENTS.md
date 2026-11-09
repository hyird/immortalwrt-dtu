# DTU firmware repository guide

## Scope

- This repository maintains TAS-682F, IT-694_s3 and Ebyte T113-I ECM30.
  Keep only the `ramips/mt76x8` and `sunxi/cortexa7` device targets, together
  with their shared build infrastructure and dependencies. Do not restore
  unrelated board targets, bootloaders or device-specific packages.
- Maintain EdgeNode directly under `package/network/services/edgenode` and its
  LuCI application under `package/luci-app-edgenode`.
- Base firmware on the official ImmortalWrt `openwrt-25.12` stable branch
  (Linux 6.12). The current synchronized upstream commit is
  `3682a14dd5ec6382af65d7e72b5b415b5f3173a8`. Review each stable-branch
  update and preserve the device pruning and local customizations. Keep
  `packages` and `luci` pinned in `feeds.conf.default` to reviewed commits from
  their official `openwrt-25.12` branches; do not track master, add private
  feed forks, or duplicate EdgeNode packages in a feed.
- Use `main` as the default and sole maintained branch. Commit and push
  directly to `origin/main`; do not create feature or task branches.
  Do not merge upstream master. Synchronize only the official
  `openwrt-25.12` stable branch; changing release series requires explicit
  user approval.
- Every EdgeNode change must remain backward compatible with deployed firmware,
  including protocol messages, platform APIs, configuration, tasks, and upgrade
  transport. Do not remove an old path until a tested migration and compatibility
  window have been provided and the user has explicitly approved the break.
- Preserve both HTTP/WS and HTTPS/WSS platform connectivity. Firmware bytes
  for new EdgeNode builds must travel through the authenticated platform
  WebSocket with bounded chunks and backpressure. The platform must retain its
  tokenized direct-download path for legacy firmware that does not advertise WS
  firmware streaming. Keep HTTPS certificate-policy changes separate unless
  they are explicitly requested and tested.
- Keep each platform's session, configuration, and outbox isolated, while one
  application-level acquisition scheduler serializes shared physical I/O.
  Serial settings are selected per task, and TCP Server listeners are shared
  by port; lower numeric platform priority is scheduled first.
- Allow every enrolled platform to open its own terminal session. Keep PTYs,
  terminal identities, sequencing, acknowledgements, and flow-control state
  isolated per platform connection, with concurrent sessions bounded by
  `EDGE_MAX_PLATFORMS`.
- Every enrolled platform may request network changes and firmware upgrades.
  Bind network confirmation to the initiating platform, broadcast network
  apply/confirm/rollback state to every enrolled platform, and keep global
  network/firmware mutations serialized.
- Keep the independent network watchdog responsible for restoring the
  EdgeNode main process when it is absent. A restart issued from EdgeNode's own
  web terminal can lose its start phase when stopping the process tears down
  the terminal, so the init restart path must survive PTY hangup and service
  recovery must not depend on that terminal session. Keep procd respawn enabled
  for both the main and modem-monitor instances.
- Use `configs/tastek-mt76x8.config` as the release seed configuration. It
  builds the TAS-682F and IT-694_s3 profiles together with per-device root
  filesystems.

## Build environment

- The designated build machine is `10.10.0.100` (the user's corrected address).
- The only release checkout is `/home/openwrtbuild/immortalwrt-dtu`.
- Make source changes in a local development checkout, then commit and push
  them to `origin` before starting a build.
- Treat the release checkout as pull-and-build only: never edit source files
  there or copy uncommitted files into it. Update it only with
  `git pull --ff-only` from a clean working tree.
- Git, feed, configuration, and build commands may run as either `root` or
  `openwrtbuild`. Root builds are explicitly supported: the top-level Makefile
  exports `FORCE_UNSAFE_CONFIGURE=1` for root and its recursive build commands.
  Keep all other prerequisite checks enabled; root builds do not require
  `FORCE=1`. Use one account consistently for a build checkout so generated
  files remain writable by that account.
- Update the configured feeds, then run `scripts/install-tas-feeds.sh`. This
  installs only `libev`, `libwebsockets`, `openssh-sftp-server`, `luci-light`, and
  `luci-proto-wireguard` with their dependencies.
  `scripts/libwebsockets-edgenode.patch` enables permessage-deflate, zlib and
  built-in libev in the official feed recipe without forking library sources.
  The pinned feed's libwebsockets 4.4.1 already sets client inflater completion
  state before callbacks. Keep `scripts/libwebsockets-pmd-final.patch` as the
  historical 4.5.8 backport; do not apply it to 4.4.1. The helper checks the
  library version and pre-seeds required source packages to keep the feeds
  scan warning-free. Do not use `feeds install -a`; this targeted device tree
  intentionally omits dependencies of unused feed packages.
- Copy `configs/tastek-mt76x8.config` to `.config`, run `make defconfig`, and perform
  a full firmware image build; a package-only compile is not a release build.
  For the separate ECM30 target, use `configs/ebyte-ecm30.config` instead.
  Preserve the factory 8192-byte logical page / 524288-byte eraseblock view.
  Keep boot0, U-Boot and secure storage write-protected; NAND installation and
  upgrades operate on UBI volumes within sys and require verified readback.
  First installation from the factory layout must use the same LuCI/sysupgrade
  path from the RAM recovery image; keep migration inside the NAND platform
  hooks, retain mbr/boot-resource/env/env-redund, and never fall back to
  formatting sys. Remove the unused private/UDISK volumes during upgrade
  and assign their space to rootfs_data, as explicitly requested by the user.
  Allocate 16 eraseblocks to the SquashFS rootfs (8 MiB NAND / 7.875 MiB UBI
  capacity); assign the remaining UBI space to rootfs_data. Shrinking the
  earlier 32/64-eraseblock allocations requires the RAM recovery upgrade path.
  Do not run global `make clean` for routine source-only updates when the build
  checkout has a verified clean baseline. Use it only when the toolchain,
  target, seed configuration, or dependency baseline changed, or when stale
  outputs are suspected.

## Release verification

- Verify the generated manifest and unpack the final root filesystem.
- Confirm the selected EdgeNode version, Telnet client and server, tcpdump-mini,
  LuCI, firewall policy, embedded key permissions, and all ELF dependencies.
- Record the image size and SHA-256 digest and keep the image, manifest, seed
  config, and checksum list together as one release artifact set.
- Do not flash a device or rotate embedded keys without explicit user approval.
