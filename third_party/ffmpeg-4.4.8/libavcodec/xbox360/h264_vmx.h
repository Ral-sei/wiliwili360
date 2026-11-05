/*
 * Xbox 360 (Xenon / VMX128) H.264 DSP kernels -- interface.
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

#ifndef AVCODEC_XBOX360_H264_VMX_H
#define AVCODEC_XBOX360_H264_VMX_H

#include <stddef.h>
#include <stdint.h>

/* Same shape as FFmpeg's h264_chroma_mc_func. */
typedef void (*p6_mc_func)(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                           int h, int x, int y);

void ff_put_h264_chroma_mc8_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y);
void ff_put_h264_chroma_mc4_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y);
void ff_avg_h264_chroma_mc8_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y);
void ff_avg_h264_chroma_mc4_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                 int h, int x, int y);

/* 0 when every kernel matches the C reference for all weights, widths 4/8,
 * heights 4/8 and several strides; nonzero on the first mismatch. */
int ff_h264_vmx_chroma_selftest(p6_mc_func c_put8, p6_mc_func c_put4,
                                p6_mc_func c_avg8, p6_mc_func c_avg4);

#endif /* AVCODEC_XBOX360_H264_VMX_H */
