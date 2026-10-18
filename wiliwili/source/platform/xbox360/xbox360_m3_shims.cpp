// Xbox 360 linker shims for platform gaps that are still absent from XDK.

#include <cpr/multiperform.h>
#include <cpr/session.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cpr {
MultiPerform::MultiPerform() = default;
MultiPerform::~MultiPerform() = default;

// Xbox: curl_multi_poll() is not part of the Xenon curl build, so upstream
// cpr/multiperform.cpp is filtered out of the Xbox cpr target (see
// library/CMakeLists.txt) and these two members are the only definitions
// the linker sees. Run the queued sessions one at a time instead of through a
// multi handle — callers (BilibiliClient::get_pgc_all_filter) only need the
// responses back in AddSession order, not concurrency.
//
// Do NOT set Session::isUsedInMultiPerform: it makes Session::Get() bail out
// in DoEasyPerform(), which is meant for handles already attached to a multi
// handle. We go through the ordinary easy path instead.
void MultiPerform::AddSession(std::shared_ptr<Session>& session, HttpMethod method) {
    if (method == HttpMethod::DOWNLOAD_REQUEST) is_download_multi_perform = true;
    sessions_.emplace_back(session, method);
}

std::vector<Response> MultiPerform::Get() {
    // Upstream Get() re-prepares every queued session as GET before running
    // them, regardless of the method passed to AddSession().
    std::vector<Response> responses;
    responses.reserve(sessions_.size());
    for (auto& pair : sessions_) {
        pair.second = HttpMethod::GET_REQUEST;
        responses.push_back(pair.first->Get());
    }
    return responses;
}
} // namespace cpr

namespace std { namespace __1 {
unsigned long stoul(const string& s, size_t* idx, int base) {
    const char* begin = s.c_str(); char* end = nullptr;
    unsigned long value = strtoul(begin, &end, base);
    if (idx) *idx = static_cast<size_t>(end - begin);
    return value;
}
} }

extern "C" void __cxa_rethrow() { std::abort(); }
//
// Sections 1-3 include real headers and provide stub method bodies.
// Sections 4-8 provide raw symbols for C/library functions.

// ============================================================================
// std::__fs::filesystem — implemented in xbox360_filesystem.cpp.
// ============================================================================
// The placeholder bodies that used to live here broke i18n locale discovery
// and config saving at runtime. Real implementations backed by the XDK Win32
// file APIs now live in xbox360_filesystem.cpp (same target).

// ============================================================================
// 3. std::chrono stubs
// ============================================================================
#include <chrono>
#include <xtl.h>

namespace std {
namespace __1 {
namespace chrono {

system_clock::time_point system_clock::now() noexcept {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;

    // FILETIME is measured in 100 ns intervals since 1601-01-01 UTC.
    constexpr unsigned long long unixEpoch = 116444736000000000ULL;
    if (value.QuadPart <= unixEpoch)
        return time_point(duration(0));

    const unsigned long long unix100ns = value.QuadPart - unixEpoch;
    const auto micros = std::chrono::microseconds(static_cast<long long>(unix100ns / 10));
    return time_point(std::chrono::duration_cast<duration>(micros));
}

system_clock::time_point system_clock::from_time_t(time_t t) noexcept {
    return time_point(duration(static_cast<duration::rep>(t) * duration::period::den));
}

time_t system_clock::to_time_t(const time_point& tp) noexcept {
    auto d = tp.time_since_epoch();
    return static_cast<time_t>(d.count() / duration::period::den);
}

} // namespace chrono
} // namespace __1
} // namespace std

// ============================================================================
// 4. std::stoi / std::stoll / std::to_string
// ============================================================================
#include <string>

extern "C" {
long strtol(const char*, char**, int);
long long strtoll(const char*, char**, int);
}

