/*
 *  Platform abstraction layer
 *
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#include "common.h"

#if defined(MBEDTLS_PLATFORM_C)

#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/error.h"

#if defined(_XBOX) && defined(MBEDTLS_PLATFORM_MEMORY)
/* wiliwili 五轮埋点：CKE 状态"线程在睡觉、墙钟在走"（cpu << dur），
   头号嫌疑是 9 个握手线程在 mbedtls 分配器/底层堆锁上排队。
   这里对每次 mbedtls 分配计时，≥10ms 直接打印。 */
#include <xtl.h>
extern void DbgPrint(const char *fmt, ...);
#if !defined(X360_RUNTIME_PROBES) || !X360_RUNTIME_PROBES
/* Xbox 360 诊断探针总开关（默认 OFF）：关闭时本文件的 [tls] 分配器探针编译为空。 */
#define DbgPrint(...) ((void)0)
#endif
static unsigned long x360_alloc_ms(void)
{
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if(freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        if(freq.QuadPart == 0)
            freq.QuadPart = 1;
    }
    QueryPerformanceCounter(&now);
    return (unsigned long)((double)now.QuadPart * 1000.0 / (double)freq.QuadPart);
}
#endif

/* The compile time configuration of memory allocation via the macros
 * MBEDTLS_PLATFORM_{FREE/CALLOC}_MACRO takes precedence over the runtime
 * configuration via mbedtls_platform_set_calloc_free(). So, omit everything
 * related to the latter if MBEDTLS_PLATFORM_{FREE/CALLOC}_MACRO are defined. */
#if defined(MBEDTLS_PLATFORM_MEMORY) &&                 \
    !(defined(MBEDTLS_PLATFORM_CALLOC_MACRO) &&        \
    defined(MBEDTLS_PLATFORM_FREE_MACRO))

#if !defined(MBEDTLS_PLATFORM_STD_CALLOC)
static void *platform_calloc_uninit(size_t n, size_t size)
{
    ((void) n);
    ((void) size);
    return NULL;
}

#define MBEDTLS_PLATFORM_STD_CALLOC   platform_calloc_uninit
#endif /* !MBEDTLS_PLATFORM_STD_CALLOC */

#if !defined(MBEDTLS_PLATFORM_STD_FREE)
static void platform_free_uninit(void *ptr)
{
    ((void) ptr);
}

#define MBEDTLS_PLATFORM_STD_FREE     platform_free_uninit
#endif /* !MBEDTLS_PLATFORM_STD_FREE */

static void * (*mbedtls_calloc_func)(size_t, size_t) = MBEDTLS_PLATFORM_STD_CALLOC;
static void (*mbedtls_free_func)(void *) = MBEDTLS_PLATFORM_STD_FREE;

void *mbedtls_calloc(size_t nmemb, size_t size)
{
#if defined(_XBOX)
    void *p;
    const unsigned long t0 = x360_alloc_ms();
    p = (*mbedtls_calloc_func)(nmemb, size);
    {
        const unsigned long dt = x360_alloc_ms() - t0;
        if(dt >= 10)
            DbgPrint("[tls] calloc n=%lu sz=%lu dt=%lu ms", (unsigned long)nmemb,
                     (unsigned long)size, dt);
    }
    return p;
#else
    return (*mbedtls_calloc_func)(nmemb, size);
#endif
}

void mbedtls_free(void *ptr)
{
#if defined(_XBOX)
    if(ptr != NULL) {
        const unsigned long t0 = x360_alloc_ms();
        (*mbedtls_free_func)(ptr);
        {
            const unsigned long dt = x360_alloc_ms() - t0;
            if(dt >= 10)
                DbgPrint("[tls] free dt=%lu ms", dt);
        }
        return;
    }
#endif
    (*mbedtls_free_func)(ptr);
}

