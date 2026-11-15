# Third-party components

The C backend is distributed under the repository's MIT license. Its pinned
static dependencies retain their upstream licenses and notices:

| Component | Selected license | Source and version |
|---|---|---|
| Mbed TLS | Apache-2.0 (upstream offers Apache-2.0 OR GPL-2.0-or-later) | 3.6.7, `deps.lock.json` |
| nghttp2 | MIT | 1.71.0, `deps.lock.json` |
| yyjson | MIT | 0.13.0, commit `6447536015f3d600f3d65323b10976103b337ca7` |
| musl libc | MIT and notices supplied with the toolchain | Static ARM build uses the existing musl 1.2.6 toolchain |

`tools/build_deps.py` verifies each archive and copies upstream LICENSE/COPYING
texts plus the dependency lock file to `<prefix>/share/licenses/`. Redistributed
binary bundles must include those files, the musl notices supplied by the
toolchain, this notice, and the repository LICENSE. Frontend icon notices remain
in `../web/ICONS_LICENSE.txt`. No vendor voice library is copied into this backend.

Mbed TLS's application-time/pthread configuration is in
`config/mbedtls-user.h`; upstream algorithm/certificate verification defaults
are retained. No TLS/X.509 verification bypass or external credential-forwarding
endpoint is enabled in production. Test endpoint hooks compile only with
`QDC_TESTING` and enforce loopback.
