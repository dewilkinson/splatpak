// PayloadDecode.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Decoding of .gstx payloads (Clause 8 of docs/GSTX_FORMAT_SPECIFICATION.md), inside the codec library
// so every host (SplatLab, the streaming runtime, the UE5 plugin) reads packages through one reader.
// Open code: the entropy stages it undoes are the codec library's own run-length stage (payload codec 0)
// and Brotli (payload codec 1, RFC 7932; libs/brotli is compiled into this library).

#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>

namespace Splat
{
    class PayloadDecode
    {
    public:
        // Undoes the entropy stage of one payload: false when the bytes are not a payload of this codec or do
        // not decode to exactly dstSize bytes. codec is the header's payloadCodec: 0 = run-length stage, 1 = Brotli.
        static bool Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize, uint32_t codec);

        // One payload all the way back to interleaved records: entropy stage, then the byte-plane transpose
        // undone. recordBytes is the record (or SH) stride. outRecords gets count * recordBytes bytes.
        static bool DecodeRecords(const uint8_t* src, size_t srcSize, size_t count, size_t recordBytes, uint32_t codec, std::vector<uint8_t>& outRecords);
    };
}
