/*
 * Xbox 360 (Xenon / VMX128) H.264 deblocking filter kernels.
 *
 * Covers the eight non-mbaff loop filter entry points of H264DSPContext
 * for 8-bit pixels: v/h direction x luma/chroma x normal/intra.  The
 * geometry and per-lane arithmetic mirror the C reference in
 * h264dsp_template.c exactly:
 *
 *  - The "v" filters run along a horizontal edge: the filter crosses rows
 *    (pix[-3*stride] .. pix[2*stride]) and steps along columns.  Luma
 *    covers 16 columns in the 4 tc0 groups of 4; chroma covers 8 columns
 *    in the 4 tc0 groups of 2 (tc0[i] applies to columns 2i .. 2i+1).
 *  - The "h" filters run along a vertical edge: the filter crosses columns
 *    (pix[-3] .. pix[2]) and steps down rows in the same tc0 grouping.
 *
 * Semantics checked against the C on paper before porting, then enforced
 * by the runtime self check below (full-buffer compare, C vs VMX, with
 * negative/zero tc0 and out-of-range alpha/beta cases):
 *
 *  - luma normal: tc0[i] < 0 skips the group; with tc0[i] == 0 the C
 *    still runs the p0/q0 delta with tc = 0+ep+eq but never writes
 *    p1'/q1'.  The kernels clamp tc to >= 0 and let a zero-clip produce
 *    the same no-op writes, and gate every store on the d mask which is
 *    cleared for negative tc.
 *  - chroma normal: the C's skip condition (tc0[i] <= 0, via the
 *    "(tc0[i] - 1U) + 1" trick) is likewise equivalent to a zero delta.
 *  - p1'/q1'/p2'/q2' values always stay inside [0,255] on their own (the
 *    deltas move the sample toward an already-in-range target), so the
 *    saturating byte packs used for the stores never disagree with the
 *    C's unclipped 8-bit stores.
 *
 * The h-direction kernels transpose each 4-row group's two 4-byte halves
 * into per-column vectors (rows packed in r0,r2,r1,r3 order - the row
 * masks and the chroma tc masks below encode that order), filter, and
 * transpose back into 8-byte row writes covering columns -4..+3.
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

/* ------------------------------------------------------------------ */
/* Per-lane helpers                                                    */
/* ------------------------------------------------------------------ */

P6_INLINE v8 p6_db_splat(int v)
{
    return vec_splats((short)v);
}

/* Small shift counts / rounding addends, read through volatile.  LLVM folds
 * (x << k) + y into x * 2^k + y and then selects vmladduhm - which Xenon
 * dropped (see the header comment in h264_qpel_vmx.c).  An opaque count
 * keeps the shift-and-add sequence intact; the cost is a few loads per
 * kernel entry. */
static const volatile short p6_db_cnt_v[4][8] __attribute__((aligned(16))) = {
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 2, 2, 2, 2, 2, 2, 2, 2 },
    { 3, 3, 3, 3, 3, 3, 3, 3 },
    { 4, 4, 4, 4, 4, 4, 4, 4 },
};

P6_INLINE v8 p6_db_cnt(int n)
{
    short t[8];
    v8 r;

    for (int i = 0; i < 8; i++)
        t[i] = p6_db_cnt_v[n - 1][i];
    __builtin_memcpy(&r, t, 16);
    return r;
}

P6_INLINE v8 p6_db_abs(v8 a, v8 b)
{
    v8 d = vec_sub(a, b);
    return vec_max(d, vec_sub(p6_db_splat(0), d));
}

/* a < b per lane, all-ones mask where true */
P6_INLINE v8 p6_db_lt(v8 a, v8 b)
{
    return vec_cmpgt(b, a);
}

P6_INLINE v8 p6_db_clip(v8 v, v8 tc)
{
    return vec_max(vec_min(v, tc), vec_sub(p6_db_splat(0), tc));
}

/* p6_db_sel(a, b, m): b where m is all-ones, a where m is zero.  Spelled as
 * bit operations because the altivec.h overloads are ambiguous for a plain
 * short mask. */
P6_INLINE v8 p6_db_sel(v8 a, v8 b, v8 m)
{
    return vec_or(vec_andc(a, m), vec_and(b, m));
}

/* ------------------------------------------------------------------ */
/* Filter cores                                                        */
/* ------------------------------------------------------------------ */

/* Luma normal filter across one edge position per lane.  tc is already
 * clamped to >= 0; neg marks the lanes whose tc0 was negative (skip). */
