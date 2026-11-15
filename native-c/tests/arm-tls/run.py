#!/usr/bin/env python3
"""Build/run one sealed 48 MiB ARM/Linux 3.18 loopback TLS/H2 VM.

Every credential is synthetic. No USB/device/Apple endpoint/WAN is accessed.
"""
import argparse
from datetime import timezone
from email.utils import parsedate_to_datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'tools/native-c-validation'))
from preflight import validate_preflight

CASES = ('tls12', 'tls13', 'wrongname', 'wrongca')
LOG_CAP = 1048576


def unique(pattern, text, message):
    matches = list(re.finditer(pattern, text, re.M))
    if len(matches) != 1:
        raise ValueError(message)
    return matches[0]


def validate_log(text):
    if len(text.encode()) > LOG_CAP:
        raise ValueError('log exceeds 1 MiB')
    text = text.replace('\r\n', '\n').lstrip('\0')
    for marker in ('QDC_TLS_FAILED', 'QDC_TLS_CASE_FAILED', 'QDC_TLS_PEER_FAILED',
                   'QDC_TLS_INIT_FAILED', 'Kernel panic', 'Out of memory:',
                   'Unable to handle kernel', 'SIGSEGV', 'QDC_ENTROPY_FAILED'):
        if marker in text:
            raise ValueError('runtime failure: ' + marker)
    kernel = unique(r'^\x00?QDC_TLS_KERNEL release=3\.18\.44 machine=(arm\w+) epoch=(\d+) '
                    r'dual_gate=1 network=loopback-only$', text, 'missing ARM dual gate/kernel')
    entropy = validate_preflight(text, 'sms')
    ready = unique(r'^QDC_ENTROPY_READY .+$', text, 'missing entropy ready')
    start = unique(r'^QDC_TLS_RUNTIME_BEGIN admission_kib=8192 absolute_floor_kib=6144 '
                   r'max_drop_kib=3072 baseline_available_kib=(\d+) getrandom_return=32$',
                   text, 'missing runtime admission and ready getrandom')
    if not kernel.start() < ready.start() < start.start():
        raise ValueError('entropy must precede runtime')
    baseline = int(start[1])
    if baseline < 8192:
        raise ValueError('admission below 8 MiB')
    if 'tls unit: PASS (HTTP Date validation, DNS ID/question/truncation/pointer-loop rejection)' not in text:
        raise ValueError('production unit checks missing')
    begins = list(re.finditer(r'^QDC_TLS_CASE_BEGIN case=(\w+)$', text, re.M))
    if tuple(match[1] for match in begins) != CASES:
        raise ValueError('missing, duplicate or unordered case')
    cases = {}
    for i, begin in enumerate(begins):
        name = begin[1]
        block = text[begin.end():begins[i+1].start() if i+1 < len(begins) else len(text)]
        negative = name in ('wrongname', 'wrongca')
        outcome = 'rejected' if negative else 'accepted'
        result = unique(r'^QDC_TLS_CASE_PASS case=' + name + r' outcome=' + outcome +
                        r' client_exit=' + str(int(negative)) + r' peer_exit=0 '
                        r'elapsed_ms=(\d+) min_available_kib=(\d+)$', block,
                        'missing actual peer/client result: ' + name)
        elapsed, minimum = map(int, result.groups())
        if not 0 < elapsed <= 12000 or minimum < 6144 or baseline - minimum > 3072:
            raise ValueError('runtime limits violated: ' + name)
        sample = unique(r'^QDC_TLS_SAMPLE case=' + name + r' peer_rss_kib=(\d+) '
                        r'peer_pss_kib=(\d+) client_rss_kib=(\d+) client_pss_kib=(\d+) interval_ms=20$',
                        block, 'missing process sample: ' + name)
        pr, pp, cr, cp = map(int, sample.groups())
        if not pr >= pp > 0 or not cr >= cp > 0:
            raise ValueError('invalid RSS/PSS sample: ' + name)
        client = unique(r'^QDC_TLS_CLIENT case=' + name + '$', block, 'missing client: ' + name)
        info = {'outcome':outcome, 'elapsed_ms':elapsed, 'min_available_kib':minimum,
                'peer_rss_kib':pr, 'peer_pss_kib':pp, 'client_rss_kib':cr, 'client_pss_kib':cp}
        if negative:
            rejection = unique(r'^QDC_TLS_PEER_CERTIFICATE_REJECT version=TLSv1\.2 fatal_alert=1 '
                               r'description=(42|46|48) tls_code=-\d+ h2_requests=0$',
                               block, 'missing concrete certificate fatal alert: ' + name)
            if 'REQUEST_COMPLETE' in block or 'PEER_PASS' in block or 'NEGOTIATED' in block:
                raise ValueError('untrusted peer exchanged application data: ' + name)
            if not block[client.end():].startswith('\nTLS rejected: 苹果推送 TLS 验证失败或超时\n'):
                raise ValueError('missing client certificate rejection: ' + name)
            info['version'] = 'TLSv1.2'
            info['certificate_alert_description'] = int(rejection[1])
        else:
            version = 'TLSv1.2' if name == 'tls12' else 'TLSv1.3'
            unique(r'^QDC_TLS_PEER_NEGOTIATED version=' + re.escape(version) + r' alpn=h2$',
                   block, 'missing negotiated protocol: ' + name)
            unique(r'^QDC_TLS_PEER_REQUEST_COMPLETE method=POST headers=6 body_bytes=2 request_end_stream=1$',
                   block, 'incomplete HTTP/2 request: ' + name)
            unique(r'^QDC_TLS_PEER_RESPONSE_COMPLETE status=200 body_bytes=12 response_end_stream=1$',
                   block, 'incomplete HTTP/2 response: ' + name)
            unique(r'^QDC_TLS_PEER_CLIENT_CLOSED closed=1 tls_code=(0|-30848|-80)$',
                   block, 'client did not close after response: ' + name)
            unique(r'^QDC_TLS_PEER_PASS case=' + name + '$', block, 'peer did not complete: ' + name)
            response = unique(r'^status=200 h2=1 date=(.{29}) body=\{"arm":true\}$',
                              block, 'client did not receive full response: ' + name)
            date = parsedate_to_datetime(response[1])
            if date.tzinfo != timezone.utc or not int(kernel[2]) <= date.timestamp() <= int(kernel[2]) + 160:
                raise ValueError('invalid authenticated Date: ' + name)
            info.update(version=version, status=200, http2=True, response_body='{"arm":true}', date=response[1])
        cases[name] = info
    cleanup = unique(r'^QDC_TLS_CLEANUP children=0 sockets=0 files=0$', text, 'cleanup missing')
    final = unique(r'^QDC_TLS_RUNTIME_PASS cases=4 baseline_available_kib=(\d+) min_available_kib=(\d+)$',
                   text, 'runtime pass missing')
    minimum = int(final[2])
    if int(final[1]) != baseline or minimum < 6144 or baseline-minimum > 3072 or cleanup.end() > final.start():
        raise ValueError('invalid final memory/cleanup result')
    if any(minimum > case['min_available_kib'] for case in cases.values()):
        raise ValueError('inconsistent final memory minimum')
    return {'status':'passed', 'device_executed':False, 'network':'none; guest loopback only',
            'kernel_release':'3.18.44', 'machine':kernel[1], 'guest_epoch':int(kernel[2]),
            'memory_mib':48, 'cpus':1, 'entropy_preflight':entropy,
            'admission_kib':8192, 'absolute_floor_kib':6144, 'max_drop_kib':3072,
            'baseline_available_kib':baseline, 'min_available_kib':minimum, 'cases':cases,
            'guest_cleanup':{'children':0, 'sockets':0, 'files':0}}


