"""Run as root inside `unshare --mount --net`; all links and mounts are disposable.

Uses the production plan's nftables output, real WireGuard tunnels and identical
client/virtual addresses on both platforms. Never run in the host network namespace.
"""
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile
import time

def run(*args, input=None):
    return subprocess.check_output(args, input=input, text=True).strip()

def inside(namespace, *args, input=None):
    return run('ip', 'netns', 'exec', namespace, *args, input=input)

def main():
    assert os.geteuid() == 0, 'requires root in a disposable mount/network namespace'
    assert os.environ.get('EDGE_VPN_ISOLATED_TEST') == '1', 'must use the isolated runner'
    assert os.readlink('/proc/self/ns/net') != os.environ['EDGE_VPN_HOST_NETNS']
    assert os.readlink('/proc/self/ns/mnt') != os.environ['EDGE_VPN_HOST_MNTNS']
    fixture = pathlib.Path(sys.argv[1]).resolve()
    run('mount', '--make-rprivate', '/')
    with tempfile.TemporaryDirectory(prefix='edge-vpn-netns-') as temporary:
        work = pathlib.Path(temporary)
        (work/'netns').mkdir()
        pathlib.Path('/var/run/netns').mkdir(exist_ok=True)
        run('mount', '--bind', str(work/'netns'), '/var/run/netns')
        processes = []
        try:
            run('ip', 'link', 'set', 'lo', 'up')
            run('sysctl', '-q', '-w', 'net.ipv4.ip_forward=1', 'net.ipv4.conf.all.rp_filter=0')
            for name in ['lan', 'nodeA', 'nodeB', 'hubA', 'hubB']:
                run('ip', 'netns', 'add', name)
                inside(name, 'ip', 'link', 'set', 'lo', 'up')
            run('ip', 'link', 'add', 'lan-root', 'type', 'veth', 'peer', 'name', 'lan-peer')
            run('ip', 'link', 'set', 'lan-peer', 'netns', 'lan')
            run('ip', 'addr', 'add', '192.168.1.1/24', 'dev', 'lan-root')
            run('ip', 'link', 'set', 'lan-root', 'up')
            inside('lan', 'ip', 'addr', 'add', '192.168.1.42/24', 'dev', 'lan-peer')
            inside('lan', 'ip', 'link', 'set', 'lan-peer', 'up')
            run('nft', '-f', '-', input='table ip transit {\n chain srcnat {\n type nat hook postrouting priority srcnat; policy accept;\n ip saddr 169.254.240.0/28 masquerade\n }\n}\n')
            for slot, name in enumerate(['A', 'B']):
                node, hub = 'node'+name, 'hub'+name
                root_ip, hub_ip = f'198.18.0.{slot*4+1}', f'198.18.0.{slot*4+2}'
                host_ip, peer_ip = f'169.254.240.{slot*4+1}', f'169.254.240.{slot*4+2}'
                run('ip', 'link', 'add', 'wan'+name, 'type', 'veth', 'peer', 'name', 'peer'+name)
                run('ip', 'link', 'set', 'peer'+name, 'netns', hub)
                run('ip', 'addr', 'add', root_ip+'/30', 'dev', 'wan'+name)
                run('ip', 'link', 'set', 'wan'+name, 'up')
                inside(hub, 'ip', 'addr', 'add', hub_ip+'/30', 'dev', 'peer'+name)
                inside(hub, 'ip', 'link', 'set', 'peer'+name, 'up')
                node_key, hub_key = run('wg', 'genkey'), run('wg', 'genkey')
                node_public = run('wg', 'pubkey', input=node_key+'\n')
                hub_public = run('wg', 'pubkey', input=hub_key+'\n')
                (work/('node'+name+'.key')).write_text(node_key)
                (work/('hub'+name+'.key')).write_text(hub_key)
                os.chmod(work/('node'+name+'.key'), 0o600)
                os.chmod(work/('hub'+name+'.key'), 0o600)
                # Transport socket is born in the physical network namespace.
                run('ip', 'link', 'add', 'transport'+name, 'type', 'wireguard')
                run('wg', 'set', 'transport'+name, 'private-key', str(work/('node'+name+'.key')), 'peer', hub_public,
                    'endpoint', hub_ip+':55133', 'allowed-ips', '100.96.0.0/11,172.16.0.0/12', 'persistent-keepalive', '1')
                run('ip', 'link', 'set', 'transport'+name, 'netns', node)
                inside(node, 'ip', 'link', 'set', 'transport'+name, 'name', 'wg')
                inside(hub, 'ip', 'link', 'add', 'wg', 'type', 'wireguard')
                inside(hub, 'wg', 'set', 'wg', 'private-key', str(work/('hub'+name+'.key')), 'listen-port', '55133',
                       'peer', node_public, 'allowed-ips', '100.96.0.2/32,172.24.1.0/24')
                inside(hub, 'ip', 'addr', 'add', '100.96.0.8/32', 'dev', 'wg')
                inside(hub, 'ip', 'link', 'set', 'wg', 'up')
                inside(hub, 'ip', 'route', 'add', '172.16.0.0/12', 'dev', 'wg')
                run('ip', 'link', 'add', 'envpn'+str(slot)+'h', 'type', 'veth', 'peer', 'name', 'uplink'+name)
                run('ip', 'link', 'set', 'uplink'+name, 'netns', node)
                inside(node, 'ip', 'link', 'set', 'uplink'+name, 'name', 'uplink')
                run('ip', 'addr', 'add', host_ip+'/30', 'dev', 'envpn'+str(slot)+'h')
                run('ip', 'link', 'set', 'envpn'+str(slot)+'h', 'up')
                inside(node, 'ip', 'addr', 'add', peer_ip+'/30', 'dev', 'uplink')
                inside(node, 'ip', 'link', 'set', 'uplink', 'up')
                inside(node, 'ip', 'addr', 'add', '100.96.0.2/32', 'dev', 'wg')
                inside(node, 'ip', 'route', 'add', 'default', 'via', host_ip, 'dev', 'uplink')
                inside(node, 'ip', 'route', 'add', '100.96.0.0/11', 'dev', 'wg')
                inside(node, 'ip', 'route', 'add', '172.16.0.0/12', 'dev', 'wg')
                inside(node, 'ip', 'route', 'add', 'table', '200', 'default', 'via', host_ip, 'dev', 'uplink')
                inside(node, 'ip', 'rule', 'add', 'priority', '100', 'iif', 'wg', 'lookup', '200')
                inside(node, 'sysctl', '-q', '-w', 'net.ipv4.ip_forward=1', 'net.ipv4.conf.all.rp_filter=0',
                       'net.ipv4.conf.uplink.rp_filter=0', 'net.ipv4.conf.wg.rp_filter=0')
                rules = run(str(fixture), '--rules', *(['second'] if slot else []))
                inside(node, 'nft', '-f', '-', input=rules)
                inside(node, 'ip', 'link', 'set', 'wg', 'up')
            echo = 'import socket\ns=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(("192.168.1.42",45000))\nwhile True:\n data,addr=s.recvfrom(100);s.sendto(data+str(addr[0]).encode(),addr)\n'
            processes.append(subprocess.Popen(['ip','netns','exec','lan',sys.executable,'-u','-c',echo]))
            time.sleep(2)
            client = 'import socket,sys\ns=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.bind(("100.96.0.8",33000))\nfor i in range(20):\n data=(sys.argv[1]+str(i)).encode();s.sendto(data,(sys.argv[2],45000));reply=s.recv(100);assert reply==data+b"192.168.1.1",reply\nprint("PASS "+sys.argv[1])\n'
            def probe(name, target='172.24.1.42'):
                return inside('hub'+name, sys.executable, '-c', client, name, target)
            with concurrent.futures.ThreadPoolExecutor() as executor:
                assert list(executor.map(probe, ['A','B'])) == ['PASS A','PASS B']
            # Both flows have identical inner source IP/port, destination IP/port.
            assert inside('nodeA','wg','show','wg','transfer') != inside('nodeB','wg','show','wg','transfer')
            try:
                probe('B', '172.24.2.42')
                raise AssertionError('unmapped subnet unexpectedly reachable')
            except subprocess.CalledProcessError:
                pass
            run('ip','netns','delete','nodeA')
            assert probe('B') == 'PASS B'
            print('PASS identical addresses and flow tuples remain isolated; disabling A preserves B; unmapped destinations blocked')
        finally:
            for process in processes:
                process.terminate(); process.wait(timeout=5)
            for name in ['nodeA','nodeB','hubA','hubB','lan']:
                subprocess.run(['ip','netns','delete',name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            run('umount','/var/run/netns')

if __name__ == '__main__':
    if os.environ.get('EDGE_VPN_ISOLATED_TEST') == '1':
        main()
    else:
        environment = dict(os.environ, EDGE_VPN_ISOLATED_TEST='1',
                           EDGE_VPN_HOST_NETNS=os.readlink('/proc/self/ns/net'),
                           EDGE_VPN_HOST_MNTNS=os.readlink('/proc/self/ns/mnt'))
        subprocess.check_call(['unshare','--mount','--net',sys.executable,__file__,*sys.argv[1:]], env=environment)
