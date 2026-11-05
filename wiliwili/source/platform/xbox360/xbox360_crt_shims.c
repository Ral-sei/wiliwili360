// Xenon CRT helpers that neither the XDK's libcMT nor OXDK's runtime provides.
//
// These are plain implementations, not placeholders: the console CRT leaves out
// a handful of C99/C11 entry points that clang's headers still declare, and the
// MSVC intrinsics that MS compatibility mode expects from <intrin.h>.
//
// Compiled as C with the project's Xenon flags (freestanding, -fno-builtin,
// +xenon-abi) so it cannot pull in host headers by accident.

typedef unsigned int uint32_t_x;
typedef unsigned long long uint64_t_x;
typedef unsigned long ulong_x;

// --- C99 string to integer -------------------------------------------------
// libcMT has _strtoi64/_strtoui64 with the MSVC spelling; clang's headers
// declare strtoll/strtoull and wiliwili's callers use those.

static int xbox360_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

static int xbox360_skip_space_and_sign(const char** cursor, int* negative)
{
    const char* s = *cursor;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        ++s;
    if (*s == '+' || *s == '-') {
        *negative = (*s == '-');
        ++s;
    }
    *cursor = s;
    return 0;
}

static uint64_t_x xbox360_strtoull_core(const char* nptr, char** endptr, int base, int* negative)
{
    const char* s = nptr;
    xbox360_skip_space_and_sign(&s, negative);

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    uint64_t_x value = 0;
    const char* digits_start = s;
    for (; *s; ++s) {
        int digit = xbox360_digit_value(*s);
        if (digit < 0 || digit >= base)
            break;
        value = value * (uint64_t_x)base + (uint64_t_x)digit;
    }

    if (endptr)
        *endptr = (char*)(s == digits_start ? nptr : s);
    return value;
}

unsigned long long strtoull(const char* nptr, char** endptr, int base)
{
    int negative = 0;
    return xbox360_strtoull_core(nptr, endptr, base, &negative);
}

long long strtoll(const char* nptr, char** endptr, int base)
{
    int negative = 0;
    uint64_t_x value = xbox360_strtoull_core(nptr, endptr, base, &negative);
    return negative ? -(long long)value : (long long)value;
}

// --- C99 float helpers -----------------------------------------------------
// Soft-float targets end up with calls for these even at -O2, and libcMT's
// copies live behind the Microsoft spellings.

double round(double value)
{
    double truncated = (double)(long long)value;
    double fraction = value - truncated;
    if (fraction >= 0.5)
        return truncated + 1.0;
    if (fraction <= -0.5)
        return truncated - 1.0;
    return truncated;
}

float roundf(float value)
{
    float truncated = (float)(long long)value;
    float fraction = value - truncated;
    if (fraction >= 0.5f)
        return truncated + 1.0f;
    if (fraction <= -0.5f)
        return truncated - 1.0f;
    return truncated;
}

float fminf(float a, float b)
{
    if (a != a)
        return b;
    if (b != b)
        return a;
    return a < b ? a : b;
}

float fmaxf(float a, float b)
{
    if (a != a)
        return b;
    if (b != b)
        return a;
    return a > b ? a : b;
}

// --- MSVC intrinsics -------------------------------------------------------
// clang's MS compatibility mode declares these in <intrin.h> and the XDK
// headers call them; the console CRT ships them only for the Microsoft
// compiler's own code generation.

// _InterlockedIncrement/_InterlockedDecrement are clang builtins on Xenon
// (type: long (volatile long *)), no user definition needed.

// --- Console stubs ---------------------------------------------------------
// WriteConsoleW is asked for by code compiled against Windows headers that
// assumes a Win32 console. There is none: report failure so callers fall back.
int WriteConsoleW(void* console, const void* buffer, unsigned long length,
                  unsigned long* written, void* reserved)
{
    (void)console;
    (void)buffer;
    (void)length;
    (void)reserved;
    if (written)
        *written = 0;
    return 0;
}

// _Unwind_Resume is the C++ personality's landing pad target. The build is
// -fno-exceptions, so nothing may reach it; stopping here is the honest result
// rather than pretending to unwind.
extern void abort(void);

void _Unwind_Resume(void* exception)
{
    (void)exception;
    abort();
}

// --- POSIX spellings the XDK's oldnames.lib used to alias -------------------
//
// xav.lib is a static XMF media pipeline and calls the plain POSIX names
// (open/close/read/write/lseek/isatty/fdopen/dup2/unlink/tempnam/vsnprintf/
// vsnprintf_s/_setjmp/j1).  In a normal MSVC link those come from oldnames.lib,
// which is nothing but 139-byte "machine 0x0000" objects holding
// /alternatename directives -- coff2elf refuses them, so OXDK links without any
// of the aliases.  Each shim below is the same forwarding the alias table would
// have produced; libcMT does define every "_"-prefixed target.
//
// Only reachable when XBOX360_XAV is on: nothing else in wiliwili calls them.

