//
// Copyright 2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// copyimage.cpp: Defines image copying functions

#include "image_util/copyimage.h"
#include "common/unsafe_buffers.h"

#if ((defined(__ARM_NEON__) || defined(__ARM_NEON)) && defined(__aarch64__))
#    include <arm_neon.h>
#endif

namespace angle
{

namespace
{
inline uint32_t SwizzleBGRAToRGBA(uint32_t argb)
{
    return ((argb & 0x000000FF) << 16) |  // Move BGRA blue to RGBA blue
           ((argb & 0x00FF0000) >> 16) |  // Move BGRA red to RGBA red
           ((argb & 0xFF00FF00));         // Keep alpha and green
}

void CopyBGRA8ToRGBA8Fast(const uint8_t *source,
                          int srcYAxisPitch,
                          uint8_t *dest,
                          int destYAxisPitch,
                          int destWidth,
                          int destHeight)
{
#if ((defined(__ARM_NEON__) || defined(__ARM_NEON)) && defined(__aarch64__))
    // NEON can process 4 pixels per iteration using a lookup table.
    // For correctness on little-endian, the mask bytes are arranged as {B,G,R,A} -> {R,G,B,A}.
    static const uint8_t shuffleMaskData[16] = {
        2, 1, 0, 3,  6, 5, 4, 7,  10, 9, 8, 11,  14, 13, 12, 15
    };
    uint8x16_t shuffleMask = vld1q_u8(shuffleMaskData);

    // Only use SIMD if width is large enough and pitches are 4-byte aligned.
    if (destWidth >= 4 && (srcYAxisPitch % 4 == 0) && (destYAxisPitch % 4 == 0))
    {
        const int srcStride = srcYAxisPitch / 4;
        const int dstStride = destYAxisPitch / 4;
        for (int y = 0; y < destHeight; ++y)
        {
            const uint32_t *src32 = reinterpret_cast<const uint32_t*>(source + y * srcYAxisPitch);
            uint32_t *dst32       = reinterpret_cast<uint32_t*>(dest + y * destYAxisPitch);
            int x = 0;
            for (; x + 3 < destWidth; x += 4)
            {
                uint8x16_t in = vld1q_u8(reinterpret_cast<const uint8_t*>(&src32[x]));
                uint8x16_t out = vqtbl1q_u8(in, shuffleMask);
                vst1q_u8(reinterpret_cast<uint8_t*>(&dst32[x]), out);
            }
            // Handle remaining pixels with scalar.
            for (; x < destWidth; ++x)
            {
                dst32[x] = SwizzleBGRAToRGBA(src32[x]);
            }
        }
        return;
    }
#endif

    // Fallback scalar loop.
    for (int y = 0; y < destHeight; ++y)
    {
        const uint32_t *src32 =
            reinterpret_cast<const uint32_t *>(ANGLE_UNSAFE_TODO(source + y * srcYAxisPitch));
        uint32_t *dest32 =
            reinterpret_cast<uint32_t *>(ANGLE_UNSAFE_TODO(dest + y * destYAxisPitch));
        const uint32_t *end32 = ANGLE_UNSAFE_TODO(src32 + destWidth);
        while (src32 != end32)
        {
            ANGLE_UNSAFE_TODO(*dest32++ = SwizzleBGRAToRGBA(*src32++));
        }
    }
}

void CopyRGBA8ToRGBA8Fast(const uint8_t *source,
                          int srcYAxisPitch,
                          uint8_t *dest,
                          int destYAxisPitch,
                          int destWidth,
                          int destHeight)
{
    // If Y axis pitch is packed and the source is also packed and stored contiguously, copy the
    // whole source to the dest in a single memcpy operation.
    if (destYAxisPitch == destWidth * 4 && srcYAxisPitch == destWidth * 4)
    {
        size_t totalSize = destHeight * destWidth * 4;
        ANGLE_UNSAFE_TODO(memcpy(dest, source, totalSize));
        return;
    }

    // If X axis pitch is 4 bytes but Y axis pitch is not packed, copy the source to dest line by
    // line.
    for (int y = 0; y < destHeight; ++y)
    {
        const uint8_t *src = ANGLE_UNSAFE_TODO(source + y * srcYAxisPitch);
        uint8_t *dst       = ANGLE_UNSAFE_TODO(dest + y * destYAxisPitch);
        ANGLE_UNSAFE_TODO(memcpy(dst, src, destWidth * 4));
    }
}
}  // namespace

void CopyBGRA8ToRGBA8(const uint8_t *source,
                      int srcXAxisPitch,
                      int srcYAxisPitch,
                      uint8_t *dest,
                      int destXAxisPitch,
                      int destYAxisPitch,
                      int destWidth,
                      int destHeight)
{
    if (srcXAxisPitch == 4 && destXAxisPitch == 4)
    {
        CopyBGRA8ToRGBA8Fast(source, srcYAxisPitch, dest, destYAxisPitch, destWidth, destHeight);
        return;
    }

    for (int y = 0; y < destHeight; ++y)
    {
        uint8_t *dst       = ANGLE_UNSAFE_TODO(dest + y * destYAxisPitch);
        const uint8_t *src = ANGLE_UNSAFE_TODO(source + y * srcYAxisPitch);
        const uint8_t *end = ANGLE_UNSAFE_TODO(src + destWidth * srcXAxisPitch);

        while (src != end)
        {
            *reinterpret_cast<uint32_t *>(dst) =
                SwizzleBGRAToRGBA(*reinterpret_cast<const uint32_t *>(src));
            ANGLE_UNSAFE_TODO({
                src += srcXAxisPitch;
                dst += destXAxisPitch;
            })
        }
    }
}

void CopyRGBA8ToRGBA8(const uint8_t *source,
                      int srcXAxisPitch,
                      int srcYAxisPitch,
                      uint8_t *dest,
                      int destXAxisPitch,
                      int destYAxisPitch,
                      int destWidth,
                      int destHeight)
{
    if (srcXAxisPitch == 4 && destXAxisPitch == 4)
    {
        CopyRGBA8ToRGBA8Fast(source, srcYAxisPitch, dest, destYAxisPitch, destWidth, destHeight);
        return;
    }

    for (int y = 0; y < destHeight; ++y)
    {
        uint8_t *dst       = ANGLE_UNSAFE_TODO(dest + y * destYAxisPitch);
        const uint8_t *src = ANGLE_UNSAFE_TODO(source + y * srcYAxisPitch);
        const uint8_t *end = ANGLE_UNSAFE_TODO(src + destWidth * srcXAxisPitch);

        while (src != end)
        {
            *reinterpret_cast<uint32_t *>(dst) = *reinterpret_cast<const uint32_t *>(src);
            ANGLE_UNSAFE_TODO({
                src += srcXAxisPitch;
                dst += destXAxisPitch;
            })
        }
    }
}

}  // namespace angle