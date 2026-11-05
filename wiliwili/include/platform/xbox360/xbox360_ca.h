#pragma once

// Embedded CA trust store for the Xbox 360 build.
//
// The bundle is generated from third_party/ca/cacert.pem at build time and
// compiled into the XEX (a retail title is a single file, so there is no loose
// certificate asset to deploy). See docs/XBOX360_TLS_CA.md.
//
// The chain is parsed once and never freed: mbedtls_ssl_conf_ca_chain() stores
// a pointer and verification only reads it, so sharing it across the curl and
// FFmpeg TLS stacks is safe. Call x360_ca_init() after
// x360_mbedtls_pool_init() and before any HTTPS request.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int ok;              // 1 when the chain is usable
    int certs;           // number of trusted certificates loaded
    int skipped;         // certificates the bundle contained but mbedTLS rejected
    int parseMs;         // time spent parsing the bundle (milliseconds)
    long long unixTime;  // system clock sampled during init
} x360_ca_stats;

// Parses the embedded bundle (plus game:\config\wiliwili\cacert.pem when that
// file exists) into the process-wide trust store. Returns 0 on success and -1
// when no usable chain could be built. Safe to call more than once.
int x360_ca_init(x360_ca_stats* out);

// The process-wide trust store, or NULL before a successful x360_ca_init().
// Cast to const mbedtls_x509_crt* in callers that include mbedTLS headers.
const void* x360_ca_chain(void);

// Whether the console clock is plausible enough for certificate validity
// checks. A console that has never synced its clock reports a time before the
// first certificate in the bundle was issued, which would reject every chain.
int x360_tls_clock_ok(void);

// Effective peer-verification switch for every TLS stack in the process:
// curl (via cpr VerifySsl) and FFmpeg/mpv (tls_mbedtls.c) both follow it.
// ProgramConfig pushes the user setting here after applying the console-clock
// downgrade, so the two transports can never disagree. Default is off; a build
// that never wires the setting keeps the historical verify-off behaviour.
void x360_tls_set_verify(int on);
int x360_tls_verify(void);

#ifdef __cplusplus
}
#endif