P6_INLINE void p6_db_luma8(v8 p2, v8 p1, v8 p0, v8 q0, v8 q1, v8 q2,
                           v8 tc, v8 neg, v8 av, v8 bv,
                           v8 *o_p1, v8 *o_p0, v8 *o_q0, v8 *o_q1)
{
    const v8 one   = p6_db_cnt(1);
    const v8 two   = p6_db_cnt(2);
    const v8 three = p6_db_cnt(3);
    const v8 four  = p6_db_cnt(4);
    v8 d  = vec_and(p6_db_lt(p6_db_abs(p0, q0), av),
                vec_and(p6_db_lt(p6_db_abs(p1, p0), bv),
                        p6_db_lt(p6_db_abs(q1, q0), bv)));
    v8 ep = p6_db_lt(p6_db_abs(p2, p0), bv);
    v8 eq = p6_db_lt(p6_db_abs(q2, q0), bv);
    v8 mid = vec_sra(vec_add(vec_add(p0, q0), one), one);
    v8 t, x, tc2, p1f, q1f, p0f, q0f;

    d = vec_andc(d, neg);

    t   = p6_db_clip(vec_sub(vec_sra(vec_add(p2, mid), one), p1), tc);
    p1f = vec_add(p1, t);
    t   = p6_db_clip(vec_sub(vec_sra(vec_add(q2, mid), one), q1), tc);
    q1f = vec_add(q1, t);

    tc2 = vec_add(vec_add(tc, vec_and(ep, one)), vec_and(eq, one));
    x   = vec_add(vec_sl(vec_sub(q0, p0), two), vec_sub(p1, q1));
    x   = p6_db_clip(vec_sra(vec_add(x, four), three), tc2);
    p0f = vec_add(p0, x);
    q0f = vec_sub(q0, x);

    *o_p1 = p6_db_sel(p1, p1f, vec_and(d, ep));
    *o_q1 = p6_db_sel(q1, q1f, vec_and(d, eq));
    *o_p0 = p6_db_sel(p0, p0f, d);
    *o_q0 = p6_db_sel(q0, q0f, d);
}

/* Luma intra (strong/weak) filter. */
P6_INLINE void p6_db_luma8_intra(v8 p3, v8 p2, v8 p1, v8 p0,
                                 v8 q0, v8 q1, v8 q2, v8 q3,
                                 v8 av, v8 bv,
                                 v8 *o_p2, v8 *o_p1, v8 *o_p0,
                                 v8 *o_q0, v8 *o_q1, v8 *o_q2)
{
    const v8 one   = p6_db_cnt(1);
    const v8 two   = p6_db_cnt(2);
    const v8 three = p6_db_cnt(3);
    const v8 four  = p6_db_cnt(4);
    v8 d     = vec_and(p6_db_lt(p6_db_abs(p0, q0), av),
                   vec_and(p6_db_lt(p6_db_abs(p1, p0), bv),
                           p6_db_lt(p6_db_abs(q1, q0), bv)));
    v8 inner = p6_db_lt(p6_db_abs(p0, q0),
                        vec_add(vec_sra(av, two), two));
    v8 ep    = p6_db_lt(p6_db_abs(p2, p0), bv);
    v8 eq    = p6_db_lt(p6_db_abs(q2, q0), bv);
    v8 sp    = vec_and(d, vec_and(inner, ep));
    v8 sq    = vec_and(d, vec_and(inner, eq));
    v8 p0s, p1s, p2s, p0w, q0s, q1s, q2s, q0w;

    p0s = vec_sra(vec_add(vec_add(vec_add(vec_add(p2, vec_sl(p1, one)),
                                          vec_sl(p0, one)),
                                  vec_add(vec_sl(q0, one), q1)),
                          four), three);
    p1s = vec_sra(vec_add(vec_add(vec_add(p2, p1),
                                   vec_add(p0, q0)), two), two);
    p2s = vec_sra(vec_add(vec_add(vec_add(vec_sl(p3, one),
                                          vec_add(vec_sl(p2, one), p2)),
                                  vec_add(vec_add(p1, p0), q0)),
                          four), three);
    p0w = vec_sra(vec_add(vec_add(vec_add(vec_sl(p1, one), p0), q1),
                          two), two);

    q0s = vec_sra(vec_add(vec_add(vec_add(vec_add(p1, vec_sl(p0, one)),
                                          vec_sl(q0, one)),
                                  vec_add(vec_sl(q1, one), q2)),
                          four), three);
    q1s = vec_sra(vec_add(vec_add(vec_add(p0, q0),
                                   vec_add(q1, q2)), two), two);
    q2s = vec_sra(vec_add(vec_add(vec_add(vec_sl(q3, one),
                                          vec_add(vec_sl(q2, one), q2)),
                                  vec_add(vec_add(q1, q0), p0)),
                          four), three);
    q0w = vec_sra(vec_add(vec_add(vec_add(vec_sl(q1, one), q0), p1),
                          two), two);

    *o_p0 = p6_db_sel(p0, p6_db_sel(p0w, p0s, sp), d);
    *o_q0 = p6_db_sel(q0, p6_db_sel(q0w, q0s, sq), d);
    *o_p1 = p6_db_sel(p1, p1s, sp);
    *o_q1 = p6_db_sel(q1, q1s, sq);
    *o_p2 = p6_db_sel(p2, p2s, sp);
    *o_q2 = p6_db_sel(q2, q2s, sq);
}

