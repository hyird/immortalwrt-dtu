# TAS-682F firmware repository guide

## Scope

- This repository is the authoritative firmware source for TAS-682F.
- Maintain EdgeNode directly under `package/network/services/edgenode` and its
  LuCI application under `package/luci-app-edgenode`.
- Track the official ImmortalWrt `packages` and `luci` master feeds directly;
  do not add private feed forks or duplicate EdgeNode packages in a feed.
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

- The designated build machine is `10.10.0.101`.
- The only release checkout is `/home/openwrtbuild/immortalwrt-dtu`.
- Make source changes in a local development checkout, then commit and push
  them to `origin` before starting a build.
- Treat the release checkout as pull-and-build only: never edit source files
  there or copy uncommitted files into it. Update it only with
  `git pull --ff-only` from a clean working tree.
- Run Git, feed, configuration, and build commands as the unprivileged
  `openwrtbuild` user.
- Update the configured feeds, then run `scripts/install-tas-feeds.sh`. This
  installs only `libev`, `libuwsc`, `openssh-sftp-server`, `luci-light`, and
  `luci-proto-wireguard` with their dependencies;
  the helper pre-seeds the three source packages needed to keep the feeds scan
  warning-free. Do not use `feeds install -a`; this targeted device tree
  intentionally omits dependencies of unused feed packages.
- Copy `configs/tastek-mt76x8.config` to `.config`, run `make defconfig`, and perform
  a full firmware image build; a package-only compile is not a release build.
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
