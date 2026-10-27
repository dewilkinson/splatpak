// SplatPlanes.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Compact splat planes (docs/design/compact-splat-planes.md): fixed-size, randomly addressable arrays, one per kind of
// field, each in the representation that suits its range. Store compact, compute in fp32: Decode before any arithmetic.
//   PositionPlane  8 bytes: 21-bit fixed point per axis over the scan's bounds, packed in one uint64 (x in bits 0..20,
//                  y in 21..41, z in 42..62)
//   ShapePlane    16 bytes: log-scales fp16, opacity logit fp16, rotation smallest-three (3 x 16 bits plus a 2-bit index)
//   ColourPlane    6 bytes: the DC colour, unorm16 per channel (SplatPoint::color is clamped to [0, 1])

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <immintrin.h>
#include <thread>
#include <vector>
#include "WaveletTypes.h"

namespace Splat
{
    // f(i) for every i in [0, n), on all cores (at least 64k items per thread)
    template <class F>
    void PlaneParallelFor(size_t n, F&& f)
    {
        const unsigned threads = std::max(1u, std::min(std::thread::hardware_concurrency(), (unsigned)(n / 65536 + 1)));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++)
            pool.emplace_back([&, t]()
            {
                const size_t first = n * t / threads, last = n * (t + 1) / threads;
                for (size_t i = first; i < last; i++) f(i);
            });
        for (auto& th : pool) th.join();
    }

    // fp16 through F16C; unorm16 over [0, 1]
    inline uint16_t ToHalf(float v) { return (uint16_t)_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(v), _MM_FROUND_TO_NEAREST_INT), 0); }
    inline float FromHalf(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
    inline uint16_t ToUnorm16(float v) { return (uint16_t)(std::min(1.0f, std::max(0.0f, v)) * 65535.0f + 0.5f); }
    inline float FromUnorm16(uint16_t u) { return u * (1.0f / 65535.0f); }

    namespace PositionPlane
    {
        constexpr uint32_t kBits = 21;
        constexpr uint32_t kMax = (1u << kBits) - 1u;

        // The box the positions are quantised over: the scan's bounds (an extent of zero on an axis is widened to 1e-6)
        struct Frame
        {
            XMFLOAT3 min = { 0, 0, 0 };
            XMFLOAT3 extent = { 1, 1, 1 };
        };

        inline Frame FrameOf(const XMFLOAT3& mn, const XMFLOAT3& mx)
        {
            Frame f;
            f.min = mn;
            f.extent = XMFLOAT3(std::max(1e-6f, mx.x - mn.x), std::max(1e-6f, mx.y - mn.y), std::max(1e-6f, mx.z - mn.z));
            return f;
        }

        inline uint32_t Quantise(float p, float mn, float extent)
        {
            const float t = std::min(1.0f, std::max(0.0f, (p - mn) / extent));
            return (uint32_t)(t * (float)kMax + 0.5f);
        }

        inline uint64_t Encode(const XMFLOAT3& p, const Frame& f)
        {
            return (uint64_t)Quantise(p.x, f.min.x, f.extent.x) | ((uint64_t)Quantise(p.y, f.min.y, f.extent.y) << kBits) |
                   ((uint64_t)Quantise(p.z, f.min.z, f.extent.z) << (2 * kBits));
        }

        inline uint32_t AxisX(uint64_t v) { return (uint32_t)(v & kMax); }
        inline uint32_t AxisY(uint64_t v) { return (uint32_t)((v >> kBits) & kMax); }
        inline uint32_t AxisZ(uint64_t v) { return (uint32_t)((v >> (2 * kBits)) & kMax); }

        inline XMFLOAT3 Decode(uint64_t v, const Frame& f)
        {
            const float s = 1.0f / (float)kMax;
            return XMFLOAT3(f.min.x + AxisX(v) * s * f.extent.x, f.min.y + AxisY(v) * s * f.extent.y, f.min.z + AxisZ(v) * s * f.extent.z);
        }

        // A 30-bit Morton code straight from the integers: the top 10 bits of each axis interleaved (z, y, x from the top,
        // as SpatialOctree::ComputeMorton30). NOT the runtime's curve, which quantises floor(t * 1023): see the design note.
        inline uint32_t Dilate10(uint32_t x)
        {
            x &= 0x000003ff;
            x = (x ^ (x << 16)) & 0xff0000ff;
            x = (x ^ (x << 8)) & 0x0300f00f;
            x = (x ^ (x << 4)) & 0x030c30c3;
            x = (x ^ (x << 2)) & 0x09249249;
            return x;
        }
        inline uint32_t Morton30(uint64_t v)
        {
            constexpr uint32_t shift = kBits - 10;
            return (Dilate10(AxisZ(v) >> shift) << 2) | (Dilate10(AxisY(v) >> shift) << 1) | Dilate10(AxisX(v) >> shift);
        }

        // Every point's position into plane (sized to points.size()), on all cores
        template <class PointArray>
        void Build(const PointArray& points, const Frame& f, std::vector<uint64_t>& plane)
        {
            plane.resize(points.size());
            PlaneParallelFor(points.size(), [&](size_t i) { plane[i] = Encode(points[i].position, f); });
        }
    }

    namespace ShapePlane
    {
        // One Gaussian's shape. Scales are stored as their natural log and opacity as its logit (the .ply's own forms), so
        // fp16 keeps about three significant digits of each at any size: for opacity that is both the opacity near 0 and
        // the transmittance 1 - opacity near 1, which the merge raises to a power (unorm16 rounded every opacity within
        // 8e-6 of 1 to exactly 1, a transmittance of 0, and moved a tenth of the merged Gaussians a step or more). The
        // rotation drops its largest component (recomputed on decode; the whole quaternion's sign makes it positive)
        // and keeps the other three as unorm16 over [-1/sqrt2, +1/sqrt2].
        struct Record
        {
            uint16_t logScale[3];
            uint16_t logitOpacity;
            uint64_t rotation; // bits 0..1 the dropped component, then 16 bits per kept component in order
        };
        static_assert(sizeof(Record) == 16, "ShapePlane::Record is 16 bytes");

        constexpr float kRotationRange = 0.70710678f;

        inline uint64_t EncodeRotation(const XMFLOAT4& q)
        {
            const float c[4] = { q.x, q.y, q.z, q.w };
            const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
            int largest = 0;
            for (int k = 1; k < 4; k++) if (std::fabs(c[k]) > std::fabs(c[largest])) largest = k;
            const float sign = (c[largest] < 0.0f ? -1.0f : 1.0f) / (len > 0.0f ? len : 1.0f);
            uint64_t v = (uint64_t)largest;
            int shift = 2;
            for (int k = 0; k < 4; k++)
            {
                if (k == largest) continue;
                v |= (uint64_t)ToUnorm16(c[k] * sign / kRotationRange * 0.5f + 0.5f) << shift;
                shift += 16;
            }
            return v;
        }

        inline XMFLOAT4 DecodeRotation(uint64_t v)
        {
            const int largest = (int)(v & 3);
            float c[4] = {};
            float sum = 0.0f;
            int shift = 2;
            for (int k = 0; k < 4; k++)
            {
                if (k == largest) continue;
                c[k] = (FromUnorm16((uint16_t)(v >> shift)) * 2.0f - 1.0f) * kRotationRange;
                sum += c[k] * c[k];
                shift += 16;
            }
            c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
            return XMFLOAT4(c[0], c[1], c[2], c[3]);
        }

        inline Record Encode(const SplatAttributes& a)
        {
            Record r;
            r.logScale[0] = ToHalf(std::log(std::max(1e-30f, a.scale.x)));
            r.logScale[1] = ToHalf(std::log(std::max(1e-30f, a.scale.y)));
            r.logScale[2] = ToHalf(std::log(std::max(1e-30f, a.scale.z)));
            const float o = std::min(1.0f, std::max(0.0f, a.opacity));
            r.logitOpacity = ToHalf(std::log(o) - std::log1p(-o)); // 0 and 1 go to -inf and +inf, which decode exactly
            r.rotation = EncodeRotation(a.rotation);
            return r;
        }

        // Writes a's scale, rotation and opacity; its other fields are left as they are
        inline void Decode(const Record& r, SplatAttributes& a)
        {
            a.scale = XMFLOAT3(std::exp(FromHalf(r.logScale[0])), std::exp(FromHalf(r.logScale[1])), std::exp(FromHalf(r.logScale[2])));
            a.opacity = 1.0f / (1.0f + std::exp(-FromHalf(r.logitOpacity)));
            a.rotation = DecodeRotation(r.rotation);
        }

        template <class AttributeArray>
        void Build(const AttributeArray& attributes, std::vector<Record>& plane)
        {
            plane.resize(attributes.size());
            PlaneParallelFor(attributes.size(), [&](size_t i) { plane[i] = Encode(attributes[i]); });
        }
    }

    namespace ColourPlane
    {
        // unorm16 rather than fp16: the colour lies in [0, 1], where fp16's step near 1 is 4.9e-4 and unorm16's 1.5e-5
        struct Record { uint16_t rgb[3]; };
        static_assert(sizeof(Record) == 6, "ColourPlane::Record is 6 bytes");

        inline Record Encode(const XMFLOAT3& c) { return { { ToUnorm16(c.x), ToUnorm16(c.y), ToUnorm16(c.z) } }; }
        inline XMFLOAT3 Decode(const Record& r) { return XMFLOAT3(FromUnorm16(r.rgb[0]), FromUnorm16(r.rgb[1]), FromUnorm16(r.rgb[2])); }

        template <class PointArray>
        void Build(const PointArray& points, std::vector<Record>& plane)
        {
            plane.resize(points.size());
            PlaneParallelFor(points.size(), [&](size_t i) { plane[i] = Encode(points[i].color); });
        }
    }
}