int mbedtls_platform_set_calloc_free(void *(*calloc_func)(size_t, size_t),
                                     void (*free_func)(void *))
{
    mbedtls_calloc_func = calloc_func;
    mbedtls_free_func = free_func;
    return 0;
}

#if defined(_XBOX)
#include <string.h>
/* -----------------------------------------------------------------------
 * wiliwili #11：mbedtls 私有分配池（§5.8-3）
 *
 * 五轮实测：CKE 阶段 21.6s 墙钟里线程只执行 0.5s（blocked 而非 CPU-bound），
 * 慢的全是 4~68B 的微小 MPI 分配（p50=20ms / max=60ms），free 同量级，
 * 且 68 字节 calloc 也要 20~60ms → 时间耗在 CRT 全局堆锁上排队；
 * 持锁者是 51.8MB 快照缓冲等大块 alloc/free（页级申请/回收）。
 *
 * 做法：启动期一次性向 CRT 要 4MB arena，此后小块(≤4096B)全部在池内
 * 用 size-class 空闲链复用——不进 CRT 堆、不触发页回收、锁内只有链表操作
 * （≤4KB 的 memset 放在锁外），持锁时间微秒级。
 * 大块（如 16KB 的 ssl in/out buffer）与 arena 耗尽时回退 CRT；
 * free() 用地址区间判断归属，两条路径天然可区分，无需指针登记表。
 * --------------------------------------------------------------------- */
#define X360_POOL_ARENA_SZ ( 4u * 1024u * 1024u )
#define X360_POOL_HDR      ( (size_t) 16 )
#define X360_POOL_MIN_LOG2 ( 4 )                              /* 16B */
#define X360_POOL_NCLASS   ( 9 )                              /* 16,32,...,4096 */
#define X360_POOL_MAX_CLASS ( 1u << ( X360_POOL_MIN_LOG2 + X360_POOL_NCLASS - 1 ) )
#define X360_POOL_MAGIC    ( 0x58573630u )                    /* 'XW60' */

typedef struct x360_pool_blk
{
    struct x360_pool_blk *next;
    unsigned int          cls;
    unsigned int          magic;
} x360_pool_blk;

/* 七轮结论：池子注册成功、持锁仅微秒级，但 EnterCriticalSection 在 6 路
 * 并发下每次入队要等 20ms 的整数倍（内核阻塞/唤醒 ≈ 1 个调度 quantum）。
 * 84407 次慢操作里 cs 合计 2326 秒、hold 仅 10 秒、memset 71ms、时钟 23ms。
 * → 微秒级临界区改用自旋锁：先紧自旋，拿不到再 Sleep(0) 让出（保证持锁者
 *   被换下去时能拿到 hardware thread，避免活锁）。 */
static volatile LONG     x360_pool_spin;
static volatile LONG     x360_pool_state;    /* 0 未初始化 / 1 初始化中 / 2 就绪 / 3 池不可用(走 CRT) */
static volatile LONG     x360_pool_exhausted;
static unsigned char    *x360_pool_base;     /* CRT 原始指针，free 归属判断用 */
static size_t            x360_pool_bytes;
static unsigned char    *x360_pool_bump;
static unsigned char    *x360_pool_end;
static void             *x360_pool_fl[ X360_POOL_NCLASS ];

static int x360_pool_is_ready( void )
{
    /* interlocked 读 = acquire 屏障，保证看到 base/end 的写入 */
    return InterlockedCompareExchange( &x360_pool_state, 2, 2 ) == 2;
}

/* 池锁：临界区只有"链表 push/pop + 一次打点"，微秒级，自旋即可。
 * 紧自旋拿不到 → Sleep(0) 让出，让可能被换下去的持锁者拿到 hardware
 * thread；绝不能睡进内核等事件（实测每次唤醒要 20ms）。 */
static void x360_pool_lock( void )
{
    unsigned int i;
    for( i = 0; i < 64; i++ ) {
        if( InterlockedCompareExchange( &x360_pool_spin, 1, 0 ) == 0 )
            return;
    }
    while( InterlockedCompareExchange( &x360_pool_spin, 1, 0 ) != 0 )
        Sleep( 0 );
}

