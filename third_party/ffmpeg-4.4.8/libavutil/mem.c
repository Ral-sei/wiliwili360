/*
 * default memory allocator for libavutil
 * Copyright (c) 2002 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * default memory allocator for libavutil
 */

#define _XOPEN_SOURCE 600

#include "config.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if HAVE_MALLOC_H
#include <malloc.h>
#endif

#include "avassert.h"
#include "avutil.h"
#include "common.h"
#include "dynarray.h"
#include "intreadwrite.h"
#include "mem.h"

#ifdef MALLOC_PREFIX

#define malloc         AV_JOIN(MALLOC_PREFIX, malloc)
#define memalign       AV_JOIN(MALLOC_PREFIX, memalign)
#define posix_memalign AV_JOIN(MALLOC_PREFIX, posix_memalign)
#define realloc        AV_JOIN(MALLOC_PREFIX, realloc)
#define free           AV_JOIN(MALLOC_PREFIX, free)

void *malloc(size_t size);
void *memalign(size_t align, size_t size);
int   posix_memalign(void **ptr, size_t align, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

#endif /* MALLOC_PREFIX */

#include "mem_internal.h"

#if defined(_XBOX)
/* [heap-slow] P5: 第 5 轮堆锁争用假说的 FFmpeg 侧探针。
 *
 * round-4 在 wiliwili 侧实测：焦点切换冻结 = 构造侧（map 插入，分配密集）
 * 与析构侧（成员析构+delete，释放密集）对称变慢 120~660ms，而零分配段恒 0ms
 * —— 指向 CRT 堆锁被长期占用。直播软解时 FFmpeg 是最可疑的持锁者（每帧
 * 大块 av_malloc/av_free）。这里给 av_* 三大漏斗加 5ms 阈值点名：单次长
 * 操作按 bytes 打 [heap-slow]，可直接区分"持锁者"（大块释放/分配）与
 * "受害者"（小块反复等待）。配合 wiliwili 侧全局 operator new/delete 探针
 * （xbox360_perf.cpp）两侧对账。
 * 只走 DbgPrint：不进 Logger（fmt 会再分配，且主线程日志通道本身可疑）。
 * 限流：令牌桶 64 枚、每 5s 回填（round-5b：一次性预算会被启动风暴耗尽，
 * 12s 析构风暴期整段失明 —— 持锁者恰好在预算耗尽后才现身）；≥50ms 的
 * 大鱼绕过令牌必放行，风暴刷屏由回填速率（≈13 条/s）兜底。 */
#include "libavutil/time.h"

extern void DbgPrint(const char *fmt, ...);
#if !defined(X360_RUNTIME_PROBES) || !X360_RUNTIME_PROBES
/* Xbox 360 诊断探针总开关（默认 OFF，wiliwili 侧同名）：关闭时 [heap-slow]
   行不再输出。注意本文件由 ffmpeg 独立构建，改动需重建 ffmpeg 静态库
   （build/ffmpeg-x360-tls）才生效。 */
#define DbgPrint(...) ((void)0)
#endif

static volatile int x360_heap_tok = 64;
static int64_t x360_heap_tok_last = 0;

static void x360_heap_note(const char *op, size_t n, int64_t t0)
{
    int64_t us = av_gettime_relative() - t0;
    if (us < 5000)
        return;
    if (us < 50000) {
        int64_t now = av_gettime_relative();
        if (now - x360_heap_tok_last >= 5000000) {
            x360_heap_tok_last = now;
            __sync_lock_test_and_set(&x360_heap_tok, 64);
        }
        if (__sync_fetch_and_add(&x360_heap_tok, 0) <= 0)
            return;
        __sync_fetch_and_sub(&x360_heap_tok, 1);
    }
    if (n)
        DbgPrint("[heap-slow] %s %u B %d ms", op, (unsigned)n, (int)(us / 1000));
    else
        DbgPrint("[heap-slow] %s %d ms", op, (int)(us / 1000));
}

#define X360_HEAP_T0    int64_t x360_t0_ = av_gettime_relative()
#define X360_HEAP_NOTE(op, n) x360_heap_note(op, n, x360_t0_)
#else
#define X360_HEAP_T0    do { } while (0)
#define X360_HEAP_NOTE(op, n) do { } while (0)
#endif

#define ALIGN (HAVE_AVX512 ? 64 : (HAVE_AVX ? 32 : 16))

/* NOTE: if you want to override these functions with your own
 * implementations (not recommended) you have to link libav* as
 * dynamic libraries and remove -Wl,-Bsymbolic from the linker flags.
 * Note that this will cost performance. */

static size_t max_alloc_size= INT_MAX;

void av_max_alloc(size_t max){
    max_alloc_size = max;
}

void *av_malloc(size_t size)
{
    void *ptr = NULL;
    X360_HEAP_T0;

    if (size > max_alloc_size)
        return NULL;

#if HAVE_POSIX_MEMALIGN
    if (size) //OS X on SDK 10.6 has a broken posix_memalign implementation
    if (posix_memalign(&ptr, ALIGN, size))
        ptr = NULL;
#elif HAVE_ALIGNED_MALLOC
    ptr = _aligned_malloc(size, ALIGN);
#elif HAVE_MEMALIGN
#ifndef __DJGPP__
    ptr = memalign(ALIGN, size);
#else
    ptr = memalign(size, ALIGN);
#endif
    /* Why 64?
     * Indeed, we should align it:
     *   on  4 for 386
     *   on 16 for 486
     *   on 32 for 586, PPro - K6-III
     *   on 64 for K7 (maybe for P3 too).
     * Because L1 and L2 caches are aligned on those values.
     * But I don't want to code such logic here!
     */
    /* Why 32?
     * For AVX ASM. SSE / NEON needs only 16.
     * Why not larger? Because I did not see a difference in benchmarks ...
     */
    /* benchmarks with P3
     * memalign(64) + 1          3071, 3051, 3032
     * memalign(64) + 2          3051, 3032, 3041
     * memalign(64) + 4          2911, 2896, 2915
     * memalign(64) + 8          2545, 2554, 2550
     * memalign(64) + 16         2543, 2572, 2563
     * memalign(64) + 32         2546, 2545, 2571
     * memalign(64) + 64         2570, 2533, 2558
     *
     * BTW, malloc seems to do 8-byte alignment by default here.
     */
#else
    ptr = malloc(size);
#endif
    if(!ptr && !size) {
        size = 1;
        ptr= av_malloc(1);
    }
#if CONFIG_MEMORY_POISONING
    if (ptr)
        memset(ptr, FF_MEMORY_POISON, size);
#endif
    X360_HEAP_NOTE("av_malloc", size);
    return ptr;
}

void *av_realloc(void *ptr, size_t size)
{
    X360_HEAP_T0;
    if (size > max_alloc_size)
        return NULL;

#if HAVE_ALIGNED_MALLOC
    {
        void *r = _aligned_realloc(ptr, size + !size, ALIGN);
        X360_HEAP_NOTE("av_realloc", size);
        return r;
    }
#else
    {
        void *r = realloc(ptr, size + !size);
        X360_HEAP_NOTE("av_realloc", size);
        return r;
    }
#endif
}

void *av_realloc_f(void *ptr, size_t nelem, size_t elsize)
{
    size_t size;
    void *r;

    if (av_size_mult(elsize, nelem, &size)) {
        av_free(ptr);
        return NULL;
    }
    r = av_realloc(ptr, size);
    if (!r)
        av_free(ptr);
    return r;
}

int av_reallocp(void *ptr, size_t size)
{
    void *val;

    if (!size) {
        av_freep(ptr);
        return 0;
    }

    memcpy(&val, ptr, sizeof(val));
    val = av_realloc(val, size);

    if (!val) {
        av_freep(ptr);
        return AVERROR(ENOMEM);
    }

    memcpy(ptr, &val, sizeof(val));
    return 0;
}

void *av_malloc_array(size_t nmemb, size_t size)
{
    size_t result;
    if (av_size_mult(nmemb, size, &result) < 0)
        return NULL;
    return av_malloc(result);
}

void *av_mallocz_array(size_t nmemb, size_t size)
{
    size_t result;
    if (av_size_mult(nmemb, size, &result) < 0)
        return NULL;
    return av_mallocz(result);
}

void *av_realloc_array(void *ptr, size_t nmemb, size_t size)
{
    size_t result;
    if (av_size_mult(nmemb, size, &result) < 0)
        return NULL;
    return av_realloc(ptr, result);
}

int av_reallocp_array(void *ptr, size_t nmemb, size_t size)
{
    void *val;

    memcpy(&val, ptr, sizeof(val));
    val = av_realloc_f(val, nmemb, size);
    memcpy(ptr, &val, sizeof(val));
    if (!val && nmemb && size)
        return AVERROR(ENOMEM);

    return 0;
}

void av_free(void *ptr)
{
    X360_HEAP_T0;
#if HAVE_ALIGNED_MALLOC
    _aligned_free(ptr);
#else
    free(ptr);
#endif
    X360_HEAP_NOTE("av_free", 0);
}

void av_freep(void *arg)
{
    void *val;

    memcpy(&val, arg, sizeof(val));
    memcpy(arg, &(void *){ NULL }, sizeof(val));
    av_free(val);
}

void *av_mallocz(size_t size)
{
    void *ptr = av_malloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void *av_calloc(size_t nmemb, size_t size)
{
    size_t result;
    if (av_size_mult(nmemb, size, &result) < 0)
        return NULL;
    return av_mallocz(result);
}

char *av_strdup(const char *s)
{
    char *ptr = NULL;
    if (s) {
        size_t len = strlen(s) + 1;
        ptr = av_realloc(NULL, len);
        if (ptr)
            memcpy(ptr, s, len);
    }
    return ptr;
}

char *av_strndup(const char *s, size_t len)
{
    char *ret = NULL, *end;

    if (!s)
        return NULL;

    end = memchr(s, 0, len);
    if (end)
        len = end - s;

    ret = av_realloc(NULL, len + 1);
    if (!ret)
        return NULL;

    memcpy(ret, s, len);
    ret[len] = 0;
    return ret;
}

void *av_memdup(const void *p, size_t size)
{
    void *ptr = NULL;
    if (p) {
        ptr = av_malloc(size);
        if (ptr)
            memcpy(ptr, p, size);
    }
    return ptr;
}

int av_dynarray_add_nofree(void *tab_ptr, int *nb_ptr, void *elem)
{
    void **tab;
    memcpy(&tab, tab_ptr, sizeof(tab));

    FF_DYNARRAY_ADD(INT_MAX, sizeof(*tab), tab, *nb_ptr, {
        tab[*nb_ptr] = elem;
        memcpy(tab_ptr, &tab, sizeof(tab));
    }, {
        return AVERROR(ENOMEM);
    });
    return 0;
}

void av_dynarray_add(void *tab_ptr, int *nb_ptr, void *elem)
{
    void **tab;
    memcpy(&tab, tab_ptr, sizeof(tab));

    FF_DYNARRAY_ADD(INT_MAX, sizeof(*tab), tab, *nb_ptr, {
        tab[*nb_ptr] = elem;
        memcpy(tab_ptr, &tab, sizeof(tab));
    }, {
        *nb_ptr = 0;
        av_freep(tab_ptr);
    });
}

void *av_dynarray2_add(void **tab_ptr, int *nb_ptr, size_t elem_size,
                       const uint8_t *elem_data)
{
    uint8_t *tab_elem_data = NULL;

    FF_DYNARRAY_ADD(INT_MAX, elem_size, *tab_ptr, *nb_ptr, {
        tab_elem_data = (uint8_t *)*tab_ptr + (*nb_ptr) * elem_size;
        if (elem_data)
            memcpy(tab_elem_data, elem_data, elem_size);
        else if (CONFIG_MEMORY_POISONING)
            memset(tab_elem_data, FF_MEMORY_POISON, elem_size);
    }, {
        av_freep(tab_ptr);
        *nb_ptr = 0;
    });
    return tab_elem_data;
}

static void fill16(uint8_t *dst, int len)
{
    uint32_t v = AV_RN16(dst - 2);

    v |= v << 16;

    while (len >= 4) {
        AV_WN32(dst, v);
        dst += 4;
        len -= 4;
    }

    while (len--) {
        *dst = dst[-2];
        dst++;
    }
}

static void fill24(uint8_t *dst, int len)
{
#if HAVE_BIGENDIAN
    uint32_t v = AV_RB24(dst - 3);
    uint32_t a = v << 8  | v >> 16;
    uint32_t b = v << 16 | v >> 8;
    uint32_t c = v << 24 | v;
#else
    uint32_t v = AV_RL24(dst - 3);
    uint32_t a = v       | v << 24;
    uint32_t b = v >> 8  | v << 16;
    uint32_t c = v >> 16 | v << 8;
#endif

    while (len >= 12) {
        AV_WN32(dst,     a);
        AV_WN32(dst + 4, b);
        AV_WN32(dst + 8, c);
        dst += 12;
        len -= 12;
    }

    if (len >= 4) {
        AV_WN32(dst, a);
        dst += 4;
        len -= 4;
    }

    if (len >= 4) {
        AV_WN32(dst, b);
        dst += 4;
        len -= 4;
    }

    while (len--) {
        *dst = dst[-3];
        dst++;
    }
}

static void fill32(uint8_t *dst, int len)
{
    uint32_t v = AV_RN32(dst - 4);

#if HAVE_FAST_64BIT
    uint64_t v2= v + ((uint64_t)v<<32);
    while (len >= 32) {
        AV_WN64(dst   , v2);
        AV_WN64(dst+ 8, v2);
        AV_WN64(dst+16, v2);
        AV_WN64(dst+24, v2);
        dst += 32;
        len -= 32;
    }
#endif

    while (len >= 4) {
        AV_WN32(dst, v);
        dst += 4;
        len -= 4;
    }

    while (len--) {
        *dst = dst[-4];
        dst++;
    }
}

void av_memcpy_backptr(uint8_t *dst, int back, int cnt)
{
    const uint8_t *src = &dst[-back];
    if (!back)
        return;

    if (back == 1) {
        memset(dst, *src, cnt);
    } else if (back == 2) {
        fill16(dst, cnt);
    } else if (back == 3) {
        fill24(dst, cnt);
    } else if (back == 4) {
        fill32(dst, cnt);
    } else {
        if (cnt >= 16) {
            int blocklen = back;
            while (cnt > blocklen) {
                memcpy(dst, src, blocklen);
                dst       += blocklen;
                cnt       -= blocklen;
                blocklen <<= 1;
            }
            memcpy(dst, src, cnt);
            return;
        }
        if (cnt >= 8) {
            AV_COPY32U(dst,     src);
            AV_COPY32U(dst + 4, src + 4);
            src += 8;
            dst += 8;
            cnt -= 8;
        }
        if (cnt >= 4) {
            AV_COPY32U(dst, src);
            src += 4;
            dst += 4;
            cnt -= 4;
        }
        if (cnt >= 2) {
            AV_COPY16U(dst, src);
            src += 2;
            dst += 2;
            cnt -= 2;
        }
        if (cnt)
            *dst = *src;
    }
}

void *av_fast_realloc(void *ptr, unsigned int *size, size_t min_size)
{
    if (min_size <= *size)
        return ptr;

    if (min_size > max_alloc_size) {
        *size = 0;
        return NULL;
    }

    min_size = FFMIN(max_alloc_size, FFMAX(min_size + min_size / 16 + 32, min_size));

    ptr = av_realloc(ptr, min_size);
    /* we could set this to the unmodified min_size but this is safer
     * if the user lost the ptr and uses NULL now
     */
    if (!ptr)
        min_size = 0;

    *size = min_size;

    return ptr;
}

void av_fast_malloc(void *ptr, unsigned int *size, size_t min_size)
{
    ff_fast_malloc(ptr, size, min_size, 0);
}

void av_fast_mallocz(void *ptr, unsigned int *size, size_t min_size)
{
    ff_fast_malloc(ptr, size, min_size, 1);
}
