/*
 * Xbox 360 (Xenon / VMX128) H.264 quarter-pel motion compensation.
 *
 * Unlike the chroma filter (arbitrary 0..64 weights, see h264_vmx.c) the
 * 6-tap quarter-pel filter has fixed coefficients 20/-5/1, and
 *
 *     20*x = (x << 4) + (x << 2),   5*x = (x << 2) + x
 *
 * so the whole filter needs no multiply at all - which matters because Xenon
 * dropped the vector integer multiply-add family.  The horizontal sum fits in
 * int16 (|sum| <= 52*255 = 13260), so 8 pixels are filtered per vector with
 * 16-bit lanes; the two-pass (hv) variant needs int32 for the second pass
 * (|sum| <= 52*13260 = 689520).
 *
 * Bit-exactness against FFmpeg's C reference is asserted by the standalone
 * harness in cbffmpeg/tests/p6_h264 before the tables are installed, and again
 * at runtime by ff_h264_vmx_qpel_selftest().
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
#include "h264_qpel_vmx.h"

/* ------------------------------------------------------------------------ */
/* Filter core: 20*(a2+a3) - 5*(a1+a4) + (a0+a5)                             */
/* ------------------------------------------------------------------------ */

/* Xenon dropped the vector integer multiply family, and *LLVM's IR canonical
 * form* is the problem: (x << 4) + (x << 2) is folded to x * 20 by InstCombine
 * no matter how it is spelled, and the PPC backend then selects vmladduhm /
 * vmulouh / vmsumuhm - none of which exist on Xenon.  (Observed on hardware:
 * illegal instruction c000001d at a vmladduhm in this very filter.)  Reading
 * the shift counts through a volatile keeps the value unknown at compile time
 * so the fold cannot happen; callers load them once per kernel entry, which
 * makes the cost a handful of loads per block. */
static const volatile short p6_sh4_16[8] __attribute__((aligned(16))) = {4,4,4,4,4,4,4,4};
static const volatile short p6_sh2_16[8] __attribute__((aligned(16))) = {2,2,2,2,2,2,2,2};
static const volatile int   p6_sh4_32[4] __attribute__((aligned(16))) = {4,4,4,4};
static const volatile int   p6_sh2_32[4] __attribute__((aligned(16))) = {2,2,2,2};

P6_INLINE v8 p6_load_sh16(const volatile short *src)
{
    short t[8];
    for (int i = 0; i < 8; i++)
        t[i] = src[i];
    v8 r;
    __builtin_memcpy(&r, t, 16);
    return r;
}

P6_INLINE v4 p6_load_sh32(const volatile int *src)
{
    int t[4];
    for (int i = 0; i < 4; i++)
        t[i] = src[i];
    v4 r;
    __builtin_memcpy(&r, t, 16);
    return r;
}

P6_INLINE v8 p6_q6_16(v8 a0, v8 a1, v8 a2, v8 a3, v8 a4, v8 a5, v8 s4, v8 s2)
{
    v8 p23 = vec_add(a2, a3);
    v8 p14 = vec_add(a1, a4);
    v8 p05 = vec_add(a0, a5);
    v8 r = vec_add(vec_sl(p23, s4), vec_sl(p23, s2));
    r = vec_sub(r, vec_sl(p14, s2));
    r = vec_sub(r, p14);
    return vec_add(r, p05);
}

P6_INLINE v4 p6_q6_32(v4 a0, v4 a1, v4 a2, v4 a3, v4 a4, v4 a5, v4 s4, v4 s2)
{
    v4 p23 = vec_add(a2, a3);
    v4 p14 = vec_add(a1, a4);
    v4 p05 = vec_add(a0, a5);
    v4 r = vec_add(vec_sl(p23, s4), vec_sl(p23, s2));
    r = vec_sub(r, vec_sl(p14, s2));
    r = vec_sub(r, p14);
    return vec_add(r, p05);
}

/* (x + 16) >> 5, clipped to 0..255; 8 usable bytes in lanes 0..7. */
P6_INLINE v16 p6_fin5(v8 r)
{
    const v8 rnd = vec_splats((short)16);
    const v8 sh  = vec_splats((short)5);
    return (v16)vec_packsu(vec_sra(vec_add(r, rnd), sh), vec_sra(vec_add(r, rnd), sh));
}

