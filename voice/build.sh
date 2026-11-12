#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p voice/build
docker build --platform linux/amd64 -t qdc507-voice-builder:local voice
docker run --rm --platform linux/amd64 -v "$PWD/voice:/src:ro" -v "$PWD/voice/build:/out" qdc507-voice-builder:local sh -ec '
 gcc -Wall -Wextra -Werror -shared -fPIC /src/tests/fake_audio.c -o /tmp/libfake.so
 gcc -Wall -Wextra -Werror -O2 -pthread /src/network_pcm_bridge.c -ldl -o /tmp/pcm-test
 python3 /src/tests/test_stream.py
 arm-linux-gnueabi-gcc -Wall -Wextra -Werror -O2 -march=armv7-a -mfloat-abi=soft -fno-pie -no-pie -pthread /src/network_pcm_bridge.c -Wl,-z,relro,-z,now,-z,noexecstack,--export-dynamic-symbol=main -ldl -o /out/network-pcm-bridge
 arm-linux-gnueabi-strip /out/network-pcm-bridge
 arm-linux-gnueabi-gcc -Wall -Wextra -Werror -shared -fPIC -O2 -march=armv7-a -mfloat-abi=soft /src/offline_calibration.c -Wl,-z,relro,-z,now,-z,noexecstack -o /out/offline-calibration.so
 arm-linux-gnueabi-readelf --version-info /out/network-pcm-bridge | sed -n "s/.*Name: \\(GLIBC_[0-9.]*\\).*/\\1/p" | sort -u
 sha256sum /out/network-pcm-bridge
'