/* Chroma normal filter (4-tap across, tc already clamped to >= 0). */
P6_INLINE void p6_db_chroma8(v8 p1, v8 p0, v8 q0, v8 q1, v8 tc,
                             v8 av, v8 bv, v8 *o_p0, v8 *o_q0)
{
    const v8 two   = p6_db_cnt(2);
    const v8 three = p6_db_cnt(3);
    const v8 four  = p6_db_cnt(4);
    v8 d = vec_and(p6_db_lt(p6_db_abs(p0, q0), av),
               vec_and(p6_db_lt(p6_db_abs(p1, p0), bv),
                       p6_db_lt(p6_db_abs(q1, q0), bv)));
    v8 x = vec_add(vec_sl(vec_sub(q0, p0), two), vec_sub(p1, q1));

    x = p6_db_clip(vec_sra(vec_add(x, four), three), tc);
    *o_p0 = p6_db_sel(p0, vec_add(p0, x), d);
    *o_q0 = p6_db_sel(q0, vec_sub(q0, x), d);
}

/* Chroma intra filter. */
P6_INLINE void p6_db_chroma8_intra(v8 p1, v8 p0, v8 q0, v8 q1,
                                   v8 av, v8 bv, v8 *o_p0, v8 *o_q0)
{
    const v8 one = p6_db_cnt(1);
    const v8 two = p6_db_cnt(2);
    v8 d  = vec_and(p6_db_lt(p6_db_abs(p0, q0), av),
                vec_and(p6_db_lt(p6_db_abs(p1, p0), bv),
                        p6_db_lt(p6_db_abs(q1, q0), bv)));
    v8 pn = vec_sra(vec_add(vec_add(vec_add(vec_sl(p1, one), p0), q1),
                            two), two);
    v8 qn = vec_sra(vec_add(vec_add(vec_add(vec_sl(q1, one), q0), p1),
                            two), two);

    *o_p0 = p6_db_sel(p0, pn, d);
    *o_q0 = p6_db_sel(q0, qn, d);
}

/* ------------------------------------------------------------------ */
/* h-direction transpose / repack helpers                              */
/* ------------------------------------------------------------------ */

/* Exact 4-byte load into the low bytes of a vector (no over-read). */
P6_INLINE v16 p6_db_ld4(const uint8_t *p)
{
    unsigned w;
    v16 r = vec_splats((unsigned char)0);

    __builtin_memcpy(&w, p, 4);
    __builtin_memcpy(&r, &w, 4);
    return r;
}

/* Four 4-byte rows (p3p2p1p0 / q0q1q2q3 per row) -> four packed column
 * groups in row order r0,r2,r1,r3: bytes 0-3 = column of byte 0, etc. */
P6_INLINE v16 p6_db_t4(const uint8_t *r0, const uint8_t *r1,
                       const uint8_t *r2, const uint8_t *r3)
{
    v16 a0 = p6_db_ld4(r0);
    v16 a1 = p6_db_ld4(r1);
    v16 a2 = p6_db_ld4(r2);
    v16 a3 = p6_db_ld4(r3);
    v16 t0 = vec_mergeh(a0, a1);
    v16 t1 = vec_mergeh(a2, a3);

    return vec_mergeh(t0, t1);
}

/* One column group (4 rows) widened into the low four lanes; lanes 4-7
 * pick up the following group and are discarded by the repack below. */
P6_INLINE v8 p6_db_col(v16 u, int byte_off)
{
    return p6_widen_lo(vec_sld(u, u, (unsigned char)byte_off));
}

/* {a lanes 0..3 packed | b lanes 0..3 packed} */
P6_INLINE v16 p6_db_pack2(v8 a, v8 b)
{
    static const unsigned char m[16] __attribute__((aligned(16))) =
        { 0, 1, 2, 3, 16, 17, 18, 19, 0, 0, 0, 0, 0, 0, 0, 0 };
    v16 za = (v16)vec_packsu(a, p6_db_splat(0));
    v16 zb = (v16)vec_packsu(b, p6_db_splat(0));

    return vec_perm(za, zb, vec_ld(0, m));
}

/* 8-byte row write at dst-4 (columns -4..+3), rows extracted from the
 * repacked uL2||uR2 pair by the row masks below. */
P6_INLINE void p6_db_st_row(uint8_t *dst, v16 uL2, v16 uR2,
                            const unsigned char *rowmask)
{
    p6_store(dst - 4, vec_perm(uL2, uR2, vec_ld(0, rowmask)), 8);
}

/* repack masks (see the friend implementation in ffmpeg-xenon) */
static const unsigned char p6_db_join8[16] __attribute__((aligned(16))) =
    { 0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23 };
static const unsigned char p6_db_join8h[16] __attribute__((aligned(16))) =
    { 0, 1, 2, 3, 4, 5, 6, 7, 24, 25, 26, 27, 28, 29, 30, 31 };