/* (x + 512) >> 10, clipped to 0..255; 8 usable bytes in lanes 0..7. */
P6_INLINE v16 p6_fin10(v4 lo, v4 hi)
{
    const v4 rnd = vec_splats(512);
    const v4 sh  = vec_splats(10);
    v8 p = (v8)vec_packsu(vec_sra(vec_add(lo, rnd), sh),
                          vec_sra(vec_add(hi, rnd), sh));
    return (v16)vec_packsu(p, p);
}

/* ------------------------------------------------------------------------ */
/* One-pass kernels (8 pixels per vector; the 16-wide entry points are the   */
/* same four 8x8 calls FFmpeg's C uses)                                      */
/* ------------------------------------------------------------------------ */

/* Horizontal 6-tap.  rows x cols block; src points at its top-left, the taps
 * reach src[-2] .. src[cols+3]. */
P6_INLINE void p6_h_lowpass(uint8_t *dst, const uint8_t *src,
                            ptrdiff_t dstride, ptrdiff_t sstride,
                            int rows, int cols, const int avg)
{
    const v8 s4 = p6_load_sh16(p6_sh4_16);
    const v8 s2 = p6_load_sh16(p6_sh2_16);

    for (int i = 0; i < rows; i++) {
        v16 L  = p6_loadu(src - 2);
        v8  r  = p6_q6_16(p6_widen_lo(L),
                          p6_widen_lo(vec_sld(L, L, 1)),
                          p6_widen_lo(vec_sld(L, L, 2)),
                          p6_widen_lo(vec_sld(L, L, 3)),
                          p6_widen_lo(vec_sld(L, L, 4)),
                          p6_widen_lo(vec_sld(L, L, 5)), s4, s2);
        v16 o = p6_fin5(r);
        if (avg)                       /* op_avg: (dst + x + 1) >> 1 */
            o = vec_avg(o, p6_loadu(dst));
        p6_store(dst, o, cols);
        dst += dstride;
        src += sstride;
    }
}

/* Vertical 6-tap over an 8-bit source.  rows output rows, cols columns. */
P6_INLINE void p6_v_lowpass(uint8_t *dst, const uint8_t *src,
                            ptrdiff_t dstride, ptrdiff_t sstride,
                            int rows, int cols, const int avg)
{
    const v8 s4 = p6_load_sh16(p6_sh4_16);
    const v8 s2 = p6_load_sh16(p6_sh2_16);
    v8 R[16];
    for (int k = 0; k < rows + 5; k++)
        R[k] = p6_widen_lo(p6_loadu(src + (k - 2) * sstride));

    for (int i = 0; i < rows; i++) {
        v16 o = p6_fin5(p6_q6_16(R[i], R[i + 1], R[i + 2],
                                 R[i + 3], R[i + 4], R[i + 5], s4, s2));
        if (avg)
            o = vec_avg(o, p6_loadu(dst));
        p6_store(dst, o, cols);
        dst += dstride;
    }
}

