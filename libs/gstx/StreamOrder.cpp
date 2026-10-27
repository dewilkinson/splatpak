// StreamOrder.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Open-build streaming order: plain chunk order, not the private scheduler. Each level's list is
// simply chunk-index order, a load list takes the blocks the camera can see first (coarse levels
// first), then the rest, and a view-driven request ranks by frustum band and level only.
// Same header, same signatures as the private build, so nothing else in the codebase changes.

#include "StreamOrder.h"
#include <algorithm>

namespace Splat
{
    void StreamOrder::Clear()
    {
        m_blocks.clear();
        m_slotOfHandle.clear();
        for (int l = 0; l < kMaxStreamLevels; l++) { m_levelList[l].clear(); m_cursor[l] = 0; }
        m_cursorEpoch = 0xFFFFFFFFu;
    }

    void StreamOrder::Build(const std::vector<Block>& blocks)
    {
        Clear();
        m_blocks = blocks;
        std::stable_sort(m_blocks.begin(), m_blocks.end(), [](const Block& a, const Block& b) { return a.chunkIndex < b.chunkIndex; });
        uint32_t maxHandle = 0;
        for (const Block& b : m_blocks) maxHandle = std::max(maxHandle, b.handle);
        m_slotOfHandle.assign(m_blocks.empty() ? 0 : (size_t)maxHandle + 1, 0);
        for (uint32_t i = 0; i < (uint32_t)m_blocks.size(); i++)
        {
            const int level = std::max(0, std::min(m_blocks[i].lodLevel, kMaxStreamLevels - 1));
            m_slotOfHandle[m_blocks[i].handle] = (uint32_t)m_levelList[level].size();
            m_levelList[level].push_back(i);
        }
    }

    void StreamOrder::Rewind(uint32_t handle)
    {
        if (handle >= m_slotOfHandle.size()) return;
        const uint32_t slot = m_slotOfHandle[handle];
        for (int l = 0; l < kMaxStreamLevels; l++)
            if (slot < m_levelList[l].size() && m_blocks[m_levelList[l][slot]].handle == handle) { m_cursor[l] = std::min(m_cursor[l], (size_t)slot); return; }
    }

    void StreamOrder::BuildLoadList(int /*targetLOD*/, size_t maxEntries, uint32_t residencyEpoch, const StateFn& state, const VisibleFn& visible,
                                    std::vector<uint32_t>& outHandles)
    {
        outHandles.clear();
        if (m_cursorEpoch != residencyEpoch)
        {
            for (int l = 0; l < kMaxStreamLevels; l++) m_cursor[l] = 0;
            m_cursorEpoch = residencyEpoch;
        }
        // The persistent cursor only moves past resident blocks; each pass walks the rest, so a block offered
        // this frame but not delivered is offered again next frame.
        for (int l = 0; l < kMaxStreamLevels; l++)
            while (m_cursor[l] < m_levelList[l].size() && state(m_blocks[m_levelList[l][m_cursor[l]]].handle).resident) m_cursor[l]++;
        for (int pass = 0; pass < 2; pass++)
            for (int l = kMaxStreamLevels - 1; l >= 0; l--)
                for (size_t p = m_cursor[l]; p < m_levelList[l].size(); p++)
                {
                    if (outHandles.size() >= maxEntries) return;
                    const uint32_t handle = m_blocks[m_levelList[l][p]].handle;
                    const BlockState s = state(handle);
                    if (!s.resident && !s.evictionPending && visible(handle) == (pass == 0)) outHandles.push_back(handle);
                }
    }

    float StreamOrder::DemandPriority(int lodLevel, float /*detailScore*/, bool inFrustum, bool inNeighborBand,
                                      float /*centerFactor*/, float /*proximityFactor*/, bool prioritizeView)
    {
        // Three plain bands (in view, next to the view, elsewhere), coarser levels first within a band.
        float priority = 1000.0f;
        if (prioritizeView) priority += (inFrustum ? 200.0f : (inNeighborBand ? 100.0f : 0.0f));
        priority += (float)(lodLevel + 1);
        return priority;
    }
}