static void x360_pool_unlock( void )
{
    InterlockedExchange( &x360_pool_spin, 0 );
}

static void x360_pool_init_once( void )
{
    LONG prev = InterlockedCompareExchange( &x360_pool_state, 1, 0 );

    if( prev == 2 || prev == 3 )
        return;
    if( prev == 1 ) {
        /* 别的线程正在初始化，等它完成 */
        while( InterlockedCompareExchange( &x360_pool_state, 1, 1 ) == 1 )
            Sleep( 0 );
        return;
    }

    {
        unsigned char *mem = (unsigned char *) calloc( 1, X360_POOL_ARENA_SZ + 32 );
        if( mem != NULL ) {
            x360_pool_base  = mem;
            x360_pool_bytes = X360_POOL_ARENA_SZ + 32;
            x360_pool_bump  = (unsigned char *)
                ( ( (size_t) mem + 15u ) & ~(size_t) 15u );
            x360_pool_end   = mem + x360_pool_bytes;
        }
    }
    InterlockedExchange( &x360_pool_state, x360_pool_end != NULL ? 2 : 3 );
    DbgPrint( "[tls] pool %s arena=%uKB maxclass=%uB",
              x360_pool_end != NULL ? "ready" : "UNAVAILABLE(fallback CRT)",
              (unsigned) ( X360_POOL_ARENA_SZ / 1024 ),
              (unsigned) X360_POOL_MAX_CLASS );
}

static void *x360_pool_calloc( size_t nmemb, size_t size );
static void  x360_pool_free( void *ptr );

/* 七轮诊断：最坏一次等锁（ms），只写不读，用来看清"20ms 到底花在哪" */
static volatile LONG x360_pool_cs_max;

/* 供应用启动期调用（早于任何 curl/mbedtls 请求）：
 * 提前建池并把自己注册为 mbedtls 的分配器。
 * 七轮：打印函数指针，坐实"注册真的生效了"（否则小块仍走 CRT 全局堆）。 */
void x360_mbedtls_pool_init( void )
{
    x360_pool_init_once();
    if( x360_pool_is_ready() ) {
        mbedtls_platform_set_calloc_free( x360_pool_calloc, x360_pool_free );
        DbgPrint( "[tls] pool registered fn=%p pool=%p crt=%p",
                  (void *) mbedtls_calloc_func,
                  (const void *) x360_pool_calloc,
                  (const void *) calloc );
    }
}