/* Two-pass.  tmp holds the raw horizontal sums (rows+5 rows x cols int16). */
P6_INLINE void p6_hv_lowpass(uint8_t *dst, int16_t *tmp, const uint8_t *src,
                             ptrdiff_t dstride, ptrdiff_t tmpstride,
                             ptrdiff_t sstride, int rows, int cols, const int avg)
{
    const v8  s4  = p6_load_sh16(p6_sh4_16);
    const v8  s2  = p6_load_sh16(p6_sh2_16);
    const v4 s4w = p6_load_sh32(p6_sh4_32);
    const v4 s2w = p6_load_sh32(p6_sh2_32);

    for (int k = 0; k < rows + 5; k++) {
        /* -2: the 6-tap reaches two pixels to the left, so lane j must hold
         * src[j - 2] once vec_sld() has shifted by k */
        v16 L  = p6_loadu(src + (k - 2) * sstride - 2);
        v8  r  = p6_q6_16(p6_widen_lo(L),
                          p6_widen_lo(vec_sld(L, L, 1)),
                          p6_widen_lo(vec_sld(L, L, 2)),
                          p6_widen_lo(vec_sld(L, L, 3)),
                          p6_widen_lo(vec_sld(L, L, 4)),
                          p6_widen_lo(vec_sld(L, L, 5)), s4, s2);
        /* only cols lanes are valid; writing a full 16 bytes would run into
         * the next row (and off the end of the buffer for cols < 8) */
        __builtin_memcpy(tmp + k * tmpstride, &r, cols * 2);
    }

    v4 lo[16], hi[16];
    for (int k = 0; k < rows + 5; k++) {
        /* the tmp buffer is a stack array: not necessarily 16-byte aligned,
         * so this cannot be a vec_ld (clang's vec_ld ignores the low bits).
         * Only cols lanes were written, zero the rest. */
        v8 t;
        __builtin_memset(&t, 0, 16);
        __builtin_memcpy(&t, tmp + k * tmpstride, cols * 2);
        lo[k] = vec_unpackh(t);
        hi[k] = vec_unpackl(t);
    }

    for (int i = 0; i < rows; i++) {
        v16 out = p6_fin10(p6_q6_32(lo[i], lo[i + 1], lo[i + 2],
                                    lo[i + 3], lo[i + 4], lo[i + 5], s4w, s2w),
                           p6_q6_32(hi[i], hi[i + 1], hi[i + 2],
                                    hi[i + 3], hi[i + 4], hi[i + 5], s4w, s2w));
        if (avg)
            out = vec_avg(out, p6_loadu(dst));
        p6_store(dst, out, cols);
        dst += dstride;
    }
}

/* op_avg variants of mc20/mc02/mc22 route the filter through op_avg, i.e.
 * the filtered result is averaged with the destination instead of replacing
 * it (h264qpel_template.c calls the OPNAME lowpass for those three). */
#define put_AVG 0
#define avg_AVG 1

/* ---- per-size entry points (what ff_h264qpel_init installs) ---- */

#define P6_LP_DEF(N)                                                          \
static void p6_h##N (uint8_t *dst, const uint8_t *src, ptrdiff_t d, ptrdiff_t s, const int avg) \
{ p6_h_lowpass(dst, src, d, s, N, N, avg); }                                   \
static void p6_v##N (uint8_t *dst, const uint8_t *src, ptrdiff_t d, ptrdiff_t s, const int avg) \
{ p6_v_lowpass(dst, src, d, s, N, N, avg); }                                   \
static void p6_hv##N(uint8_t *dst, int16_t *tmp, const uint8_t *src,           \
                     ptrdiff_t d, ptrdiff_t ts, ptrdiff_t s, const int avg)    \
{ p6_hv_lowpass(dst, tmp, src, d, ts, s, N, N, avg); }

P6_LP_DEF(2)
P6_LP_DEF(4)
P6_LP_DEF(8)

/* 16 = four 8x8 calls, exactly like the C reference */
static void p6_h16(uint8_t *dst, const uint8_t *src, ptrdiff_t d, ptrdiff_t s, const int avg)
{
    p6_h_lowpass(dst,             src,             d, s, 8, 8, avg);
    p6_h_lowpass(dst + 8,         src + 8,         d, s, 8, 8, avg);
    p6_h_lowpass(dst + 8 * d,     src + 8 * s,     d, s, 8, 8, avg);
    p6_h_lowpass(dst + 8 * d + 8, src + 8 * s + 8, d, s, 8, 8, avg);
}
static void p6_v16(uint8_t *dst, const uint8_t *src, ptrdiff_t d, ptrdiff_t s, const int avg)
{
    p6_v_lowpass(dst,             src,             d, s, 8, 8, avg);
    p6_v_lowpass(dst + 8,         src + 8,         d, s, 8, 8, avg);
    p6_v_lowpass(dst + 8 * d,     src + 8 * s,     d, s, 8, 8, avg);
    p6_v_lowpass(dst + 8 * d + 8, src + 8 * s + 8, d, s, 8, 8, avg);
}
static void p6_hv16(uint8_t *dst, int16_t *tmp, const uint8_t *src,
                    ptrdiff_t d, ptrdiff_t ts, ptrdiff_t s, const int avg)
{
    p6_hv_lowpass(dst,             tmp,     src,             d, ts, s, 8, 8, avg);
    p6_hv_lowpass(dst + 8,         tmp + 8, src + 8,         d, ts, s, 8, 8, avg);
    p6_hv_lowpass(dst + 8 * d,     tmp,     src + 8 * s,     d, ts, s, 8, 8, avg);
    p6_hv_lowpass(dst + 8 * d + 8, tmp + 8, src + 8 * s + 8, d, ts, s, 8, 8, avg);
}

