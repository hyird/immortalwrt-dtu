#!/bin/sh

set -eu

cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

# 官方 feeds 默认关闭 WebSocket 扩展。调整构建开关并回移有来源的上游修复，不复制依赖包。
patch_file="$PWD/scripts/libwebsockets-edgenode.patch"
if patch --dry-run --forward -p1 -d feeds/packages < "$patch_file" >/dev/null 2>&1; then
    patch --forward -p1 -d feeds/packages < "$patch_file"
elif ! patch --dry-run --reverse -p1 -d feeds/packages < "$patch_file" >/dev/null 2>&1; then
    echo "libwebsockets feed recipe changed; review EdgeNode compression build options" >&2
    exit 1
fi

# 由 OpenWrt 标准 Prepare 阶段应用上游修复；遇到同名异内容补丁时拒绝覆盖。
backport_source="$PWD/scripts/libwebsockets-pmd-final.patch"
backport_target="feeds/packages/libs/libwebsockets/patches/950-edgenode-pmd-final.patch"
if [ -e "$backport_target" ] && ! cmp -s "$backport_source" "$backport_target"; then
    echo "libwebsockets backport differs; review before replacing" >&2
    exit 1
fi
mkdir -p "$(dirname "$backport_target")"
cp "$backport_source" "$backport_target"

# The feeds helper scans the current package tree before installing a feed
# source. Seed every in-tree/feed package referenced by a selected package,
# including LuCI's host-side CSS/Lua minifiers, so the scan can resolve their
# virtual host packages without warnings.
rm -rf package/feeds
mkdir -p package/feeds/packages package/feeds/luci
ln -s ../../../feeds/packages/libs/libev package/feeds/packages/libev
ln -s ../../../feeds/packages/libs/libwebsockets package/feeds/packages/libwebsockets
ln -s ../../../feeds/packages/lang/lua/luasrcdiet package/feeds/packages/luasrcdiet
ln -s ../../../feeds/luci/modules/luci-base package/feeds/luci/luci-base
ln -s ../../../feeds/luci/libs/rpcd-mod-luci package/feeds/luci/rpcd-mod-luci
ln -s ../../../feeds/packages/net/cgi-io package/feeds/packages/cgi-io
ln -s ../../../feeds/luci/contrib/package/ucode-mod-html package/feeds/luci/ucode-mod-html
ln -s ../../../feeds/luci/contrib/package/csstidy package/feeds/luci/csstidy
ln -s ../../../feeds/luci/contrib/package/lucihttp package/feeds/luci/lucihttp

./scripts/feeds install -p packages -f libev
./scripts/feeds install -p packages -f libwebsockets
./scripts/feeds install -p packages -f openssh-sftp-server
./scripts/feeds install -p luci -f luci-light
./scripts/feeds install -p luci -f luci-proto-wireguard