static void *x360_pool_calloc( size_t nmemb, size_t size )
{
    size_t          bytes = nmemb * size;
    size_t          cls_size;
    unsigned int    cls;
    x360_pool_blk  *blk = NULL;
    unsigned char  *mem;
    unsigned long   t0 = 0, t1 = 0, ta = 0, t2 = 0, t3 = 0;
    unsigned long   tot, cs = 0, hold = 0, zero = 0, q = 0;
    int             timed = 0;

    /* 乘法溢出检查（calloc 语义：溢出返回 NULL） */
    if( nmemb != 0 && bytes / nmemb != size )
        return calloc( nmemb, size );
    if( bytes == 0 )
        bytes = 1;

    /* 超过最大 size class（如 16KB ssl 缓冲）→ 走 CRT，不进池 */
    if( bytes > X360_POOL_MAX_CLASS ) {
        t0 = x360_alloc_ms();
        mem = (unsigned char *) calloc( nmemb, size );
        t1 = x360_alloc_ms();
        if( t1 - t0 >= 10 )
            DbgPrint( "[tls] pc big n=%lu sz=%lu dt=%lu(crt)",
                      (unsigned long) nmemb, (unsigned long) size, t1 - t0 );
        return mem;
    }

    if( !x360_pool_is_ready() ) {
        x360_pool_init_once();
        if( !x360_pool_is_ready() ) {
            t0 = x360_alloc_ms();
            mem = (unsigned char *) calloc( nmemb, size );
            t1 = x360_alloc_ms();
            if( t1 - t0 >= 10 )
                DbgPrint( "[tls] pc noready n=%lu sz=%lu dt=%lu(crt)",
                          (unsigned long) nmemb, (unsigned long) size, t1 - t0 );
            return mem;
        }
    }

    cls = 0;
    cls_size = (size_t) 1u << X360_POOL_MIN_LOG2;
    while( cls_size < bytes && cls + 1 < X360_POOL_NCLASS ) {
        cls_size <<= 1;
        cls++;
    }
    if( cls_size < bytes ) {         /* 理论上到不了（bytes ≤ MAX_CLASS） */
        t0 = x360_alloc_ms();
        mem = (unsigned char *) calloc( nmemb, size );
        t1 = x360_alloc_ms();
        if( t1 - t0 >= 10 )
            DbgPrint( "[tls] pc class n=%lu sz=%lu dt=%lu(crt)",
                      (unsigned long) nmemb, (unsigned long) size, t1 - t0 );
        return mem;
    }

    t0 = x360_alloc_ms();            /* 池路径计时起点 */
    timed = 1;

    x360_pool_lock();
    ta = x360_alloc_ms();            /* 进锁后立刻打点：cs = 等锁时间 */
    blk = (x360_pool_blk *) x360_pool_fl[ cls ];
    if( blk != NULL ) {
        x360_pool_fl[ cls ] = blk->next;
    }
    else if( (size_t) ( x360_pool_end - x360_pool_bump ) >= cls_size + X360_POOL_HDR ) {
        blk = (x360_pool_blk *) (void *) x360_pool_bump;
        x360_pool_bump += cls_size + X360_POOL_HDR;
        blk->cls   = cls;
        blk->magic = X360_POOL_MAGIC;
    }
    x360_pool_unlock();
    t2 = x360_alloc_ms();

    cs   = ta - t0;                  /* 等锁 */
    hold = t2 - ta;                  /* 持锁（含两次打点本身的开销） */
    if( (LONG) cs > InterlockedCompareExchange( &x360_pool_cs_max, 0, 0 ) )
        InterlockedExchange( &x360_pool_cs_max, (LONG) cs );

    if( blk == NULL ) {
        /* arena 耗尽：一次性告警，之后这些请求回退 CRT（仍正确，只是慢） */
        if( InterlockedCompareExchange( &x360_pool_exhausted, 1, 0 ) == 0 )
            DbgPrint( "[tls] pool arena exhausted, falling back to CRT" );
        mem = (unsigned char *) calloc( nmemb, size );
        if( timed ) {
            t3 = x360_alloc_ms();
            if( t3 - t0 >= 10 )
                DbgPrint( "[tls] pc poolmiss n=%lu sz=%lu dt=%lu(cs=%lu hold=%lu)",
                          (unsigned long) nmemb, (unsigned long) size,
                          t3 - t0, cs, hold );
        }
        return mem;
    }

    mem = (unsigned char *) blk + X360_POOL_HDR;
    memset( mem, 0, bytes );         /* calloc 语义；放在锁外 */
    t3 = x360_alloc_ms();
    zero = t3 - t2;
    tot  = t3 - t0;
    if( tot >= 10 ) {
        unsigned long qa = x360_alloc_ms();
        unsigned long qb = x360_alloc_ms();
        q = qb - qa;                 /* 两次连续打点之差 ≈ 时钟本身的开销 */
        DbgPrint( "[tls] pa n=%lu sz=%lu tot=%lu cs=%lu hold=%lu zero=%lu q=%lu",
                  (unsigned long) nmemb, (unsigned long) size,
                  tot, cs, hold, zero, q );
    }
    return mem;
}