/* ------------------------------------------------------------------------ */
/* Byte level helpers: copy, put/avg, and the l2 (average of two halves) ops */
/* ------------------------------------------------------------------------ */

P6_INLINE void p6_copy_block(uint8_t *dst, const uint8_t *src, ptrdiff_t dstride,
                             ptrdiff_t sstride, int rows, int cols)
{
    for (int i = 0; i < rows; i++) {
        __builtin_memcpy(dst, src, cols);
        dst += dstride;
        src += sstride;
    }
}

P6_INLINE void p6_put_px(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                         int rows, int cols)
{
    p6_copy_block(dst, src, stride, stride, rows, cols);
}

P6_INLINE void p6_avg_px(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                         int rows, int cols)
{
    for (int i = 0; i < rows; i++) {
        p6_store(dst, vec_avg(p6_loadu(dst), p6_loadu(src)), cols);
        dst += stride;
        src += stride;
    }
}

/* rnd_avg(src1, src2) -> dst, or rnd_avg(dst, rnd_avg(src1, src2)) */
P6_INLINE void p6_put_l2(uint8_t *dst, const uint8_t *s1, const uint8_t *s2,
                         ptrdiff_t dstride, ptrdiff_t s1s, ptrdiff_t s2s,
                         int rows, int cols)
{
    for (int i = 0; i < rows; i++) {
        p6_store(dst, vec_avg(p6_loadu(s1), p6_loadu(s2)), cols);
        dst += dstride;
        s1  += s1s;
        s2  += s2s;
    }
}

P6_INLINE void p6_avg_l2(uint8_t *dst, const uint8_t *s1, const uint8_t *s2,
                         ptrdiff_t dstride, ptrdiff_t s1s, ptrdiff_t s2s,
                         int rows, int cols)
{
    for (int i = 0; i < rows; i++) {
        v16 h = vec_avg(p6_loadu(s1), p6_loadu(s2));
        p6_store(dst, vec_avg(p6_loadu(dst), h), cols);
        dst += dstride;
        s1  += s1s;
        s2  += s2s;
    }
}

/* ------------------------------------------------------------------------ */
/* The 16 sub-pel positions, mirroring h264qpel_template.c's H264_MC         */
/* ------------------------------------------------------------------------ */

#define P6_H264_MC(OP, SIZE)                                                  \
static void ff_##OP##_h264_qpel##SIZE##_mc00(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ p6_##OP##_px(dst, src, stride, SIZE, SIZE); }                               \
static void ff_##OP##_h264_qpel##SIZE##_mc10(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t half[SIZE*SIZE];                                                    \
  p6_h##SIZE(half, src, SIZE, stride, 0);                                        \
  p6_##OP##_l2(dst, src, half, stride, stride, SIZE, SIZE, SIZE); }           \
static void ff_##OP##_h264_qpel##SIZE##_mc20(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ p6_h##SIZE(dst, src, stride, stride, OP##_AVG); }                                     \
static void ff_##OP##_h264_qpel##SIZE##_mc30(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t half[SIZE*SIZE];                                                    \
  p6_h##SIZE(half, src, SIZE, stride, 0);                                        \
  p6_##OP##_l2(dst, src + 1, half, stride, stride, SIZE, SIZE, SIZE); }       \
static void ff_##OP##_h264_qpel##SIZE##_mc01(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  uint8_t half[SIZE*SIZE];                                                    \
  p6_copy_block(full, src - stride*2, SIZE, stride, SIZE+5, SIZE);            \
  p6_v##SIZE(half, full_mid, SIZE, SIZE, 0);                                     \
  p6_##OP##_l2(dst, full_mid, half, stride, SIZE, SIZE, SIZE, SIZE); }        \
static void ff_##OP##_h264_qpel##SIZE##_mc02(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  p6_copy_block(full, src - stride*2, SIZE, stride, SIZE+5, SIZE);            \
  p6_v##SIZE(dst, full_mid, stride, SIZE, OP##_AVG); }                                  \
