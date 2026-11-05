/*
 * Xbox 360 (Xenon / VMX128) H.264 DSP kernels.
 *
 * Xenon removed 22 vector *integer* multiply-add instructions (vmule*, vmulo*,
 * vmladduhm, vmsum*, ...), which is exactly what FFmpeg's libavcodec/ppc
 * H.264 code uses, so `--disable-altivec` was needed to build at all.  The
 * replacement path is the FP bridge: vcfsx -> vmaddfp -> vctsxs, which is
 * bit-exact for integer products while |a*b + c| < 2^24 (chroma 4-tap sum is
 * at most 65280).  See cbffmpeg/docs/p6_altivec_preflight.md.
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

/* vec_unpackh of a u8 is a *sign* extension, the & 0x00FF clears the high
 * halfword back to the unsigned value. */
P6_INLINE vf p6_cvt_lo(v8 v) { return vec_ctf(vec_unpackh(v), 0); }
P6_INLINE vf p6_cvt_hi(v8 v) { return vec_ctf(vec_unpackl(v), 0); }

/* (A*a + B*b + C*c + D*d + 32) >> 6 for one row, 8 output bytes in lanes 0..7. */
P6_INLINE v16 p6_mc_row(const uint8_t *src, ptrdiff_t stride,
                        vf vA, vf vB, vf vC, vf vD)
{
    const v8  mask = (v8)vec_splats((unsigned short)0x00FF);
    const vf  v32  = vec_splats(32.0f);
    const vf  vInv = vec_splats(1.0f / 64.0f);

    v16 r0  = p6_loadu(src);
    v16 r0s = vec_sld(r0, r0, 1);
    v16 r1  = p6_loadu(src + stride);
    v16 r1s = vec_sld(r1, r1, 1);

    v8 a = vec_and((v8)vec_unpackh((vs8)r0),  mask);
    v8 b = vec_and((v8)vec_unpackh((vs8)r0s), mask);
    v8 c = vec_and((v8)vec_unpackh((vs8)r1),  mask);
    v8 d = vec_and((v8)vec_unpackh((vs8)r1s), mask);

    vf lo = vec_madd(vA, p6_cvt_lo(a), v32);
    lo = vec_madd(vB, p6_cvt_lo(b), lo);
    lo = vec_madd(vC, p6_cvt_lo(c), lo);
    lo = vec_madd(vD, p6_cvt_lo(d), lo);
    v4 ilo = vec_cts(vec_mul(lo, vInv), 0);

    vf hi = vec_madd(vA, p6_cvt_hi(a), v32);
    hi = vec_madd(vB, p6_cvt_hi(b), hi);
    hi = vec_madd(vC, p6_cvt_hi(c), hi);
    hi = vec_madd(vD, p6_cvt_hi(d), hi);
    v4 ihi = vec_cts(vec_mul(hi, vInv), 0);

    v8  p16 = (v8)vec_pack(ilo, ihi);
    return (v16)vec_pack(p16, p16);
}

/* w = 4 or 8 destination pixels, avg = 0 (put) or 1 (rnd_avg with dst).
 * The weights are 0..7 and A+B+C+D == 64, so the special cases FFmpeg's C
 * reference carves out (D == 0, B + C == 0) fall out of the same formula. */
P6_INLINE void p6_mc(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                     int h, int x, int y, const int w, const int avg)
{
    const int A = (8 - x) * (8 - y);
    const int B = (    x) * (8 - y);
    const int C = (8 - x) * (    y);
    const int D = (    x) * (    y);
    const vf vA = vec_splats((float)A);
    const vf vB = vec_splats((float)B);
    const vf vC = vec_splats((float)C);
    const vf vD = vec_splats((float)D);

    for (int i = 0; i < h; i++) {
        v16 p8 = p6_mc_row(src, stride, vA, vB, vC, vD);
        if (avg)                       /* vavgub == (a + b + 1) >> 1 == rnd_avg */
            p8 = vec_avg(p8, p6_loadu(dst));
        if (w == 8) {
            uint64_t o;
            __builtin_memcpy(&o, &p8, 8);
            __builtin_memcpy(dst, &o, 8);
        } else {
            uint32_t o;
            __builtin_memcpy(&o, &p8, 4);
            __builtin_memcpy(dst, &o, 4);
        }
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

    c->put_h264_chroma_pixels_tab[0] = (h264_chroma_mc_func)ff_put_h264_chroma_mc8_x360;
    c->put_h264_chroma_pixels_tab[1] = (h264_chroma_mc_func)ff_put_h264_chroma_mc4_x360;
    c->avg_h264_chroma_pixels_tab[0] = (h264_chroma_mc_func)ff_avg_h264_chroma_mc8_x360;
    c->avg_h264_chroma_pixels_tab[1] = (h264_chroma_mc_func)ff_avg_h264_chroma_mc4_x360;

    av_log(NULL, AV_LOG_INFO, "x360 h264 chroma: VMX128 kernels on\n");
}

#endif /* XBOX && !P6_STANDALONE */
#endif /* XBOX || P6_STANDALONE */
