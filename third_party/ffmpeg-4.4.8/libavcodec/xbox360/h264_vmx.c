/*
 * Xbox 360 (Xenon / VMX128) H.264 DSP kernels.
 *
 * Xenon removed 22 vector *integer* multiply-add instructions (vmule*, vmulo*,
 * vmladduhm, vmsum*, ...), which is exactly what FFmpeg's libavcodec/ppc
 * H.264 code uses, so `--disable-altivec` was needed to build at all.  Chroma
 * MC avoids multiply altogether: the 2-D bilinear weights collapse to a
 * separable pair of 3-bit shift/add stages (see p6_chroma_interp below),
 * bit-exact for every weight.  Earlier revisions routed the four weights
 * through an FP bridge (vcfsx -> vmaddfp -> vctsxs, bit-exact while
 * |a*b + c| < 2^24); the separable integer form replaced it (ported from the
 * ffmpeg-xenon FFmpeg 9.0.2 port, commit 1eed789).  Shift counts go through
 * volatile so LLVM cannot fold the shift/add sequences back into an integer
 * multiply (see the p6_q6_16 note in h264_qpel_vmx.c).  See
 * cbffmpeg/docs/p6_altivec_preflight.md.
 *
 * This file is compiled with -maltivec -fno-vectorize -fno-slp-vectorize so
 * every vector instruction comes from the hand written vec_* below and clang
 * cannot reintroduce a dropped instruction via auto-vectorisation.
 *
 * The same file is compiled standalone by cbffmpeg/tests/p6_h264 (with
 * -DP6_STANDALONE) so the code validated against the C reference on real
 * hardware is byte for byte the code that ships.
 *
 * Copyright (c) 2026 wiliwili Xbox 360 port
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

/* Only the Xenon build (and the standalone hardware test) has a reason to
 * compile this; other targets have working integer vector maths. */
#if defined(XBOX) || defined(P6_STANDALONE)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vmx_util.h"
#include "h264_vmx.h"

/* 3-bit multiplier: k * v for runtime k = 0..7, shifts and adds only.
 * The shift counts are the volatile splats passed in from p6_mc, so the
 * sequence cannot be folded back into vmladduhm (see the file header). */
P6_INLINE v8 p6_mul_small(v8 v, int k, v8 one, v8 two)
{
    switch (k) {
    case 0:  return vec_splats((short)0);
    case 1:  return v;
    case 2:  return vec_sl(v, one);
    case 3:  return vec_add(v, vec_sl(v, one));
    case 4:  return vec_sl(v, two);
    case 5:  return vec_add(v, vec_sl(v, two));
    case 6:  return vec_add(vec_sl(v, one), vec_sl(v, two));
    default: return vec_add(vec_add(v, vec_sl(v, one)), vec_sl(v, two));
    }
}

/* Separable form of the C reference
 *   (A*s0 + B*s1 + C*s2 + D*s3 + 32) >> 6,
 * A=(8-x)*(8-y), B=x*(8-y), C=(8-x)*y, D=x*y:
 *   h0  = (8-x)*s0 + x*s1 = 8*s0 + x*(s1 - s0)
 *   h1  = (8-x)*s2 + x*s3 = 8*s2 + x*(s3 - s2)
 *   out = (8-y)*h0 + y*h1 = 8*h0 + y*(h1 - h0)
 * Two 3-bit multiplies per stage instead of four 2-D weights, and every
 * intermediate fits a signed halfword: h0/h1 <= 2040, out <= 16320 (plus
 * the 32 rounding constant still < 2^15), so the arithmetic >> 6 matches
 * the C reference bit for bit.  The rounding constant is 32 (1 << 5). */
P6_INLINE v8 p6_chroma_interp(v8 s0, v8 s1, v8 s2, v8 s3,
                              int x, int y, v8 one, v8 two)
{
    const v8 three = vec_splats((short)3);
    const v8 round = vec_splats((short)32);
    const v8 sh    = vec_splats((short)6);

    v8 h0  = vec_add(vec_sl(s0, three), p6_mul_small(vec_sub(s1, s0), x, one, two));
    v8 h1  = vec_add(vec_sl(s2, three), p6_mul_small(vec_sub(s3, s2), x, one, two));
    v8 out = vec_add(vec_sl(h0, three), p6_mul_small(vec_sub(h1, h0), y, one, two));
    return vec_sra(vec_add(out, round), sh);
}