static void ff_##OP##_h264_qpel##SIZE##_mc03(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  uint8_t half[SIZE*SIZE];                                                    \
  p6_copy_block(full, src - stride*2, SIZE, stride, SIZE+5, SIZE);            \
  p6_v##SIZE(half, full_mid, SIZE, SIZE, 0);                                     \
  p6_##OP##_l2(dst, full_mid + SIZE, half, stride, SIZE, SIZE, SIZE, SIZE); } \
static void ff_##OP##_h264_qpel##SIZE##_mc11(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  uint8_t halfH[SIZE*SIZE], halfV[SIZE*SIZE];                                 \
  p6_h##SIZE(halfH, src, SIZE, stride, 0);                                       \
  p6_copy_block(full, src - stride*2, SIZE, stride, SIZE+5, SIZE);            \
  p6_v##SIZE(halfV, full_mid, SIZE, SIZE, 0);                                    \
  p6_##OP##_l2(dst, halfH, halfV, stride, SIZE, SIZE, SIZE, SIZE); }          \
static void ff_##OP##_h264_qpel##SIZE##_mc31(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  uint8_t halfH[SIZE*SIZE], halfV[SIZE*SIZE];                                 \
  p6_h##SIZE(halfH, src, SIZE, stride, 0);                                       \
  p6_copy_block(full, src - stride*2 + 1, SIZE, stride, SIZE+5, SIZE);        \
  p6_v##SIZE(halfV, full_mid, SIZE, SIZE, 0);                                    \
  p6_##OP##_l2(dst, halfH, halfV, stride, SIZE, SIZE, SIZE, SIZE); }          \
static void ff_##OP##_h264_qpel##SIZE##_mc13(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  uint8_t halfH[SIZE*SIZE], halfV[SIZE*SIZE];                                 \
  p6_h##SIZE(halfH, src + stride, SIZE, stride, 0);                              \
  p6_copy_block(full, src - stride*2, SIZE, stride, SIZE+5, SIZE);            \
  p6_v##SIZE(halfV, full_mid, SIZE, SIZE, 0);                                    \
  p6_##OP##_l2(dst, halfH, halfV, stride, SIZE, SIZE, SIZE, SIZE); }          \
static void ff_##OP##_h264_qpel##SIZE##_mc33(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  uint8_t halfH[SIZE*SIZE], halfV[SIZE*SIZE];                                 \
  p6_h##SIZE(halfH, src + stride, SIZE, stride, 0);                              \
  p6_copy_block(full, src - stride*2 + 1, SIZE, stride, SIZE+5, SIZE);        \
  p6_v##SIZE(halfV, full_mid, SIZE, SIZE, 0);                                    \
  p6_##OP##_l2(dst, halfH, halfV, stride, SIZE, SIZE, SIZE, SIZE); }          \
static void ff_##OP##_h264_qpel##SIZE##_mc22(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ int16_t tmp[SIZE*(SIZE+5)];                                                 \
  p6_hv##SIZE(dst, tmp, src, stride, SIZE, stride, OP##_AVG); }                         \
static void ff_##OP##_h264_qpel##SIZE##_mc21(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ int16_t tmp[SIZE*(SIZE+5)];                                                 \
  uint8_t halfH[SIZE*SIZE], halfHV[SIZE*SIZE];                                \
  p6_h##SIZE(halfH, src, SIZE, stride, 0);                                       \
  p6_hv##SIZE(halfHV, tmp, src, SIZE, SIZE, stride, 0);                          \
  p6_##OP##_l2(dst, halfH, halfHV, stride, SIZE, SIZE, SIZE, SIZE); }         \
static void ff_##OP##_h264_qpel##SIZE##_mc23(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ int16_t tmp[SIZE*(SIZE+5)];                                                 \
  uint8_t halfH[SIZE*SIZE], halfHV[SIZE*SIZE];                                \
  p6_h##SIZE(halfH, src + stride, SIZE, stride, 0);                              \
  p6_hv##SIZE(halfHV, tmp, src, SIZE, SIZE, stride, 0);                          \
  p6_##OP##_l2(dst, halfH, halfHV, stride, SIZE, SIZE, SIZE, SIZE); }         \
