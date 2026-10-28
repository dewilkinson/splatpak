// ByteShuffle.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// PUBLIC BUILD NOTE: the private version of this project follows the byte-plane transpose
// with a run-length entropy codec. This open build keeps the transpose (a standard,
// well-published technique -- see e.g. TIFF's horizontal predictor, HDF5 shuffle filters)
// but stores the shuffled bytes uncompressed rather than run-length coding them: packages
// built with this codebase are correspondingly larger on disk, not smaller. Same function
// signatures as the private build, so nothing else in the codebase needs to change.

#pragma once
#include <vector>
#include <cstdint>

namespace Splat
{
    class ByteShuffle
    {
    public:
        // Transposes structured bytes so identical channels sit contiguously
        static std::vector<uint8_t> Shuffle(const uint8_t* src, size_t numElements, size_t elementSize);

        // Untransposes shuffled bytes back to interleaved struct layout
        static void Unshuffle(const uint8_t* src, uint8_t* dst, size_t numElements, size_t elementSize);

        // Open build: straight pass-through, no entropy coding (see file header above)
        static std::vector<uint8_t> CompressShuffled(const std::vector<uint8_t>& in);

        // Open build: straight pass-through, no entropy coding (see file header above)
        static bool DecompressShuffled(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstExpectedSize);
    };
}
