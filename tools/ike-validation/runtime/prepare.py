"""Create reviewed stage cards on this computer. Never connects to a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import sys

HERE = Path(__file__).resolve().parent
LIMIT = 1 << 20
PINS = {
    'charon': '96bf484a445a1d9332a9614b231c5beeeb82bc47c8a9cf0aaf3bbc35642acf37',
    'swanctl': '0493bd368affea54ecf950701175ec6531c6d6de8c22b4889f7867a4ac81af7f',
    'tun.ko': '2b44764dc5bee73faab5671bc5575cb08cb08c8cb68854fb8ef1a85b5ca8637d',
    'qdc-tun-probe': 'f590c4c4fd9b8fcc8b5fba45326e14d64ebbc0752abcc3c286ae5b1094d58c5b',
}


def checked_id(value):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,48}', value):
        raise ValueError('run id must contain 1..48 ASCII letters/digits/_/-')
    return value


def split_blob(blob):
    if not blob:
        raise ValueError('empty artifact')
    return [blob[n:n+LIMIT] for n in range(0, len(blob), LIMIT)]


def write_private(path, data):
    with path.open('xb') as stream:
        path.chmod(0o600)
        stream.write(data)


READONLY = """set -eu
uname -a
cat /proc/sys/kernel/random/boot_id /proc/sys/kernel/tainted /proc/modules
sha256sum /proc/config.gz
cat /proc/mounts
for qdc_n in 1 2 3; do cat /proc/meminfo; sleep 5; done
ip addr show
ip route show table all
ip -6 route show table all
cat /proc/net/udp /proc/net/udp6
cat /proc/sys/net/core/xfrm_acq_expires 2>/dev/null || true
sha256sum /etc/resolv.conf
iptables-save
ls -ld /run /dev /dev/net /dev/net/tun /sys/module/tun /run/qdc-ike 2>/dev/null || true
for qdc_p in /proc/[0-9]*; do
  qdc_x=$(readlink "$qdc_p/exe" 2>/dev/null || true)
  case "$qdc_x" in *charon*|*swanctl*|*smsweb*|*/qdc-ike-guard)
    echo "$qdc_p $qdc_x"; cat "$qdc_p/stat";; esac
