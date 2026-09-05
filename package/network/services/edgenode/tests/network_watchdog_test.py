import pathlib
import subprocess
import tempfile

source = pathlib.Path(__file__).resolve().parents[4] / 'base-files/files/usr/sbin/network-watchdog'
functions = source.read_text().split('until last_online=')[0]
with tempfile.TemporaryDirectory(prefix='edge-watchdog-test-') as name:
    root = pathlib.Path(name)
    for pid, parent in [(1, 0), (101, 1), (202, 101)]:
        directory = root / 'proc' / str(pid)
        directory.mkdir(parents=True)
        (directory / 'cmdline').write_bytes(b'/usr/sbin/edgenode\0' if pid != 1 else b'/sbin/procd\0')
        (directory / 'stat').write_text(f'{pid} (edgenode) S {parent} ' + '0 ' * 17 + '42\n')
    (root / 'health').mkdir()
    (root / 'service').write_text(f'#!/bin/sh\necho "$1" >>"{root}/calls"\n')
    (root / 'service').chmod(0o700)
    functions = functions.replace('/proc/', str(root / 'proc') + '/')
    functions = functions.replace('/tmp/edgenode/', str(root / 'health') + '/')
    functions = functions.replace('/etc/init.d/edgenode', str(root / 'service'))
    scenario = r'''
pidof() { echo '202 101'; }
logger() { :; }
tick() { echo "$1 0" > ROOT/proc/uptime; }
health() { echo "101 $1" > ROOT/health/loop.health; }
check_count() {
    count=0
    [ ! -f ROOT/calls ] || count=$(wc -l < ROOT/calls)
    [ "$count" -eq "$1" ] || { echo "unexpected recovery count: $count expected $1"; exit 1; }
}
echo 101 > ROOT/health/loop.watchdog
tick 0; health 0; ensure_edgenode_running
[ "$edgenode_pid" = 101 ] || exit 1
tick 119; ensure_edgenode_running; check_count 0
tick 120; health 120000; ensure_edgenode_running; check_count 0
tick 239; ensure_edgenode_running; check_count 0
tick 240; ensure_edgenode_running
wait "$recovery_pid"; recovery_pid=''; check_count 1
grep -qx restart ROOT/calls || exit 1
# A new process gets startup grace even if it has not written health yet.
main_identity=''; rm ROOT/health/loop.health
tick 300; ensure_edgenode_running
tick 419; ensure_edgenode_running; check_count 1
tick 420; ensure_edgenode_running
wait "$recovery_pid"; recovery_pid=''; check_count 2
# Legacy launcher has no opt-in marker; do not restart old firmware forever.
rm ROOT/health/loop.watchdog
tick 900; ensure_edgenode_running; check_count 2
# A healthy loop remains healthy regardless of platform connectivity.
echo 101 > ROOT/health/loop.watchdog
main_identity=''
for now in 1000 1090 1180 1270 1360; do
    tick "$now"; health "$now"; ensure_edgenode_running; check_count 2
done
echo 'network watchdog tests passed'
'''.replace('ROOT', str(root))
    subprocess.run(['sh', '-n'], input=functions + scenario, text=True, check=True)
    subprocess.run(['sh'], input=functions + scenario, text=True, check=True, timeout=10)