static void ff_##OP##_h264_qpel##SIZE##_mc12(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  int16_t tmp[SIZE*(SIZE+5)];                                                 \
  uint8_t halfV[SIZE*SIZE], halfHV[SIZE*SIZE];                                \
  p6_copy_block(full, src - stride*2, SIZE, stride, SIZE+5, SIZE);            \
  p6_v##SIZE(halfV, full_mid, SIZE, SIZE, 0);                                    \
  p6_hv##SIZE(halfHV, tmp, src, SIZE, SIZE, stride, 0);                          \
  p6_##OP##_l2(dst, halfV, halfHV, stride, SIZE, SIZE, SIZE, SIZE); }         \
static void ff_##OP##_h264_qpel##SIZE##_mc32(uint8_t *dst, const uint8_t *src, ptrdiff_t stride) \
{ uint8_t full[SIZE*(SIZE+5)];                                                \
  uint8_t *const full_mid = full + SIZE*2;                                    \
  int16_t tmp[SIZE*(SIZE+5)];                                                 \
  uint8_t halfV[SIZE*SIZE], halfHV[SIZE*SIZE];                                \
  p6_copy_block(full, src - stride*2 + 1, SIZE, stride, SIZE+5, SIZE);        \
  p6_v##SIZE(halfV, full_mid, SIZE, SIZE, 0);                                    \
  p6_hv##SIZE(halfHV, tmp, src, SIZE, SIZE, stride, 0);                          \
  p6_##OP##_l2(dst, halfV, halfHV, stride, SIZE, SIZE, SIZE, SIZE); }

P6_H264_MC(put, 4)
P6_H264_MC(avg, 4)
P6_H264_MC(put, 8)
P6_H264_MC(avg, 8)
P6_H264_MC(put, 16)
P6_H264_MC(avg, 16)

/* ------------------------------------------------------------------------ */
/* Table installation + runtime self check                                   */
/* ------------------------------------------------------------------------ */

#define P6_TAB(OP, SIZE, IDX)                                                 \
    do {                                                                      \
        t[(IDX)*16 +  0] = ff_##OP##_h264_qpel##SIZE##_mc00;                  \
        t[(IDX)*16 +  1] = ff_##OP##_h264_qpel##SIZE##_mc10;                  \
        t[(IDX)*16 +  2] = ff_##OP##_h264_qpel##SIZE##_mc20;                  \
        t[(IDX)*16 +  3] = ff_##OP##_h264_qpel##SIZE##_mc30;                  \
        t[(IDX)*16 +  4] = ff_##OP##_h264_qpel##SIZE##_mc01;                  \
        t[(IDX)*16 +  5] = ff_##OP##_h264_qpel##SIZE##_mc11;                  \
        t[(IDX)*16 +  6] = ff_##OP##_h264_qpel##SIZE##_mc21;                  \
        t[(IDX)*16 +  7] = ff_##OP##_h264_qpel##SIZE##_mc31;                  \
        t[(IDX)*16 +  8] = ff_##OP##_h264_qpel##SIZE##_mc02;                  \
        t[(IDX)*16 +  9] = ff_##OP##_h264_qpel##SIZE##_mc12;                  \
        t[(IDX)*16 + 10] = ff_##OP##_h264_qpel##SIZE##_mc22;                  \
        t[(IDX)*16 + 11] = ff_##OP##_h264_qpel##SIZE##_mc32;                  \
        t[(IDX)*16 + 12] = ff_##OP##_h264_qpel##SIZE##_mc03;                  \
        t[(IDX)*16 + 13] = ff_##OP##_h264_qpel##SIZE##_mc13;                  \
        t[(IDX)*16 + 14] = ff_##OP##_h264_qpel##SIZE##_mc23;                  \
        t[(IDX)*16 + 15] = ff_##OP##_h264_qpel##SIZE##_mc33;                  \
    } while (0)

void ff_h264qpel_install_x360(p6_qpel_func *put_tab, p6_qpel_func *avg_tab)
{
    p6_qpel_func *t = put_tab;
    P6_TAB(put, 16, 0);
    P6_TAB(put,  8, 1);
    P6_TAB(put,  4, 2);
    t = avg_tab;
    P6_TAB(avg, 16, 0);
    P6_TAB(avg,  8, 1);
    P6_TAB(avg,  4, 2);
}

/* ------------------------------------------------------------------------ */
/* Runtime self check                                                        */
/* ------------------------------------------------------------------------ */

#define P6_QST_STRIDE 48
#define P6_QST_SIZE   16
#define P6_QST_MARGIN 8      /* rows/cols of padding above and left of src */

static uint32_t p6_qst_rng = 0x2545F491u;
static uint32_t p6_qst_rand(void)
{
    p6_qst_rng ^= p6_qst_rng << 13;
    p6_qst_rng ^= p6_qst_rng >> 17;
    p6_qst_rng ^= p6_qst_rng << 5;
    return p6_qst_rng;
}

static int p6_qst_cmp(const uint8_t *a, const uint8_t *b, int size)
{
    for (int i = 0; i < size; i++)
        for (int j = 0; j < size; j++)
            if (a[i * size + j] != b[i * size + j])
                return 0;
    return 1;
}

int ff_h264_vmx_qpel_selftest(const p6_qpel_func *put_c, const p6_qpel_func *avg_c)
{
    static const int sizes[3] = { 16, 8, 4 };
    uint8_t buf[P6_QST_STRIDE * P6_QST_STRIDE];
    uint8_t ref[P6_QST_SIZE * P6_QST_SIZE];
    uint8_t got[P6_QST_SIZE * P6_QST_SIZE];
    uint8_t seed[P6_QST_SIZE * P6_QST_SIZE];
    p6_qpel_func mine[2 * 4 * 16];
    p6_qpel_func theirs[2 * 4 * 16];

    ff_h264qpel_install_x360(mine, mine + 4 * 16);

    for (int i = 0; i < 3 * 16; i++) {
        theirs[i] = put_c[i];
        theirs[4 * 16 + i] = avg_c[i];
    }

    for (size_t i = 0; i < sizeof(seed); i++)
        seed[i] = (uint8_t)p6_qst_rand();
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)p6_qst_rand();

    for (int si = 0; si < 3; si++) {
        const int size = sizes[si];
        const uint8_t *src = buf + P6_QST_MARGIN * P6_QST_STRIDE + P6_QST_MARGIN;

        for (int pos = 0; pos < 16; pos++) {
            for (int variant = 0; variant < 2; variant++) {
                const p6_qpel_func *tab = variant ? theirs + 4 * 16 : theirs;
                const p6_qpel_func *own = variant ? mine + 4 * 16 : mine;

                __builtin_memcpy(ref, seed, sizeof(ref));
                __builtin_memcpy(got, seed, sizeof(got));
                tab[si * 16 + pos](ref, src, size);
                own[si * 16 + pos](got, src, size);

                if (!p6_qst_cmp(ref, got, size))
                    return 1 + (si << 8) + (pos << 4) + variant;
            }
        }
    }
    return 0;
}

#endif /* XBOX || P6_STANDALONE */

/* ------------------------------------------------------------------------ */
/* FFmpeg glue (not compiled by the standalone hardware test)                */
/* ------------------------------------------------------------------------ */

#if defined(XBOX) && !defined(P6_STANDALONE)

#include "libavcodec/h264qpel.h"
#include "libavutil/log.h"

extern int xbox360_vmx128_enabled;

void ff_h264qpel_init_x360(H264QpelContext *c, int bit_depth)
{
    static int selftest = -1;

    if (bit_depth != 8 || !xbox360_vmx128_enabled)
        return;

    if (selftest < 0)
        selftest = ff_h264_vmx_qpel_selftest(&c->put_h264_qpel_pixels_tab[0][0],
                                             &c->avg_h264_qpel_pixels_tab[0][0]);

    if (selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 qpel: VMX128 self check FAILED (%d), using C\n",
               selftest);
        return;
    }

    ff_h264qpel_install_x360(&c->put_h264_qpel_pixels_tab[0][0],
                             &c->avg_h264_qpel_pixels_tab[0][0]);

    av_log(NULL, AV_LOG_INFO, "x360 h264 qpel: VMX128 kernels on\n");
}

#endif /* XBOX && !P6_STANDALONE */
