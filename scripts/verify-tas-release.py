#!/usr/bin/env python3
"""Verify both final mt76x8 images; retain pristine rootfs trees for inspection.
Usage: verify-tas-release.py CHECKOUT ARTIFACT_DIRECTORY [tas|ecm30]
"""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tarfile


checkout, output = (Path(value).resolve() for value in sys.argv[1:3])
device = sys.argv[3] if len(sys.argv) > 3 else 'tas'
assert device in ('tas', 'ecm30')
ecm30 = device == 'ecm30'
prefix = 'immortalwrt-sunxi-cortexa7' if ecm30 else 'immortalwrt-ramips-mt76x8'
manifest = (output / (prefix + ('-ebyte_ecm30' if ecm30 else '') + '.manifest')).read_text()
packages = dict(line.split(' - ', 1) for line in manifest.splitlines() if ' - ' in line)
package_source = (checkout / 'package/network/services/edgenode/Makefile').read_text()
package_version = re.search(r'(?m)^PKG_VERSION:=(\S+)$', package_source)[1]
package_release = re.search(r'(?m)^PKG_RELEASE:=(\S+)$', package_source)[1]
assert packages['edgenode'] == f'{package_version}-r{package_release}'
for package in ('tcpdump-mini', 'luci-base', 'luci-app-edgenode', 'firewall4', 'busybox'):
    assert package in packages, package
checksums = dict((line.split()[-1].lstrip('*'), line.split()[0])
                 for line in (output / 'sha256sums').read_text().splitlines())
results = []
for profile in (('ebyte_ecm30',) if ecm30 else ('tastek_it692', 'tastek_it694')):
    image = output / f'{prefix}-{profile}-squashfs-sysupgrade.bin'
    data = image.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    assert checksums[image.name] == digest
    image_size = len(data)
    if ecm30:
        with tarfile.open(image) as archive:
            member = archive.getmember('sysupgrade-ebyte_ecm30/root')
            assert member.isfile()
            data = archive.extractfile(member).read()
            assert archive.extractfile('sysupgrade-ebyte_ecm30/kernel').read(8) == b'ANDROID!'
        assert len(data) <= 16 * (524288 - 8192), 'ECM30 rootfs exceeds 16-eraseblock UBI capacity'
    else:
        assert len(data) <= 7872 * 1024 + 65536, 'image exceeds device partition budget'
    offset = -1
    while True:
        offset = data.find(b'hsqs', offset + 1)
        assert offset >= 0, 'SquashFS missing'
        if offset + 96 > len(data):
            continue
        used = struct.unpack_from('<Q', data, offset + 40)[0]
        if struct.unpack_from('<H', data, offset + 28)[0] == 4 and 96 <= used <= len(data) - offset:
            break
    squashfs = output / (profile + '.squashfs')
    squashfs.write_bytes(data[offset:offset + used])
    root = output / (profile + '.rootfs')
    assert not root.exists(), 'verification destination already exists'
    subprocess.run([str(checkout / 'staging_dir/host/bin/unsquashfs4'), '-no-progress',
                    '-d', str(root), str(squashfs)], check=True, stdout=subprocess.DEVNULL)
    binary = root / 'usr/sbin/edgenode'
    assert package_version.encode() in binary.read_bytes()
    if ecm30:
        assert 'dtu-io' in packages and (root / 'usr/sbin/dtu-io').is_file()
        assert (root / 'etc/init.d/dtu-io').stat().st_mode & 0o111
        assert (root / 'lib/upgrade/platform.sh').read_bytes() == (checkout / 'target/linux/sunxi/base-files/lib/upgrade/platform.sh').read_bytes()
    for tool in ('telnet', 'telnetd'):
        assert any((root / directory / tool).is_symlink() or (root / directory / tool).is_file()
                   for directory in ('bin', 'usr/bin', 'usr/sbin')), tool
    assert (root / 'usr/sbin/tcpdump').is_file() or (root / 'usr/bin/tcpdump').is_file()
    assert (root / 'usr/share/ucode/luci').exists() or (root / 'usr/lib/lua/luci').exists()
    firewall = (root / 'etc/config/firewall').read_text()
    policies = {}
    for name in ('lan', 'wan'):
        zone = next(section for section in re.split(r'(?m)^config ', firewall)
                    if re.search(r"option\s+name\s+['\"]?" + name + r"['\"]?(?:\s|$)", section))
        policies[name] = re.search(r"option\s+input\s+['\"]?([A-Z]+)", zone)[1]
    assert policies['lan'] == 'ACCEPT' and policies['wan'] in ('REJECT', 'DROP')
    keys, missing, elf_count = [], [], 0
    libraries = {path.name for path in root.rglob('*.so*')}
    def target_exists(path):
        for _ in range(16):
            if not path.is_symlink():
                return path.is_file()
            link = os.readlink(path)
            path = root / link.lstrip('/') if link.startswith('/') else path.parent / link
        return False
    for path in root.rglob('*'):
        if not path.is_file() or path.is_symlink():
            continue
        name = path.name.lower()
        with path.open('rb') as stream:
            file_prefix = stream.read(128)
        if ('private' in name or name.endswith('.key') or 'host_key' in name or
                name in ('id_rsa', 'id_ed25519') or b'PRIVATE KEY-----' in file_prefix or
                path == root / 'etc/config/4ginfo'):
            mode = stat.S_IMODE(path.stat().st_mode)
            assert not mode & 0o077, 'private key permissions: ' + str(path.relative_to(root))
            assert path.stat().st_uid == 0
            keys.append({'path': str(path.relative_to(root)), 'mode': oct(mode), 'uid': 0})
        with path.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        elf_count += 1
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True, stderr=subprocess.DEVNULL)
        for dependency in re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic):
            if dependency not in libraries or not any(target_exists(root / directory / dependency)
                    for directory in ('lib', 'usr/lib', 'usr/lib/iptables', 'usr/lib/nftables')):
                missing.append([str(path.relative_to(root)), dependency])
        headers = subprocess.check_output(['readelf', '-l', str(path)], text=True, stderr=subprocess.DEVNULL)
        for interpreter in re.findall(r'Requesting program interpreter: ([^\]]+)', headers):
            assert target_exists(root / interpreter.lstrip('/')), interpreter
    assert not missing, missing
    init = (root / 'etc/init.d/edgenode').read_text()
    monitor_init = (root / 'etc/init.d/4ginfo').read_text()
    assert 'procd_set_param respawn 3600 5 -1' in init
    monitor = monitor_init.split('procd_open_instance monitor', 1)[1].split('procd_close_instance', 1)[0]
    assert 'procd_set_param respawn 3600 5 -1' in monitor
    result = dict(image=image.name, size=image_size, sha256=digest, rootfs_bytes=used,
                  edgenode=packages['edgenode'], elf_count=elf_count, missing_dependencies=missing,
                  private_key_metadata=keys, firewall=policies,
                  edgenode_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    results.append(result)
if ecm30:
    recovery = list(output.glob(prefix + '-ebyte_ecm30-*initramfs*'))
    assert recovery
    for image in recovery:
        assert hashlib.sha256(image.read_bytes()).hexdigest() == checksums[image.name]
(output / 'verification.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
