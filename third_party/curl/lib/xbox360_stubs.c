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

/* ------------------------------------------------------------------------
 * 自有 DNS 客户端（A 记录），绕开 XNet 的 8 条上限。
 *
 * XNDNS 只有 aina[8]，devkit 上链接的 checked xam 在解析到超过 8 条 A 记录时
 * 会直接 assert（0xC0000420），整台主机卡死。B 站直播弹幕节点
 * zj-cn-live-comet.chat.bilibili.com 走 CNAME 后一次返回 15 条 A 记录，
 * 所以"打开直播就卡死"是必现的。这里自己发 UDP 查询、自己解析，A 记录取
 * 前 8 条即可，XNetDnsLookup 只作为最后兜底保留。
 *
 * 解析器用国内公共 DNS（与 PS4 分支的 primaryDNSStr 一致），按序尝试；
 * 全部失败时返回 0，由调用方回退到 XNet。
 * ------------------------------------------------------------------------ */
#define X360_DNS_RESOLVERS 3
static const unsigned char x360_dns_servers[X360_DNS_RESOLVERS][4] = {
    { 223,   5,   5,   5 },   /* AliDNS */
    { 119,  29,  29,  29 },   /* DNSPod */
    { 114, 114, 114, 114 },   /* 114DNS */
};

static int x360_dns_build_query(unsigned char* buf, int cap, const char* name,
                                unsigned short id)
{
    int n = 12;
    size_t i, len;

    if (!name || !name[0]) return 0;
    len = strlen(name);
    /* 18 = header tail + root + QTYPE/QCLASS; 11 = EDNS0 OPT record below. */
    if (len > 253 || cap < (int)len + 29) return 0;

    memset(buf, 0, 12);
    buf[0] = (unsigned char)(id >> 8);
    buf[1] = (unsigned char)(id & 0xff);
    buf[2] = 0x01;   /* RD */
    buf[5] = 0x01;   /* QDCOUNT = 1 */

    i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && name[j] != '.') j++;
        if (j == i || j - i > 63) return 0;   /* empty or over-long label */
        buf[n++] = (unsigned char)(j - i);
        memcpy(buf + n, name + i, j - i);
        n += (int)(j - i);
        i = j + 1;
    }
    buf[n++] = 0;          /* root label */
    buf[n++] = 0; buf[n++] = 1;   /* QTYPE  A */
    buf[n++] = 0; buf[n++] = 1;   /* QCLASS IN */

    /* EDNS0 OPT: advertise a 1232-byte UDP payload.  Without it the servers
       cap the answer at 512 bytes and a CNAME chain plus 9-15 A records (the
       live comet host) comes back truncated with ancount == 0, which used to
       send us back to XNetDnsLookup and its 8-record assertion. */
    buf[n++] = 0;                 /* root */
    buf[n++] = 0; buf[n++] = 41;  /* TYPE  OPT */
    buf[n++] = 0x04; buf[n++] = 0xd0;  /* CLASS = 1232 */
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;  /* TTL */
    buf[n++] = 0; buf[n++] = 0;   /* RDLEN */
    buf[11] = 0x01;               /* ARCOUNT = 1 */
    return n;
}

/* Returns the offset just past the (possibly compressed) name, or -1. */
static int x360_dns_skip_name(const unsigned char* buf, int len, int off)
{
    int guard = 0;
    while (off >= 0 && off < len && guard++ < 128) {
        unsigned char c = buf[off];
        if (c == 0) return off + 1;
        if ((c & 0xc0) == 0xc0) return off + 2;   /* pointer terminates the name */
        if (c & 0xc0) return -1;
        off += 1 + (int)c;
    }
    return -1;
}

static int x360_dns_parse_response(const unsigned char* buf, int len,
                                   unsigned short id, struct in_addr* out, int max)
{
    int off, qd, an, i, found = 0;

    if (len < 12) return 0;
    if (((buf[0] << 8) | buf[1]) != (int)id) return 0;   /* wrong transaction */
    if ((buf[2] & 0x80) == 0) return 0;                 /* not a response */

    qd = (buf[4] << 8) | buf[5];
    an = (buf[6] << 8) | buf[7];

    off = 12;
    for (i = 0; i < qd; i++) {
        off = x360_dns_skip_name(buf, len, off);
        if (off < 0 || off + 4 > len) return 0;
        off += 4;
    }

    for (i = 0; i < an && found < max; i++) {
        int rtype, rclass, rdlen;
        off = x360_dns_skip_name(buf, len, off);
        if (off < 0 || off + 10 > len) break;
        rtype  = (buf[off] << 8) | buf[off + 1];
        rclass = (buf[off + 2] << 8) | buf[off + 3];
        rdlen  = (buf[off + 8] << 8) | buf[off + 9];
        off += 10;
        if (rdlen < 0 || off + rdlen > len) break;
        if (rtype == 1 && rclass == 1 && rdlen == 4) {
            memcpy(&out[found], buf + off, 4);
            found++;
        }
        off += rdlen;
    }
    return found;
}

