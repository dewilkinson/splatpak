// ShadowMeshDisplay.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// How SplatLab draws a shadow or occluder mesh (ShadowMesh.h): smoother than it is stored. Loop subdivision rounds the
// silhouettes of the low-polygon mesh (each pass quarters the triangles and pulls convex parts slightly inward, so the
// mesh pokes through the splats less, never more), and a smooth per-vertex normal replaces the flat facets. Display
// only: the package keeps the mesh as built.
//
// Vertices are 20 bytes, as ShadowMesh.hlsl reads them: float3 position, uint colour (0xAABBGGRR), uint normal
// (octahedral, two snorm16).

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "../../libs/gstx/ShadowMesh.h"

namespace Splat
{
    struct ShadowMeshDisplay
    {
        static constexpr uint32_t kVertexWords = 5;
        std::vector<uint32_t> vertices; // kVertexWords per vertex
        std::vector<uint32_t> indices;
        uint32_t VertexCount() const { return (uint32_t)(vertices.size() / kVertexWords); }
    };

    namespace ShadowMeshDisplayDetail
    {
        struct V { float x, y, z; };
        inline V Add(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        inline V Sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        inline V Mul(V a, float s) { return { a.x * s, a.y * s, a.z * s }; }
        inline V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        inline V Colour(uint32_t c) { return { (float)(c & 0xFF), (float)((c >> 8) & 0xFF), (float)((c >> 16) & 0xFF) }; }
        inline uint32_t PackColour(V c)
        {
            auto b = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 255.0f)); };
            return 0xFF000000u | b(c.x) | (b(c.y) << 8) | (b(c.z) << 16);
        }
        // Octahedral encoding of a unit normal into two snorm16
        inline uint32_t PackNormal(V n)
        {
            const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
            if (l1 <= 0.0f) return 0;
            float u = n.x / l1, v = n.y / l1;
            if (n.z < 0.0f)
            {
                const float pu = (1.0f - std::abs(v)) * (u >= 0.0f ? 1.0f : -1.0f), pv = (1.0f - std::abs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
                u = pu; v = pv;
            }
            auto s = [](float f) { return (uint32_t)(uint16_t)(int16_t)std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f); };
            return s(u) | (s(v) << 16);
        }

        // One Loop subdivision pass over a closed triangle mesh (an edge with other than two triangles falls back to its
        // midpoint, and its ends stay put)
        inline void LoopPass(std::vector<V>& pos, std::vector<V>& col, std::vector<uint32_t>& idx)
        {
            const uint32_t n = (uint32_t)pos.size();
            struct Edge { uint32_t opp[2] = { UINT32_MAX, UINT32_MAX }; int faces = 0; uint32_t mid = 0; };
            std::unordered_map<uint64_t, Edge> edges;
            edges.reserve(idx.size());
            auto key = [](uint32_t a, uint32_t b) { return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a; };
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
                for (int k = 0; k < 3; k++)
                {
                    Edge& e = edges[key(idx[t + k], idx[t + (k + 1) % 3])];
                    if (e.faces < 2) e.opp[e.faces] = idx[t + (k + 2) % 3];
                    e.faces++;
                }

            // Old vertices: (1 - n beta) v + beta (sum of neighbours); vertices on a bad edge keep their place
            std::vector<V> sumN(n, V{ 0, 0, 0 }), sumC(n, V{ 0, 0, 0 });
            std::vector<int> valence(n, 0);
            std::vector<uint8_t> pinned(n, 0);
            for (const auto& kv : edges)
            {
                const uint32_t a = (uint32_t)(kv.first >> 32), b = (uint32_t)kv.first;
                sumN[a] = Add(sumN[a], pos[b]); sumN[b] = Add(sumN[b], pos[a]);
                sumC[a] = Add(sumC[a], col[b]); sumC[b] = Add(sumC[b], col[a]);
                valence[a]++; valence[b]++;
                if (kv.second.faces != 2) pinned[a] = pinned[b] = 1;
            }
            std::vector<V> newPos(pos), newCol(col);
            for (uint32_t i = 0; i < n; i++)
            {
                if (pinned[i] || valence[i] < 3) continue;
                const float beta = valence[i] == 3 ? 3.0f / 16.0f : 3.0f / (8.0f * valence[i]);
                const float keep = 1.0f - valence[i] * beta;
                newPos[i] = Add(Mul(pos[i], keep), Mul(sumN[i], beta));
                newCol[i] = Add(Mul(col[i], keep), Mul(sumC[i], beta));
            }
            // Edge vertices: 3/8 of each end and 1/8 of each opposite vertex
            for (auto& kv : edges)
            {
                const uint32_t a = (uint32_t)(kv.first >> 32), b = (uint32_t)kv.first;
                Edge& e = kv.second;
                e.mid = (uint32_t)newPos.size();
                if (e.faces == 2)
                {
                    newPos.push_back(Add(Mul(Add(pos[a], pos[b]), 0.375f), Mul(Add(pos[e.opp[0]], pos[e.opp[1]]), 0.125f)));
                    newCol.push_back(Add(Mul(Add(col[a], col[b]), 0.375f), Mul(Add(col[e.opp[0]], col[e.opp[1]]), 0.125f)));
                }
                else
                {
                    newPos.push_back(Mul(Add(pos[a], pos[b]), 0.5f));
                    newCol.push_back(Mul(Add(col[a], col[b]), 0.5f));
                }
            }
            // Each triangle becomes four, keeping its winding
            std::vector<uint32_t> newIdx;
            newIdx.reserve(idx.size() * 4);
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
            {
                const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
                const uint32_t ab = edges[key(a, b)].mid, bc = edges[key(b, c)].mid, ca = edges[key(c, a)].mid;
                const uint32_t tris[12] = { a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca };
                newIdx.insert(newIdx.end(), tris, tris + 12);
            }
            pos.swap(newPos);
            col.swap(newCol);
            idx.swap(newIdx);
        }
    }

    // The level as drawn: subdivided `passes` times (each quadruples the triangles), with smooth normals
    inline void BuildShadowMeshDisplay(const ShadowMeshLevel& level, int passes, ShadowMeshDisplay& out)
    {
        using namespace ShadowMeshDisplayDetail;
        std::vector<V> pos(level.positions.size()), col(level.positions.size());
        for (size_t i = 0; i < pos.size(); i++)
        {
            pos[i] = { level.positions[i].x, level.positions[i].y, level.positions[i].z };
            col[i] = Colour(i < level.colours.size() ? level.colours[i] : 0xFF808080u);
        }
        std::vector<uint32_t> idx = level.indices;
        for (int p = 0; p < passes; p++) LoopPass(pos, col, idx);

        // Area-weighted vertex normals (the cross product's length is twice the triangle's area)
        std::vector<V> nrm(pos.size(), V{ 0, 0, 0 });
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
        {
            const V f = Cross(Sub(pos[idx[t + 1]], pos[idx[t]]), Sub(pos[idx[t + 2]], pos[idx[t]]));
            for (int k = 0; k < 3; k++) nrm[idx[t + k]] = Add(nrm[idx[t + k]], f);
        }
        out.vertices.resize(pos.size() * ShadowMeshDisplay::kVertexWords);
        for (size_t i = 0; i < pos.size(); i++)
        {
            uint32_t* w = &out.vertices[i * ShadowMeshDisplay::kVertexWords];
            memcpy(w, &pos[i], 12);
            w[3] = PackColour(col[i]);
            const float len = std::sqrt(nrm[i].x * nrm[i].x + nrm[i].y * nrm[i].y + nrm[i].z * nrm[i].z);
            w[4] = len > 0.0f ? PackNormal(Mul(nrm[i], 1.0f / len)) : 0;
        }
        out.indices.swap(idx);
    }
}
