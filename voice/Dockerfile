FROM debian@sha256:e5b6442dd2e9684cf5e87d8338b5968f3b348636fc0be6d7850a381e3731a2bd
RUN printf 'deb http://deb.debian.org/debian bullseye main\n' > /etc/apt/sources.list && apt-get update && apt-get install -y --allow-downgrades --no-install-recommends libc6=2.31-13+deb11u11 gcc gcc-arm-linux-gnueabi libc6-dev libc6-dev-armel-cross binutils-arm-linux-gnueabi python3 && rm -rf /var/lib/apt/lists/*
