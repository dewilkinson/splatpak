// ByteShuffle.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open-build codec: transpose only, no entropy coding (see ByteShuffle.h). Kept as its own
// pass so a real coder can be dropped in later without touching any caller.

#include "ByteShuffle.h"
#include <cstring>

namespace Splat
{
    std::vector<uint8_t> ByteShuffle::Shuffle(const uint8_t* src, size_t numElements, size_t elementSize)
    {
        size_t totalBytes = numElements * elementSize;
        std::vector<uint8_t> dst(totalBytes);

        for (size_t c = 0; c < elementSize; c++)
        {
            size_t channelOffset = c * numElements;
            for (size_t i = 0; i < numElements; i++)
            {
                dst[channelOffset + i] = src[i * elementSize + c];
            }
        }

        return dst;
    }

    void ByteShuffle::Unshuffle(const uint8_t* src, uint8_t* dst, size_t numElements, size_t elementSize)
    {
        for (size_t c = 0; c < elementSize; c++)
        {
            size_t channelOffset = c * numElements;
            for (size_t i = 0; i < numElements; i++)
            {
                dst[i * elementSize + c] = src[channelOffset + i];
            }
        }
    }

    std::vector<uint8_t> ByteShuffle::CompressShuffled(const std::vector<uint8_t>& in)
    {
        // Straight pass-through: see the "PUBLIC BUILD NOTE" in ByteShuffle.h.
        return in;
    }

    bool ByteShuffle::DecompressShuffled(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstExpectedSize)
    {
        if (srcSize != dstExpectedSize) return false;
        std::memcpy(dst, src, dstExpectedSize);
        return true;
    }
}
