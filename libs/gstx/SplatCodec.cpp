// SplatCodec.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open stand-in for the splat-mode encoder and decoder (see README.md in this directory). It
// writes and reads the record layout documented in SplatCodec.h and docs/GSTX_FORMAT_SPECIFICATION.md
// with plain rounding everywhere, keeps no spherical harmonics above the DC term, and derives the
// coarser levels by a fixed footprint growth. Packages it writes carry no SH stream.

#include "SplatCodec.h"
#include <algorithm>
#include <cmath>

namespace Splat
{
    namespace
    {
        inline uint32_t Unorm(float v, uint32_t maxCode) { return (uint32_t)(std::min(1.0f, std::max(0.0f, v)) * (float)maxCode + 0.5f); }
        inline float FromUnorm(uint32_t code, uint32_t maxCode) { return (float)code / (float)maxCode; }

        inline uint32_t PackQuaternion(XMFLOAT4 q)
        {
            float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (len < 1e-8f) { q = XMFLOAT4(0, 0, 0, 1); len = 1.0f; }
            q.x /= len; q.y /= len; q.z /= len; q.w /= len;
            const float c[4] = { q.x, q.y, q.z, q.w };
            uint32_t largest = 0;
            for (uint32_t i = 1; i < 4; i++) if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
            const float sign = c[largest] < 0.0f ? -1.0f : 1.0f;
            uint32_t packed = largest, shift = 2;
            for (uint32_t i = 0; i < 4; i++)
            {
                if (i == largest) continue;
                packed |= Unorm((c[i] * sign * 1.41421356f) * 0.5f + 0.5f, 1023) << shift;
                shift += 10;
            }
            return packed;
        }

        inline XMFLOAT4 UnpackQuaternion(uint32_t packed)
        {
            const uint32_t largest = packed & 3u;
            float c[4] = { 0, 0, 0, 0 }, sumSq = 0.0f;
            uint32_t shift = 2;
            for (uint32_t i = 0; i < 4; i++)
            {
                if (i == largest) continue;
                c[i] = (FromUnorm((packed >> shift) & 0x3FFu, 1023) * 2.0f - 1.0f) * 0.70710678f;
                sumSq += c[i] * c[i];
                shift += 10;
            }
            c[largest] = std::sqrt(std::max(0.0f, 1.0f - sumSq));
            return XMFLOAT4(c[0], c[1], c[2], c[3]);
        }
    }

    uint32_t SplatCodec::SHCoefficientsPerChannel(uint32_t degree) { degree = std::min(degree, 3u); return (degree + 1) * (degree + 1) - 1; }
    uint32_t SplatCodec::SHRecordBytes(uint32_t degree) { return 3u * SHCoefficientsPerChannel(degree); }

    void SplatCodec::ComputeScaleRange(const SplatAttributeArray& attributes, uint32_t maxLODLevels, float& outLog2Min, float& outLog2Max)
    {
        float lo = 1e9f, hi = -1e9f;
        for (const SplatAttributes& a : attributes)
        {
            const float s[3] = { a.scale.x, a.scale.y, a.scale.z };
            for (float v : s) { const float l = std::log2(std::max(v, 1e-9f)); lo = std::min(lo, l); hi = std::max(hi, l); }
        }
        if (lo > hi) { lo = -14.0f; hi = 2.0f; }
        lo = std::max(lo, -30.0f) - 0.05f;
        hi = std::min(hi, 10.0f) + 0.5f * (float)maxLODLevels + 0.05f;
        if (hi - lo < 1.0f) hi = lo + 1.0f;
        outLog2Min = lo; outLog2Max = hi;
    }

    SplatAttributes SplatCodec::AttributesForLevel(const SplatAttributes& source, uint32_t level)
    {
        SplatAttributes a = source;
        const float grow = std::exp2(0.5f * ((float)level - (float)source.bakedLevel));
        a.scale.x *= grow; a.scale.y *= grow; a.scale.z *= grow;
        a.bakedLevel = level;
        return a;
    }

    SplatAttributes SplatCodec::DefaultAttributes(const SplatPoint& splat)
    {
        SplatAttributes a;
        const float sigma = std::max(1e-5f, splat.radius * 0.5f);
        a.scale = XMFLOAT3(sigma, sigma, sigma);
        a.rotation = XMFLOAT4(0, 0, 0, 1);
        a.opacity = 1.0f;
        return a;
    }

    XMFLOAT4 SplatCodec::RotateQuaternionHalfTurnX(const XMFLOAT4& q) { return XMFLOAT4(q.w, -q.z, q.y, -q.x); }

    void SplatCodec::ApplyAxisFlipToSH(float* sh)
    {
        static const float sign[15] = { -1, -1, 1, -1, 1, 1, -1, 1, -1, 1, -1, -1, 1, -1, 1 };
        for (uint32_t k = 0; k < 15; k++) for (uint32_t c = 0; c < 3; c++) sh[3 * k + c] *= sign[k];
    }

