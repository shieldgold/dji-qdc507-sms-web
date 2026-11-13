FROM qdc507-tun-gcc493:local
COPY compiler-debs /tmp/compiler-debs
RUN cd /tmp/compiler-debs && sha256sum -c SHA256SUMS \
    && for p in *.deb; do dpkg-deb -x "$p" /; done \
    && ldconfig && rm -rf /tmp/compiler-debs
COPY pkgconf-src.tar.gz /tmp/pkgconf-src.tar.gz
RUN echo '6cf8fe1821343e96764e32d92e182be6b90023c023440c11838ff776f5bd6b1d  /tmp/pkgconf-src.tar.gz' | sha256sum -c - \
    && mkdir /tmp/pkgconf && tar -xf /tmp/pkgconf-src.tar.gz -C /tmp/pkgconf --strip-components=1 \
    && cd /tmp/pkgconf && make -f Makefile.lite -j4 SYSTEM_LIBDIR=/usr/lib SYSTEM_INCLUDEDIR=/usr/include PKG_DEFAULT_PATH=/work/toolchain/lib/pkgconfig \
    && cp pkgconf-lite /usr/bin/pkg-config && pkg-config --version \
    && rm -rf /tmp/pkgconf /tmp/pkgconf-src.tar.gz
