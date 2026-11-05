# Xbox 360 embedded CA bundle

`cacert.pem` is the Mozilla-derived CA root bundle published by the curl
project. The Xbox 360 build embeds it into the XEX at build time so HTTPS works
without deploying a loose certificate file next to the executable (a retail XEX
is a single file).

## Provenance

| Field | Value |
|---|---|
| Source | <https://curl.se/ca/cacert.pem> |
| Extracted from Mozilla | Tue Oct 20 13:32:40 2026 GMT |
| Downloaded | 2026-10-26 |
| SHA-256 | `796F1389BFEA13C0ED78F663B765BAAFF54A508E5483786BD6ECE73C5E3E3CBE` |
| Certificates | 118 |
| License | MPL-2.0 (Mozilla CA certificate data) |

The SHA-256 above is the value pinned in `CMakeLists.txt`
(`XBOX360_CA_SHA256`). `scripts/gen_xbox360_ca_bundle.py` refuses to generate
the embedded array when the file does not match, so a corrupted or swapped
bundle fails the build instead of silently shipping.

## Updating

1. Download a new `cacert.pem` from the source URL.
2. Update the table above and the `XBOX360_CA_SHA256` value in `CMakeLists.txt`.
3. Rebuild. The startup log prints
   `[x360-tls] ca=<n> skipped=<n> parse=<ms>ms clock=<unix>` so the new bundle
   can be verified on hardware.

A runtime override is also supported: if
`game:\config\wiliwili\cacert.pem` exists it is appended to the embedded chain
(useful when a root expires and no rebuild is possible). It is never used to
replace the embedded bundle, so a bad file cannot narrow trust.

## Why not read it from `game:\resources\`?

`resources/` is deployed as loose files for the devkit/RGH workflow, but a
distributable Xbox 360 title is a single XEX. Embedding keeps one artifact and
removes a user-writable trust anchor from the normal path.

## Notes

- The bundle contains a few Ed25519 roots that mbedTLS 2.28 cannot parse.
  `mbedtls_x509_crt_parse()` skips them and returns the number of skipped
  certificates; the Xbox loader treats a positive return as success and logs
  the count as `skipped=`.
- `MBEDTLS_FS_IO` stays disabled in `mbedtls/config-xbox360.h`; the bundle is
  parsed from memory.
