// ColourTransform.h
// SplatPak Core -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The colour decorrelation of .gstx v11 payloads (Clause 8.1 of docs/GSTX_FORMAT_SPECIFICATION.md): RGB byte
// triples stored as reversible YCoCg-R, computed modulo 256, so the entropy stage sees one luma plane and two small
// chroma planes instead of three correlated channels. A writer applies it to the interleaved records before the
// byte shuffle; a reader undoes it after the unshuffle. Open code: the transform is published and standard, and
// every reader needs it. Header-only.

#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

namespace Splat
{
    // SFLWFileHeader::payloadTransforms: which streams carry which transform
    enum PayloadTransformBits : uint32_t
    {
        PAYLOAD_TRANSFORM_RECORD_YCOCG = 1u << 0, // Splat records: the colour bytes (offset 10, 11, 12)
        PAYLOAD_TRANSFORM_SH_YCOCG     = 1u << 1, // SH stream: every coefficient's R, G, B triple
        PAYLOAD_TRANSFORM_SH_RESIDUAL  = 1u << 2, // SH stream of every level but a chunk's coarsest: residuals from the next coarser level (SHCoding.h)
        PAYLOAD_SH_BITS_SHIFT          = 8,       // Bits 8-11, 12-15, 16-19: SH Precision of bands 1, 2, 3 (1 to 7; 0 = 8), which a reader
        PAYLOAD_SH_BITS_MASK           = 0xFFFu << 8, // of residuals needs to predict as the writer did
        PAYLOAD_TRANSFORMS_DEFAULT     = PAYLOAD_TRANSFORM_RECORD_YCOCG | PAYLOAD_TRANSFORM_SH_YCOCG | PAYLOAD_TRANSFORM_SH_RESIDUAL, // What a writer sets
        PAYLOAD_TRANSFORMS_KNOWN       = PAYLOAD_TRANSFORMS_DEFAULT | PAYLOAD_SH_BITS_MASK, // Every bit a reader understands
    };

    struct ColourTransform
    {
        static constexpr size_t kRecordColourOffset = 10; // r, g, b of a 20-byte splat record (Clause 7.2)

        // Half of a byte taken as signed, rounded down (an arithmetic shift)
        static inline uint8_t Half(uint8_t v) { return (uint8_t)((int8_t)v >> 1); }

        // (r, g, b) -> (y, co, cg) and back; each lifting step adds a function of values the other side has, so
        // the inverse is exact for all 2^24 inputs
        static inline void Forward(uint8_t& a, uint8_t& b, uint8_t& c)
        {
            const uint8_t r = a, g = b, bl = c;
            const uint8_t co = (uint8_t)(r - bl);
            const uint8_t t  = (uint8_t)(bl + Half(co));
            const uint8_t cg = (uint8_t)(g - t);
            const uint8_t y  = (uint8_t)(t + Half(cg));
            a = y; b = co; c = cg;
        }
        static inline void Inverse(uint8_t& a, uint8_t& b, uint8_t& c)
        {
            const uint8_t y = a, co = b, cg = c;
            const uint8_t t  = (uint8_t)(y - Half(cg));
            const uint8_t g  = (uint8_t)(cg + t);
            const uint8_t bl = (uint8_t)(t - Half(co));
            const uint8_t r  = (uint8_t)(bl + co);
            a = r; b = g; c = bl;
        }

        // Splat records, interleaved: the colour triple of each
        static void ApplyRecords(uint8_t* records, size_t count, size_t stride)
        {
            for (size_t i = 0; i < count; i++) { uint8_t* p = records + i * stride + kRecordColourOffset; Forward(p[0], p[1], p[2]); }
        }
        static void UndoRecords(uint8_t* records, size_t count, size_t stride)
        {
            for (size_t i = 0; i < count; i++) { uint8_t* p = records + i * stride + kRecordColourOffset; Inverse(p[0], p[1], p[2]); }
        }

        // SH stream, interleaved: every triple of every entry (stride is a multiple of 3, Clause 9.1)
        static void ApplySH(uint8_t* sh, size_t bytes)
        {
            for (size_t i = 0; i + 2 < bytes; i += 3) Forward(sh[i], sh[i + 1], sh[i + 2]);
        }
        static void UndoSH(uint8_t* sh, size_t bytes)
        {
            for (size_t i = 0; i + 2 < bytes; i += 3) Inverse(sh[i], sh[i + 1], sh[i + 2]);
        }

        // Mean order-0 entropy, in bits per byte, over the byte planes of count interleaved records of stride bytes:
        // what the figures the bake and the save report are measured with. hist is reusable scratch.
        static double MeanPlaneEntropy(const uint8_t* records, size_t count, size_t stride)
        {
            if (count == 0 || stride == 0) return 0.0;
            double sum = 0.0;
            for (size_t b = 0; b < stride; b++)
            {
                size_t hist[256] = {};
                for (size_t i = 0; i < count; i++) hist[records[i * stride + b]]++;
                double e = 0.0;
                for (size_t h : hist)
                    if (h) { const double q = (double)h / (double)count; e -= q * std::log2(q); }
                sum += e;
            }
            return sum / (double)stride;
        }
    };
}
