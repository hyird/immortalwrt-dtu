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
- Run `make clean`, copy `configs/tas-682f.config` to `.config`, run
  `make defconfig`, and perform a full firmware build; a package-only compile is
  not a release build.

## Release verification

- Verify the generated manifest and unpack the final root filesystem.
- Confirm the selected EdgeNode version, Telnet client and server, tcpdump-mini,
  LuCI, firewall policy, embedded key permissions, and all ELF dependencies.
- Record the image size and SHA-256 digest and keep the image, manifest, seed
  config, and checksum list together as one release artifact set.
- Do not flash a device or rotate embedded keys without explicit user approval.