static const unsigned char p6_db_join4[16] __attribute__((aligned(16))) =
    { 0, 1, 2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 0, 0, 0, 0 };
static const unsigned char p6_db_fill12[16] __attribute__((aligned(16))) =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 18, 19 };
static const unsigned char p6_db_q0join[16] __attribute__((aligned(16))) =
    { 16, 17, 18, 19, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const unsigned char p6_db_keeps[16] __attribute__((aligned(16))) =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 28, 29, 30, 31 };
/* row r of the packed (r0,r2,r1,r3) column groups */
static const unsigned char p6_db_row0[16] __attribute__((aligned(16))) =
    { 0, 4, 8, 12, 16, 20, 24, 28, 0, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char p6_db_row1[16] __attribute__((aligned(16))) =
    { 2, 6, 10, 14, 18, 22, 26, 30, 0, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char p6_db_row2[16] __attribute__((aligned(16))) =
    { 1, 5, 9, 13, 17, 21, 25, 29, 0, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char p6_db_row3[16] __attribute__((aligned(16))) =
    { 3, 7, 11, 15, 19, 23, 27, 31, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ------------------------------------------------------------------ */
/* The eight entry points                                              */
/* ------------------------------------------------------------------ */

static void p6_db_v_luma(uint8_t *pix, ptrdiff_t stride,
                         int alpha, int beta, int8_t *tc0)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);
    v16 r2 = p6_loadu(pix - 3 * stride);
    v16 r1 = p6_loadu(pix - 2 * stride);
    v16 r0 = p6_loadu(pix - 1 * stride);
    v16 s0 = p6_loadu(pix);
    v16 s1 = p6_loadu(pix + 1 * stride);
    v16 s2 = p6_loadu(pix + 2 * stride);
    v8 op1[2], op0[2], oq0[2], oq1[2];

    for (int h = 0; h < 2; h++) {
        short tcv[8];
        v8 tc, neg;

        /* columns 8h..8h+7: tc0[2h] x 4 lanes, tc0[2h+1] x 4 lanes */
        tcv[0] = tcv[1] = tcv[2] = tcv[3] = (short)tc0[2 * h];
        tcv[4] = tcv[5] = tcv[6] = tcv[7] = (short)tc0[2 * h + 1];
        __builtin_memcpy(&tc, tcv, 16);
        neg = vec_cmpgt(zero, tc);
        tc  = vec_max(tc, zero);

        p6_db_luma8(h ? p6_widen_hi(r2) : p6_widen_lo(r2),
                    h ? p6_widen_hi(r1) : p6_widen_lo(r1),
                    h ? p6_widen_hi(r0) : p6_widen_lo(r0),
                    h ? p6_widen_hi(s0) : p6_widen_lo(s0),
                    h ? p6_widen_hi(s1) : p6_widen_lo(s1),
                    h ? p6_widen_hi(s2) : p6_widen_lo(s2),
                    tc, neg, av, bv,
                    &op1[h], &op0[h], &oq0[h], &oq1[h]);
    }

    p6_store(pix - 2 * stride, (v16)vec_packsu(op1[0], op1[1]), 16);
    p6_store(pix - 1 * stride, (v16)vec_packsu(op0[0], op0[1]), 16);
    p6_store(pix,              (v16)vec_packsu(oq0[0], oq0[1]), 16);
    p6_store(pix + 1 * stride, (v16)vec_packsu(oq1[0], oq1[1]), 16);
}

static void p6_db_v_luma_intra(uint8_t *pix, ptrdiff_t stride,
                               int alpha, int beta)
{
    const v8 av = p6_db_splat(alpha);
    const v8 bv = p6_db_splat(beta);
    v16 r3 = p6_loadu(pix - 4 * stride);
    v16 r2 = p6_loadu(pix - 3 * stride);
    v16 r1 = p6_loadu(pix - 2 * stride);
    v16 r0 = p6_loadu(pix - 1 * stride);
    v16 s0 = p6_loadu(pix);
    v16 s1 = p6_loadu(pix + 1 * stride);
    v16 s2 = p6_loadu(pix + 2 * stride);
    v16 s3 = p6_loadu(pix + 3 * stride);
    v8 op2[2], op1[2], op0[2], oq0[2], oq1[2], oq2[2];

    for (int h = 0; h < 2; h++) {
        p6_db_luma8_intra(h ? p6_widen_hi(r3) : p6_widen_lo(r3),
                          h ? p6_widen_hi(r2) : p6_widen_lo(r2),
                          h ? p6_widen_hi(r1) : p6_widen_lo(r1),
                          h ? p6_widen_hi(r0) : p6_widen_lo(r0),
                          h ? p6_widen_hi(s0) : p6_widen_lo(s0),
                          h ? p6_widen_hi(s1) : p6_widen_lo(s1),
                          h ? p6_widen_hi(s2) : p6_widen_lo(s2),
                          h ? p6_widen_hi(s3) : p6_widen_lo(s3),
                          av, bv,
                          &op2[h], &op1[h], &op0[h],
                          &oq0[h], &oq1[h], &oq2[h]);
    }

    p6_store(pix - 3 * stride, (v16)vec_packsu(op2[0], op2[1]), 16);
    p6_store(pix - 2 * stride, (v16)vec_packsu(op1[0], op1[1]), 16);
    p6_store(pix - 1 * stride, (v16)vec_packsu(op0[0], op0[1]), 16);
    p6_store(pix,              (v16)vec_packsu(oq0[0], oq0[1]), 16);
    p6_store(pix + 1 * stride, (v16)vec_packsu(oq1[0], oq1[1]), 16);
    p6_store(pix + 2 * stride, (v16)vec_packsu(oq2[0], oq2[1]), 16);
}

static void p6_db_v_chroma(uint8_t *pix, ptrdiff_t stride,
                           int alpha, int beta, int8_t *tc0)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);
    v8 p1 = p6_widen_lo(p6_loadu(pix - 2 * stride));
    v8 p0 = p6_widen_lo(p6_loadu(pix - 1 * stride));
    v8 q0 = p6_widen_lo(p6_loadu(pix));
    v8 q1 = p6_widen_lo(p6_loadu(pix + 1 * stride));
    short tcv[8];
    v8 tc, op0, oq0;

    /* 8 columns in the 4 tc0 groups of 2 */
    tcv[0] = tcv[1] = (short)tc0[0];
    tcv[2] = tcv[3] = (short)tc0[1];
    tcv[4] = tcv[5] = (short)tc0[2];
    tcv[6] = tcv[7] = (short)tc0[3];
    __builtin_memcpy(&tc, tcv, 16);
    tc = vec_max(tc, zero);

    p6_db_chroma8(p1, p0, q0, q1, tc, av, bv, &op0, &oq0);

    p6_store(pix - 1 * stride, (v16)vec_packsu(op0, zero), 8);
    p6_store(pix,              (v16)vec_packsu(oq0, zero), 8);
}

