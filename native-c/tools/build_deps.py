#!/usr/bin/env python3
"""Build pinned C libraries only; never probes hardware or Apple endpoints."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def run(args, *, cwd=None, env=None, log=None):
    if log:
        with log.open('w') as stream:
            subprocess.run(args, cwd=cwd, env=env, check=True,
                           stdout=stream, stderr=subprocess.STDOUT)
    else:
        subprocess.run(args, cwd=cwd, env=env, check=True)


def unpack(archive, destination):
    with tarfile.open(archive) as bundle:
        if hasattr(tarfile, 'data_filter'):
            bundle.extractall(destination, filter='data')
        else:
            for entry in bundle.getmembers():
                path = PurePosixPath(entry.name)
                if path.is_absolute() or '..' in path.parts or entry.isdev():
                    raise RuntimeError('Unsafe source archive path')
                if entry.issym() or entry.islnk():
                    raise RuntimeError('This Python needs data_filter for archive links')
            bundle.extractall(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['host', 'arm'], default='host')
    parser.add_argument('--work', type=Path)
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--source-cache', type=Path,
                        help='Optional directory with the already verified archives')
    parser.add_argument('--cc')
    parser.add_argument('--ar')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    work = (args.work or (Path('/work/native-c-deps-v1') if args.target == 'arm'
                         else Path.home() / '.cache/qdc507-native-c/host')).resolve()
    prefix = (args.prefix or work / 'prefix').resolve()
    cc = args.cc or ('/work/toolchain/bin/musl-gcc' if args.target == 'arm' else os.environ.get('CC', 'cc'))
    ar = args.ar or ('arm-linux-gnueabi-ar' if args.target == 'arm' else 'ar')
    ranlib = 'arm-linux-gnueabi-ranlib' if args.target == 'arm' else 'ranlib'
    cflags = '-Os -ffunction-sections -fdata-sections'
    if args.target == 'arm':
        machine = subprocess.check_output([cc, '-dumpmachine'], text=True).strip()
        if not machine.startswith('arm'):
            raise RuntimeError('ARM target requires an ARM compiler')
        cflags += ' -march=armv7-a -mfloat-abi=soft'
    lock_bytes = (ROOT / 'deps.lock.json').read_bytes()
    lock = json.loads(lock_bytes)
    user_config = (ROOT / 'config/mbedtls-user.h').read_bytes()
    stamp = hashlib.sha256(lock_bytes + user_config + (cc + ar + cflags).encode()).hexdigest()
    work.mkdir(parents=True, exist_ok=True)
    cache, sources, logs = work / 'downloads', work / 'sources', work / 'logs'
    for path in [cache, sources, logs, prefix / 'lib', prefix / 'include', prefix / 'share/licenses']:
        path.mkdir(parents=True, exist_ok=True)
    for name, item in lock.items():
        filename = item['url'].rsplit('/', 1)[1]
        if not filename.endswith(('.bz2', '.xz', '.gz')):
            filename = name + '-' + item['commit'] + '.tar.gz'
        archive = cache / filename
        if not archive.exists():
            candidate = args.source_cache / filename if args.source_cache else None
            if candidate and candidate.exists():
                shutil.copyfile(candidate, archive)
            else:
                with urllib.request.urlopen(item['url'], timeout=45) as response:
                    data = response.read(16 * 1024 * 1024 + 1)
                if len(data) > 16 * 1024 * 1024:
                    raise RuntimeError('Source archive exceeds limit')
                archive.write_bytes(data)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError(f'{name}: source SHA256 mismatch; archive retained for diagnosis')
        if not (sources / item['root']).exists():
            unpack(archive, sources)
    needed = ['libmbedcrypto.a', 'libmbedx509.a', 'libmbedtls.a', 'libnghttp2.a', 'libyyjson.a']
    stamp_file = work / 'deps.stamp'
    if stamp_file.exists() and stamp_file.read_text().strip() == stamp and all((prefix / 'lib' / n).exists() for n in needed):
        print(prefix)
        return
    mbed = work / 'mbedtls-build'
    if mbed.exists():
        shutil.rmtree(mbed)
    shutil.copytree(sources / lock['mbedtls']['root'], mbed)
    shutil.copyfile(ROOT / 'config/mbedtls-user.h', mbed / 'include/mbedtls/qdc-user.h')
    with (mbed / 'include/mbedtls/mbedtls_config.h').open('a') as config:
        config.write('\n#include "mbedtls/qdc-user.h"\n')
    run(['make', '-C', str(mbed / 'library'), '-j' + str(max(1, args.jobs)),
         'CC=' + cc, 'AR=' + ar, 'RL=' + ranlib, 'CFLAGS=' + cflags], log=logs / 'mbedtls.log')
    shutil.copytree(mbed / 'include', prefix / 'include', dirs_exist_ok=True)
    for name in needed[:3]:
        shutil.copyfile(mbed / 'library' / name, prefix / 'lib' / name)
    ng = work / 'nghttp2-build'
    ng.mkdir(exist_ok=True)
    env = dict(os.environ, CC=cc, AR=ar, RANLIB=ranlib, CFLAGS=cflags)
    configure = [str(sources / lock['nghttp2']['root'] / 'configure'), '--prefix=' + str(prefix),
                 '--enable-lib-only', '--disable-shared', '--enable-static', '--disable-dependency-tracking']
    if args.target == 'arm':
        configure.append('--host=arm-linux-musleabi')
    run(configure, cwd=ng, env=env, log=logs / 'nghttp2-configure.log')
    run(['make', '-j' + str(max(1, args.jobs))], cwd=ng, env=env, log=logs / 'nghttp2.log')
    run(['make', 'install'], cwd=ng, env=env, log=logs / 'nghttp2-install.log')
    yy = sources / lock['yyjson']['root'] / 'src'
    obj = work / 'yyjson.o'
    run([cc, *cflags.split(), '-DYYJSON_READER_DEPTH_LIMIT=128', '-DYYJSON_WRITER_DEPTH_LIMIT=128',
         '-I' + str(yy), '-c', str(yy / 'yyjson.c'), '-o', str(obj)])
    run([ar, 'rc', str(prefix / 'lib/libyyjson.a'), str(obj)])
    run([ranlib, str(prefix / 'lib/libyyjson.a')])
    shutil.copyfile(yy / 'yyjson.h', prefix / 'include/yyjson.h')
    for name, item in lock.items():
        source = sources / item['root']
        license_file = source / ('COPYING' if name == 'nghttp2' else 'LICENSE')
        shutil.copyfile(license_file, prefix / 'share/licenses' / (name + '.txt'))
    shutil.copyfile(ROOT / 'deps.lock.json', prefix / 'share/licenses/deps.lock.json')
    stamp_file.write_text(stamp + '\n')
    print(prefix)


if __name__ == '__main__':
    main()
