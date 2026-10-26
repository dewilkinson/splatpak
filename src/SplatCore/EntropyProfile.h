// EntropyProfile.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// What the lossless tiers before the codec do to the data, in bits per byte, for the 5-Tier Compression panel. Order-0
// entropy of a stream cannot show them: Tier 3 (Morton order, byte planes) only moves bytes, and a histogram does not
// change when its bytes are reordered. So each tier is measured by what a simple coder could exploit at that point:
//
//   Tier 2  the records and harmonics interleaved, as the GPU holds them: order-0 entropy of the stream.
//   Tier 3  in Morton order and split into byte planes: per plane, the better of its order-0 entropy and the order-0
//           entropy of its deltas (each byte minus the same byte of the previous record), so a plane gains when its
//           field has its own narrow distribution, or when Morton order makes neighbouring records alike.
//   Tier 4  the same, after the value transforms the package applies: colour as YCoCg, and every level's harmonics as
//           the residual from its parent in the next coarser level (SHCoding.h), the coarsest level as YCoCg.
//
// Measured on samples: runs of records in stored order, each with its parent level for the residuals.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../libs/gstx/ColourTransform.h"
#include "../../libs/gstx/SHCoding.h"

namespace Splat
{
    struct EntropyProfile
    {
        bool   valid = false;
        double tier2 = 0.0, tier3 = 0.0, tier4 = 0.0; // Bits per byte, records and harmonics together (weighted by bytes)
        double records[3] = {}, sh[3] = {};           // The same, per stream (tiers 2, 3, 4)
        double seconds = 0.0;
    };

    // A run of records in stored order, with its parent level when the harmonics are coded as residuals
    struct EntropySample
    {
        const uint8_t* records = nullptr;
        size_t count = 0;
        const uint8_t* sh = nullptr;                   // Absolute harmonics (nullptr: none)
        const uint8_t* parentRecords = nullptr;        // The next coarser level (nullptr: this is the coarsest)
        size_t parentCount = 0;
        const std::vector<uint8_t>* parentSH = nullptr; // Its absolute harmonics
        double weight = 1.0;                           // Bytes this sample stands for, over its own bytes
    };

    namespace Entropy
    {
        inline double Order0(const uint8_t* data, size_t n, size_t step = 1)
        {
            if (n == 0) return 0.0;
            uint32_t hist[256] = {};
            for (size_t i = 0; i < n; i++) hist[data[i * step]]++;
            double h = 0.0;
            for (uint32_t c : hist)
                if (c) { const double p = (double)c / (double)n; h -= p * std::log2(p); }
            return h;
        }

        // Mean over the byte planes of count records of stride bytes: the better of each plane's order-0 entropy and
        // its deltas' (the first record against zero)
        inline double PlaneBest(const uint8_t* data, size_t count, size_t stride)
        {
            if (count == 0 || stride == 0) return 0.0;
            std::vector<uint8_t> delta(count);
            double sum = 0.0;
            for (size_t b = 0; b < stride; b++)
            {
                uint8_t prev = 0;
                for (size_t i = 0; i < count; i++) { const uint8_t v = data[i * stride + b]; delta[i] = (uint8_t)(v - prev); prev = v; }
                sum += std::min(Order0(data + b, count, stride), Order0(delta.data(), count));
            }
            return sum / (double)stride;
        }

        inline EntropyProfile Measure(const std::vector<EntropySample>& samples, size_t recordStride, size_t shStride, const uint8_t bits[3])
        {
            EntropyProfile out;
            double rw = 0.0, sw = 0.0;
            for (const EntropySample& s : samples)
            {
                if (s.count == 0 || !s.records) continue;
                const double rb = (double)(s.count * recordStride) * s.weight;
                std::vector<uint8_t> rec(s.records, s.records + s.count * recordStride);
                out.records[0] += rb * Order0(rec.data(), rec.size());
                out.records[1] += rb * PlaneBest(rec.data(), s.count, recordStride);
                ColourTransform::ApplyRecords(rec.data(), s.count, recordStride);
                out.records[2] += rb * PlaneBest(rec.data(), s.count, recordStride);
                rw += rb;
                if (!s.sh || shStride == 0) continue;
                const double sb = (double)(s.count * shStride) * s.weight;
                std::vector<uint8_t> sh(s.sh, s.sh + s.count * shStride);
                out.sh[0] += sb * Order0(sh.data(), sh.size());
                out.sh[1] += sb * PlaneBest(sh.data(), s.count, shStride);
                if (s.parentRecords && s.parentSH && s.parentCount > 0)
                {
                    // Residuals against the parent level, as the package stores them (the parent is read, not written)
                    std::vector<const uint8_t*> recs = { s.records, s.parentRecords };
                    std::vector<size_t> counts = { s.count, s.parentCount };
                    std::vector<std::vector<uint8_t>*> shs = { &sh, const_cast<std::vector<uint8_t>*>(s.parentSH) };
                    SHCoding::EncodeResiduals(recs, counts, recordStride, shs, shStride, bits);
                }
                else ColourTransform::ApplySH(sh.data(), sh.size());
                out.sh[2] += sb * PlaneBest(sh.data(), s.count, shStride);
                sw += sb;
            }
            if (rw <= 0.0) return out;
            for (int t = 0; t < 3; t++)
            {
                const double total = out.records[t] + out.sh[t];
                out.records[t] /= rw;
                if (sw > 0.0) out.sh[t] /= sw;
                (t == 0 ? out.tier2 : t == 1 ? out.tier3 : out.tier4) = total / (rw + sw);
            }
            out.valid = true;
            return out;
        }
    }
}
