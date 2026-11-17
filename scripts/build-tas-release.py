#!/usr/bin/env python3
"""Pull-and-build release driver, invoked only after local changes are pushed.
Usage: build-tas-release.py EXPECTED_COMMIT OUTPUT_DIRECTORY [tas|ecm30]
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

checkout = Path('/home/openwrtbuild/immortalwrt-dtu')
commit, output_arg = sys.argv[1:3]
device = sys.argv[3] if len(sys.argv) > 3 else 'tas'
assert device in ('tas', 'ecm30')
ecm30 = device == 'ecm30'
seed = 'configs/ebyte-ecm30.config' if ecm30 else 'configs/tastek-mt76x8.config'
target_name = 'sunxi/cortexa7' if ecm30 else 'ramips/mt76x8'
target_config = 'CONFIG_TARGET_sunxi_cortexa7=y' if ecm30 else 'CONFIG_TARGET_ramips_mt76x8=y'
prefix = 'immortalwrt-sunxi-cortexa7' if ecm30 else 'immortalwrt-ramips-mt76x8'
output = Path(output_arg).resolve()
assert output.is_relative_to(Path('/home/openwrtbuild/releases'))
output.mkdir(mode=0o700, parents=True, exist_ok=False)
lock = open('/home/openwrtbuild/.dtu-release.lock', 'a')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)


def status(stage, **extra):
    temporary = output / 'status.tmp'
    temporary.write_text(json.dumps(dict(stage=stage, commit=commit, pid=os.getpid(), **extra)) + '\n')
    temporary.replace(output / 'status.json')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=checkout, text=True).strip()


def run(args, label, timeout=14400):
    status(label)
    with (output / (label + '.log')).open('wb') as log:
        subprocess.run(args, cwd=checkout, stdout=log, stderr=subprocess.STDOUT,
                       timeout=timeout, check=True)


try:
    assert git('status', '--porcelain', '--untracked-files=all') == '', 'checkout is not clean'
    assert git('branch', '--show-current') == 'main'
    run(['git', 'pull', '--ff-only', 'origin', 'main'], 'pull', 300)
    assert git('rev-parse', 'HEAD') == commit
    assert git('status', '--porcelain', '--untracked-files=all') == ''
    old_config = (checkout / '.config').read_text() if (checkout / '.config').exists() else ''
    run(['./scripts/feeds', 'update', '-a'], 'feeds-update', 1800)
    run(['./scripts/install-tas-feeds.sh'], 'feeds-install', 1800)
    for feed, revision in (('packages', 'fa25995d74a720c1c98bf6265d3dd8b4eb620d1f'),
                           ('luci', 'c38e5665e826f5e6e2ac9168e167ee368d0969d9')):
        assert git('-C', 'feeds/' + feed, 'rev-parse', 'HEAD') == revision
    shutil.copy2(checkout / seed, checkout / '.config')
    run(['make', 'defconfig'], 'defconfig', 1200)
    if target_config not in old_config:
        run(['make', 'clean'], 'target-change-clean', 1800)
    run(['make', '-j16'], 'image-build')
    target = checkout / 'bin/targets' / target_name
    profiles = ('ebyte_ecm30',) if ecm30 else ('tastek_it692', 'tastek_it694')
    names = [prefix + '-' + profile + '-squashfs-sysupgrade.bin' for profile in profiles]
    if ecm30:
        recovery = list(target.glob(prefix + '-ebyte_ecm30-*initramfs*'))
        assert recovery, 'missing RAM recovery image'
        names += [path.name for path in recovery if path.is_file()]
    manifest = prefix + ('-ebyte_ecm30' if ecm30 else '') + '.manifest'
    for name in names + [manifest, 'sha256sums', 'profiles.json',
                          'config.buildinfo', 'feeds.buildinfo', 'version.buildinfo']:
        shutil.copy2(target / name, output / name)
    shutil.copy2(checkout / seed, output / 'seed.config')
    shutil.copy2(checkout / '.config', output / 'build.config')
    (output / 'source.commit').write_text(commit + '\n')
    run(['python3', 'scripts/verify-tas-release.py', str(checkout), str(output), device], 'verify-images', 1200)
    if ecm30:
        run(['python3', 'scripts/test-ecm30-upgrade.py'], 'upgrade-safety-tests', 1200)
    assert git('status', '--porcelain', '--untracked-files=all') == ''
    status('images-verified', images=names)
except BaseException as error:
    status('failed', reason=str(error))
    raise
