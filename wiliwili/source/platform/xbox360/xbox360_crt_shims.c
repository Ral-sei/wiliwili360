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

unsigned char _BitScanReverse(unsigned long* index, unsigned long mask)
{
    if (!mask)
        return 0;
    unsigned long bit = 31;
    while (!(mask & (1u << bit)))
        --bit;
    *index = bit;
    return 1;
}

unsigned char _BitScanForward(unsigned long* index, unsigned long mask)
{
    if (!mask)
        return 0;
    unsigned long bit = 0;
    while (!(mask & (1u << bit)))
        ++bit;
    *index = bit;
    return 1;
}

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
