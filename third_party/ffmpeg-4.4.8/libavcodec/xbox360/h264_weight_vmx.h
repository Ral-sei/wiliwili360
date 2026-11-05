/*
 * Xbox 360 (Xenon / VMX128) H.264 weighted prediction -- interface.
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

#ifndef AVCODEC_XBOX360_H264_WEIGHT_VMX_H
#define AVCODEC_XBOX360_H264_WEIGHT_VMX_H

#include <stddef.h>
#include <stdint.h>

void ff_weight_h264_pixels16_x360(uint8_t *block, ptrdiff_t stride, int height,
                                  int log2_denom, int weight, int offset);
void ff_weight_h264_pixels8_x360(uint8_t *block, ptrdiff_t stride, int height,
                                 int log2_denom, int weight, int offset);
void ff_weight_h264_pixels4_x360(uint8_t *block, ptrdiff_t stride, int height,
                                 int log2_denom, int weight, int offset);

void ff_biweight_h264_pixels16_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                    int height, int log2_denom, int weightd,
                                    int weights, int offset);
void ff_biweight_h264_pixels8_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                   int height, int log2_denom, int weightd,
                                   int weights, int offset);
void ff_biweight_h264_pixels4_x360(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
                                   int height, int log2_denom, int weightd,
                                   int weights, int offset);

#endif /* AVCODEC_XBOX360_H264_WEIGHT_VMX_H */
