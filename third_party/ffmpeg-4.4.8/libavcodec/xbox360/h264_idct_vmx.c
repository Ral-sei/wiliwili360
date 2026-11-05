/*
 * Xbox 360 (Xenon / VMX128) H.264 4x4 IDCT.
 *
 * Bit-exact against h264idct_template.c: pass 1 stores back into the int16
 * coefficient block, so its arithmetic wraps at 16 bits; pass 2 is plain
 * int arithmetic and only shifts to range at the very end.  The kernels
 * therefore run pass 1 on 8 int16 lanes and widen to int32 for pass 2.
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

/* dsp-table prototypes (int stride, like H264DSPContext) */
void ff_h264_idct_add_x360(uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct_dc_add_x360(uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct_add16_x360(uint8_t *dst, const int *block_offset,
                             int16_t *block, int stride, const uint8_t *nnzc);
void ff_h264_idct_add16intra_x360(uint8_t *dst, const int *block_offset,
                                  int16_t *block, int stride, const uint8_t *nnzc);
void ff_h264_idct_add8_x360(uint8_t **dst, const int *block_offset,
                            int16_t *block, int stride, const uint8_t *nnzc);
void ff_h264_idct_add8_422_x360(uint8_t **dst, const int *block_offset,
                                int16_t *block, int stride, const uint8_t *nnzc);
void ff_h264_idct8_add_x360(uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct8_dc_add_x360(uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct8_add4_x360(uint8_t *dst, const int *block_offset,
                             int16_t *block, int stride, const uint8_t *nnzc);

/* The "+1<<5" rounding term only lives in lane 0.  Unlike the qpel shift-add
 * sequences (see h264_qpel_vmx.c) plain shifts by 1 and 6 cannot be folded
 * into a multiply, so they are built with vec_splats and fold away. */
static const unsigned char p6_rnd32_16[16] __attribute__((aligned(16))) =
    { 0x00, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* 4x4 transpose between the two IDCT passes.  pa = [r0 | r1] interleaved,
 * pb = [r2 | r3]; y01 ends up as [X0 | X1] and y23 as [X2 | X3], where
 * X_m[i] = r_i[m].  Lane numbers are 16-bit element indices in the
 * concatenation pa||pb, so each element contributes two control bytes. */
static const unsigned char p6_tr01[16] __attribute__((aligned(16))) =
    { 0, 1,  2, 3,  16, 17,  18, 19,   4, 5,  6, 7,  20, 21,  22, 23 };
static const unsigned char p6_tr23[16] __attribute__((aligned(16))) =
    { 8, 9,  10, 11,  24, 25,  26, 27,   12, 13,  14, 15,  28, 29,  30, 31 };

/* Four destination bytes as one 32-bit word.  dst is only 4-byte aligned and
 * may sit at the very end of a plane, so no wide load here. */
P6_INLINE unsigned p6_ld4(const uint8_t *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  |  (unsigned)p[3];
}

/* Gather the 4x4 destination block into one vector: lane j holds row j's four
 * pixels.  The C uses 4 byte loads per row; one word per row is already 4x
 * fewer memory operations. */
P6_INLINE v16 p6_ld_dst4x4(const uint8_t *dst, ptrdiff_t stride)
{
    unsigned t[4];
    t[0] = p6_ld4(dst);
    t[1] = p6_ld4(dst + stride);
    t[2] = p6_ld4(dst + 2 * stride);
    t[3] = p6_ld4(dst + 3 * stride);
    v16 r;
    __builtin_memcpy(&r, t, 16);
    return r;
}

/* Write one 4-byte row.  dst is 4-byte aligned (dsp contract) but not 16, so
 * this has to be an element store, not vec_st.
 *
 * stvewx picks the element to store from the *address* (the slot the address
 * falls into within its 16-byte quad), not from element 0, so the row has to
 * be rotated into that slot first.  Getting this wrong silently writes row 0
 * into whichever row the address happens to select. */
P6_INLINE void p6_st4(uint8_t *dst, v16 v)
{
    const unsigned idx = (unsigned)(uintptr_t)dst & 12u;

    if (idx) {
        v16 ctl = vec_and(vec_add(vec_ld(0, p6_perm_ident),
                                  vec_splats((unsigned char)(16u - idx))),
                          vec_splats((unsigned char)0x1f));
        v = vec_perm(v, v, ctl);
    }
    vec_ste((v4)v, 0, (int *)dst);
}

/* Rows 0..3 of out are packed as bytes 0..3 / 4..7 / 8..11 / 12..15. */
P6_INLINE void p6_st_rows4(uint8_t *dst, ptrdiff_t stride, v16 out)
{
    p6_st4(dst,              out);
    p6_st4(dst +     stride, vec_sld(out, out, 4));
    p6_st4(dst + 2 * stride, vec_sld(out, out, 8));
    p6_st4(dst + 3 * stride, vec_sld(out, out, 12));
}

void ff_h264_idct_add_x360(uint8_t *dst, int16_t *block, int stride)
{
    const v8 s1  = vec_splats((unsigned short)1);
    const __vector unsigned int s1w = vec_splats(1u);
    const __vector unsigned int s6w = vec_splats(6u);
    v16 b0, b1;

    __builtin_memcpy(&b0, block, 16);
    __builtin_memcpy(&b1, block + 8, 16);

    /* block[i + 4*j]: lane i of row j.  A v8 holds two rows, so the odd rows
     * come back with vec_sld(a, a, 8). */
    v8 j0 = vec_add((v8)b0, (v8)vec_ld(0, p6_rnd32_16));
    v8 j1 = vec_sld((v8)b0, (v8)b0, 8);
    v8 j2 = (v8)b1;
    v8 j3 = vec_sld((v8)b1, (v8)b1, 8);

    /* pass 1: 16-bit arithmetic *with wrap-around*, matching the C's stores
     * back into the int16 dctcoef array.  r_k[i] ends up as block[i + 4*k]. */
    v8 z0 = vec_add(j0, j2);
    v8 z1 = vec_sub(j0, j2);
    v8 z2 = vec_sub(vec_sra(j1, s1), j3);
    v8 z3 = vec_add(j1, vec_sra(j3, s1));
    v8 r0 = vec_add(z0, z3);
    v8 r1 = vec_add(z1, z2);
    v8 r2 = vec_sub(z1, z2);
    v8 r3 = vec_sub(z0, z3);

    /* Pass 2 reads the *contiguous* groups block[4i .. 4i+3] (== r_i[0..3]),
     * not the strided ones pass 1 used, so the two passes sit either side of a
     * transpose: X_m[i] = r_i[m].  vec_mergeh pairs the rows up, the two
     * permutes interleave the pairs, and vec_unpackh/l do the transpose and
     * the widen to int32 (the C never re-truncates after pass 1) in one go. */
    v8 pa = vec_mergeh(r0, r1), pb = vec_mergeh(r2, r3);
    v8 y01 = vec_perm(pa, pb, vec_ld(0, p6_tr01));
    v8 y23 = vec_perm(pa, pb, vec_ld(0, p6_tr23));
    v4 a0 = vec_unpackh(y01), a1 = vec_unpackl(y01);
    v4 a2 = vec_unpackh(y23), a3 = vec_unpackl(y23);
    v4 w0 = vec_add(a0, a2);
    v4 w1 = vec_sub(a0, a2);
    v4 w2 = vec_sub(vec_sra(a1, s1w), a3);
    v4 w3 = vec_add(a1, vec_sra(a3, s1w));
    v4 o0 = vec_sra(vec_add(w0, w3), s6w);
    v4 o1 = vec_sra(vec_add(w1, w2), s6w);
    v4 o2 = vec_sra(vec_sub(w1, w2), s6w);
    v4 o3 = vec_sra(vec_sub(w0, w3), s6w);

    v16 px = p6_ld_dst4x4(dst, stride);
    v8  pl = p6_widen_lo(px), ph = p6_widen_hi(px);
    /* add before clipping: o* is signed and av_clip_pixel() only wraps once */
    v16 out = (v16)vec_packsu(
        (v8)vec_packsu(vec_add(o0, vec_unpackh(pl)), vec_add(o1, vec_unpackl(pl))),
        (v8)vec_packsu(vec_add(o2, vec_unpackh(ph)), vec_add(o3, vec_unpackl(ph))));

    p6_st_rows4(dst, stride, out);

    __builtin_memset(block, 0, 16 * sizeof(int16_t));
}

void ff_h264_idct_dc_add_x360(uint8_t *dst, int16_t *block, int stride)
{
    const int dc = (block[0] + 32) >> 6;

    block[0] = 0;

    {
        v16 px = p6_ld_dst4x4(dst, stride);
        v8  v  = vec_splats((short)dc);
        v16 out = (v16)vec_packsu(vec_add(p6_widen_lo(px), v),
                                  vec_add(p6_widen_hi(px), v));
        p6_st_rows4(dst, stride, out);
    }
}

/* ------------------------------------------------------------------------ */
/* 8x8 IDCT                                                                 */
/* ------------------------------------------------------------------------ */

/* The 8x8 butterfly shifts *accumulated* sums ((a7 >> 2) and friends), so
 * unlike the 4x4 it cannot run on 16-bit wrapping lanes: a wrapped sum
 * shifts different bits than the C reference's int arithmetic.  Both passes
 * therefore run on int32 lanes, 4 columns per invocation, and pass 1's
 * results truncate back into the int16 coefficient block exactly like the
 * C's stores (vec_pack is a truncating pack, see the 4x4 transpose above).
 * Pass 2 never truncates before the final >> 6, again like the C. */

/* Exact 8-byte load (no over-read at plane edges). */
P6_INLINE v16 p6_ld8(const uint8_t *p)
{
    uint64_t w;
    v16 r = vec_splats((unsigned char)0);

    __builtin_memcpy(&w, p, 8);
    __builtin_memcpy(&r, &w, 8);
    return r;
}

P6_INLINE void p6_idct8_1d(v4 s[8])
{
    const __vector unsigned int one = vec_splats(1u);
    const __vector unsigned int two = vec_splats(2u);
    v4 a0 = vec_add(s[0], s[4]);
    v4 a2 = vec_sub(s[0], s[4]);
    v4 a4 = vec_sub(vec_sra(s[2], one), s[6]);
    v4 a6 = vec_add(vec_sra(s[6], one), s[2]);
    v4 b0 = vec_add(a0, a6);
    v4 b2 = vec_add(a2, a4);
    v4 b4 = vec_sub(a2, a4);
    v4 b6 = vec_sub(a0, a6);
    v4 a1 = vec_sub(vec_sub(s[5], s[3]), vec_add(s[7], vec_sra(s[7], one)));
    v4 a3 = vec_sub(vec_add(s[7], s[1]), vec_add(s[3], vec_sra(s[3], one)));
    v4 a5 = vec_add(vec_sub(s[7], s[1]), vec_add(s[5], vec_sra(s[5], one)));
    v4 a7 = vec_add(vec_add(s[5], s[3]), vec_add(s[1], vec_sra(s[1], one)));
    v4 b1 = vec_add(vec_sra(a7, two), a1);
    v4 b3 = vec_add(a3, vec_sra(a5, two));
    v4 b5 = vec_sub(vec_sra(a3, two), a5);
    v4 b7 = vec_sub(a7, vec_sra(a1, two));

    s[0] = vec_add(b0, b7);
    s[7] = vec_sub(b0, b7);
    s[1] = vec_add(b2, b5);
    s[6] = vec_sub(b2, b5);
    s[2] = vec_add(b4, b3);
    s[5] = vec_sub(b4, b3);
    s[3] = vec_add(b6, b1);
    s[4] = vec_sub(b6, b1);
}

void ff_h264_idct8_add_x360(uint8_t *dst, int16_t *block, int stride)
{
    const __vector unsigned int s6 = vec_splats(6u);
    v8 rows[8], inter[8], tr[8];
    v4 t0[8], t1[8];

    block[0] = (int16_t)(block[0] + 32);

    for (int k = 0; k < 8; k++)
        __builtin_memcpy(&rows[k], block + 8 * k, 16);
    __builtin_memset(block, 0, 64 * sizeof(int16_t));

    /* pass 1: column transforms, columns 0-3 and 4-7 per invocation */
    for (int g = 0; g < 2; g++) {
        v4 s[8], *out = g ? t1 : t0;

        for (int k = 0; k < 8; k++)
            s[k] = g ? vec_unpackl(rows[k]) : vec_unpackh(rows[k]);
        p6_idct8_1d(s);
        for (int i = 0; i < 8; i++)
            out[i] = s[i];
    }

    /* truncate pass-1 results back to int16 like the C's block stores */
    for (int i = 0; i < 8; i++)
        inter[i] = vec_pack(t0[i], t1[i]);

    /* 8x8 transpose so pass 2 walks the rows */
    {
        v8 a0 = vec_mergeh(inter[0], inter[4]);
        v8 a1 = vec_mergel(inter[0], inter[4]);
        v8 a2 = vec_mergeh(inter[1], inter[5]);
        v8 a3 = vec_mergel(inter[1], inter[5]);
        v8 a4 = vec_mergeh(inter[2], inter[6]);
        v8 a5 = vec_mergel(inter[2], inter[6]);
        v8 a6 = vec_mergeh(inter[3], inter[7]);
        v8 a7 = vec_mergel(inter[3], inter[7]);
        v8 b0 = vec_mergeh(a0, a4);
        v8 b1 = vec_mergel(a0, a4);
        v8 b2 = vec_mergeh(a1, a5);
        v8 b3 = vec_mergel(a1, a5);
        v8 b4 = vec_mergeh(a2, a6);
        v8 b5 = vec_mergel(a2, a6);
        v8 b6 = vec_mergeh(a3, a7);
        v8 b7 = vec_mergel(a3, a7);

        tr[0] = vec_mergeh(b0, b4);
        tr[1] = vec_mergel(b0, b4);
        tr[2] = vec_mergeh(b1, b5);
        tr[3] = vec_mergel(b1, b5);
        tr[4] = vec_mergeh(b2, b6);
        tr[5] = vec_mergel(b2, b6);
        tr[6] = vec_mergeh(b3, b7);
        tr[7] = vec_mergel(b3, b7);
    }

    /* pass 2: row transforms in int32, then (sum >> 6) + pixel */
    for (int g = 0; g < 2; g++) {
        v4 s[8], *out = g ? t1 : t0;

        for (int k = 0; k < 8; k++)
            s[k] = g ? vec_unpackl(tr[k]) : vec_unpackh(tr[k]);
        p6_idct8_1d(s);
        for (int i = 0; i < 8; i++)
            out[i] = s[i];
    }

    for (int i = 0; i < 8; i++) {
        v8 delta = vec_pack(vec_sra(t0[i], s6), vec_sra(t1[i], s6));
        v8 px    = p6_widen_lo(p6_ld8(dst + i * stride));

        p6_store(dst + i * stride,
                 (v16)vec_packsu(vec_add(px, delta), vec_splats((short)0)), 8);
    }
}

void ff_h264_idct8_dc_add_x360(uint8_t *dst, int16_t *block, int stride)
{
    const int dc = (block[0] + 32) >> 6;

    block[0] = 0;

    {
        const v8 v = vec_splats((short)dc);

        for (int i = 0; i < 8; i++) {
            v8 px = p6_widen_lo(p6_ld8(dst + i * stride));

            p6_store(dst + i * stride,
                     (v16)vec_packsu(vec_add(px, v), vec_splats((short)0)), 8);
        }
    }
}

#if defined(XBOX) && !defined(P6_STANDALONE)

#include "libavcodec/h264dsp.h"
#include "libavcodec/h264dec.h" /* scan8 */
#include "libavcodec/h264idct.h" /* ff_h264_idct_add8_8_c etc. */
#include "libavcodec/version.h"
#include "libavutil/log.h"

/* FFmpeg renamed the IDCT slots in H264DSPContext in 5.0. */
#if LIBAVCODEC_VERSION_MAJOR < 60
#define P6_IDCT_ADD        h264_idct_add
#define P6_IDCT_DC_ADD     h264_idct_dc_add
#define P6_IDCT_ADD16      h264_idct_add16
#define P6_IDCT_ADD16INTRA h264_idct_add16intra
#define P6_IDCT_ADD8       h264_idct_add8
#define P6_IDCT8_ADD       h264_idct8_add
#define P6_IDCT8_DC_ADD    h264_idct8_dc_add
#define P6_IDCT8_ADD4      h264_idct8_add4
#else
#define P6_IDCT_ADD        idct_add
#define P6_IDCT_DC_ADD     idct_dc_add
#define P6_IDCT_ADD16      idct_add16
#define P6_IDCT_ADD16INTRA idct_add16intra
#define P6_IDCT_ADD8       idct_add8
#define P6_IDCT8_ADD       idct8_add
#define P6_IDCT8_DC_ADD    idct8_dc_add
#define P6_IDCT8_ADD4      idct8_add4
#endif

extern int xbox360_vmx128_enabled;

/* Compare against the C functions FFmpeg installed just before this call. */
static int p6_idct_selftest(const H264DSPContext *h)
{
    static const int strides[3] = { 4, 32, 96 };
    int16_t cblk[16], vblk[16];
    /* dst is "align 4" in the dsp contract, so only test 4-byte offsets */
    uint8_t cdst[4 * 96 + 16] __attribute__((aligned(16)));
    uint8_t vdst[4 * 96 + 16] __attribute__((aligned(16)));
    uint32_t rng = 0x9E3779B9u;

    for (unsigned i = 0; i < sizeof(cblk) / sizeof(cblk[0]); i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        cblk[i] = (int16_t)rng;
    }

    for (int s = 0; s < 3; s++) {
        const int stride = strides[s];
        for (int off = 0; off < 16; off += 4) {
            for (int i = 0; i < 16; i++) {
                cblk[i] = vblk[i] = (int16_t)(rng = rng * 1664525u + 1013904223u);
                if ((i & 3) == 0)               /* keep DC realistic */
                    cblk[i] = vblk[i] = (int16_t)((int)(rng >> 20) - 2048);
            }
            for (int k = 0; k < (int)sizeof(cdst); k++) {
                rng = rng * 1664525u + 1013904223u;
                vdst[k] = cdst[k] = (uint8_t)(rng >> 24);
            }

            h->P6_IDCT_ADD(cdst + off, cblk, stride);
            ff_h264_idct_add_x360(vdst + off, vblk, stride);
            for (int k = 0; k < 4 * stride + 4; k++)
                if (cdst[off + k] != vdst[off + k])
                    return 1 + (s << 8) + (off << 4);

            for (int i = 0; i < 16; i++)
                if (cblk[i] != vblk[i])
                    return 2 + (s << 8) + (off << 4);

            /* dc-only path */
            for (int i = 0; i < 16; i++)
                cblk[i] = vblk[i] = 0;
            cblk[0] = vblk[0] = (int16_t)((int)(rng >> 20) - 2048);
            for (int k = 0; k < (int)sizeof(cdst); k++) {
                rng = rng * 1664525u + 1013904223u;
                vdst[k] = cdst[k] = (uint8_t)(rng >> 24);
            }
            h->P6_IDCT_DC_ADD(cdst + off, cblk, stride);
            ff_h264_idct_dc_add_x360(vdst + off, vblk, stride);
            for (int k = 0; k < 4 * stride + 4; k++)
                if (cdst[off + k] != vdst[off + k])
                    return 3 + (s << 8) + (off << 4);
            for (int i = 0; i < 16; i++)
                if (cblk[i] != vblk[i])
                    return 4 + (s << 8) + (off << 4);
        }
    }
    return 0;
}

/* Multi-block dispatchers.  ff_h264_idct_add16 / add16intra / add8 and the
 * 4:2:2 variant in h264idct_template.c call the *static* C single-block
 * kernels directly, not the dsp function pointers, so swapping the single
 * blocks above never reached the inter-MB residual path (which is why the
 * M4 A/B measured "in the noise").  These wrap the VMX single-block kernels
 * with the exact dispatch of the C originals — including the nnz==1 DC fast
 * paths and the add8_422 loop's odd i+4 index arithmetic, quirks and all. */
void ff_h264_idct_add16_x360(uint8_t *dst, const int *block_offset,
                             int16_t *block, int stride, const uint8_t *nnzc)
{
    for (int i = 0; i < 16; i++) {
        const int nnz = nnzc[scan8[i]];
        if (nnz) {
            if (nnz == 1 && block[i * 16])
                ff_h264_idct_dc_add_x360(dst + block_offset[i], block + i * 16, stride);
            else
                ff_h264_idct_add_x360(dst + block_offset[i], block + i * 16, stride);
        }
    }
}

void ff_h264_idct_add16intra_x360(uint8_t *dst, const int *block_offset,
                                  int16_t *block, int stride, const uint8_t *nnzc)
{
    for (int i = 0; i < 16; i++) {
        if (nnzc[scan8[i]])
            ff_h264_idct_add_x360(dst + block_offset[i], block + i * 16, stride);
        else if (block[i * 16])
            ff_h264_idct_dc_add_x360(dst + block_offset[i], block + i * 16, stride);
    }
}

void ff_h264_idct_add8_x360(uint8_t **dst, const int *block_offset,
                            int16_t *block, int stride, const uint8_t *nnzc)
{
    for (int j = 1; j < 3; j++)
        for (int i = j * 16; i < j * 16 + 4; i++) {
            if (nnzc[scan8[i]])
                ff_h264_idct_add_x360(dst[j - 1] + block_offset[i], block + i * 16, stride);
            else if (block[i * 16])
                ff_h264_idct_dc_add_x360(dst[j - 1] + block_offset[i], block + i * 16, stride);
        }
}

void ff_h264_idct_add8_422_x360(uint8_t **dst, const int *block_offset,
                                int16_t *block, int stride, const uint8_t *nnzc)
{
    for (int j = 1; j < 3; j++)
        for (int i = j * 16; i < j * 16 + 4; i++) {
            if (nnzc[scan8[i]])
                ff_h264_idct_add_x360(dst[j - 1] + block_offset[i], block + i * 16, stride);
            else if (block[i * 16])
                ff_h264_idct_dc_add_x360(dst[j - 1] + block_offset[i], block + i * 16, stride);
        }
    for (int j = 1; j < 3; j++)
        for (int i = j * 16 + 4; i < j * 16 + 8; i++) {
            if (nnzc[scan8[i + 4]])
                ff_h264_idct_add_x360(dst[j - 1] + block_offset[i + 4], block + i * 16, stride);
            else if (block[i * 16])
                ff_h264_idct_dc_add_x360(dst[j - 1] + block_offset[i + 4], block + i * 16, stride);
        }
}

void ff_h264_idct8_add4_x360(uint8_t *dst, const int *block_offset,
                             int16_t *block, int stride, const uint8_t *nnzc)
{
    for (int i = 0; i < 16; i += 4) {
        const int nnz = nnzc[scan8[i]];
        if (nnz) {
            if (nnz == 1 && block[i * 16])
                ff_h264_idct8_dc_add_x360(dst + block_offset[i], block + i * 16, stride);
            else
                ff_h264_idct8_add_x360(dst + block_offset[i], block + i * 16, stride);
        }
    }
}

/* Compare the multi-block wrappers against the C helpers FFmpeg installed
 * just before this call.  dst is "align 16" in this dsp contract. */
static int p6_idct_mb_selftest(const H264DSPContext *h)
{
    static const int strides[2] = { 32, 64 };
    uint8_t cbuf[2][20 * 64 + 64], vbuf[2][20 * 64 + 64];
    uint8_t *cdst[2] = { cbuf[0], cbuf[1] }, *vdst[2] = { vbuf[0], vbuf[1] };
    int16_t cblk[640], vblk[640];
    int off[48];
    uint8_t nnz[64];
    uint32_t rng = 0x2545F491u;

    for (int s = 0; s < 2; s++) {
        const int stride = strides[s];
        for (int it = 0; it < 200; it++) {
            const int which = it & 3;

            for (int k = 0; k < 48; k++)
                off[k] = (k & 3) * 4 + ((k >> 2) & 3) * 4 * stride;
            for (int k = 0; k < 64; k++) {
                rng = rng * 1664525u + 1013904223u;
                nnz[k] = (uint8_t)((rng >> 16) % 4 == 0 ? 0 : 1 + ((rng >> 24) & 1));
            }
            for (int k = 0; k < 640; k++) {
                rng = rng * 1664525u + 1013904223u;
                cblk[k] = vblk[k] = (int16_t)(rng >> 16);
            }
            /* realistic DCs, occasionally zero to cover the "nnz==1 && dc==0"
             * branch of add16 (which then runs the full add, not dc_add) */
            for (int k = 0; k < 640; k += 16) {
                rng = rng * 1664525u + 1013904223u;
                cblk[k] = vblk[k] = (int16_t)((int)(rng >> 20) - 2048);
                if ((rng & 7) == 0)
                    cblk[k] = vblk[k] = 0;
            }
            for (int p = 0; p < 2; p++)
                for (int k = 0; k < (int)sizeof(cbuf[0]); k++) {
                    rng = rng * 1664525u + 1013904223u;
                    cbuf[p][k] = vbuf[p][k] = (uint8_t)(rng >> 24);
                }

            switch (which) {
            case 0:
                h->P6_IDCT_ADD16(cdst[0], off, cblk, stride, nnz);
                ff_h264_idct_add16_x360(vdst[0], off, vblk, stride, nnz);
                break;
            case 1:
                h->P6_IDCT_ADD16INTRA(cdst[0], off, cblk, stride, nnz);
                ff_h264_idct_add16intra_x360(vdst[0], off, vblk, stride, nnz);
                break;
            case 2:
                /* not the table slot: for 4:2:2 content it holds the 422
                 * variant instead, so compare against the C function */
                ff_h264_idct_add8_8_c(cdst, off, cblk, stride, nnz);
                ff_h264_idct_add8_x360(vdst, off, vblk, stride, nnz);
                break;
            default:
                ff_h264_idct_add8_422_8_c(cdst, off, cblk, stride, nnz);
                ff_h264_idct_add8_422_x360(vdst, off, vblk, stride, nnz);
                break;
            }

            for (int p = 0; p < 2; p++)
                if (memcmp(cbuf[p], vbuf[p], sizeof(cbuf[0])))
                    return 1 + which + (s << 4);
            if (memcmp(cblk, vblk, sizeof(cblk)))
                return 5 + which + (s << 4);
        }
    }
    return 0;
}

/* Compare the 8x8 kernels against the C functions FFmpeg installed just
 * before this call (full-range random coefficients on purpose: the 8x8
 * intermediates overflow int16, which is exactly what the int32 passes
 * must get right).  dst is "align 8" in this dsp contract. */
static int p6_idct8_selftest(const H264DSPContext *h)
{
    static const int strides[3] = { 8, 32, 96 };
    int16_t cblk[256], vblk[256];
    uint8_t cdst[8 * 96 + 16] __attribute__((aligned(16)));
    uint8_t vdst[8 * 96 + 16] __attribute__((aligned(16)));
    uint32_t rng = 0x9E3779B9u;

    for (int s = 0; s < 3; s++) {
        const int stride = strides[s];
        const int max_off = stride >= 16 ? 16 : 8;

        for (int off = 0; off < max_off; off += 8) {
            for (int i = 0; i < 64; i++) {
                rng = rng * 1664525u + 1013904223u;
                cblk[i] = vblk[i] = (int16_t)(rng >> 16);
            }
            for (int k = 0; k < (int)sizeof(cdst); k++) {
                rng = rng * 1664525u + 1013904223u;
                vdst[k] = cdst[k] = (uint8_t)(rng >> 24);
            }

            h->P6_IDCT8_ADD(cdst + off, cblk, stride);
            ff_h264_idct8_add_x360(vdst + off, vblk, stride);
            if (memcmp(cdst, vdst, sizeof(cdst)))
                return 1 + (s << 8) + (off << 4);
            if (memcmp(cblk, vblk, 64 * sizeof(int16_t)))
                return 2 + (s << 8) + (off << 4);

            /* dc-only path */
            for (int i = 0; i < 64; i++)
                cblk[i] = vblk[i] = 0;
            rng = rng * 1664525u + 1013904223u;
            cblk[0] = vblk[0] = (int16_t)((int)(rng >> 16) - 32768);
            for (int k = 0; k < (int)sizeof(cdst); k++) {
                rng = rng * 1664525u + 1013904223u;
                vdst[k] = cdst[k] = (uint8_t)(rng >> 24);
            }
            h->P6_IDCT8_DC_ADD(cdst + off, cblk, stride);
            ff_h264_idct8_dc_add_x360(vdst + off, vblk, stride);
            if (memcmp(cdst, vdst, sizeof(cdst)))
                return 3 + (s << 8) + (off << 4);
            if (memcmp(cblk, vblk, 64 * sizeof(int16_t)))
                return 4 + (s << 8) + (off << 4);
        }
    }

    /* multi-block dispatch: four 8x8 blocks side by side */
    {
        static const int astrides[2] = { 32, 64 };
        uint8_t cbuf[16 * 64 + 64], vbuf[16 * 64 + 64];
        int off[16];
        uint8_t nnz[64];

        for (int s = 0; s < 2; s++) {
            const int stride = astrides[s];

            for (int it = 0; it < 100; it++) {
                for (int k = 0; k < 16; k++)
                    off[k] = (k / 4) * 8;
                for (int k = 0; k < 64; k++) {
                    rng = rng * 1664525u + 1013904223u;
                    nnz[k] = (uint8_t)((rng >> 16) % 3 == 0 ? 0
                                       : 1 + ((rng >> 24) & 1));
                }
                for (int k = 0; k < 256; k++) {
                    rng = rng * 1664525u + 1013904223u;
                    cblk[k] = vblk[k] = (int16_t)(rng >> 16);
                }
                /* realistic DCs, occasionally zero to cover the
                 * "nnz==1 && dc==0" branch of the add4 dispatch */
                for (int k = 0; k < 256; k += 64) {
                    rng = rng * 1664525u + 1013904223u;
                    cblk[k] = vblk[k] = (int16_t)((int)(rng >> 16));
                    if ((rng & 7) == 0)
                        cblk[k] = vblk[k] = 0;
                }
                for (int k = 0; k < (int)sizeof(cbuf); k++) {
                    rng = rng * 1664525u + 1013904223u;
                    vbuf[k] = cbuf[k] = (uint8_t)(rng >> 24);
                }

                h->P6_IDCT8_ADD4(cbuf, off, cblk, stride, nnz);
                ff_h264_idct8_add4_x360(vbuf, off, vblk, stride, nnz);
                if (memcmp(cbuf, vbuf, sizeof(cbuf)))
                    return 5 + (s << 4);
                if (memcmp(cblk, vblk, sizeof(cblk)))
                    return 6 + (s << 4);
            }
        }
    }
    return 0;
}

void ff_h264dsp_idct_init_x360(H264DSPContext *h, int bit_depth, int chroma_format_idc)
{
    static int selftest = -1;
    static int mb_selftest = -1;
    static int idct8_selftest = -1;

    if (bit_depth != 8 || !xbox360_vmx128_enabled)
        return;

    if (selftest < 0)
        selftest = p6_idct_selftest(h);

    if (selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 idct: self check FAILED (%d), using C\n", selftest);
        return;
    }

    h->P6_IDCT_ADD    = ff_h264_idct_add_x360;
    h->P6_IDCT_DC_ADD = ff_h264_idct_dc_add_x360;

    /* The 8x8 family is checked separately (its add4 wrapper too). */
    if (idct8_selftest < 0)
        idct8_selftest = p6_idct8_selftest(h);

    if (idct8_selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 idct: 8x8 self check FAILED (%d), idct8 stays C\n",
               idct8_selftest);
    } else {
        h->P6_IDCT8_ADD    = ff_h264_idct8_add_x360;
        h->P6_IDCT8_DC_ADD = ff_h264_idct8_dc_add_x360;
        h->P6_IDCT8_ADD4   = ff_h264_idct8_add4_x360;
    }

    /* The add16/add8 wrappers must be checked before being installed: the
     * selftest compares them against the C helpers still in the table. */
    if (mb_selftest < 0)
        mb_selftest = p6_idct_mb_selftest(h);

    if (mb_selftest) {
        av_log(NULL, AV_LOG_WARNING,
               "x360 h264 idct: multi-block self check FAILED (%d), add16/add8 stay C\n",
               mb_selftest);
    } else {
        h->P6_IDCT_ADD16      = ff_h264_idct_add16_x360;
        h->P6_IDCT_ADD16INTRA = ff_h264_idct_add16intra_x360;
        /* 4:2:2 installs ff_h264_idct_add8_422 into the same slot */
        h->P6_IDCT_ADD8       = chroma_format_idc == 2 ? ff_h264_idct_add8_422_x360
                                                       : ff_h264_idct_add8_x360;
    }

    av_log(NULL, AV_LOG_INFO, "x360 h264 idct: VMX128 kernels on\n");
}

#endif /* XBOX && !P6_STANDALONE */

#endif /* XBOX || P6_STANDALONE */