static void p6_db_v_chroma_intra(uint8_t *pix, ptrdiff_t stride,
                                 int alpha, int beta)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);
    v8 p1 = p6_widen_lo(p6_loadu(pix - 2 * stride));
    v8 p0 = p6_widen_lo(p6_loadu(pix - 1 * stride));
    v8 q0 = p6_widen_lo(p6_loadu(pix));
    v8 q1 = p6_widen_lo(p6_loadu(pix + 1 * stride));
    v8 op0, oq0;

    p6_db_chroma8_intra(p1, p0, q0, q1, av, bv, &op0, &oq0);

    p6_store(pix - 1 * stride, (v16)vec_packsu(op0, zero), 8);
    p6_store(pix,              (v16)vec_packsu(oq0, zero), 8);
}

static void p6_db_h_luma(uint8_t *pix, ptrdiff_t stride,
                         int alpha, int beta, int8_t *tc0)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);

    for (int i = 0; i < 4; i++) {
        uint8_t *base = pix + i * 4 * stride;
        v16 uL = p6_db_t4(base - 4, base - 4 + stride,
                          base - 4 + 2 * stride, base - 4 + 3 * stride);
        v16 uR = p6_db_t4(base, base + stride,
                          base + 2 * stride, base + 3 * stride);
        v8 p2 = p6_db_col(uL, 4), p1 = p6_db_col(uL, 8), p0 = p6_db_col(uL, 12);
        v8 q0 = p6_db_col(uR, 0), q1 = p6_db_col(uR, 4), q2 = p6_db_col(uR, 8);
        v8 tc = p6_db_splat(tc0[i]);
        v8 neg = vec_cmpgt(zero, tc);
        v8 op1, op0, oq0, oq1;
        v16 uL2, uR2;

        tc = vec_max(tc, zero);
        p6_db_luma8(p2, p1, p0, q0, q1, q2, tc, neg, av, bv,
                    &op1, &op0, &oq0, &oq1);

        /* uL = [p3|p2|p1|p0] columns -> [p3|p2|p1'|p0'] */
        uL2 = vec_perm(uL, p6_db_pack2(op1, op0), vec_ld(0, p6_db_join8));
        /* uR = [q0|q1|q2|q3] columns -> [q0'|q1'|q2|q3] */
        uR2 = vec_perm(p6_db_pack2(oq0, oq1), uR, vec_ld(0, p6_db_join8h));

        p6_db_st_row(base,              uL2, uR2, p6_db_row0);
        p6_db_st_row(base + stride,     uL2, uR2, p6_db_row1);
        p6_db_st_row(base + 2 * stride, uL2, uR2, p6_db_row2);
        p6_db_st_row(base + 3 * stride, uL2, uR2, p6_db_row3);
    }
}

