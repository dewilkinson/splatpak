// SplatCodec.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Splat mode: the 20-byte 3D Gaussian record (PackedSplatGPU) and its spherical-harmonics
// side stream, as written into .gstx packages of splat format 1 (see
// docs/GSTX_FORMAT_SPECIFICATION.md). This header is the data contract; the encoder, the
// decoder and the level derivation live in SplatCodec.cpp inside the gstx library.

#pragma once
#include <vector>
#include <cstdint>
#include "../../src/SplatCore/WaveletTypes.h"
#include "LiftingWavelet.h"

namespace Splat
{
    // Everything a splat encode or decode needs besides the points themselves. The same values
    // travel in the package header so a decoder reproduces the encoder's quantisation exactly.
    struct SplatEncodeParams
    {
        uint32_t shDegree     = 1;      // Spherical-harmonics degree kept in the SH stream: 0 (none), 1 (low), 2 (medium) or 3 (high)
        float    scaleLog2Min = -14.0f; // log2 of the scale a scale byte of 0 decodes to
        float    scaleLog2Max = 2.0f;   // log2 of the scale a scale byte of 255 decodes to
        XMFLOAT3 aabbMin      = { 0.0f, 0.0f, 0.0f }; // Global bounds the 16-bit positions span
        XMFLOAT3 aabbMax      = { 1.0f, 1.0f, 1.0f };
    };

    class SplatCodec
    {
    public:
        static constexpr uint32_t kRecordBytes = 20;

        // Spherical-harmonics bookkeeping for a degree (0, 1, 2 or 3): coefficients per colour channel
        // above the DC term (0, 3, 8, 15) and bytes per point in the SH stream (three channels, one
        // signed byte each).
        static uint32_t SHCoefficientsPerChannel(uint32_t degree);
        static uint32_t SHRecordBytes(uint32_t degree);

        // The log2 scale range the package's 8-bit scale fields span, from the source attributes.
        // Widened for the coarser levels the bake derives (their footprints grow with the level) and
        // padded so no source scale lands on the clamp.
        static void ComputeScaleRange(const std::vector<SplatAttributes>& attributes, uint32_t maxLODLevels,
                                      float& outLog2Min, float& outLog2Max);

        // The attributes a point takes at a given level of the pyramid: the source Gaussian's rotation,
        // opacity and harmonics with the footprint grown to the level's spacing.
        static SplatAttributes AttributesForLevel(const SplatAttributes& source, uint32_t level);

        // Attributes for a point that has no source entry (a plain point cloud, a synthetic cloud):
        // an isotropic Gaussian sized from the splat's disc radius, fully opaque, DC colour only.
        static SplatAttributes DefaultAttributes(const SplatPoint& splat);

        // The coarse levels' Gaussians: every coarse point of the pyramid stands for its two children (see
        // WaveletDecompositionResult), so it takes one Gaussian merged from theirs -- position, colour and harmonics as
        // means weighted by opacity, the covariance of the pair (both children's covariances plus their
        // spread about the mean), and the opacity that keeps the pair's coverage -- instead of one child's Gaussian
        // grown and made more opaque. Fills result.mergedAttributes and points every coarse point at its entry
        // (sourceIndex = sourceAttributes.size() + entry), with the merged position and colour. Level 0 points take
        // their Gaussian from sourceAttributes. The open stand-in library leaves the pyramid as it is.
        static void MergeLevels(WaveletDecompositionResult& result, const std::vector<SplatAttributes>& sourceAttributes);

        // Encodes one level. splats[i].sourceIndex selects the entry of sourceAttributes the point's
        // Gaussian comes from (kNoSplatSource -> DefaultAttributes), grown to the level; an index past
        // the end of sourceAttributes selects merged[index - sourceAttributes.size()] (MergeLevels), used as
        // it is. outRecords gets one PackedSplatGPU per splat; outSH gets splats.size() *
        // SHRecordBytes(params.shDegree) bytes (empty for degree 0).
        static void Encode(const std::vector<SplatPoint>& splats, const std::vector<SplatAttributes>& sourceAttributes,
                           uint32_t level, const SplatEncodeParams& params,
                           std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH,
                           const std::vector<SplatAttributes>* merged = nullptr);

        // Decodes one level back to splats plus attributes. sh may be null (no SH stream). Every decoded
        // splat gets sourceIndex = sourceIndexBase + i, so a caller appending outAttributes to its
        // attribute array keeps the links valid; bakedLevel is written into every attribute.
        static void Decode(const std::vector<PackedSplatGPU>& records, const uint8_t* sh, size_t shBytes,
                           const SplatEncodeParams& params, uint32_t bakedLevel, uint32_t sourceIndexBase,
                           std::vector<SplatPoint>& outSplats, std::vector<SplatAttributes>& outAttributes);

        // The import's axis change (3DGS +Y down / +Z forward to +Y up) is a half turn about X. These
        // apply that turn to the pieces of a Gaussian that are not plain positions: the rotation
        // quaternion (x, y, z, w) and the harmonics (each basis function only changes sign).
        static XMFLOAT4 RotateQuaternionHalfTurnX(const XMFLOAT4& q);
        static void     ApplyAxisFlipToSH(float* sh45);

        // Debug / test aid: decodes one record's fields without the surrounding arrays.
        static void DecodeRecord(const PackedSplatGPU& record, const SplatEncodeParams& params,
                                 XMFLOAT3& outPosition, XMFLOAT3& outScale, XMFLOAT4& outRotation,
                                 float& outOpacity, XMFLOAT3& outColor, float& outSHScale);
    };
}