    // The stand-in keeps the decimated pyramid: every coarse point keeps one child's Gaussian, grown by AttributesForLevel
    void SplatCodec::MergeLevels(WaveletDecompositionResult& result, const SplatAttributeArray&)
    {
        result.mergedAttributes.clear();
    }

    void SplatCodec::Encode(const SplatPointArray& splats, const SplatAttributeArray& sourceAttributes,
                            uint32_t level, const SplatEncodeParams& params,
                            std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH,
                            const SplatAttributeArray* /*merged*/)
    {
        // The stand-in stores no harmonics: the SH stream is all zero and every record's SH flag is clear.
        outRecords.resize(splats.size());
        outSH.assign(splats.size() * (size_t)SHRecordBytes(params.shDegree), 0);
        const XMFLOAT3 ext(std::max(1e-6f, params.aabbMax.x - params.aabbMin.x), std::max(1e-6f, params.aabbMax.y - params.aabbMin.y), std::max(1e-6f, params.aabbMax.z - params.aabbMin.z));
        const float range = std::max(1e-6f, params.scaleLog2Max - params.scaleLog2Min);
        auto scaleByte = [&](float s) { return Unorm((std::log2(std::max(s, 1e-9f)) - params.scaleLog2Min) / range, 255); };
        for (size_t i = 0; i < splats.size(); i++)
        {
            const SplatPoint& s = splats[i];
            const SplatAttributes attr = (s.sourceIndex != kNoSplatSource && s.sourceIndex < sourceAttributes.size())
                ? AttributesForLevel(sourceAttributes[s.sourceIndex], level) : AttributesForLevel(DefaultAttributes(s), level);
            PackedSplatGPU r;
            r.word0 = Unorm((s.position.x - params.aabbMin.x) / ext.x, 65535) | (Unorm((s.position.y - params.aabbMin.y) / ext.y, 65535) << 16);
            r.word1 = Unorm((s.position.z - params.aabbMin.z) / ext.z, 65535) | (scaleByte(attr.scale.x) << 16) | (scaleByte(attr.scale.y) << 24);
            r.word2 = scaleByte(attr.scale.z) | (Unorm(attr.opacity, 255) << 8) | (Unorm(s.color.x, 255) << 16) | (Unorm(s.color.y, 255) << 24);
            r.word3 = Unorm(s.color.z, 255);
            r.word4 = PackQuaternion(attr.rotation);
            outRecords[i] = r;
        }
    }

    void SplatCodec::DecodeRecord(const PackedSplatGPU& r, const SplatEncodeParams& params, XMFLOAT3& outPosition, XMFLOAT3& outScale,
                                  XMFLOAT4& outRotation, float& outOpacity, XMFLOAT3& outColor, float& outSHScale)
    {
        const XMFLOAT3 ext(std::max(1e-6f, params.aabbMax.x - params.aabbMin.x), std::max(1e-6f, params.aabbMax.y - params.aabbMin.y), std::max(1e-6f, params.aabbMax.z - params.aabbMin.z));
        auto scaleFrom = [&](uint32_t b) { return std::exp2(params.scaleLog2Min + FromUnorm(b, 255) * (params.scaleLog2Max - params.scaleLog2Min)); };
        outPosition = XMFLOAT3(params.aabbMin.x + FromUnorm(r.word0 & 0xFFFFu, 65535) * ext.x, params.aabbMin.y + FromUnorm(r.word0 >> 16, 65535) * ext.y, params.aabbMin.z + FromUnorm(r.word1 & 0xFFFFu, 65535) * ext.z);
        outScale = XMFLOAT3(scaleFrom((r.word1 >> 16) & 0xFFu), scaleFrom((r.word1 >> 24) & 0xFFu), scaleFrom(r.word2 & 0xFFu));
        outOpacity = FromUnorm((r.word2 >> 8) & 0xFFu, 255);
        outColor = XMFLOAT3(FromUnorm((r.word2 >> 16) & 0xFFu, 255), FromUnorm((r.word2 >> 24) & 0xFFu, 255), FromUnorm(r.word3 & 0xFFu, 255));
        outSHScale = 0.0f; // The stand-in reads no harmonics
        outRotation = UnpackQuaternion(r.word4);
    }

    void SplatCodec::Decode(const std::vector<PackedSplatGPU>& records, const uint8_t*, size_t, const SplatEncodeParams& params, uint32_t bakedLevel,
                            uint32_t sourceIndexBase, SplatPointArray& outSplats, SplatAttributeArray& outAttributes)
    {
        outSplats.resize(records.size());
        outAttributes.resize(records.size());
        for (size_t i = 0; i < records.size(); i++)
        {
            SplatAttributes& a = outAttributes[i];
            SplatPoint& s = outSplats[i];
            float shScale = 0.0f;
            DecodeRecord(records[i], params, s.position, a.scale, a.rotation, a.opacity, s.color, shScale);
            a.bakedLevel = bakedLevel;
            float sorted[3] = { a.scale.x, a.scale.y, a.scale.z };
            std::sort(sorted, sorted + 3);
            s.radius = std::max(1e-5f, sorted[1] * 1.25f);
            s.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            s.sourceIndex = sourceIndexBase + (uint32_t)i;
        }
    }
}
