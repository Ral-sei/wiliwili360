/*
 * Xbox 360 XDK stubs for missing CRT/Win32 functions needed by libcurl
 */

#include "curl_setup.h"
#include <xtl.h>

/* Xbox 360 doesn't have stat()/fstat(); stub them out.
   We don't use multipart form uploads, so these never actually get called
   for our HTTP streaming use case. */
int stat(const char *path, struct stat *buf)
{
    (void)path;
    (void)buf;
    return -1;
}

int fstat(int fd, struct stat *buf)
{
    (void)fd;
    (void)buf;
    return -1;
}

/* Xbox 360 doesn't have ExpandEnvironmentStringsA; stub it.
   getenv.c uses this but we don't use environment variables. */
DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize)
{
    (void)lpSrc;
    (void)lpDst;
    (void)nSize;
    return 0;
}

struct hostent* gethostbyname(const char* name)
{
    /* All requests must supply CURLOPT_RESOLVE from XnetResolver. */
    (void)name;
    WSASetLastError(WSAHOST_NOT_FOUND);
    return NULL;
}
