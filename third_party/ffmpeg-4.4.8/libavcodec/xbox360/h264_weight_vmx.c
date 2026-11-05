/*
 * Xbox 360 (Xenon / VMX128) H.264 weighted prediction.
 *
 * weight / weightd / weights are runtime scalars, so unlike the quarter-pel
 * filter there is no shift-add form and the FP bridge is the only option:
 * |v*weight + offset| <= 255*128 + 16256 < 2^24, so vcfsx -> vmaddfp -> vctsxs
 * is exact.  The final >> log2_denom is done as an exact multiply by 2^-k plus
 * truncation: for non-negative values that is identical to an arithmetic
 * shift, and for negative values both routes end at clip() == 0.
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

#if defined(XBOX) || defined(P6_STANDALONE)

#include "vmx_util.h"
#include "h264_weight_vmx.h"

P6_INLINE vf p6_vcvt_lo(v8 v) { return vec_ctf(vec_unpackh(v), 0); }
P6_INLINE vf p6_vcvt_hi(v8 v) { return vec_ctf(vec_unpackl(v), 0); }

/* One row of width w, w in {4, 8, 16}: each quarter of the row is one v4. */
P6_INLINE void p6_weight_row(uint8_t *block, vf vw, vf voff, vf vinv, int w)
{
    v16 b  = p6_loadu(block);
    v8  lo = p6_widen_lo(b), hi = p6_widen_hi(b);

    v4 i0 = vec_cts(vec_mul(vec_madd(p6_vcvt_lo(lo), vw, voff), vinv), 0);
    v4 i1 = vec_cts(vec_mul(vec_madd(p6_vcvt_hi(lo), vw, voff), vinv), 0);
    v4 i2 = vec_cts(vec_mul(vec_madd(p6_vcvt_lo(hi), vw, voff), vinv), 0);
    v4 i3 = vec_cts(vec_mul(vec_madd(p6_vcvt_hi(hi), vw, voff), vinv), 0);

    v8 s0 = (v8)vec_packsu(i0, i1);
    v8 s1 = (v8)vec_packsu(i2, i3);
    p6_store(block, (v16)vec_packsu(s0, s1), w);
}

P6_INLINE void p6_biweight_row(uint8_t *dst, const uint8_t *src,
                               vf vwd, vf vws, vf voff, vf vinv, int w)
{
    v16 s  = p6_loadu(src);
    v16 d  = p6_loadu(dst);
    v8  sl = p6_widen_lo(s), sh = p6_widen_hi(s);
    v8  dl = p6_widen_lo(d), dh = p6_widen_hi(d);

    vf g0 = vec_madd(p6_vcvt_lo(dl), vwd, vec_madd(p6_vcvt_lo(sl), vws, voff));
    vf g1 = vec_madd(p6_vcvt_hi(dl), vwd, vec_madd(p6_vcvt_hi(sl), vws, voff));
    vf g2 = vec_madd(p6_vcvt_lo(dh), vwd, vec_madd(p6_vcvt_lo(sh), vws, voff));
    vf g3 = vec_madd(p6_vcvt_hi(dh), vwd, vec_madd(p6_vcvt_hi(sh), vws, voff));

    v4 i0 = vec_cts(vec_mul(g0, vinv), 0);
    v4 i1 = vec_cts(vec_mul(g1, vinv), 0);
    v4 i2 = vec_cts(vec_mul(g2, vinv), 0);
    v4 i3 = vec_cts(vec_mul(g3, vinv), 0);

    v8 o0 = (v8)vec_packsu(i0, i1);
    v8 o1 = (v8)vec_packsu(i2, i3);
    p6_store(dst, (v16)vec_packsu(o0, o1), w);
}

#define P6_WEIGHT_DEF(N)                                                      \
void ff_weight_h264_pixels##N##_x360(uint8_t *block, ptrdiff_t stride,        \
                                     int height, int log2_denom,              \
                                     int weight, int offset)                  \
{                                                                             \
    int off = (unsigned)offset << log2_denom;                                 \
    if (log2_denom)                                                           \
        off += 1 << (log2_denom - 1);                                         \
    const vf vw   = vec_splats((float)weight);                                \
    const vf voff = vec_splats((float)off);                                   \
    const vf vinv = vec_splats(1.0f / (float)(1 << log2_denom));              \
    for (int y = 0; y < height; y++, block += stride)                         \
        p6_weight_row(block, vw, voff, vinv, N);                              \
}                                                                             \
void ff_biweight_h264_pixels##N##_x360(uint8_t *dst, uint8_t *src,            \
                                       ptrdiff_t stride, int height,          \
                                       int log2_denom, int weightd,           \
                                       int weights, int offset)               \
{                                                                             \
    int off = (unsigned)(((offset + 1) | 1) << log2_denom);                   \
    const vf vwd  = vec_splats((float)weightd);                               \
    const vf vws  = vec_splats((float)weights);                               \
    const vf voff = vec_splats((float)off);                                   \
    const vf vinv = vec_splats(0.5f / (float)(1 << log2_denom));              \
    for (int y = 0; y < height; y++, dst += stride, src += stride)            \
        p6_biweight_row(dst, src, vwd, vws, voff, vinv, N);                   \
}

