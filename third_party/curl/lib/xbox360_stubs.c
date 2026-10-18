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

/* DbgPrint: the only runtime evidence for what a lookup actually returned.
   Declared here (as in cbffmpeg's compat) because no XDK header exposes it. */
extern void DbgPrint(const char* fmt, ...);

/* 结果槽位环：POSIX 说 gethostbyname 的返回值在「下一次调用」之前有效，
   但并发调用必须各自拿到独立的存储，否则调用方 memcpy 的那一刻数据已经被
   别的线程覆盖。真机实测（视频页一次并发请求 burst）表现是：
     - [XNET:1] XNetDnsRelease failed: 10022  ← 两个线程释放同一个 XNDNS*；
     - ssl_handshake (-0x7280) EOF / (-0x7780) fatal alert ← 拿到别人的 IP，
       SNI 对不上，对端直接关连接；
     - Operation timed out after N milliseconds ← 另一个线程把 dns 指针改掉后，
       本线程在已置位的事件上空转，请求永远不结束。
   槽位数只需远大于「调用方返回后到 memcpy 完成」之间可能发生的解析次数，
   8 已经绰绰有余；序号用原子自增，不引入锁。 */
#define X360_DNS_RING 8

/* DNS 结果小缓存：XNet resolver 在本平台不能并发使用（见下方注释），
 * 必须持锁完成整个 lookup，这正是「视频页 burst 的十几个请求一个个
 * 排队等解析」的直接原因。API 与封面实际只涉及 api.bilibili.com /
 * i0.hdslb.com / *.bilivideo.com 等少数域名，给 gethostbyname 加一层
 * 带 TTL 的缓存后，同一页面 burst 内只需真正解析一次，其余直接命中。
 * TTL 与 curl 的 CURLOPT_DNS_CACHE_TIMEOUT=60 对齐。 */
#define X360_DNS_CACHE 16
#define X360_DNS_CACHE_TTL_MS (60 * 1000)

struct x360_dns_cache_entry {
    char name[256];
    struct in_addr addr;
    DWORD expire_tick;
};

static struct x360_dns_cache_entry x360_dns_cache[X360_DNS_CACHE];
static unsigned x360_dns_cache_seq = 0;

/* 调用时必须持 x360_dns_lock；命中返回 1，未命中/过期返回 0 并清除 */
static int x360_dns_cache_get(const char* name, struct in_addr* out)
{
    DWORD now = GetTickCount();
    unsigned i, oldest = 0;
    DWORD oldest_expire = 0xFFFFFFFFu;
    for (i = 0; i < X360_DNS_CACHE; i++) {
        if (x360_dns_cache[i].name[0] == '\0')
            continue;
        if (x360_dns_cache[i].expire_tick != 0 && (long)(now - x360_dns_cache[i].expire_tick) >= 0) {
            /* 过期：顺手清掉 */
            x360_dns_cache[i].name[0] = '\0';
            continue;
        }
        if (stricmp(x360_dns_cache[i].name, name) == 0) {
            *out = x360_dns_cache[i].addr;
            return 1;
        }
        if (x360_dns_cache[i].expire_tick < oldest_expire) {
            oldest_expire = x360_dns_cache[i].expire_tick;
            oldest = i;
        }
    }
    (void)oldest; (void)oldest_expire;
    return 0;
}

/* 调用时必须持 x360_dns_lock；简单的先到先淘汰（条目数很小，无需 LRU） */
static void x360_dns_cache_put(const char* name, struct in_addr addr)
{
    unsigned i, victim = x360_dns_cache_seq++ % X360_DNS_CACHE;
    for (i = 0; i < X360_DNS_CACHE; i++) {
        if (x360_dns_cache[i].name[0] == '\0') {
            victim = i;
            break;
        }
    }
    strncpy(x360_dns_cache[victim].name, name, sizeof(x360_dns_cache[victim].name) - 1);
    x360_dns_cache[victim].name[sizeof(x360_dns_cache[victim].name) - 1] = '\0';
    x360_dns_cache[victim].addr = addr;
    x360_dns_cache[victim].expire_tick = GetTickCount() + X360_DNS_CACHE_TTL_MS;
}

/* XNet's asynchronous resolver is not safe to use concurrently on this
   target. In particular, overlapping lookups can make a completed XNDNS
   handle appear owned by another caller when it is released. Keep the
   resolver critical section small and serialize only the XNet lookup. */
