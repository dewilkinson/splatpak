// LiftingWavelet.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// PUBLIC BUILD NOTE: the private version of this project builds its LOD pyramid with a
// second-generation lifting wavelet (predict/update steps with deadband sparsification).
// This open build substitutes a much simpler decimation scheme with the same external
// shape (same struct layout, same function signature) so the rest of the codebase needs
// no changes: every coarser level is just every other point, with normal/colour averaged
// and radius scaled up to keep surface coverage. See LiftingWavelet.cpp for the details.

#pragma once
#include <vector>
#include <cstdint>
#include "GstxTypes.h"

namespace Splat
{
    struct WaveletLODLevel
    {
        uint32_t level = 0;
        float geometricError = 0.0f;
        SplatPointArray splats;
    };

    // Every coarse point lodLevels[l].splats[i] (l >= 1) is built from lodLevels[l - 1].splats[2i] and [2i + 1], the
    // pairing SplatCodec::MergeLevels relies on.
    struct WaveletDecompositionResult
    {
        std::vector<WaveletLODLevel> lodLevels;
        // Splat mode, after SplatCodec::MergeLevels: the Gaussians the coarse points stand for, each merged from its two
        // children. A coarse point's sourceIndex refers here, past the end of the source attributes (index - their count).
        SplatAttributeArray mergedAttributes;
    };

    class LiftingWavelet
    {
    public:
        // Builds a multi-resolution LOD pyramid for one spatial chunk. See the file header
        // above: this open build uses simple decimation, not a true lifting wavelet.
        static WaveletDecompositionResult DecomposeChunk(
            const SplatPointArray& inputSplats,
            uint32_t maxLODLevels = 4,
            float deadbandThreshold = 0.003f); // Unused in this build; kept for signature compatibility
    };
}
