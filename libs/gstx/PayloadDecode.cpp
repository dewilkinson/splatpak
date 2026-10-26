// PayloadDecode.cpp
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// See PayloadDecode.h.

#include "PayloadDecode.h"
#include "../../src/SplatCore/WaveletTypes.h"
#include "ByteShuffle.h"
#include <brotli/decode.h>

namespace Splat
{
    bool PayloadDecode::Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize, uint32_t codec)
    {
        if (codec == PAYLOAD_CODEC_LIBRARY) return ByteShuffle::DecompressShuffled(src, srcSize, dst, dstSize);
        if (codec != PAYLOAD_CODEC_BROTLI) return false;
        if (dstSize == 0) return srcSize == 0;
        size_t decoded = dstSize;
        return BrotliDecoderDecompress(srcSize, src, &decoded, dst) == BROTLI_DECODER_RESULT_SUCCESS && decoded == dstSize;
    }

    bool PayloadDecode::DecodeRecords(const uint8_t* src, size_t srcSize, size_t count, size_t recordBytes, uint32_t codec, std::vector<uint8_t>& outRecords)
    {
        const size_t bytes = count * recordBytes;
        std::vector<uint8_t> shuffled(bytes);
        if (!Decompress(src, srcSize, shuffled.data(), bytes, codec)) return false;
        outRecords.resize(bytes);
        ByteShuffle::Unshuffle(shuffled.data(), outRecords.data(), count, recordBytes);
        return true;
    }
}
