// Xbox 360 CA trust store.
//
// The Mozilla root bundle is embedded in the XEX by
// scripts/gen_xbox360_ca_bundle.py (wired up in CMakeLists.txt) because a
// retail title is a single file and cannot carry a loose cacert.pem.
//
// mbedTLS is built without MBEDTLS_FS_IO, so the bundle is parsed from memory.
// Parsing happens once; the resulting chain lives for the whole process and is
// shared by curl and FFmpeg.

#define MBEDTLS_CONFIG_FILE "mbedtls/config-xbox360.h"

#define NONET
#include <xtl.h>
// xtl.h does not declare DbgPrint in this SDK configuration; the other Xbox
// translation units declare it the same way.
extern void DbgPrint(const char* fmt, ...);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mbedtls/platform.h>
#include <mbedtls/x509_crt.h>

#include "platform/xbox360/xbox360_ca.h"

extern const unsigned char x360_ca_bundle_pem[];
extern const unsigned int x360_ca_bundle_pem_len;

// Not declared by any XDK header; a normal xboxkrnl.exe export (the same
// declaration cbffmpeg compat/xbox360/posix_time.c uses).
void KeQuerySystemTime(LARGE_INTEGER* systemTime);

// 100ns ticks between the FILETIME epoch (1601-01-01) and the Unix epoch.
#define X360_FILETIME_TO_UNIX_100NS 116444736000000000LL
#define X360_TICKS_PER_SECOND 10000000LL

// The only trustworthy wall clock on this platform.  The XDK CRT's
// time()/_time64() returns garbage (<unix_seconds> << 32, observed live as
// 7704238758480052224 for 2026-11-04); cbffmpeg compat/xbox360/posix_time.c
// records the same and deliberately does not override the CRT symbol.  Every
// TLS consumer in this file must go through this function instead.
static long long x360_unix_seconds(void)
{
    LARGE_INTEGER fileTime;
    KeQuerySystemTime(&fileTime);
    return (fileTime.QuadPart - X360_FILETIME_TO_UNIX_100NS) / X360_TICKS_PER_SECOND;
}

// mbedtls_time hook (installed by x360_ca_init via
// mbedtls_platform_set_time): certificate validity checks in x509.c and the
// session timestamps in ssl_cli.c all read this.
static mbedtls_time_t x360_mbedtls_time(mbedtls_time_t* out)
{
    mbedtls_time_t now = (mbedtls_time_t)x360_unix_seconds();
    if (out != NULL)
        *out = now;
    return now;
}

// Optional emergency override. It is appended to (never replaces) the embedded
// bundle, so a missing or corrupt file cannot narrow the trust store.
#define X360_CA_OVERRIDE_PATH "game:\\config\\wiliwili\\cacert.pem"
#define X360_CA_MAX_FILE (1024u * 1024u)

// 2024-01-01T00:00:00Z. Earlier than any root currently in the bundle, so a
// clock below this cannot possibly validate a live certificate chain.
#define X360_TLS_MIN_PLAUSIBLE_TIME 1704067200LL

static mbedtls_x509_crt g_ca;
static int g_state;  // 0 = untouched, 1 = ready, -1 = failed
static int g_certs;
static int g_skipped;
static int g_parse_ms;
static int g_verify;  // effective TLS peer verification (setting + clock downgrade)

static int count_certs(const mbedtls_x509_crt* chain)
{
    int count = 0;
    for (const mbedtls_x509_crt* cursor = chain; cursor != NULL; cursor = cursor->next)
    {
        // The head node is filled by the parser; an empty one has no raw data.
        if (cursor->raw.p != NULL && cursor->raw.len != 0)
            ++count;
    }
    return count;
}

// Appends game:\config\wiliwili\cacert.pem when present. Returns the number of
// extra certificates, 0 when the file is absent, or -1 when it exists but
// cannot be parsed (the embedded chain stays usable either way).
static int parse_override(void)
{
    FILE* file = fopen(X360_CA_OVERRIDE_PATH, "rb");
    if (file == NULL)
        return 0;

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return -1;
    }

    long size = ftell(file);
    if (size <= 0 || (unsigned long)size > X360_CA_MAX_FILE || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        DbgPrint("[x360-tls] ignoring %s (bad size)\n", X360_CA_OVERRIDE_PATH);
        return -1;
    }

    unsigned char* buffer = (unsigned char*)malloc((size_t)size + 1);
    if (buffer == NULL)
    {
        fclose(file);
        return -1;
    }

    size_t read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    if (read_size != (size_t)size)
    {
        free(buffer);
        DbgPrint("[x360-tls] ignoring %s (short read)\n", X360_CA_OVERRIDE_PATH);
        return -1;
    }

    buffer[size] = '\0';

    int before = count_certs(&g_ca);
    int ret = mbedtls_x509_crt_parse(&g_ca, buffer, (size_t)size + 1);
    memset(buffer, 0, (size_t)size + 1);
    free(buffer);

    if (ret < 0)
    {
        DbgPrint("[x360-tls] override %s failed (-0x%04x)\n", X360_CA_OVERRIDE_PATH, -ret);
        return -1;
    }

    int added = count_certs(&g_ca) - before;
    DbgPrint("[x360-tls] override %s: +%d certs, %d skipped\n", X360_CA_OVERRIDE_PATH, added, ret);
    return added > 0 ? added : 0;
}

int x360_ca_init(x360_ca_stats* out)
{
    // Install the trustworthy clock first: certificate validity checks depend
    // on it, and nothing may run a TLS handshake before this function.  Setting
    // it more than once is harmless.
    mbedtls_platform_set_time(x360_mbedtls_time);

    if (out != NULL)
    {
        out->ok = 0;
        out->certs = g_certs;
        out->skipped = g_skipped;
        out->parseMs = g_parse_ms;
        out->unixTime = x360_unix_seconds();
    }

    if (g_state != 0)
        return g_state > 0 ? 0 : -1;

    mbedtls_x509_crt_init(&g_ca);

    unsigned long start = GetTickCount();
    int ret = mbedtls_x509_crt_parse(&g_ca, x360_ca_bundle_pem, x360_ca_bundle_pem_len);
    g_parse_ms = (int)(GetTickCount() - start);

    if (ret < 0)
    {
        g_state = -1;
        DbgPrint("[x360-tls] embedded CA bundle failed (-0x%04x)\n", -ret);
        return -1;
    }

    // A positive return value is the number of certificates mbedTLS could not
    // parse (Ed25519 roots and other unsupported key types), not a failure.
    g_skipped = ret > 0 ? ret : 0;

    parse_override();

    g_certs = count_certs(&g_ca);
    g_state = g_certs > 0 ? 1 : -1;

    if (out != NULL)
    {
        out->ok = g_state > 0;
        out->certs = g_certs;
        out->skipped = g_skipped;
        out->parseMs = g_parse_ms;
        out->unixTime = x360_unix_seconds();
    }

    DbgPrint("[x360-tls] embedded CA: %d certs, %d skipped, %d ms\n", g_certs, g_skipped, g_parse_ms);
    return g_state > 0 ? 0 : -1;
}

const void* x360_ca_chain(void)
{
    return g_state > 0 ? (const void*)&g_ca : NULL;
}

int x360_tls_clock_ok(void)
{
    // x360_unix_seconds, not time(): the CRT clock is garbage and would make
    // this check meaningless (see the note above x360_unix_seconds).
    return x360_unix_seconds() >= X360_TLS_MIN_PLAUSIBLE_TIME;
}

void x360_tls_set_verify(int on)
{
    g_verify = on ? 1 : 0;
}

int x360_tls_verify(void)
{
    return g_verify;
}
