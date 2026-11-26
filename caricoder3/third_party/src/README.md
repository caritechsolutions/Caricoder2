# Source of the libraries caricoder3 builds for itself

`scripts/build-libs.sh` builds these, statically, into
`/opt/caricoder3-libs`. `SHA256SUMS` is checked before each is unpacked.

| Archive | What | From | Licence |
|---|---|---|---|
| `fdk-aac-2.0.3.tar.gz` | AAC decode and encode, with HE-AAC | github.com/mstorsjo/fdk-aac, tag `v2.0.3`, unchanged | Fraunhofer FDK AAC licence (`NOTICE` inside) |
| `srt-1.5.4.tar.gz` | SRT | github.com/Haivision/srt, tag `v1.5.4`, unchanged | MPL-2.0 |
| `librist-0.2.11.tar.gz` | RIST | see below | BSD-2-Clause |

## librist

Put together from three sources, because upstream's own archive
(code.videolan.org) carries copies of two other libraries and Ubuntu's
copy of it has them taken out:

- librist 0.2.11: Ubuntu's `librist_0.2.11+dfsg.orig.tar.xz`, unchanged;
- `contrib/mbedtls/library` and `include`: Mbed TLS 2.28.10
  (github.com/Mbed-TLS/mbedtls, tag `mbedtls-2.28.10`; Apache-2.0 or
  GPL-2.0-or-later). Upstream librist carries an older 2.x;
- `contrib/contrib_cJSON/cjson`: cJSON 1.7.18 (github.com/DaveGamble/cJSON; MIT).

One change: `contrib/mbedtls/meson.build` lists seven more files of
Mbed TLS, which 2.28 has and the older 2.x did not (`constant_time.c`,
`mps_reader.c`, `mps_trace.c`, `psa_crypto_aead.c`, `psa_crypto_cipher.c`,
`psa_crypto_hash.c`, `psa_crypto_mac.c`).

Built with `-Dbuiltin_mbedtls=true -Dbuiltin_cjson=true`, so the library
holds both and needs nothing else installed.
