# TAS-682F firmware repository guide

## Scope

- This repository is the authoritative firmware source for TAS-682F.
- Maintain EdgeNode directly under `package/network/services/edgenode` and its
  LuCI application under `package/luci-app-edgenode`.
- Track the official ImmortalWrt `packages` and `luci` master feeds directly;
  do not add private feed forks or duplicate EdgeNode packages in a feed.
- Preserve compatibility with the deployed EdgeNode protocol unless a protocol
  migration and server compatibility path are implemented and tested together.
- Preserve both HTTP/WS and HTTPS/WSS platform connectivity. An HTTP platform
  must also accept HTTP firmware download URLs; keep HTTPS certificate-policy
  changes separate unless they are explicitly requested and tested.
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
- Use `configs/tas-682f.config` as the release seed configuration.

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
  installs only `libev`, `libuwsc`, `openssh-sftp-server` and `luci-light` with their dependencies;
  the helper pre-seeds the three source packages needed to keep the feeds scan
  warning-free. Do not use `feeds install -a`; this single-device tree
  intentionally omits dependencies of unused feed packages.
- Copy `configs/tas-682f.config` to `.config`, run `make defconfig`, and perform
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