def qemu_command(kernel, initrd, serial):
    return ['qemu-system-arm', '-M', 'virt-5.2', '-cpu', 'cortex-a7', '-smp', '1', '-m', '48M',
            '-nodefaults', '-display', 'none', '-monitor', 'none', '-no-reboot',
            '-serial', 'file:' + str(serial), '-nic', 'none',
            '-global', 'virtio-mmio.force-legacy=true',
            '-object', 'rng-random,filename=/dev/urandom,id=rng0',
            '-device', 'virtio-rng-device,rng=rng0', '-kernel', str(kernel), '-initrd', str(initrd),
            '-append', 'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init '
            'rng_core.default_quality=1000 qdc.sms.lab=1 qdc.tls.lab=1']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes():
    selected = [REPO/'native-c'/f'src/{name}.c' for name in ('tls','crypto','clock','common')]
    selected += [REPO/'native-c/tests/test_tls.c', REPO/'native-c/config/mbedtls-user.h',
                 REPO/'native-c/deps.lock.json', REPO/'tools/native-c-validation/entropy_wait.c',
                 REPO/'tools/native-c-validation/entropy_wait.h', REPO/'tools/native-c-validation/preflight.py']
    selected += sorted(path for path in Path(__file__).parent.iterdir() if path.suffix in ('.c','.h','.py','.sh'))
    return {str(path.relative_to(REPO)):sha(path) for path in selected}


def certificates(directory):
    directory.mkdir(mode=0o700)
    def openssl(*args):
        subprocess.run(['openssl', *args], cwd=directory, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    for prefix, subject in (('ca','Offline ARM Loopback CA'), ('wrong','Untrusted ARM Loopback CA')):
        openssl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                '-nodes', '-keyout', prefix+'.key', '-out', prefix+'.pem', '-days', '2',
                '-subj', '/CN='+subject, '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
    openssl('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
            '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost')
    (directory/'ext.cnf').write_text('subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\n'
                                   'keyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n')
    openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial',
            '-out', 'server.pem', '-days', '2', '-extfile', 'ext.cnf')
    metadata = subprocess.check_output(['openssl', 'x509', '-in', 'server.pem', '-noout',
                                         '-dates', '-subject', '-issuer', '-ext', 'subjectAltName'],
                                        cwd=directory, timeout=15)
    (directory/'certificate-metadata.txt').write_bytes(metadata)
    (directory/'epoch').write_text(str(int(time.time()))+'\n')
    for name in ('ca.key','wrong.key','server.csr','ca.srl','ext.cnf'):
        (directory/name).unlink()


