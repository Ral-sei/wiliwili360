#include <xtl.h>
#include <string.h>
#include <time.h>

#include "mbedtls/platform_util.h"

static int XboxIsLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

struct tm *mbedtls_platform_gmtime_r(
    const mbedtls_time_t *input,
    struct tm *output)
{
    static const unsigned __int64 kUnixEpochFileTime =
        116444736000000000ui64;
    static const unsigned __int64 kTicksPerSecond = 10000000ui64;
    static const int kDaysBeforeMonth[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    unsigned __int64 seconds;
    unsigned __int64 file_time_value;
    FILETIME file_time;
    SYSTEMTIME system_time;
    int year;

    if (!input || !output || *input < 0)
        return NULL;
    seconds = (unsigned __int64)*input;
    if (seconds > (0x7FFFFFFFFFFFFFFFui64 - kUnixEpochFileTime) /
            kTicksPerSecond)
        return NULL;
    file_time_value = kUnixEpochFileTime + seconds * kTicksPerSecond;
    file_time.dwLowDateTime = (DWORD)file_time_value;
    file_time.dwHighDateTime = (DWORD)(file_time_value >> 32);
    if (!FileTimeToSystemTime(&file_time, &system_time))
        return NULL;

    memset(output, 0, sizeof(*output));
    year = (int)system_time.wYear;
    output->tm_year = year - 1900;
    output->tm_mon = (int)system_time.wMonth - 1;
    output->tm_mday = (int)system_time.wDay;
    output->tm_hour = (int)system_time.wHour;
    output->tm_min = (int)system_time.wMinute;
    output->tm_sec = (int)system_time.wSecond;
    output->tm_wday = (int)system_time.wDayOfWeek;
    output->tm_yday = kDaysBeforeMonth[output->tm_mon] +
        output->tm_mday - 1;
    if (output->tm_mon > 1 && XboxIsLeapYear(year))
        ++output->tm_yday;
    output->tm_isdst = 0;
    return output;
}
