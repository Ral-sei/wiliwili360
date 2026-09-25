// M3 linker shims: provide every undefined symbol the Xbox 360 build needs
// to produce a linkable XEX, without yet implementing the real backends.
//
// Sections 1-3 include real headers and provide stub method bodies.
// Sections 4-8 provide raw symbols for C/library functions.

// ============================================================================
// 1. cpr stubs — HTTP transport (M4 territory)
// ============================================================================
#include <cpr/session.h>
#include <cpr/multiperform.h>
#include <cpr/curlholder.h>
#include <cpr/curlmultiholder.h>
#include <cpr/parameters.h>
#include <cpr/payload.h>
#include <cpr/proxies.h>
#include <cpr/cookies.h>
#include <cpr/proxyauth.h>
#include <cpr/util.h>
#include <cpr/curl_container.h>
#include <memory>

namespace cpr {

// CurlHolder
CurlHolder::CurlHolder() {}
CurlHolder::~CurlHolder() {}

// CurlMultiHolder
CurlMultiHolder::CurlMultiHolder() {}
CurlMultiHolder::~CurlMultiHolder() {}

// Session
Session::Session() {}
std::shared_ptr<Session> Session::GetSharedPtrFromThis() { return nullptr; }
std::shared_ptr<CurlHolder> Session::GetCurlHolder() { return std::make_shared<CurlHolder>(); }
void Session::SetUrl(const Url&) {}
void Session::SetHeader(const Header&) {}
void Session::SetBody(Body&&) {}
void Session::SetBody(const Body&) {}
void Session::SetPayload(Payload&&) {}
void Session::SetPayload(const Payload&) {}
void Session::SetParameters(const Parameters&) {}
void Session::SetParameters(Parameters&&) {}
void Session::SetCookies(const Cookies&) {}
void Session::SetProxies(Proxies&&) {}
void Session::SetProxies(const Proxies&) {}
void Session::SetTimeout(const Timeout&) {}
void Session::SetConnectTimeout(const ConnectTimeout&) {}
void Session::SetRange(const Range&) {}
void Session::SetVerifySsl(const VerifySsl&) {}
Response Session::Get() { return {}; }
Response Session::Post() { return {}; }

// MultiPerform
MultiPerform::MultiPerform() = default;
MultiPerform::~MultiPerform() = default;
std::vector<Response> MultiPerform::Get() { return {}; }
void MultiPerform::AddSession(std::shared_ptr<Session>&, HttpMethod) {}

// EncodedAuthentication
EncodedAuthentication::~EncodedAuthentication() noexcept = default;

// Parameters
Parameters::Parameters(const std::initializer_list<Parameter>&) {}

// Payload
Payload::Payload(const std::initializer_list<Pair>&) {}

// Proxies
Proxies::Proxies(const std::initializer_list<std::pair<const std::string, std::string>>&) {}

// Cookie / Cookies
const std::string Cookie::GetName() const { return {}; }
const std::string Cookie::GetValue() const { return {}; }
void Cookies::emplace_back(const Cookie&) {}
Cookies::const_iterator Cookies::begin() const { return {}; }
Cookies::const_iterator Cookies::end() const { return {}; }

// util
namespace util {
std::string urlEncode(const std::string&) { return {}; }
}

// CaseInsensitiveCompare
bool CaseInsensitiveCompare::operator()(const std::string&, const std::string&) const noexcept { return false; }

// CurlContainer template definitions (must appear before explicit instantiation)
template <class T>
void CurlContainer<T>::Add(const T&) {}

template <class T>
void CurlContainer<T>::Add(const std::initializer_list<T>&) {}

template <class T>
const std::string CurlContainer<T>::GetContent(const CurlHolder&) const { return {}; }

// CurlContainer explicit instantiations
template class CurlContainer<Pair>;
template class CurlContainer<Parameter>;

} // namespace cpr

// ============================================================================
// 2. std::__fs::filesystem — MOVED
// ============================================================================
// The placeholder bodies that used to live here broke i18n locale discovery
// and config saving at runtime. Real implementations backed by the XDK Win32
// file APIs now live in xbox360_filesystem.cpp (same target).

// ============================================================================
// 3. std::chrono stubs
// ============================================================================
#include <chrono>

namespace std {
namespace __1 {
namespace chrono {

system_clock::time_point system_clock::now() noexcept {
    return time_point(duration(0));
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

string to_string(int v)                     { return __int_to_str(v); }
string to_string(unsigned v)                { return __uint_to_str(v); }
string to_string(long v)                    { return __int_to_str(v); }
string to_string(unsigned long v)           { return __uint_to_str(v); }
string to_string(long long v)               { return __int_to_str(v); }
string to_string(unsigned long long v)      { return __uint_to_str(v); }
string to_string(float)                     { return string("0"); }
string to_string(double)                    { return string("0"); }

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

// 8. curl stubs (M4 territory)
// ============================================================================
// cpr headers already pull in curl/curl.h with real types; just define the functions.

extern "C" {

CURL*       curl_easy_init(void)                         { return nullptr; }
CURLcode    curl_easy_setopt(CURL*, CURLoption, ...)     { return CURLE_FAILED_INIT; }
CURLcode    curl_easy_perform(CURL*)                     { return CURLE_FAILED_INIT; }
void        curl_easy_cleanup(CURL*)                     {}
CURLcode    curl_easy_getinfo(CURL*, CURLINFO, ...)      { return CURLE_FAILED_INIT; }

struct curl_slist* curl_slist_append(struct curl_slist*, const char*) { return nullptr; }
void curl_slist_free_all(struct curl_slist*) {}

CURLSH*     curl_share_init(void)                                  { return nullptr; }
CURLSHcode  curl_share_setopt(CURLSH*, CURLSHoption, ...)         { return CURLSHE_OK; }
CURLSHcode  curl_share_cleanup(CURLSH*)                            { return CURLSHE_OK; }

} // extern "C"