done
"""
RAM_CHECK = """awk '$2=="/run" && $3=="tmpfs" && $4 ~ /(^|,)rw(,|$)/ && $4 !~ /(^|,)noexec(,|$)/ {ok=1} END {exit !ok}' /proc/mounts
"""


def build_bundle(sources, expected, destination, run):
    run = checked_id(run)
    checked_id(run+'_UNLOAD')
    if set(sources) != {'charon', 'swanctl', 'tun.ko', 'qdc-tun-probe', 'qdc-ike-guard'} or set(expected) != set(sources):
        raise ValueError('exact reviewed artifact set required')
    blobs = {}
    for name, path in sources.items():
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'{name}: regular non-symlink input required')
        blob = path.read_bytes()
        if not blob or len(blob) > 4*LIMIT or hashlib.sha256(blob).hexdigest() != expected[name]:
            raise ValueError(f'{name}: size/hash mismatch')
        blobs[name] = blob
    destination.mkdir(mode=0o700, parents=False, exist_ok=False)
    payload, cards = destination/'payload', destination/'cards'
    payload.mkdir(mode=0o700); cards.mkdir(mode=0o700)
    root = '/run/qdc-tun-'+run
    manifest = {'schema': 1, 'run_id': run, 'device_executed': False, 'ram_root': root,
                'scope': 'idle only; no peers/credentials/routes/DNS/boot persistence',
                'artifacts': {}, 'stages': []}
    owned = ['RUN_ID']

    def card(name, op='shell', seconds=30, **fields):
        q = {'op': op, 'id': name, 'seconds': seconds, **fields}
        if len(q.get('command', '').encode()) > 3300:
            raise ValueError(f'{name}: exceeds controller command limit')
        write_private(cards/(name+'.json'), (json.dumps(q, ensure_ascii=False)+'\n').encode())
        manifest['stages'].append(name)

    def shell(name, command, seconds=30, **fields):
        card(name, seconds=seconds, command=command, **fields)

    shell('00-readonly', READONLY, 40)
    shell('10-create-ram', 'set -eu\numask 077\n'+RAM_CHECK+
          "test ! -e /run/qdc-ike\ntest ! -e /sys/module/tun\ntest ! -e /dev/net/tun\n"+
          "test \"$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)\" -ge 8192\n"+
          f"mkdir {root}\nprintf '%s\\n' {shlex.quote(run)} > {root}/RUN_ID\n")
    # Bootstrap the small recovery helper before an optional, authorized SMS pause.
    items = {'qdc-ike-guard': blobs['qdc-ike-guard'],
             'qdc-tun-probe': blobs['qdc-tun-probe'],
             **{n: b for n, b in blobs.items() if n not in ('qdc-ike-guard', 'qdc-tun-probe')},
             'strongswan.conf': (HERE/'idle.conf').read_bytes()}
    for index, (name, blob) in enumerate(items.items()):
        parts = []
        for number, part in enumerate(split_blob(blob)):
            filename = f'{name}.part{number:03d}'
            path = payload/filename; write_private(path, part); parts.append(filename); owned.append(filename)
            card(f'20-{index}-{number}-push', op='push', seconds=25,
                 local=str(path.resolve()), remote=root+'/'+filename)
        digest = hashlib.sha256(blob).hexdigest()
        manifest['artifacts'][name] = {'bytes': len(blob), 'sha256': digest, 'parts': parts}
        checks = ''.join(f'{hashlib.sha256((payload/p).read_bytes()).hexdigest()}  {p}\n' for p in parts)
        assembly = 'set -eu\n'+RAM_CHECK+f'cd {root}\ntest "$(cat RUN_ID)" = {shlex.quote(run)}\n'
        assembly += f'test ! -e {shlex.quote(name)}\ntest ! -e {shlex.quote(name+".new")}\n'
        assembly += "test \"$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)\" -ge 8192\n"
        assembly += "sha256sum -c <<'QDC_PART_HASHES'\n"+checks+'QDC_PART_HASHES\n'
        assembly += f'cat {" ".join(map(shlex.quote, parts))} > {shlex.quote(name+".new")}\n'
        assembly += f"printf '%s\\n' '{digest}  {name}.new' | sha256sum -c -\n"
        assembly += f'mv {shlex.quote(name+".new")} {shlex.quote(name)}\n'
        assembly += f'chmod {"0400" if name in ("tun.ko", "strongswan.conf") else "0500"} {shlex.quote(name)}\n'
        assembly += f'rm {" ".join(map(shlex.quote, parts))}\n'
        shell(f'21-{index}-assemble', assembly)
        owned.extend([name, name+'.new'])
        if name == 'qdc-ike-guard':
            shell('22-bootstrap-selftest', f'{root}/qdc-ike-guard selftest')
        if name == 'qdc-tun-probe':
            for n in range(1, 4):
                token = run+f'_LOG{n}'
                shell(f'22-log-canary-{n}', f'{root}/qdc-tun-probe watch {token} 30 5 /bin/busybox true',
                      seconds=45, ready='LOGGER_READY '+token)
            shell('23-client-memory-gate', "set -eu\nfor qdc_n in 1 2 3; do\n"
                  "  qdc_available=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)\n"
                  "  echo MEM_GATE available_kib=$qdc_available\n"
                  "  test \"$qdc_available\" -ge 12288\n  sleep 5\ndone\n", 25)
    shell('25-selftest', f'{root}/qdc-ike-guard selftest')
    token = run+'_LOAD'
    shell('30-load-tun', f'{root}/qdc-tun-probe watch {token} 10 5 /sbin/insmod {root}/tun.ko',
          seconds=30, ready='LOGGER_READY '+token)
    shell('31-check-tun', 'set -eu\ncat /proc/modules\ncat /sys/class/misc/tun/dev\nls -ld /dev/net /dev/net/tun\ncat /proc/meminfo\n')
    for stage, duration in [('40-idle-10s', 10), ('41-idle-60s', 60)]:
        token = run+('_SMOKE' if duration == 10 else '_IDLE')
        shell(stage, f'{root}/qdc-ike-guard idle {root} {token} {duration}', seconds=90,
              ready='LOGGER_READY '+token)
        owned.append(token+'.state')
    token = run+'_UNLOAD'
    shell('80-unload-tun', f'set -eu\ntest "$(cat /sys/module/tun/refcnt)" = 0\n'
          f'test ! -e /run/qdc-ike\n{root}/qdc-tun-probe watch {token} 10 5 /sbin/rmmod tun',
          seconds=30, ready='LOGGER_READY '+token)
    release = [p for p in owned if p.startswith(('charon', 'swanctl', 'tun.ko', 'strongswan.conf'))]
    shell('85-release-client-files', 'set -eu\n'+RAM_CHECK+f'test ! -L {root}\ncd {root}\n'
          f'test "$(cat RUN_ID)" = {shlex.quote(run)}\ntest ! -e /sys/module/tun\ntest ! -e /run/qdc-ike\n'
          +'rm -f '+' '.join(map(shlex.quote, release))+'\ncat /proc/meminfo\n')
    # Manual gate: CONT original supervisor, stop its recovery guard and check HTTP.
    token = run+'_POST'
    shell('86-post-restore-log', f'{root}/qdc-tun-probe watch {token} 30 5 /bin/busybox true',
          seconds=45, ready='LOGGER_READY '+token)
    # Caller first verifies owned processes exited and restores any stopped SMS loop.
    cleanup = 'set -eu\n'+RAM_CHECK+f'test ! -L {root}\ncd {root}\n'
    cleanup += f'test "$(cat RUN_ID)" = {shlex.quote(run)}\ntest ! -e /sys/module/tun\ntest ! -e /run/qdc-ike\n'
    known = owned+['sms-guard.log']
    cleanup += 'for qdc_entry in * .[!.]* ..?*; do\n'
    cleanup += '  if test -e "$qdc_entry" || test -L "$qdc_entry"; then\n'
    cleanup += '    case "$qdc_entry" in\n      '+'|'.join(map(shlex.quote, known))+') test ! -d "$qdc_entry";;\n'
    cleanup += '      *) echo "UNEXPECTED_FILE $qdc_entry"; exit 1;;\n    esac\n  fi\ndone\n'
    cleanup += 'rm -f '+' '.join(map(shlex.quote, [p for p in known if p != 'RUN_ID']))+'\nrm RUN_ID\n'
    cleanup += f'cd /\nrmdir {root}\n'
    shell('90-clean-owned-files', cleanup)
    shell('99-readonly-after', READONLY, 40)
    manifest.update(preflight_kib=12288, start_after_staging_kib=8192, stop_below_kib=6144, stop_drop_kib=3072)
    manifest['manual_gates'] = ['authorization', 'fresh USB/kernel/HTTP baseline', 'physical recovery availability',
        'SMS guard and exact process identities if pausing', 'one card at a time; no replay',
        'normal driver unload before deleting files', 'owned /dev/net/tun cleanup if mdev leaves it']
    write_private(destination/'manifest.json', (json.dumps(manifest, indent=2, ensure_ascii=False)+'\n').encode())
    write_private(destination/'READ-ME-FIRST.txt',
        ('仅电脑准备，未执行。不要把 cards 目录批量发送给控制器。\n'
         '先阅读仓库 TEMP_RUNTIME_TEST_PLAN.zh-CN.md。先授权实测，再逐项发送单张请求卡并审阅结果。\n'
         '按 manifest.stages 顺序逐项核对：每个 20-i 分片组完成后执行对应 21-i 合并卡，再进入下一组。\n'
         '短信暂停/恢复、异常处理和字符节点清理有人工核验门槛，不能靠最后一张卡替代。\n').encode())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('local', type=Path, help='existing ike-validation-local')
    parser.add_argument('--tun-local', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='new local directory, never upload destination')
    parser.add_argument('--run', required=True)
    args = parser.parse_args()
    checked_id(args.run+'_UNLOAD')
    build = json.loads((args.local/'runtime-build'/'build.json').read_text())
    sources = {name: args.local/'output'/name for name in ['charon', 'swanctl']}
    sources['tun.ko'] = args.tun_local/'output'/'tun.ko'
    sources['qdc-tun-probe'] = args.tun_local/'runtime-build'/'qdc-tun-probe'
    sources['qdc-ike-guard'] = args.local/'runtime-build'/'qdc-ike-guard'
    pins = {**PINS, 'qdc-ike-guard': build['guard_sha256']}
    sys.path.insert(0, str(HERE.parent))
    from inspect_artifact import inspect_elf
    for name in ['charon', 'swanctl', 'qdc-tun-probe', 'qdc-ike-guard']:
        inspect_elf(sources[name].read_bytes())
    manifest = build_bundle(sources, pins, args.out.absolute(), args.run)
    write_private(args.out/'build.json', (json.dumps(build, indent=2)+'\n').encode())
    print(json.dumps({'prepared': str(args.out.absolute()), 'device_executed': False,
                      'payload_bytes': sum(x['bytes'] for x in manifest['artifacts'].values())}, ensure_ascii=False))


if __name__ == '__main__':
    main()