/* w = 4 or 8 destination pixels, avg = 0 (put) or 1 (rnd_avg with dst).
 * All x/y weights share the one separable formula, including the cases the
 * C reference carves out (D == 0, B + C == 0). */
P6_INLINE void p6_mc(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                     int h, int x, int y, const int w, const int avg)
{
    /* volatile-loaded shift counts: see the file header and p6_mul_small */
    static const volatile short p6_chroma_sh1[8] __attribute__((aligned(16))) =
        { 1, 1, 1, 1, 1, 1, 1, 1 };
    static const volatile short p6_chroma_sh2[8] __attribute__((aligned(16))) =
        { 2, 2, 2, 2, 2, 2, 2, 2 };
    short t[8];
    v8 one, two;

    for (int i = 0; i < 8; i++)
        t[i] = p6_chroma_sh1[i];
    __builtin_memcpy(&one, t, 16);
    for (int i = 0; i < 8; i++)
        t[i] = p6_chroma_sh2[i];
    __builtin_memcpy(&two, t, 16);

    for (int i = 0; i < h; i++) {
        v16 r0  = p6_loadu(src);
        v16 r0s = vec_sld(r0, r0, 1);
        v16 r1  = p6_loadu(src + stride);
        v16 r1s = vec_sld(r1, r1, 1);

        v8 r = p6_chroma_interp(p6_widen_lo(r0), p6_widen_lo(r0s),
                                p6_widen_lo(r1), p6_widen_lo(r1s),
                                x, y, one, two);
        v16 p8 = (v16)vec_packsu(r, r);   /* 8 result bytes in lanes 0..7 */
        if (avg)                       /* vavgub == (a + b + 1) >> 1 == rnd_avg */
            p8 = vec_avg(p8, p6_loadu(dst));
        p6_store(dst, p8, w);
        dst += stride;
        src += stride;
    }
}

void ff_put_h264_chroma_mc8_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y)
{
    p6_mc(dst, src, stride, h, x, y, 8, 0);
}

void ff_put_h264_chroma_mc4_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y)
{
    p6_mc(dst, src, stride, h, x, y, 4, 0);
}

void ff_avg_h264_chroma_mc8_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y)
{
    p6_mc(dst, src, stride, h, x, y, 8, 1);
}

void ff_avg_h264_chroma_mc4_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y)
{
    p6_mc(dst, src, stride, h, x, y, 4, 1);
}

/* ------------------------------------------------------------------------ */
/* Runtime self check                                                        */
/* ------------------------------------------------------------------------ */

/* largest stride in the table below is 96, largest h is 8 -> 768 bytes of
 * live rows, plus 48 bytes of overread for the align-down loader. */
#define P6_ST_BUF   (8 * 96 + 64)

static int p6_st_cmp(const uint8_t *a, const uint8_t *b, ptrdiff_t stride, int h, int w)
{
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++)
            if (a[i * stride + j] != b[i * stride + j])
                return 0;
    return 1;
}

static uint32_t p6_st_rng = 0x9E3779B9u;
static uint32_t p6_st_rand(void)
{
    p6_st_rng ^= p6_st_rng << 13;
    p6_st_rng ^= p6_st_rng >> 17;
    p6_st_rng ^= p6_st_rng << 5;
    return p6_st_rng;
}

/* Compare every kernel against the C reference FFmpeg would otherwise use.
 * Runs once per decoder init; on any mismatch the caller keeps the C
 * functions, so a toolchain regression degrades to slow-but-correct. */
