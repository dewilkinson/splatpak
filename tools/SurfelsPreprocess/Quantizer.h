#pragma once
#include <vector>
#include "../../src/DX12/Wavelet/WaveletTypes.h"

namespace Surfels
{
    class Quantizer
    {
    public:
        // Quantizes an array of uncompressed SurfelVertex into packed 8-byte PackedSurfelGPU
        static std::vector<PackedSurfelGPU> QuantizeSurfels(
            const std::vector<SurfelVertex>& surfels,
            XMFLOAT3 aabbMin,
            XMFLOAT3 aabbMax)
        {
            std::vector<PackedSurfelGPU> packed(surfels.size());
            XMFLOAT3 aabbExtents(
                aabbMax.x - aabbMin.x + 1e-5f,
                aabbMax.y - aabbMin.y + 1e-5f,
                aabbMax.z - aabbMin.z + 1e-5f
            );

            for (size_t i = 0; i < surfels.size(); i++)
            {
                const auto& s = surfels[i];
                PackedSurfelGPU p;

                // Logarithmic radius exponent [0..3]
                uint32_t radExp = 0;
                if (s.radius > 0.2f) radExp = 3;
                else if (s.radius > 0.1f) radExp = 2;
                else if (s.radius > 0.05f) radExp = 1;
                else radExp = 0;

                p.packedPosRadius = PackPosRadius(s.position, aabbMin, aabbExtents, radExp);
                p.packedNormal = PackNormalOct16(s.normal);
                p.packedColor = PackColorRGB565(s.color);

                packed[i] = p;
            }

            return packed;
        }

        // Unquantizes packed 8-byte surfels back to SurfelVertex
        static std::vector<SurfelVertex> UnquantizeSurfels(
            const std::vector<PackedSurfelGPU>& packed,
            XMFLOAT3 aabbMin,
            XMFLOAT3 aabbMax)
        {
            std::vector<SurfelVertex> surfels(packed.size());
            XMFLOAT3 aabbExtents(
                aabbMax.x - aabbMin.x + 1e-5f,
                aabbMax.y - aabbMin.y + 1e-5f,
                aabbMax.z - aabbMin.z + 1e-5f
            );

            for (size_t i = 0; i < packed.size(); i++)
            {
                const auto& p = packed[i];
                SurfelVertex s;
                s.position = UnpackPos(p.packedPosRadius, aabbMin, aabbExtents);
                s.normal = UnpackNormalOct16(p.packedNormal);
                s.color = UnpackColorRGB565(p.packedColor);

                uint32_t radExp = (p.packedPosRadius >> 30) & 0x3;
                static const float s_radMultipliers[4] = { 0.035f, 0.06f, 0.12f, 0.25f };
                s.radius = s_radMultipliers[radExp];

                surfels[i] = s;
            }

            return surfels;
        }
    };
}