static volatile LONG x360_dns_lock = 0;

static void x360_dns_lock_acquire(void)
{
    while(InterlockedCompareExchange(&x360_dns_lock, 1, 0) != 0)
        Sleep(0);
}

static void x360_dns_lock_release(void)
{
    InterlockedExchange(&x360_dns_lock, 0);
}

struct x360_hostent_slot {
    struct hostent he;
    char name_buffer[256];
    struct in_addr address;
    char* address_list[2];
};

static struct x360_hostent_slot x360_dns_ring[X360_DNS_RING];
static unsigned x360_dns_seq = 0;

struct hostent* gethostbyname(const char* name)
{
    XNDNS* dns = NULL;          /* 每线程局部：绝不共享，杜绝重复 Release */
    WSAEVENT event_handle;
    int error;
    struct x360_hostent_slot* slot;
    unsigned seq;
    unsigned char* ip;

    if (!name || !name[0]) {
        WSASetLastError(WSAHOST_NOT_FOUND);
        return NULL;
    }

    x360_dns_lock_acquire();

    /* 缓存命中：跳过 XNet，直接在槽位里造 hostent 返回 */
    {
        struct in_addr cached;
        if (x360_dns_cache_get(name, &cached)) {
            seq  = __sync_fetch_and_add(&x360_dns_seq, 1);
            slot = &x360_dns_ring[seq % X360_DNS_RING];
            slot->address = cached;
            strncpy(slot->name_buffer, name, sizeof(slot->name_buffer) - 1);
            slot->name_buffer[sizeof(slot->name_buffer) - 1] = '\0';
            slot->address_list[0] = (char*)&slot->address;
            slot->address_list[1] = NULL;
            slot->he.h_name      = slot->name_buffer;
            slot->he.h_aliases   = NULL;
            slot->he.h_addrtype  = AF_INET;
            slot->he.h_length    = sizeof(slot->address);
            slot->he.h_addr_list = slot->address_list;
            DbgPrint("[dns] cache hit %s", name);
            x360_dns_lock_release();
            return &slot->he;
        }
    }

    event_handle = WSACreateEvent();
    if (event_handle == WSA_INVALID_EVENT) {
        x360_dns_lock_release();
        return NULL;
    }
    error = XNetDnsLookup(name, event_handle, &dns);
    if (error != 0 || !dns) {
        WSACloseEvent(event_handle);
        WSASetLastError(error ? error : WSAHOST_NOT_FOUND);
        DbgPrint("[dns] lookup failed %s err=%d", name, error);
        x360_dns_lock_release();
        return NULL;
    }
    while (dns->iStatus == WSAEINPROGRESS)
        WaitForSingleObject((HANDLE)event_handle, 25);
    if (dns->iStatus != 0 || dns->cina == 0) {
        int status = dns->iStatus;
        WSASetLastError(status ? status : WSAHOST_NOT_FOUND);
        XNetDnsRelease(dns);
        WSACloseEvent(event_handle);
        DbgPrint("[dns] resolve failed %s status=%d", name, status);
        x360_dns_lock_release();
        return NULL;
    }

    seq  = __sync_fetch_and_add(&x360_dns_seq, 1);
    slot = &x360_dns_ring[seq % X360_DNS_RING];

    slot->address = dns->aina[0];
    strncpy(slot->name_buffer, name, sizeof(slot->name_buffer) - 1);
    slot->name_buffer[sizeof(slot->name_buffer) - 1] = '\0';
    slot->address_list[0] = (char*)&slot->address;
    slot->address_list[1] = NULL;
    slot->he.h_name      = slot->name_buffer;
    slot->he.h_aliases   = NULL;
    slot->he.h_addrtype  = AF_INET;
    slot->he.h_length    = sizeof(slot->address);
    slot->he.h_addr_list = slot->address_list;

    ip = (unsigned char*)&slot->address;
    DbgPrint("[dns] #%u %s -> %u.%u.%u.%u", seq, slot->name_buffer,
             (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);

    x360_dns_cache_put(name, slot->address);
    XNetDnsRelease(dns);
    WSACloseEvent(event_handle);
    x360_dns_lock_release();
    return &slot->he;
}