int ff_h264_vmx_chroma_selftest(p6_mc_func c_put8, p6_mc_func c_put4,
                                p6_mc_func c_avg8, p6_mc_func c_avg4)
{
    static const uint8_t p6_st_strides[] = { 16, 24, 32, 40, 64, 96 };
    uint8_t srcbuf[P6_ST_BUF + 32];
    uint8_t ref[P6_ST_BUF];
    uint8_t got[P6_ST_BUF];
    uint8_t dstbuf[P6_ST_BUF];

    for (int x = 0; x <= 7; x++)
        for (int y = 0; y <= 7; y++)
            for (int hi = 0; hi < 2; hi++)
                for (int wi = 0; wi < 2; wi++)
                    for (int ai = 0; ai < 2; ai++)
                        for (unsigned si = 0; si < sizeof(p6_st_strides); si++) {
                            const int h  = hi ? 8 : 4;
                            const int w  = wi ? 8 : 4;
                            const ptrdiff_t stride = p6_st_strides[si];
                            p6_mc_func kern = ai ? (wi ? ff_avg_h264_chroma_mc8_x360
                                                      : ff_avg_h264_chroma_mc4_x360)
                                                 : (wi ? ff_put_h264_chroma_mc8_x360
                                                      : ff_put_h264_chroma_mc4_x360);
                            p6_mc_func ref_fn = ai ? (wi ? c_avg8 : c_avg4)
                                                   : (wi ? c_put8 : c_put4);

                            for (size_t i = 0; i < sizeof(srcbuf); i++)
                                srcbuf[i] = (uint8_t)p6_st_rand();
                            for (size_t i = 0; i < sizeof(dstbuf); i++)
                                dstbuf[i] = (uint8_t)p6_st_rand();

                            /* unaligned src: x/y offsets already shift src,
                             * but cover a rotated alignment too */
                            const uint8_t *s = srcbuf + (si & 7);
                            __builtin_memcpy(ref, dstbuf, sizeof(ref));
                            __builtin_memcpy(got, dstbuf, sizeof(got));

                            ref_fn(ref, (uint8_t *)s, stride, h, x, y);
                            kern  (got, (uint8_t *)s, stride, h, x, y);

                            if (!p6_st_cmp(ref, got, stride, h, w))
                                return 1;
                        }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* FFmpeg glue (not compiled by the standalone hardware test)                */
/* ------------------------------------------------------------------------ */

#if defined(XBOX) && !defined(P6_STANDALONE)

#include "libavcodec/h264chroma.h"
#include "libavutil/log.h"

extern int xbox360_vmx128_enabled;

/* Per-kernel registration mask for single-kernel A/B benchmarks:
 * bit 1 put 8-wide, bit 2 put 4-wide, bit 4 avg 8-wide, bit 8 avg 4-wide.
 * Default 15 registers everything (see X360_CHROMA_MASK in
 * scripts/ffmpeg_x360_configure_local.sh). */
#ifndef X360_CHROMA_MASK
#define X360_CHROMA_MASK 15
#endif

void ff_h264chroma_init_x360(H264ChromaContext *c, int bit_depth)
{
    static int selftest = -1;

    if (bit_depth != 8 || !xbox360_vmx128_enabled)
        return;

    if (selftest < 0)
        selftest = ff_h264_vmx_chroma_selftest(
            (p6_mc_func)c->put_h264_chroma_pixels_tab[0],
            (p6_mc_func)c->put_h264_chroma_pixels_tab[1],
            (p6_mc_func)c->avg_h264_chroma_pixels_tab[0],
            (p6_mc_func)c->avg_h264_chroma_pixels_tab[1]);

    if (selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 chroma: VMX128 self check FAILED, using C\n");
        return;
    }

#if X360_CHROMA_MASK & 1
    c->put_h264_chroma_pixels_tab[0] = (h264_chroma_mc_func)ff_put_h264_chroma_mc8_x360;
#endif
#if X360_CHROMA_MASK & 2
    c->put_h264_chroma_pixels_tab[1] = (h264_chroma_mc_func)ff_put_h264_chroma_mc4_x360;
#endif
#if X360_CHROMA_MASK & 4
    c->avg_h264_chroma_pixels_tab[0] = (h264_chroma_mc_func)ff_avg_h264_chroma_mc8_x360;
#endif
#if X360_CHROMA_MASK & 8
    c->avg_h264_chroma_pixels_tab[1] = (h264_chroma_mc_func)ff_avg_h264_chroma_mc4_x360;
#endif

    av_log(NULL, AV_LOG_INFO, "x360 h264 chroma: VMX128 kernels on (mask %d)\n",
           X360_CHROMA_MASK);
}

#endif /* XBOX && !P6_STANDALONE */
#endif /* XBOX || P6_STANDALONE */