static void p6_db_h_luma_intra(uint8_t *pix, ptrdiff_t stride,
                               int alpha, int beta)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);

    for (int i = 0; i < 4; i++) {
        uint8_t *base = pix + i * 4 * stride;
        v16 uL = p6_db_t4(base - 4, base - 4 + stride,
                          base - 4 + 2 * stride, base - 4 + 3 * stride);
        v16 uR = p6_db_t4(base, base + stride,
                          base + 2 * stride, base + 3 * stride);
        v8 p3 = p6_db_col(uL, 0), p2 = p6_db_col(uL, 4);
        v8 p1 = p6_db_col(uL, 8), p0 = p6_db_col(uL, 12);
        v8 q0 = p6_db_col(uR, 0), q1 = p6_db_col(uR, 4);
        v8 q2 = p6_db_col(uR, 8), q3 = p6_db_col(uR, 12);
        v8 op2, op1, op0, oq0, oq1, oq2;
        v16 uL2, uR2, tmp;

        p6_db_luma8_intra(p3, p2, p1, p0, q0, q1, q2, q3, av, bv,
                          &op2, &op1, &op0, &oq0, &oq1, &oq2);

        /* uL2 = [p3|p2'|p1'|p0'], uR2 = [q0'|q1'|q2'|q3] */
        tmp = vec_perm(uL, p6_db_pack2(op2, op1), vec_ld(0, p6_db_join4));
        uL2 = vec_perm(tmp, (v16)vec_packsu(op0, zero),
                       vec_ld(0, p6_db_fill12));
        tmp = vec_perm((v16)vec_packsu(oq0, zero), p6_db_pack2(oq1, oq2),
                       vec_ld(0, p6_db_join4));
        uR2 = vec_perm(tmp, uR, vec_ld(0, p6_db_keeps));

        p6_db_st_row(base,              uL2, uR2, p6_db_row0);
        p6_db_st_row(base + stride,     uL2, uR2, p6_db_row1);
        p6_db_st_row(base + 2 * stride, uL2, uR2, p6_db_row2);
        p6_db_st_row(base + 3 * stride, uL2, uR2, p6_db_row3);
    }
}

static void p6_db_h_chroma(uint8_t *pix, ptrdiff_t stride,
                           int alpha, int beta, int8_t *tc0)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);

    for (int i = 0; i < 2; i++) {
        uint8_t *base = pix + i * 4 * stride;
        v16 uL = p6_db_t4(base - 4, base - 4 + stride,
                          base - 4 + 2 * stride, base - 4 + 3 * stride);
        v16 uR = p6_db_t4(base, base + stride,
                          base + 2 * stride, base + 3 * stride);
        v8 p1 = p6_db_col(uL, 8), p0 = p6_db_col(uL, 12);
        v8 q0 = p6_db_col(uR, 0), q1 = p6_db_col(uR, 4);
        short tcv[8];
        v8 tc, op0, oq0;
        v16 uL2, uR2;

        /* 4-row group in r0,r2,r1,r3 lane order; rows 2i..2i+1 -> tc0[i].
         * lanes: r0 -> tc0[2i], r2 -> tc0[2i+1], r1 -> tc0[2i], r3 -> ... */
        tcv[0] = tcv[2] = (short)tc0[2 * i];
        tcv[1] = tcv[3] = (short)tc0[2 * i + 1];
        tcv[4] = tcv[6] = tcv[0];
        tcv[5] = tcv[7] = tcv[1];
        __builtin_memcpy(&tc, tcv, 16);
        tc = vec_max(tc, zero);

        p6_db_chroma8(p1, p0, q0, q1, tc, av, bv, &op0, &oq0);

        /* uL2 = [x|x|p1|p0'], uR2 = [q0'|q1|q2|q3] */
        uL2 = vec_perm(uL, (v16)vec_packsu(op0, zero),
                       vec_ld(0, p6_db_fill12));
        uR2 = vec_perm(uR, (v16)vec_packsu(oq0, zero),
                       vec_ld(0, p6_db_q0join));

        p6_db_st_row(base,              uL2, uR2, p6_db_row0);
        p6_db_st_row(base + stride,     uL2, uR2, p6_db_row1);
        p6_db_st_row(base + 2 * stride, uL2, uR2, p6_db_row2);
        p6_db_st_row(base + 3 * stride, uL2, uR2, p6_db_row3);
    }
}

