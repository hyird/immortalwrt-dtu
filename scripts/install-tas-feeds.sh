#!/bin/sh

set -eu

cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

# The feeds helper scans the current package tree before installing a feed
# source. Seed every in-tree/feed package referenced by a selected package,
# including LuCI's host-side CSS/Lua minifiers, so the scan can resolve their
# virtual host packages without warnings.
rm -rf package/feeds
mkdir -p package/feeds/packages package/feeds/luci
ln -s ../../../feeds/packages/libs/libev package/feeds/packages/libev
ln -s ../../../feeds/packages/libs/libuwsc package/feeds/packages/libuwsc
ln -s ../../../feeds/packages/lang/lua/luasrcdiet package/feeds/packages/luasrcdiet
ln -s ../../../feeds/luci/modules/luci-base package/feeds/luci/luci-base
ln -s ../../../feeds/luci/libs/rpcd-mod-luci package/feeds/luci/rpcd-mod-luci
ln -s ../../../feeds/packages/net/cgi-io package/feeds/packages/cgi-io
ln -s ../../../feeds/luci/contrib/package/ucode-mod-html package/feeds/luci/ucode-mod-html
ln -s ../../../feeds/luci/contrib/package/csstidy package/feeds/luci/csstidy
ln -s ../../../feeds/luci/contrib/package/lucihttp package/feeds/luci/lucihttp

./scripts/feeds install -p packages -f libev
./scripts/feeds install -p packages -f libuwsc
./scripts/feeds install -p packages -f openssh-sftp-server
./scripts/feeds install -p luci -f luci-light
./scripts/feeds install -p luci -f luci-proto-wireguard
