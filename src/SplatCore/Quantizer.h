// Quantizer.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Converts between full-precision SplatPoint and the 8-byte GPU-packed splat
// format (10:10:10:2 position, oct16 normal, RGB565 color) used everywhere on
// the streaming/rendering side. Pack/unpack helpers themselves live in WaveletTypes.h.

#pragma once
#include <vector>
#include "WaveletTypes.h"

namespace Splat
{
    class Quantizer
    {
    public:
        // 2-bit radius exponent bucket: coarse log-scale radius bucketing so it fits in the 2 spare
        // bits left over in packedPosRadius's 10:10:10 position encoding. kRadiusBucketThresholds are
        // the encode-side lower bounds for buckets 3,2,1 (bucket 0 catches everything below); these are
        // hand-tuned buckets, not a mathematically exact round-trip, so kRadiusBucketMultipliers (used
        // by UnquantizePoints below) are separately chosen to sit near the middle of each bucket's range.
        static constexpr float kRadiusBucketThresholds[3]  = { 0.2f, 0.1f, 0.05f };
        static constexpr float kRadiusBucketMultipliers[4] = { 0.035f, 0.06f, 0.12f, 0.25f };

        // Quantizes an array of uncompressed SplatPoint into packed 8-byte PackedPointGPU
        static std::vector<PackedPointGPU> QuantizePoints(
            const std::vector<SplatPoint>& splats,
            XMFLOAT3 aabbMin,
            XMFLOAT3 aabbMax)
        {
            std::vector<PackedPointGPU> packed(splats.size());
            XMFLOAT3 aabbExtents(
                aabbMax.x - aabbMin.x + 1e-5f,
                aabbMax.y - aabbMin.y + 1e-5f,
                aabbMax.z - aabbMin.z + 1e-5f
            );

            for (size_t i = 0; i < splats.size(); i++)
            {
                const auto& s = splats[i];
                PackedPointGPU p;

                // Logarithmic radius exponent [0..3]
                uint32_t radExp = 0;
                if (s.radius > kRadiusBucketThresholds[0]) radExp = 3;
                else if (s.radius > kRadiusBucketThresholds[1]) radExp = 2;
                else if (s.radius > kRadiusBucketThresholds[2]) radExp = 1;
                else radExp = 0;

                p.packedPosRadius = PackPosRadius(s.position, aabbMin, aabbExtents, radExp);
                p.packedNormal = PackNormalOct16(s.normal);
                p.packedColor = PackColorRGB565(s.color);

                packed[i] = p;
            }

            return packed;
        }

        // Unquantizes packed 8-byte splats back to SplatPoint
        static std::vector<SplatPoint> UnquantizePoints(
            const std::vector<PackedPointGPU>& packed,
            XMFLOAT3 aabbMin,
            XMFLOAT3 aabbMax)
        {
            std::vector<SplatPoint> splats(packed.size());
            XMFLOAT3 aabbExtents(
                aabbMax.x - aabbMin.x + 1e-5f,
                aabbMax.y - aabbMin.y + 1e-5f,
                aabbMax.z - aabbMin.z + 1e-5f
            );

            for (size_t i = 0; i < packed.size(); i++)
            {
                const auto& p = packed[i];
                SplatPoint s;
                s.position = UnpackPos(p.packedPosRadius, aabbMin, aabbExtents);
                s.normal = UnpackNormalOct16(p.packedNormal);
                s.color = UnpackColorRGB565(p.packedColor);

                uint32_t radExp = (p.packedPosRadius >> 30) & 0x3;
                s.radius = kRadiusBucketMultipliers[radExp];

                splats[i] = s;
            }

            return splats;
        }
    };
}

