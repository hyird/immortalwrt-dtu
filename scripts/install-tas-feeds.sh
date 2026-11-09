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

# 固定的官方稳定分支 4.4.1 解析器已在用户回调前更新 inflater draining 状态。
# 旧 4.5.8 回移修复针对重构后的函数，不应重复应用到发布版。
backport_source="$PWD/scripts/libwebsockets-pmd-final.patch"
backport_target="feeds/packages/libs/libwebsockets/patches/950-edgenode-pmd-final.patch"
lws_version=$(sed -n 's/^PKG_VERSION:=//p' feeds/packages/libs/libwebsockets/Makefile)
if [ "$lws_version" != 4.4.1 ]; then
    echo "Expected the pinned openwrt-25.12 libwebsockets 4.4.1; review parser compatibility before updating" >&2
    exit 1
fi
if [ -e "$backport_target" ]; then
    if ! cmp -s "$backport_source" "$backport_target"; then
        echo "libwebsockets backport differs; review before removing" >&2
        exit 1
    fi
    rm "$backport_target"
fi

# The feeds helper scans the current package tree before installing a feed
# source. Seed every in-tree/feed package referenced by a selected package,
# including LuCI's host-side CSS/Lua minifiers, so the scan can resolve their
# virtual host packages without warnings.
rm -rf package/feeds
mkdir -p package/feeds/packages package/feeds/luci
ln -s ../../../feeds/packages/libs/libev package/feeds/packages/libev
ln -s ../../../feeds/packages/libs/libwebsockets package/feeds/packages/libwebsockets
ln -s ../../../feeds/packages/libs/libuv package/feeds/packages/libuv
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