static void p6_db_h_chroma_intra(uint8_t *pix, ptrdiff_t stride,
                                 int alpha, int beta)
{
    const v8 zero = p6_db_splat(0);
    const v8 av   = p6_db_splat(alpha);
    const v8 bv   = p6_db_splat(beta);

    for (int i = 0; i < 2; i++) {
        uint8_t *base = pix + i * 4 * stride;
        v16 uL = p6_db_t4(base - 4, base - 4 + stride,
                          base - 4 + 2 * stride, base - 4 + 3 * stride);
        v16 uR = p6_db_t4(base, base + stride,
                          base + 2 * stride, base + 3 * stride);
        v8 p1 = p6_db_col(uL, 8), p0 = p6_db_col(uL, 12);
        v8 q0 = p6_db_col(uR, 0), q1 = p6_db_col(uR, 4);
        v8 op0, oq0;
        v16 uL2, uR2;

        p6_db_chroma8_intra(p1, p0, q0, q1, av, bv, &op0, &oq0);

        uL2 = vec_perm(uL, (v16)vec_packsu(op0, zero),
                       vec_ld(0, p6_db_fill12));
        uR2 = vec_perm(uR, (v16)vec_packsu(oq0, zero),
                       vec_ld(0, p6_db_q0join));

        p6_db_st_row(base,              uL2, uR2, p6_db_row0);
        p6_db_st_row(base + stride,     uL2, uR2, p6_db_row1);
        p6_db_st_row(base + 2 * stride, uL2, uR2, p6_db_row2);
        p6_db_st_row(base + 3 * stride, uL2, uR2, p6_db_row3);
    }
}

#if defined(XBOX) && !defined(P6_STANDALONE)

#include "libavcodec/h264dsp.h"
#include "libavcodec/version.h"
#include "libavutil/log.h"

extern int xbox360_vmx128_enabled;

/* FFmpeg dropped the h264_ prefix from H264DSPContext fields in 5.x. */
#if LIBAVCODEC_VERSION_MAJOR < 60
#define P6_DB_V_LUMA         h264_v_loop_filter_luma
#define P6_DB_V_LUMA_INTRA   h264_v_loop_filter_luma_intra
#define P6_DB_H_LUMA         h264_h_loop_filter_luma
#define P6_DB_H_LUMA_INTRA   h264_h_loop_filter_luma_intra
#define P6_DB_V_CHROMA       h264_v_loop_filter_chroma
#define P6_DB_V_CHROMA_INTRA h264_v_loop_filter_chroma_intra
#define P6_DB_H_CHROMA       h264_h_loop_filter_chroma
#define P6_DB_H_CHROMA_INTRA h264_h_loop_filter_chroma_intra
#else
#define P6_DB_V_LUMA         v_loop_filter_luma
#define P6_DB_V_LUMA_INTRA   v_loop_filter_luma_intra
#define P6_DB_H_LUMA         h_loop_filter_luma
#define P6_DB_H_LUMA_INTRA   h_loop_filter_luma_intra
#define P6_DB_V_CHROMA       v_loop_filter_chroma
#define P6_DB_V_CHROMA_INTRA v_loop_filter_chroma_intra
#define P6_DB_H_CHROMA       h_loop_filter_chroma
#define P6_DB_H_CHROMA_INTRA h_loop_filter_chroma_intra
#endif

#define P6_DB_BUFPITCH 96
#define P6_DB_ROWS     24
#define P6_DB_BUFLEN   (P6_DB_ROWS * P6_DB_BUFPITCH)

/* Run one C/VMX pair over identical buffers and compare everything. */
static int p6_db_case(const char *name,
                      void (*cfn)(uint8_t *, ptrdiff_t, int, int, int8_t *),
                      void (*vfn)(uint8_t *, ptrdiff_t, int, int, int8_t *),
                      void (*cfn_i)(uint8_t *, ptrdiff_t, int, int),
                      void (*vfn_i)(uint8_t *, ptrdiff_t, int, int),
                      ptrdiff_t stride, int col_off, int row_base,
                      int alpha, int beta, const int8_t *tc0,
                      uint32_t *rng)
{
    static uint8_t buf[P6_DB_BUFLEN], cbuf[P6_DB_BUFLEN], vbuf[P6_DB_BUFLEN];
    uint8_t *cpix, *vpix;

    for (int k = 0; k < P6_DB_BUFLEN; k++) {
        *rng = *rng * 1664525u + 1013904223u;
        buf[k] = (uint8_t)(*rng >> 24);
    }
    __builtin_memcpy(cbuf, buf, P6_DB_BUFLEN);
    __builtin_memcpy(vbuf, buf, P6_DB_BUFLEN);
    cpix = cbuf + row_base * P6_DB_BUFPITCH + col_off;
    vpix = vbuf + row_base * P6_DB_BUFPITCH + col_off;

    if (cfn) {
        cfn(cpix, stride, alpha, beta, (int8_t *)tc0);
        vfn(vpix, stride, alpha, beta, (int8_t *)tc0);
    } else {
        cfn_i(cpix, stride, alpha, beta);
        vfn_i(vpix, stride, alpha, beta);
    }

    if (__builtin_memcmp(cbuf, vbuf, P6_DB_BUFPITCH * P6_DB_ROWS)) {
        int off = 0, row, col;

        while (off < P6_DB_BUFPITCH * P6_DB_ROWS && cbuf[off] == vbuf[off])
            off++;
        row = off / P6_DB_BUFPITCH;
        col = off % P6_DB_BUFPITCH;
        av_log(NULL, AV_LOG_ERROR,
               "x360 dbg %s: stride=%d col=%d rowbase=%d alpha=%d beta=%d "
               "tc=%d,%d,%d,%d first row=%d col=%d C=%d V=%d\n",
               name, (int)stride, col_off, row_base, alpha, beta,
               tc0 ? tc0[0] : -99, tc0 ? tc0[1] : -99,
               tc0 ? tc0[2] : -99, tc0 ? tc0[3] : -99,
               row, col, cbuf[off], vbuf[off]);
        return 1;
    }
    return 0;
}

