// StreamOrder.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// PUBLIC BUILD NOTE: the private version of this project schedules streaming with a proprietary
// ordering (the delivery order of the blocks, how a frame's load list is drawn from it, and how a
// view-driven request is ranked). This open build keeps the same API and a plain order in its place:
// blocks listed coarse-to-fine in chunk-index order, the ones the camera can see first. See
// StreamOrder.cpp. Same header, same signatures as the private build, so nothing else in the codebase changes.
#pragma once
#include <vector>
#include <functional>
#include <cstdint>
#include <cstddef>
#include "GstxTypes.h"

namespace Splat
{
    // Deepest LOD hierarchy the per-level lists are split by.
    static constexpr int kMaxStreamLevels = 8;

    class StreamOrder
    {
    public:
        // One streaming block as the scheduler sees it. 'handle' is the caller's own id for the
        // block (typically its index in the caller's block array) and is what every result refers to.
        struct Block
        {
            uint32_t handle      = 0;
            int      lodLevel    = 0;
            uint32_t chunkIndex  = 0;
            XMFLOAT3 center      = { 0.0f, 0.0f, 0.0f };
            uint32_t byteSize    = 0;
            float    detailScore = 0.0f; // 0..1 from DetailGrid::SampleBox over the block's bounds
        };

        // Live residency of a block, asked of the caller while a load list is built.
        struct BlockState
        {
            bool resident        = false;
            bool evictionPending = false;
        };
        using StateFn = std::function<BlockState(uint32_t handle)>;
        // Whether the camera can see a block (the caller's test: for an object, whether the block faces the eye)
        using VisibleFn = std::function<bool(uint32_t handle)>;

        // Builds the order for a model's blocks (any input order) and prepares the per-level delivery
        // lists. Handles must be dense: 0 .. blocks.size() - 1.
        void   Build(const std::vector<Block>& blocks);
        void   Clear();
        size_t BlockCount() const { return m_blocks.size(); }

        // Fills outHandles with up to maxEntries blocks to deliver this frame, in delivery order: the
        // blocks 'visible' accepts first, then the rest. targetLOD is the level the view renders at
        // present. Whenever a block stops being resident, either residencyEpoch changes (reset: every
        // cursor rewinds) or Rewind is called for it (one eviction); 'state' reports each block's live residency.
        void BuildLoadList(int targetLOD, size_t maxEntries, uint32_t residencyEpoch, const StateFn& state, const VisibleFn& visible,
                           std::vector<uint32_t>& outHandles);

        // One block stopped being resident: only its own level's cursor goes back (to the block), where a
        // change of residencyEpoch rewinds them all. For the evictions of a moving view, which come every frame:
        // rewinding everything made each BuildLoadList rescan every resident block (1.2 ms a frame on Cthulhu).
        void Rewind(uint32_t handle);

        // Rank of a view-driven request for a block that is neither a silhouette edge nor part of the
        // coarse bootstrap envelope (the caller ranks those two tiers itself, above everything here).
        // centerFactor: 1 at the centre of the view falling to 0 at 90 degrees off-axis; proximityFactor:
        // 1 nearest the camera falling to 0 at the far range. Higher = delivered sooner.
        static float DemandPriority(int lodLevel, float detailScore, bool inFrustum, bool inNeighborBand,
                                    float centerFactor, float proximityFactor, bool prioritizeView);

    private:
        std::vector<Block>    m_blocks;                        // In the scheduler's own order
        std::vector<uint32_t> m_slotOfHandle;                  // Indexed by handle: its position in its level's list
        std::vector<uint32_t> m_levelList[kMaxStreamLevels];   // Indices into m_blocks
        size_t   m_cursor[kMaxStreamLevels] = {};
        uint32_t m_cursorEpoch = 0xFFFFFFFFu;
    };
}