static int x360_dns_ask(const unsigned char server[4], const char* name,
                        struct in_addr* out, int max)
{
    SOCKET s;
    struct sockaddr_in to;
    unsigned char q[512], r[1500];
    unsigned short id;
    int qlen, n, got = 0;
    DWORD start;

    id   = (unsigned short)GetTickCount();
    qlen = x360_dns_build_query(q, sizeof(q), name, id);
    if (qlen <= 0) return 0;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    {
        /* Same SNL opt-out curl's connect path uses; harmless if rejected. */
        int on = 1;
        setsockopt(s, SOL_SOCKET, 0x5801, (const char*)&on, sizeof(on));
    }

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port   = htons(53);
    memcpy(&to.sin_addr.s_addr, server, 4);

    if (sendto(s, (const char*)q, qlen, 0, (struct sockaddr*)&to, sizeof(to)) == qlen) {
        start = GetTickCount();
        while (GetTickCount() - start < 900) {
            fd_set rs;
            struct timeval tv;
            DWORD left = 900 - (GetTickCount() - start);

            FD_ZERO(&rs);
            FD_SET(s, &rs);
            tv.tv_sec  = 0;
            tv.tv_usec = (long)left * 1000;
            n = select(0, &rs, NULL, NULL, &tv);
            if (n <= 0) break;   /* timeout / error */
            n = recvfrom(s, (char*)r, sizeof(r), 0, NULL, NULL);
            if (n <= 0) continue;
            got = x360_dns_parse_response(r, n, id, out, max);
            if (got > 0) break;
        }
    }

    closesocket(s);
    return got;
}

/* Synchronous A-record lookup.  Returns the number of addresses written
   (at most max); 0 means every resolver failed and the caller should fall
   back to XNetDnsLookup. */
int x360_dns_lookup_a(const char* name, struct in_addr* out, int max)
{
    int i, n;

    if (!name || !out || max <= 0) return 0;
    for (i = 0; i < X360_DNS_RESOLVERS; i++) {
        n = x360_dns_ask(x360_dns_servers[i], name, out, max);
        if (n > 0) return n;
    }
    DbgPrint("[dns] udp failed %s (falling back to XNet)", name);
    return 0;
}

struct x360_hostent_slot {
    struct hostent he;
    char name_buffer[256];
    struct in_addr address;
    char* address_list[2];
};

static struct x360_hostent_slot x360_dns_ring[X360_DNS_RING];
static unsigned x360_dns_seq = 0;

static struct hostent* x360_make_hostent(const char* name, struct in_addr addr)
{
    unsigned seq = __sync_fetch_and_add(&x360_dns_seq, 1);
    struct x360_hostent_slot* slot = &x360_dns_ring[seq % X360_DNS_RING];

    slot->address = addr;
    strncpy(slot->name_buffer, name, sizeof(slot->name_buffer) - 1);
    slot->name_buffer[sizeof(slot->name_buffer) - 1] = '\0';
    slot->address_list[0] = (char*)&slot->address;
    slot->address_list[1] = NULL;
    slot->he.h_name      = slot->name_buffer;
    slot->he.h_aliases   = NULL;
    slot->he.h_addrtype  = AF_INET;
    slot->he.h_length    = sizeof(slot->address);
    slot->he.h_addr_list = slot->address_list;
    return &slot->he;
}

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

    /* 先用自有 UDP 解析器。XNet 的 XNDNS 只有 8 条 A 记录的位置，devkit 上
       的 checked xam 在直播弹幕节点返回 15 条时会直接 assert（表现为打开直播
       就卡死）。自己解析则没有这个上限，且不像 XNet resolver 那样要拿全局锁
       排队。全部公共 DNS 都失败时才回退到 XNet。 */
    {
        struct in_addr addrs[8];
        if (x360_dns_lookup_a(name, addrs, 8) > 0) {
            struct hostent* he = x360_make_hostent(name, addrs[0]);
            unsigned char* ipa = (unsigned char*)&addrs[0];
            x360_dns_cache_put(name, addrs[0]);
            DbgPrint("[dns] udp %s -> %u.%u.%u.%u", name,
                     (unsigned)ipa[0], (unsigned)ipa[1],
                     (unsigned)ipa[2], (unsigned)ipa[3]);
            x360_dns_lock_release();
            return he;
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