P6_WEIGHT_DEF(16)
P6_WEIGHT_DEF(8)
P6_WEIGHT_DEF(4)

#if defined(XBOX) && !defined(P6_STANDALONE)

#include "libavcodec/h264dsp.h"
#include "libavutil/log.h"

static void p6_weight_row_dispatch(int w, uint8_t *block, ptrdiff_t stride,
                                   int height, int log2_denom, int weight, int offset)
{
    if (w == 16)
        ff_weight_h264_pixels16_x360(block, stride, height, log2_denom, weight, offset);
    else if (w == 8)
        ff_weight_h264_pixels8_x360(block, stride, height, log2_denom, weight, offset);
    else
        ff_weight_h264_pixels4_x360(block, stride, height, log2_denom, weight, offset);
}

static void p6_biweight_row_dispatch(int w, uint8_t *dst, uint8_t *src,
                                     ptrdiff_t stride, int height, int log2_denom,
                                     int weightd, int weights, int offset)
{
    if (w == 16)
        ff_biweight_h264_pixels16_x360(dst, src, stride, height, log2_denom, weightd, weights, offset);
    else if (w == 8)
        ff_biweight_h264_pixels8_x360(dst, src, stride, height, log2_denom, weightd, weights, offset);
    else
        ff_biweight_h264_pixels4_x360(dst, src, stride, height, log2_denom, weightd, weights, offset);
}

/* Same contract as the chroma/qpel kernels: verify against the C functions
 * FFmpeg just installed, and keep them if anything disagrees. */
static int p6_weight_selftest(const H264DSPContext *h)
{
    static const int widths[3]  = { 16, 8, 4 };
    static const int params[][4] = {
        /* log2_denom, weight, offset, height */
        { 0, 32, 0, 16 }, { 5, 32, 0, 8 }, { 7, 128, 0, 4 },
        { 5, -32, 0, 16 }, { 2, 1, 3, 8 }, { 6, 64, -128, 4 },
        { 3, 100, 7, 16 }, { 1, 255, -1, 8 },
    };
    uint8_t ref[16 * 16], got[16 * 16], src[16 * 16];
    uint32_t rng = 0x2545F491u;

    for (int i = 0; i < 3; i++) {
        const int w = widths[i];
        for (unsigned p = 0; p < sizeof(params) / sizeof(params[0]); p++) {
            const int log2 = params[p][0], wt = params[p][1];
            const int off = params[p][2], hgt = params[p][3];

            for (int k = 0; k < 16 * 16; k++) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                src[k] = (uint8_t)rng;
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                ref[k] = got[k] = (uint8_t)rng;
            }

            h->weight_h264_pixels_tab[i](ref, w, hgt, log2, wt, off);
            p6_weight_row_dispatch(w, got, w, hgt, log2, wt, off);
            for (int k = 0; k < hgt * w; k++)
                if (ref[k] != got[k])
                    return 1;

            for (int k = 0; k < 16 * 16; k++)
                ref[k] = got[k] = src[k];
            h->biweight_h264_pixels_tab[i](ref, src, w, hgt, log2, wt, -wt, off);
            p6_biweight_row_dispatch(w, got, src, w, hgt, log2, wt, -wt, off);
            for (int k = 0; k < hgt * w; k++)
                if (ref[k] != got[k])
                    return 2;
        }
    }
    return 0;
}

extern int xbox360_vmx128_enabled;

void ff_h264dsp_weight_init_x360(H264DSPContext *h, int bit_depth)
{
    static int selftest = -1;

    /* Byte kernels only: the selftest buffers and p6_weight_row operate on
     * uint8_t pixels, so installing these into a 9/10-bit context would both
     * corrupt 16-bit weighted prediction and overflow the selftest arrays.
     * Same contract as the chroma/qpel/idct/deblock inits. */
    if (bit_depth != 8 || !xbox360_vmx128_enabled)
        return;

    if (selftest < 0)
        selftest = p6_weight_selftest(h);

    if (selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 weighted pred: self check FAILED (%d), using C\n",
               selftest);
        return;
    }

    h->weight_h264_pixels_tab[0] = ff_weight_h264_pixels16_x360;
    h->weight_h264_pixels_tab[1] = ff_weight_h264_pixels8_x360;
    h->weight_h264_pixels_tab[2] = ff_weight_h264_pixels4_x360;
    h->biweight_h264_pixels_tab[0] = ff_biweight_h264_pixels16_x360;
    h->biweight_h264_pixels_tab[1] = ff_biweight_h264_pixels8_x360;
    h->biweight_h264_pixels_tab[2] = ff_biweight_h264_pixels4_x360;

    av_log(NULL, AV_LOG_INFO, "x360 h264 weighted pred: VMX128 kernels on\n");
}

#endif /* XBOX && !P6_STANDALONE */

#endif /* XBOX || P6_STANDALONE */