static void x360_pool_free( void *ptr )
{
    unsigned char   *p = (unsigned char *) ptr;
    x360_pool_blk   *blk;
    unsigned long   t0, ta, t2, cs, hold, q;

    if( ptr == NULL )
        return;

    if( !x360_pool_is_ready() ) {    /* 池未建/不可用 → 一定是 CRT 指针 */
        free( ptr );
        return;
    }
    if( p < x360_pool_base || p >= x360_pool_base + x360_pool_bytes ) {
        free( ptr );                 /* 大块/回退块 → CRT */
        return;
    }

    blk = (x360_pool_blk *) (void *) ( p - X360_POOL_HDR );
    if( blk->magic != X360_POOL_MAGIC || blk->cls >= X360_POOL_NCLASS ) {
        DbgPrint( "[tls] pool free: bad header %p", ptr );
        return;                      /* 宁可泄漏也不破坏空闲链 */
    }
    t0 = x360_alloc_ms();
    x360_pool_lock();
    ta = x360_alloc_ms();            /* cs = 等锁 */
    blk->next = (x360_pool_blk *) x360_pool_fl[ blk->cls ];
    x360_pool_fl[ blk->cls ] = blk;
    x360_pool_unlock();
    t2 = x360_alloc_ms();

    cs   = ta - t0;
    hold = t2 - ta;
    if( (LONG) cs > InterlockedCompareExchange( &x360_pool_cs_max, 0, 0 ) )
        InterlockedExchange( &x360_pool_cs_max, (LONG) cs );
    if( t2 - t0 >= 10 ) {
        unsigned long qa = x360_alloc_ms();
        unsigned long qb = x360_alloc_ms();
        q = qb - qa;
        DbgPrint( "[tls] pf cls=%u tot=%lu cs=%lu hold=%lu q=%lu",
                  (unsigned) blk->cls, t2 - t0, cs, hold, q );
    }
}
#endif /* _XBOX */
#endif /* MBEDTLS_PLATFORM_MEMORY &&
          !( defined(MBEDTLS_PLATFORM_CALLOC_MACRO) &&
             defined(MBEDTLS_PLATFORM_FREE_MACRO) ) */

#if defined(MBEDTLS_PLATFORM_HAS_NON_CONFORMING_SNPRINTF)
#include <stdarg.h>
int mbedtls_platform_win32_snprintf(char *s, size_t n, const char *fmt, ...)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    va_list argp;

    va_start(argp, fmt);
    ret = mbedtls_vsnprintf(s, n, fmt, argp);
    va_end(argp);

    return ret;
}
#endif

#if defined(MBEDTLS_PLATFORM_SNPRINTF_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_SNPRINTF)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static int platform_snprintf_uninit(char *s, size_t n,
                                    const char *format, ...)
{
    ((void) s);
    ((void) n);
    ((void) format);
    return 0;
}

#define MBEDTLS_PLATFORM_STD_SNPRINTF    platform_snprintf_uninit
#endif /* !MBEDTLS_PLATFORM_STD_SNPRINTF */

int (*mbedtls_snprintf)(char *s, size_t n,
                        const char *format,
                        ...) = MBEDTLS_PLATFORM_STD_SNPRINTF;

int mbedtls_platform_set_snprintf(int (*snprintf_func)(char *s, size_t n,
                                                       const char *format,
                                                       ...))
{
    mbedtls_snprintf = snprintf_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_SNPRINTF_ALT */

#if defined(MBEDTLS_PLATFORM_HAS_NON_CONFORMING_VSNPRINTF)
#include <stdarg.h>
int mbedtls_platform_win32_vsnprintf(char *s, size_t n, const char *fmt, va_list arg)
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    /* Avoid calling the invalid parameter handler by checking ourselves */
    if (s == NULL || n == 0 || fmt == NULL) {
        return -1;
    }

#if defined(_TRUNCATE)
    ret = vsnprintf_s(s, n, _TRUNCATE, fmt, arg);
#else
    ret = vsnprintf(s, n, fmt, arg);
    if (ret < 0 || (size_t) ret == n) {
        s[n-1] = '\0';
        ret = -1;
    }
#endif

    return ret;
}
#endif