static int p6_db_selftest(const H264DSPContext *h)
{
    static const int strides[2] = { 32, 64 };
    uint32_t rng = 0x2545F491u;
    int8_t tc0[4];

    for (int s = 0; s < 2; s++) {
        const int stride = strides[s];
        for (int it = 0; it < 12; it++) {
            int alpha = (int)((rng = rng * 1664525u + 1013904223u) >> 24);
            int beta  = (int)((rng = rng * 1664525u + 1013904223u) >> 24);

            if (it & 1)
                alpha = (int)((rng >> 16) & 0x1ff);     /* up to 511 */
            for (int t = 0; t < 4; t++) {
                rng = rng * 1664525u + 1013904223u;
                tc0[t] = (int8_t)(((rng >> 16) % 36) - 3); /* -3..32 */
            }
            if (it == 2) tc0[0] = tc0[1] = tc0[2] = tc0[3] = 0;
            if (it == 3) tc0[0] = tc0[1] = tc0[2] = tc0[3] = -1;

            if (p6_db_case("v_luma", h->P6_DB_V_LUMA, p6_db_v_luma, NULL, NULL,
                           stride, 16, 8, alpha, beta, tc0, &rng))
                return 1 + (s << 8) + (it << 4);
            if (p6_db_case("h_luma", h->P6_DB_H_LUMA, p6_db_h_luma, NULL, NULL,
                           stride, 16, 8, alpha, beta, tc0, &rng))
                return 2 + (s << 8) + (it << 4);
            if (p6_db_case("v_chroma", h->P6_DB_V_CHROMA, p6_db_v_chroma, NULL, NULL,
                           stride, 16, 8, alpha, beta, tc0, &rng))
                return 3 + (s << 8) + (it << 4);
            if (p6_db_case("h_chroma", h->P6_DB_H_CHROMA, p6_db_h_chroma, NULL, NULL,
                           stride, 16, 8, alpha, beta, tc0, &rng))
                return 4 + (s << 8) + (it << 4);
            if (p6_db_case("v_luma_intra", NULL, NULL, h->P6_DB_V_LUMA_INTRA, p6_db_v_luma_intra,
                           stride, 16, 8, alpha, beta, NULL, &rng))
                return 5 + (s << 8) + (it << 4);
            if (p6_db_case("h_luma_intra", NULL, NULL, h->P6_DB_H_LUMA_INTRA, p6_db_h_luma_intra,
                           stride, 16, 8, alpha, beta, NULL, &rng))
                return 6 + (s << 8) + (it << 4);
            if (p6_db_case("v_chroma_intra", NULL, NULL, h->P6_DB_V_CHROMA_INTRA, p6_db_v_chroma_intra,
                           stride, 16, 8, alpha, beta, NULL, &rng))
                return 7 + (s << 8) + (it << 4);
            if (p6_db_case("h_chroma_intra", NULL, NULL, h->P6_DB_H_CHROMA_INTRA, p6_db_h_chroma_intra,
                           stride, 16, 8, alpha, beta, NULL, &rng))
                return 8 + (s << 8) + (it << 4);
        }
    }
    return 0;
}

av_cold void ff_h264dsp_deblock_init_x360(H264DSPContext *c,
                                          const int bit_depth,
                                          const int chroma_format_idc)
{
    static int selftest = -1;

    if (bit_depth != 8 || !xbox360_vmx128_enabled)
        return;

    if (selftest < 0)
        selftest = p6_db_selftest(c);

    if (selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 deblock: self check FAILED (%d), using C\n",
               selftest);
        return;
    }

    c->P6_DB_V_LUMA       = p6_db_v_luma;
    c->P6_DB_V_LUMA_INTRA = p6_db_v_luma_intra;
    c->P6_DB_H_LUMA       = p6_db_h_luma;
    c->P6_DB_H_LUMA_INTRA = p6_db_h_luma_intra;

    if (chroma_format_idc <= 1) {
        c->P6_DB_V_CHROMA       = p6_db_v_chroma;
        c->P6_DB_V_CHROMA_INTRA = p6_db_v_chroma_intra;
        c->P6_DB_H_CHROMA       = p6_db_h_chroma;
        c->P6_DB_H_CHROMA_INTRA = p6_db_h_chroma_intra;
    }

    av_log(NULL, AV_LOG_INFO, "x360 h264 deblock: VMX128 kernels on\n");
}

#endif /* XBOX && !P6_STANDALONE */

#endif /* XBOX || P6_STANDALONE */