def bounded_log(path):
    if not path.exists():
        return ''
    if path.stat().st_size > LOG_CAP:
        raise RuntimeError('log exceeds 1 MiB: '+path.name)
    return path.read_text(errors='replace')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path, help='private native-c-build directory')
    parser.add_argument('--label', required=True)
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}', args.label):
        parser.error('invalid label')
    base = args.base.resolve()
    kernel = base.parent/'runtime-build/lab-only-runtime-zImage'
    if not kernel.is_file():
        parser.error('missing private ARM kernel')
    out = base/'arm-tls'/args.label
    out.mkdir(parents=True, mode=0o700, exist_ok=False)
    initial_sources = source_hashes()
    certificates(out/'certs')
    build = ['docker','run','--rm','--network','none','--platform','linux/amd64',
             '-v',str(REPO)+':/repo:ro',
             '-v',str(out/'certs')+':/certs:ro', '-v',str(out)+':/out',
             '-v','qdc507-ike-build-v1:/work','qdc507-ike-builder:local',
             'sh','/repo/native-c/tests/arm-tls/build.sh']
    (out/'build-command.json').write_text(json.dumps(build, indent=2)+'\n')
    with (out/'build.log').open('wb') as stream:
        subprocess.run(build, check=True, stdout=stream, stderr=subprocess.STDOUT, timeout=120)
    bounded_log(out/'build.log')
    binaries = ('test_tls', 'lab-only-tls-peer', 'lab-only-tls-fixture', 'lab-only-entropy-wait')
    for name in binaries:
        elf = (out/(name+'.elf.txt')).read_text()
        if not all(part in elf for part in ('ELF32', 'little endian', 'ARM', 'soft-float ABI')) or 'INTERP' in elf:
            raise RuntimeError('not static ARM EABI soft-float: '+name)
    result = {'status':'built', 'device_executed':False, 'source_sha256':initial_sources,
              'binary_sha256':{name:sha(out/name) for name in binaries},
              'library_sha256':dict((line.split()[1].rsplit('/',1)[-1], line.split()[0])
                                    for line in (out/'dependency-libraries.sha256').read_text().splitlines()),
              'dependency_prefix':'/work/native-c-deps-v1/prefix',
              'kernel_sha256':sha(kernel), 'initrd_sha256':sha(out/'lab-only-tls-rootfs.cpio.gz'),
              'certificate_sha256':{name:sha(out/'certs'/name) for name in ('ca.pem','wrong.pem','server.pem')},
              'build_network':'none', 'architecture':'ARM EABI5 soft-float static', 'output':str(out)}
    if args.build_only:
        (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps({'status':'built', 'output':str(out)}))
        return
    command = qemu_command(kernel, out/'lab-only-tls-rootfs.cpio.gz', out/'serial.log')
    (out/'qemu-command.json').write_text(json.dumps(command, indent=2)+'\n')
    proc = None
    try:
        with (out/'qemu-stderr.log').open('wb') as error:
            proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=error)
            result['qemu_pid'] = proc.pid
            deadline = time.monotonic() + 95
            entropy_ready = False
            while time.monotonic() < deadline:
                text = bounded_log(out/'serial.log')
                bounded_log(out/'qemu-stderr.log')
                if not entropy_ready and 'QDC_ENTROPY_READY ' in text:
                    validate_preflight(text, 'sms')
                    entropy_ready = True
                    deadline = time.monotonic() + 70
                if 'QDC_TLS_RUNTIME_PASS ' in text or proc.poll() is not None:
                    break
                if any(token in text for token in ('QDC_TLS_FAILED', 'QDC_TLS_CASE_FAILED',
                                                   'QDC_TLS_INIT_FAILED', 'QDC_ENTROPY_FAILED', 'Kernel panic')):
                    break
                time.sleep(.1)
            else:
                raise TimeoutError('guest preflight/runtime deadline')
            result.update(validate_log(text))
            proc.wait(timeout=3)
            if proc.returncode != 0:
                raise RuntimeError('QEMU did not power off cleanly')
        result['sources_unchanged'] = source_hashes() == initial_sources
        if not result['sources_unchanged']:
            raise RuntimeError('fixture/production source changed during experiment')
    except Exception as error:
        result['status'] = 'failed'
        result['error'] = str(error)
        raise
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=3)
        result['qemu_running_after_cleanup'] = bool(proc and proc.poll() is None)
        result['qemu_exit_code'] = proc.returncode if proc else None
        (out/'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False)+'\n')
    print(json.dumps({'status':result['status'], 'output':str(out),
                      'test_tls_sha256':result['binary_sha256']['test_tls'],
                      'cases':result['cases'], 'min_available_kib':result['min_available_kib']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
