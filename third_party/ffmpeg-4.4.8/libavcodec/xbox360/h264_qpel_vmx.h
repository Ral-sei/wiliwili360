/*
 * Xbox 360 (Xenon / VMX128) H.264 quarter-pel MC -- interface.
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

#ifndef AVCODEC_XBOX360_H264_QPEL_VMX_H
#define AVCODEC_XBOX360_H264_QPEL_VMX_H

#include <stddef.h>
#include <stdint.h>

/* Same shape as FFmpeg's qpel_mc_func. */
typedef void (*p6_qpel_func)(uint8_t *dst, const uint8_t *src, ptrdiff_t stride);

/* Install the VMX128 tables.  put_tab / avg_tab are &tab[0][0] of the
 * H264QpelContext arrays, i.e. 4*16 entries indexed [size * 16 + pos]. */
void ff_h264qpel_install_x360(p6_qpel_func *put_tab, p6_qpel_func *avg_tab);

/* 0 when every installed kernel matches the C reference in *put_c / *avg_c
 * for all three block sizes and all 16 sub-pel positions. */
int ff_h264_vmx_qpel_selftest(const p6_qpel_func *put_c, const p6_qpel_func *avg_c);

#endif /* AVCODEC_XBOX360_H264_QPEL_VMX_H */