namespace std {
namespace __1 {

int stoi(const string& str, size_t* idx, int base) {
    const char* s = str.c_str();
    long v = strtol(s, nullptr, base);
    if (idx) {
        const char* p = s;
        while (*p == ' ') ++p;
        if (*p == '+' || *p == '-') ++p;
        int dz = 0;
        if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        while (p[dz] >= '0' && p[dz] <= '9') ++dz;
        *idx = static_cast<size_t>(p - s + dz);
    }
    return static_cast<int>(v);
}

long long stoll(const string& str, size_t* idx, int base) {
    const char* s = str.c_str();
    long long v = strtoll(s, nullptr, base);
    if (idx) {
        const char* p = s;
        while (*p == ' ') ++p;
        if (*p == '+' || *p == '-') ++p;
        int dz = 0;
        if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        while (p[dz] >= '0' && p[dz] <= '9') ++dz;
        *idx = static_cast<size_t>(p - s + dz);
    }
    return v;
}

static string __int_to_str(long long v) {
    char buf[32];
    int n = 0;
    unsigned long long uv;
    int neg = 0;
    if (v < 0) { neg = 1; uv = (unsigned long long)(-(v + 1)) + 1; }
    else { uv = (unsigned long long)v; }
    char tmp[24];
    int d = 0;
    do { tmp[d++] = (char)('0' + (int)(uv % 10)); uv /= 10; } while (uv);
    if (neg) buf[n++] = '-';
    while (d > 0) buf[n++] = tmp[--d];
    buf[n] = '\0';
    return string(buf);
}

static string __uint_to_str(unsigned long long v) {
    char buf[32];
    int n = 0;
    char tmp[24];
    int d = 0;
    do { tmp[d++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (d > 0) buf[n++] = tmp[--d];
    buf[n] = '\0';
    return string(buf);
}

// %g matches libc++'s own to_string() formatting closely enough for the
// diagnostics that consume these (mpv property readouts, cache rates).
static string __float_to_str(double v) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%g", v);
    if (n < 0) return string("0");
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    return string(buf);
}

string to_string(int v)                     { return __int_to_str(v); }
string to_string(unsigned v)                { return __uint_to_str(v); }
string to_string(long v)                    { return __int_to_str(v); }
string to_string(unsigned long v)           { return __uint_to_str(v); }
string to_string(long long v)               { return __int_to_str(v); }
string to_string(unsigned long long v)      { return __uint_to_str(v); }
string to_string(float v)                   { return __float_to_str((double)v); }
string to_string(double v)                  { return __float_to_str(v); }

} // namespace __1
} // namespace std

// ============================================================================
// 5. std::cerr / cout / cin / clog — ABI mismatch bridge
//    OXDK libc++ defines these with MSVC mangling (?cerr@__1@std@@...);
//    the wiliwili code expects Itanium ABI (_ZNSt3__14cerrE).
//    Provide zero-initialized BSS blobs via ELF .globl.
// ============================================================================

#define M3_STREAM_BSS(sym, nbytes) \
    __asm__( \
    ".globl " sym "\n" \
    ".type " sym ", @object\n" \
    ".weak " sym "\n" \
    sym ":\n" \
    ".zero " #nbytes "\n")

M3_STREAM_BSS("_ZNSt3__14cerrE",   64);
M3_STREAM_BSS("_ZNSt3__14coutE",   64);
M3_STREAM_BSS("_ZNSt3__13cinE",    64);
M3_STREAM_BSS("_ZNSt3__14clogE",   64);
M3_STREAM_BSS("_ZNSt3__15wcerrE",  64);
M3_STREAM_BSS("_ZNSt3__15wcoutE",  64);
M3_STREAM_BSS("_ZNSt3__14wcinE",   64);
M3_STREAM_BSS("_ZNSt3__15wclogE",  64);

#undef M3_STREAM_BSS

// ============================================================================
// 6. RTTI / __dynamic_cast — REPLACED with real libc++abi sources
// ============================================================================
// The empty __cxxabiv1 vtables and the always-nullptr __dynamic_cast that used
// to live here made every BRLS_BIND resolve() fail at runtime: resolve() does
// dynamic_cast<T*>(view) and the stub returned nullptr, so the first
// AppletFrame construction terminated and the null terminate handler turned it
// into a call to address 0. libc++abi's private_typeinfo.cpp is now compiled
// into this target instead (see CMakeLists.txt), providing the real vtables
// and the full Itanium __dynamic_cast.
//
// libc++abi's internal fatal path calls __abort_message; provide it here so
// the reason shows up in the XBDM debug output before the title dies.
#include <cstdarg>
#include <cstdlib>

extern "C" int vsnprintf(char* buffer, size_t count, const char* format,
                         va_list args); // -> _vsnprintf via the toolchain -D
extern "C" void DbgPrint(const char* format, ...); // xboxkrnl export

extern "C" __attribute__((noreturn)) void __abort_message(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    DbgPrint("wiliwili abort: %s\n", buffer);
    std::abort();
}

// ============================================================================
// 7. stb_image stubs (M3: no remote image loading)
// ============================================================================
extern "C" {

} // extern "C"

// ============================================================================
extern "C" unsigned char m3_bit_scan_reverse(unsigned long* index,
                                               unsigned long mask)
    __asm__("_BitScanReverse");
extern "C" unsigned char m3_bit_scan_reverse(unsigned long* index,
                                               unsigned long mask) {
    if (!mask) return 0;
    unsigned long bit = sizeof(mask) * 8 - 1;
    while (!(mask & (1UL << bit))) --bit;
    *index = bit;
    return 1;
}

extern "C" void* __cxa_bad_typeid() { std::abort(); }

// curl is provided by the Xbox 360 libcurl target in CMake.  The M3 stubs
// were intentionally removed once the real transport became available.
