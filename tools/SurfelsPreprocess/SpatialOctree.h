#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include "../../src/DX12/Wavelet/WaveletTypes.h"

namespace Surfels
{
    struct ChunkData
    {
        uint32_t chunkId = 0;
        XMFLOAT3 aabbMin = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 aabbMax = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 center  = { 0.0f, 0.0f, 0.0f };
        float    boundingRadius = 0.0f;
        std::vector<SurfelVertex> surfels;
    };

    class SpatialOctree
    {
    public:
        // Expands 21-bit integer to 63-bit integer with 2 zero bits inserted between each bit (for 3D Morton code)
        static uint64_t Part1By2(uint64_t n)
        {
            n &= 0x1fffff; // 21 bits
            n = (n | (n << 32)) & 0x1f00000000ffff;
            n = (n | (n << 16)) & 0x1f0000ff0000ff;
            n = (n | (n << 8))  & 0x100f00f00f00f00f;
            n = (n | (n << 4))  & 0x10c30c30c30c30c3;
            n = (n | (n << 2))  & 0x1249249249249249;
            return n;
        }

        // Computes 64-bit Morton code for a normalized point in [0, 1]^3
        static uint64_t ComputeMorton64(float x, float y, float z)
        {
            x = std::max(0.0f, std::min(1.0f, x));
            y = std::max(0.0f, std::min(1.0f, y));
            z = std::max(0.0f, std::min(1.0f, z));

            uint64_t ix = (uint64_t)(x * 2097151.0f);
            uint64_t iy = (uint64_t)(y * 2097151.0f);
            uint64_t iz = (uint64_t)(z * 2097151.0f);

            return (Part1By2(iz) << 2) | (Part1By2(iy) << 1) | Part1By2(ix);
        }

        // Partitions points into spatial chunks of maximum size chunkSizeMeters
        // Dynamic Spatial Partitioning targeting optimal 30,000 to 60,000 surfels per chunk for GPU/CPU culling efficiency
        static std::vector<ChunkData> PartitionIntoChunks(std::vector<SurfelVertex>& points, float chunkSizeMeters = 16.0f)
        {
            if (points.empty()) return {};

            // 1. Compute Global AABB
            XMFLOAT3 gMin(1e9f, 1e9f, 1e9f);
            XMFLOAT3 gMax(-1e9f, -1e9f, -1e9f);
            for (const auto& p : points)
            {
                gMin.x = std::min(gMin.x, p.position.x);
                gMin.y = std::min(gMin.y, p.position.y);
                gMin.z = std::min(gMin.z, p.position.z);

                gMax.x = std::max(gMax.x, p.position.x);
                gMax.y = std::max(gMax.y, p.position.y);
                gMax.z = std::max(gMax.z, p.position.z);
            }

            XMFLOAT3 gExtent(gMax.x - gMin.x + 1e-4f, gMax.y - gMin.y + 1e-4f, gMax.z - gMin.z + 1e-4f);
            float maxDim = std::max(gExtent.x, std::max(gExtent.y, gExtent.z));

            // Dynamically tune grid resolution based on total point count to target ~30,000 - 60,000 points per chunk (64 to 256 total chunks)
            size_t totalPoints = points.size();
            size_t targetPointsPerChunk = 45000;
            size_t targetChunkCount = std::max((size_t)8, std::min((size_t)256, (totalPoints + targetPointsPerChunk - 1) / targetPointsPerChunk));

            // Estimate grid resolution per axis (N x N x N)
            int gridRes = (int)std::ceil(std::cbrt((double)targetChunkCount));
            gridRes = std::max(2, std::min(16, gridRes));

            float effectiveChunkSize = maxDim / (float)gridRes;

            // 2. Cluster points into discrete spatial grid cells
            struct VoxelKey
            {
                int32_t x, y, z;
                bool operator==(const VoxelKey& o) const { return x == o.x && y == o.y && z == o.z; }
            };

            struct VoxelKeyHash
            {
                size_t operator()(const VoxelKey& k) const
                {
                    return ((size_t)k.x * 73856093) ^ ((size_t)k.y * 19349663) ^ ((size_t)k.z * 83492791);
                }
            };

            std::unordered_map<VoxelKey, std::vector<SurfelVertex>, VoxelKeyHash> voxelMap;
            for (const auto& p : points)
            {
                VoxelKey k;
                k.x = (int32_t)std::floor((p.position.x - gMin.x) / effectiveChunkSize);
                k.y = (int32_t)std::floor((p.position.y - gMin.y) / effectiveChunkSize);
                k.z = (int32_t)std::floor((p.position.z - gMin.z) / effectiveChunkSize);
                voxelMap[k].push_back(p);
            }

            // 3. Assemble ChunkData objects with strict bounding box recalculation
            std::vector<ChunkData> chunks;
            chunks.reserve(voxelMap.size());

            uint32_t nextId = 0;
            for (auto& pair : voxelMap)
            {
                auto& surfelList = pair.second;
                if (surfelList.empty()) continue;

                ChunkData cd;
                cd.chunkId = nextId++;
                cd.surfels = std::move(surfelList);

                XMFLOAT3 cMin(1e9f, 1e9f, 1e9f);
                XMFLOAT3 cMax(-1e9f, -1e9f, -1e9f);

                for (const auto& s : cd.surfels)
                {
                    cMin.x = std::min(cMin.x, s.position.x - s.radius);
                    cMin.y = std::min(cMin.y, s.position.y - s.radius);
                    cMin.z = std::min(cMin.z, s.position.z - s.radius);

                    cMax.x = std::max(cMax.x, s.position.x + s.radius);
                    cMax.y = std::max(cMax.y, s.position.y + s.radius);
                    cMax.z = std::max(cMax.z, s.position.z + s.radius);
                }

                cd.aabbMin = cMin;
                cd.aabbMax = cMax;
                cd.center = XMFLOAT3(
                    (cMin.x + cMax.x) * 0.5f,
                    (cMin.y + cMax.y) * 0.5f,
                    (cMin.z + cMax.z) * 0.5f
                );

                float dx = cMax.x - cd.center.x;
                float dy = cMax.y - cd.center.y;
                float dz = cMax.z - cd.center.z;
                cd.boundingRadius = std::sqrt(dx * dx + dy * dy + dz * dz);

                chunks.push_back(std::move(cd));
            }

            return chunks;
        }
    };
}

