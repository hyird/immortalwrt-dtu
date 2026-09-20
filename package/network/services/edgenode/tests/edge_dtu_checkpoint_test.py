"""模拟 tmpfs 丢失、删除最后一个通道、损坏文件及磁盘写入失败。"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

fixture = sys.argv[1]
with tempfile.TemporaryDirectory(prefix='edgenode-dtu-') as directory:
    root = Path(directory)
    def run(action, revision):
        subprocess.run([fixture, action, str(revision)], cwd=root, check=True, timeout=10)
    run('write', 1)
    shutil.rmtree(root / 'spool')
    run('read', 1)
    run('empty', 2)
    shutil.rmtree(root / 'spool')
    run('read', 2)
    checkpoint = next((root / 'checkpoints').iterdir())
    checkpoint.write_bytes(b'corrupt')
    shutil.rmtree(root / 'spool')
    run('read', 0)
    checkpoint.unlink()
    (root / 'checkpoints').rmdir()
    (root / 'checkpoints').write_text('blocks directory creation')
    run('reject', 3)
    run('read', 0)
print('PASS checkpoint reboot, empty replacement, corruption rejection and failed write rollback')
