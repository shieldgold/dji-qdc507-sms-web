FROM qdc507-voice-builder:local
# This existing image has the bullseye main-archive libc headers and Python.
RUN printf '%s\n' 'deb http://deb.debian.org/debian bullseye main' > /etc/apt/sources.list \
    && rm -f /etc/apt/sources.list.d/* \
    && apt-get update && apt-get install -y --no-install-recommends \
       make bc kmod patch xz-utils \
    && rm -rf /var/lib/apt/lists/*
