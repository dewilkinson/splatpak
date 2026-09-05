// ZstdDecompressor.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Runtime-side chunk decompressor: reverses ByteShuffle's shuffle+RLE codec back into a
// PackedSurfelGPU array. Named for the format's eventual real entropy coder; today it
// wraps the lightweight ByteShuffle RLE codec described in ByteShuffle.h.

#pragma once
#include <vector>
#include <cstdint>
#include "WaveletTypes.h"
#include "../../libs/SurfelsCore/ByteShuffle.h"

namespace Surfels
{
    class ZstdDecompressor
    {
    public:
        // Decompresses a compressed byte stream into an array of PackedSurfelGPU
        static bool DecompressChunk(
            const uint8_t* compressedData,
            size_t compressedSize,
            uint32_t surfelCount,
            std::vector<PackedSurfelGPU>& outPackedSurfels)
        {
            outPackedSurfels.resize(surfelCount);
            size_t expectedBytes = surfelCount * sizeof(PackedSurfelGPU);

            // 1. Decompress run-length byte stream
            std::vector<uint8_t> shuffled(expectedBytes);
            if (!ByteShuffle::DecompressShuffled(compressedData, compressedSize, shuffled.data(), expectedBytes))
            {
                return false;
            }

            // 2. Unshuffle bytes back to interleaved PackedSurfelGPU layout
            ByteShuffle::Unshuffle(
                shuffled.data(),
                reinterpret_cast<uint8_t*>(outPackedSurfels.data()),
                surfelCount,
                sizeof(PackedSurfelGPU)
            );

            return true;
        }
    };
}

