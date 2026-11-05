/*
 * Small shared helpers for the Xbox 360 (Xenon / VMX128) kernels.
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

#ifndef AVCODEC_XBOX360_VMX_UTIL_H
#define AVCODEC_XBOX360_VMX_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <altivec.h>
#ifdef vector
#undef vector
#endif
#ifdef pixel
#undef pixel
#endif

typedef __vector unsigned char  v16;
typedef __vector signed char    vs8;
typedef __vector signed short   v8;
typedef __vector signed int     v4;
typedef __vector float          vf;

#if defined(__GNUC__)
#define P6_INLINE static __attribute__((always_inline)) inline
#else
#define P6_INLINE static inline
#endif

/* clang defines vec_ld() as an *aligned* 16-byte load: unlike GCC it does not
 * byte-shift the address, so an unaligned pointer silently returns the aligned
 * window and the kernel computes garbage.  Every MC source pointer is
 * unaligned in the general case (x/y offsets shift src by 1..7 bytes), so load
 * through an explicit align-down plus vperm. */
static const unsigned char p6_perm_ident[16] __attribute__((aligned(16))) =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

P6_INLINE v16 p6_loadu(const uint8_t *p)
{
    const unsigned a = (unsigned)(uintptr_t)p;
    const uint8_t *base = (const uint8_t *)(a & ~15u);
    v16 ctl = vec_add(vec_ld(0, p6_perm_ident), vec_splats((unsigned char)(a & 15)));
    return vec_perm(vec_ld(0, base), vec_ld(16, base), ctl);
}

/* Bytes -> unsigned 16-bit lanes.  vec_unpackh/l() of a u8 is a *sign*
 * extension, the & 0x00FF clears the high halfword back to the unsigned
 * value. */
P6_INLINE v8 p6_widen_lo(v16 x)
{
    return vec_and((v8)vec_unpackh((vs8)x), (v8)vec_splats((unsigned short)0x00FF));
}

P6_INLINE v8 p6_widen_hi(v16 x)
{
    return vec_and((v8)vec_unpackl((vs8)x), (v8)vec_splats((unsigned short)0x00FF));
}

P6_INLINE void p6_store(uint8_t *dst, v16 v, int n)
{
    if (n == 16) {
        __builtin_memcpy(dst, &v, 16);
    } else {
        uint64_t o;
        __builtin_memcpy(&o, &v, 8);
        if (n == 8)
            __builtin_memcpy(dst, &o, 8);
        else if (n == 4)
            __builtin_memcpy(dst, &o, 4);
        else if (n == 2)
            __builtin_memcpy(dst, &o, 2);
        else
            __builtin_memcpy(dst, &o, 1);
    }
}

#endif /* AVCODEC_XBOX360_VMX_UTIL_H */
