// PointCloudGaussians.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// A plain point cloud (a .ply with positions and colours but no Gaussian fields, or a synthetic cloud)
// is shown and baked as 3D Gaussians like any splat scene: every point becomes an isotropic Gaussian at
// its position, of the point's colour, fully opaque, with no view-dependent colour, sized from the cloud's
// own point spacing so neighbouring Gaussians just close the surface. Shared by the app's loaders and the
// command-line baker, so both bake a point cloud the same way.

#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include "WaveletTypes.h"

namespace Splat
{
    namespace PointCloudGaussians
    {
        // Median distance from a point to its nearest neighbour, over a sample of up to 60,000 points, found
        // in a uniform grid sized from the cloud's surface area (three expected spacings per cell). 0 for
        // fewer than two points.
        inline float MedianSpacing(const SplatPointArray& pts)
        {
            const size_t n = pts.size();
            if (n < 2) return 0.0f;
            XMFLOAT3 mn = { FLT_MAX, FLT_MAX, FLT_MAX }, mx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (const auto& v : pts)
            {
                mn.x = std::min(mn.x, v.position.x); mn.y = std::min(mn.y, v.position.y); mn.z = std::min(mn.z, v.position.z);
                mx.x = std::max(mx.x, v.position.x); mx.y = std::max(mx.y, v.position.y); mx.z = std::max(mx.z, v.position.z);
            }
            const float ex = std::max(1e-6f, mx.x - mn.x), ey = std::max(1e-6f, mx.y - mn.y), ez = std::max(1e-6f, mx.z - mn.z);
            const float area = 2.0f * (ex * ey + ey * ez + ez * ex);
            const float cell = std::max(1e-6f, std::sqrt(area / (float)n) * 3.0f);
            auto key = [&](int ix, int iy, int iz) -> uint64_t
            {
                return ((uint64_t)(uint32_t)(ix + 0x100000) << 42) | ((uint64_t)(uint32_t)(iy + 0x100000) << 21) | (uint64_t)(uint32_t)(iz + 0x100000);
            };
            auto cellOf = [&](const XMFLOAT3& p, int& ix, int& iy, int& iz)
            {
                ix = (int)std::floor((p.x - mn.x) / cell); iy = (int)std::floor((p.y - mn.y) / cell); iz = (int)std::floor((p.z - mn.z) / cell);
            };
            std::vector<std::pair<uint64_t, uint32_t>> grid;
            grid.reserve(n);
            for (uint32_t i = 0; i < (uint32_t)n; i++)
            {
                int ix, iy, iz; cellOf(pts[i].position, ix, iy, iz);
                grid.emplace_back(key(ix, iy, iz), i);
            }
            std::sort(grid.begin(), grid.end());

            const size_t stride = std::max<size_t>(1, n / 60000);
            std::vector<float> dists;
            dists.reserve(n / stride + 1);
            for (size_t si = 0; si < n; si += stride)
            {
                const XMFLOAT3& p = pts[si].position;
                int ix, iy, iz; cellOf(p, ix, iy, iz);
                float best = FLT_MAX;
                for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                {
                    const uint64_t k = key(ix + dx, iy + dy, iz + dz);
                    for (auto it = std::lower_bound(grid.begin(), grid.end(), std::make_pair(k, 0u)); it != grid.end() && it->first == k; ++it)
                    {
                        if (it->second == (uint32_t)si) continue;
                        const XMFLOAT3& q = pts[it->second].position;
                        const float d2 = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z);
                        if (d2 > 0.0f) best = std::min(best, d2);
                    }
                }
                if (best < FLT_MAX) dists.push_back(std::sqrt(best));
            }
            if (dists.empty()) return cell / 3.0f;
            std::nth_element(dists.begin(), dists.begin() + dists.size() / 2, dists.end());
            return dists[dists.size() / 2];
        }

        // Gives every point a Gaussian: outAttributes[i] for pts[i], and pts[i].sourceIndex = i. The standard
        // deviation is half the median spacing, so neighbouring Gaussians overlap at one sigma and close the
        // surface without blurring it. Returns that sigma.
        inline float Build(SplatPointArray& pts, SplatAttributeArray& outAttributes)
        {
            outAttributes.clear();
            if (pts.empty()) return 0.0f;
            const float sigma = std::max(1e-6f, 0.5f * MedianSpacing(pts));
            outAttributes.resize(pts.size());
            for (size_t i = 0; i < pts.size(); i++)
            {
                SplatAttributes& a = outAttributes[i];
                a.scale = XMFLOAT3(sigma, sigma, sigma);
                a.rotation = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                a.opacity = 1.0f;
                a.bakedLevel = 0;
                pts[i].sourceIndex = (uint32_t)i;
                pts[i].radius = 2.0f * sigma;
            }
            return sigma;
        }
    }
}