extern int _open(const char* path, int flags, ...);
extern int _close(int fd);
extern int _read(int fd, void* buffer, unsigned int count);
extern int _write(int fd, const void* buffer, unsigned int count);
extern long _lseek(int fd, long offset, int origin);
extern int _isatty(int fd);
extern void* _fdopen(int fd, const char* mode);
extern int _dup2(int fd1, int fd2);
extern int _unlink(const char* path);
extern char* _tempnam(const char* dir, const char* prefix);
extern int _vsnprintf(char* buffer, unsigned int count, const char* format,
                      __builtin_va_list args);
extern int _vsnprintf_s(char* buffer, unsigned int sizeOfBuffer, unsigned int count,
                        const char* format, __builtin_va_list args);
extern double _j1(double x);
extern int setjmp(void* env);

int open(const char* path, int flags, ...)
{
    // _O_CREAT is the only flag that makes the mode argument meaningful.
    if (flags & 0x0100) {
        __builtin_va_list args;
        __builtin_va_start(args, flags);
        int mode = __builtin_va_arg(args, int);
        __builtin_va_end(args);
        return _open(path, flags, mode);
    }
    return _open(path, flags);
}

int close(int fd) { return _close(fd); }

int read(int fd, void* buffer, unsigned int count) { return _read(fd, buffer, count); }

int write(int fd, const void* buffer, unsigned int count) { return _write(fd, buffer, count); }

long lseek(int fd, long offset, int origin) { return _lseek(fd, offset, origin); }

int isatty(int fd) { return _isatty(fd); }

void* fdopen(int fd, const char* mode) { return _fdopen(fd, mode); }

int dup2(int fd1, int fd2) { return _dup2(fd1, fd2); }

int unlink(const char* path) { return _unlink(path); }

char* tempnam(const char* dir, const char* prefix) { return _tempnam(dir, prefix); }

// The build passes -Dvsnprintf=_vsnprintf, which would turn the definition
// below into a second _vsnprintf and collide with libcMT's.  xav.lib was built
// by MSVC and calls the plain spelling, so the real symbol is needed here.
#ifdef vsnprintf
#undef vsnprintf
#endif

int vsnprintf(char* buffer, unsigned int count, const char* format, __builtin_va_list args)
{
    return _vsnprintf(buffer, count, format, args);
}

int vsnprintf_s(char* buffer, unsigned int sizeOfBuffer, unsigned int count,
                const char* format, __builtin_va_list args)
{
    return _vsnprintf_s(buffer, sizeOfBuffer, count, format, args);
}

// _setjmp must share setjmp's caller frame.  A C wrapper would call setjmp()
// from its own frame, so longjmp() would later restore a stack pointer that is
// dead (and clang, not knowing the wrapper returns twice, may reuse the slot).
// Tail-branch instead: `b` leaves LR pointing back into xav.lib's call site and
// runs setjmp with the caller's SP, so it saves exactly the frame it would have
// saved had xav.lib called setjmp directly.
__asm__(
    ".text\n"
    ".globl _setjmp\n"
    ".type _setjmp, @function\n"
    "_setjmp:\n"
    "    b setjmp\n"
    ".size _setjmp, . - _setjmp\n");

// j1 has a "_"-prefixed twin under a different name in libcMT (_j1); an
// ordinary call is correct here, no caller-frame identity is involved.
double j1(double x) { return _j1(x); }

// --- winsock wrappers' __real_ targets -------------------------------------
//
// xav.lib references gethostbyname, which pulls xbox_winsock_extra.o in; that
// object defines __wrap_getsockopt/__wrap_WSAStartup and calls __real_*, which
// only ld.lld's --wrap can define.  wiliwili does not wrap (its networking is
// already working), so point __real_* straight at the XNet entry points.  If
// --wrap is ever enabled for this link these two become self-recursive.
extern int WSAStartup(unsigned short version, void* data);
extern int getsockopt(unsigned int socket, int level, int name, char* value, int* length);

int __real_WSAStartup(unsigned short version, void* data)
{
    return WSAStartup(version, data);
}

int __real_getsockopt(unsigned int socket, int level, int name, char* value, int* length)
{
    return getsockopt(socket, level, name, value, length);
}

// --- IsEqualGUID -----------------------------------------------------------
//
// XAV (xbox360_xav) hardware-decode path is retired: its entry points in
// xav.lib are MSVC-mangled C++ symbols the OXDK toolchain cannot translate,
// so the link always fails and nothing calls it.  The shim that satisfied
// xav.lib's IsEqualGUID reference is gone with it; d3dx9.lib / xaudio2.lib
// export a real IsEqualGUID for any remaining consumer.