#if defined(MBEDTLS_PLATFORM_VSNPRINTF_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_VSNPRINTF)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static int platform_vsnprintf_uninit(char *s, size_t n,
                                     const char *format, va_list arg)
{
    ((void) s);
    ((void) n);
    ((void) format);
    ((void) arg);
    return -1;
}

#define MBEDTLS_PLATFORM_STD_VSNPRINTF    platform_vsnprintf_uninit
#endif /* !MBEDTLS_PLATFORM_STD_VSNPRINTF */

int (*mbedtls_vsnprintf)(char *s, size_t n,
                         const char *format,
                         va_list arg) = MBEDTLS_PLATFORM_STD_VSNPRINTF;

int mbedtls_platform_set_vsnprintf(int (*vsnprintf_func)(char *s, size_t n,
                                                         const char *format,
                                                         va_list arg))
{
    mbedtls_vsnprintf = vsnprintf_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_VSNPRINTF_ALT */

#if defined(MBEDTLS_PLATFORM_PRINTF_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_PRINTF)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static int platform_printf_uninit(const char *format, ...)
{
    ((void) format);
    return 0;
}

#define MBEDTLS_PLATFORM_STD_PRINTF    platform_printf_uninit
#endif /* !MBEDTLS_PLATFORM_STD_PRINTF */

int (*mbedtls_printf)(const char *, ...) = MBEDTLS_PLATFORM_STD_PRINTF;

int mbedtls_platform_set_printf(int (*printf_func)(const char *, ...))
{
    mbedtls_printf = printf_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_PRINTF_ALT */

#if defined(MBEDTLS_PLATFORM_FPRINTF_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_FPRINTF)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static int platform_fprintf_uninit(FILE *stream, const char *format, ...)
{
    ((void) stream);
    ((void) format);
    return 0;
}

#define MBEDTLS_PLATFORM_STD_FPRINTF   platform_fprintf_uninit
#endif /* !MBEDTLS_PLATFORM_STD_FPRINTF */

int (*mbedtls_fprintf)(FILE *, const char *, ...) =
    MBEDTLS_PLATFORM_STD_FPRINTF;

int mbedtls_platform_set_fprintf(int (*fprintf_func)(FILE *, const char *, ...))
{
    mbedtls_fprintf = fprintf_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_FPRINTF_ALT */

#if defined(MBEDTLS_PLATFORM_EXIT_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_EXIT)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static void platform_exit_uninit(int status)
{
    ((void) status);
}

#define MBEDTLS_PLATFORM_STD_EXIT   platform_exit_uninit
#endif /* !MBEDTLS_PLATFORM_STD_EXIT */

void (*mbedtls_exit)(int status) = MBEDTLS_PLATFORM_STD_EXIT;

int mbedtls_platform_set_exit(void (*exit_func)(int status))
{
    mbedtls_exit = exit_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_EXIT_ALT */

#if defined(MBEDTLS_HAVE_TIME)

#if defined(MBEDTLS_PLATFORM_TIME_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_TIME)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static mbedtls_time_t platform_time_uninit(mbedtls_time_t *timer)
{
    ((void) timer);
    return 0;
}

#define MBEDTLS_PLATFORM_STD_TIME   platform_time_uninit
#endif /* !MBEDTLS_PLATFORM_STD_TIME */

mbedtls_time_t (*mbedtls_time)(mbedtls_time_t *timer) = MBEDTLS_PLATFORM_STD_TIME;

int mbedtls_platform_set_time(mbedtls_time_t (*time_func)(mbedtls_time_t *timer))
{
    mbedtls_time = time_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_TIME_ALT */

#endif /* MBEDTLS_HAVE_TIME */

#if defined(MBEDTLS_ENTROPY_NV_SEED)
#if !defined(MBEDTLS_PLATFORM_NO_STD_FUNCTIONS) && defined(MBEDTLS_FS_IO)
/* Default implementations for the platform independent seed functions use
 * standard libc file functions to read from and write to a pre-defined filename
 */
int mbedtls_platform_std_nv_seed_read(unsigned char *buf, size_t buf_len)
{
    FILE *file;
    size_t n;

    if ((file = fopen(MBEDTLS_PLATFORM_STD_NV_SEED_FILE, "rb")) == NULL) {
        return -1;
    }

    if ((n = fread(buf, 1, buf_len, file)) != buf_len) {
        fclose(file);
        mbedtls_platform_zeroize(buf, buf_len);
        return -1;
    }

    fclose(file);
    return (int) n;
}

int mbedtls_platform_std_nv_seed_write(unsigned char *buf, size_t buf_len)
{
    FILE *file;
    size_t n;

    if ((file = fopen(MBEDTLS_PLATFORM_STD_NV_SEED_FILE, "w")) == NULL) {
        return -1;
    }

    if ((n = fwrite(buf, 1, buf_len, file)) != buf_len) {
        fclose(file);
        return -1;
    }

    fclose(file);
    return (int) n;
}
#endif /* MBEDTLS_PLATFORM_NO_STD_FUNCTIONS */

#if defined(MBEDTLS_PLATFORM_NV_SEED_ALT)
#if !defined(MBEDTLS_PLATFORM_STD_NV_SEED_READ)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static int platform_nv_seed_read_uninit(unsigned char *buf, size_t buf_len)
{
    ((void) buf);
    ((void) buf_len);
    return -1;
}

#define MBEDTLS_PLATFORM_STD_NV_SEED_READ   platform_nv_seed_read_uninit
#endif /* !MBEDTLS_PLATFORM_STD_NV_SEED_READ */

#if !defined(MBEDTLS_PLATFORM_STD_NV_SEED_WRITE)
/*
 * Make dummy function to prevent NULL pointer dereferences
 */
static int platform_nv_seed_write_uninit(unsigned char *buf, size_t buf_len)
{
    ((void) buf);
    ((void) buf_len);
    return -1;
}

#define MBEDTLS_PLATFORM_STD_NV_SEED_WRITE   platform_nv_seed_write_uninit
#endif /* !MBEDTLS_PLATFORM_STD_NV_SEED_WRITE */

int (*mbedtls_nv_seed_read)(unsigned char *buf, size_t buf_len) =
    MBEDTLS_PLATFORM_STD_NV_SEED_READ;
int (*mbedtls_nv_seed_write)(unsigned char *buf, size_t buf_len) =
    MBEDTLS_PLATFORM_STD_NV_SEED_WRITE;

int mbedtls_platform_set_nv_seed(
    int (*nv_seed_read_func)(unsigned char *buf, size_t buf_len),
    int (*nv_seed_write_func)(unsigned char *buf, size_t buf_len))
{
    mbedtls_nv_seed_read = nv_seed_read_func;
    mbedtls_nv_seed_write = nv_seed_write_func;
    return 0;
}
#endif /* MBEDTLS_PLATFORM_NV_SEED_ALT */
#endif /* MBEDTLS_ENTROPY_NV_SEED */

#if !defined(MBEDTLS_PLATFORM_SETUP_TEARDOWN_ALT)
/*
 * Placeholder platform setup that does nothing by default
 */
int mbedtls_platform_setup(mbedtls_platform_context *ctx)
{
    (void) ctx;

    return 0;
}

/*
 * Placeholder platform teardown that does nothing by default
 */
void mbedtls_platform_teardown(mbedtls_platform_context *ctx)
{
    (void) ctx;
}
#endif /* MBEDTLS_PLATFORM_SETUP_TEARDOWN_ALT */

#endif /* MBEDTLS_PLATFORM_C */
