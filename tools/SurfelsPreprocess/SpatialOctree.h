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
        // Expands 10-bit integer to 30-bit integer with 2 zero bits inserted between each bit (for 3D Morton code)
        static inline uint32_t Dilate10Bit(uint32_t x)
        {
            x &= 0x000003ff;
            x = (x ^ (x << 16)) & 0xff0000ff;
            x = (x ^ (x <<  8)) & 0x0300f00f;
            x = (x ^ (x <<  4)) & 0x030c30c3;
            x = (x ^ (x <<  2)) & 0x09249249;
            return x;
        }

        // Computes 30-bit Morton code for a normalized point in [0, 1]^3 (1024x1024x1024 grid)
        static inline uint32_t ComputeMorton30(float x, float y, float z)
        {
            x = std::max(0.0f, std::min(1.0f, x));
            y = std::max(0.0f, std::min(1.0f, y));
            z = std::max(0.0f, std::min(1.0f, z));

            uint32_t ix = (uint32_t)(x * 1023.0f);
            uint32_t iy = (uint32_t)(y * 1023.0f);
            uint32_t iz = (uint32_t)(z * 1023.0f);

            return (Dilate10Bit(iz) << 2) | (Dilate10Bit(iy) << 1) | Dilate10Bit(ix);
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

            // Use the specified chunkSizeMeters directly (or auto-calculate if <= 0)
            float effectiveChunkSize = chunkSizeMeters;
            if (effectiveChunkSize <= 0.01f)
            {
                size_t totalPoints = points.size();
                size_t targetPointsPerChunk = 15000;
                size_t targetChunkCount = std::max((size_t)16, std::min((size_t)512, (totalPoints + targetPointsPerChunk - 1) / targetPointsPerChunk));
                int gridRes = (int)std::ceil(std::cbrt((double)targetChunkCount));
                gridRes = std::max(2, std::min(32, gridRes));
                effectiveChunkSize = maxDim / (float)gridRes;
            }
            effectiveChunkSize = std::max(0.1f, effectiveChunkSize);

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

        // Partitions points into high-occupancy Meshlet-sized clusters (typically 128 surfels per chunk)
        // using 64-bit Morton Z-Order spatial curve sorting for maximum GPU L2 cache locality.
        static void PartitionIntoMeshletChunks(
            std::vector<SurfelVertex>& inOutPoints,
            std::vector<MeshletChunkGPU>& outMeshletChunks,
            uint32_t surfelsPerMeshlet = 128,
            bool enableMorton = true)
        {
            outMeshletChunks.clear();
            if (inOutPoints.empty()) return;

            size_t numPoints = inOutPoints.size();

            if (enableMorton)
            {
                // 1. Compute Global AABB
                XMFLOAT3 gMin(1e9f, 1e9f, 1e9f);
                XMFLOAT3 gMax(-1e9f, -1e9f, -1e9f);
                for (const auto& p : inOutPoints)
                {
                    gMin.x = std::min(gMin.x, p.position.x);
                    gMin.y = std::min(gMin.y, p.position.y);
                    gMin.z = std::min(gMin.z, p.position.z);

                    gMax.x = std::max(gMax.x, p.position.x);
                    gMax.y = std::max(gMax.y, p.position.y);
                    gMax.z = std::max(gMax.z, p.position.z);
                }

                XMFLOAT3 gExtent(
                    std::max(1e-4f, gMax.x - gMin.x),
                    std::max(1e-4f, gMax.y - gMin.y),
                    std::max(1e-4f, gMax.z - gMin.z)
                );

                // 2. Compute 30-bit Morton Code for each point and sort
                struct MortonPoint
                {
                    uint32_t code;
                    uint32_t originalIndex;
                };

                std::vector<MortonPoint> mortonList(numPoints);

                #pragma omp parallel for
                for (int i = 0; i < (int)numPoints; i++)
                {
                    const auto& p = inOutPoints[i];
                    float nx = (p.position.x - gMin.x) / gExtent.x;
                    float ny = (p.position.y - gMin.y) / gExtent.y;
                    float nz = (p.position.z - gMin.z) / gExtent.z;

                    mortonList[i].code = ComputeMorton30(nx, ny, nz);
                    mortonList[i].originalIndex = (uint32_t)i;
                }

                std::sort(mortonList.begin(), mortonList.end(), [](const MortonPoint& a, const MortonPoint& b) {
                    if (a.code != b.code) return a.code < b.code;
                    return a.originalIndex < b.originalIndex;
                });

                // 3. Reorder points in Morton order
                std::vector<SurfelVertex> reorderedPoints(numPoints);
                #pragma omp parallel for
                for (int i = 0; i < (int)numPoints; i++)
                {
                    reorderedPoints[i] = inOutPoints[mortonList[i].originalIndex];
                }
                inOutPoints = std::move(reorderedPoints);
            }

            // 4. Build 128-surfel Meshlet chunks
            uint32_t numChunks = (uint32_t)((numPoints + surfelsPerMeshlet - 1) / surfelsPerMeshlet);
            outMeshletChunks.resize(numChunks);

            for (uint32_t c = 0; c < numChunks; c++)
            {
                uint32_t startIdx = c * surfelsPerMeshlet;
                uint32_t count = std::min(surfelsPerMeshlet, (uint32_t)(numPoints - startIdx));

                XMFLOAT3 cMin(1e9f, 1e9f, 1e9f);
                XMFLOAT3 cMax(-1e9f, -1e9f, -1e9f);

                for (uint32_t i = 0; i < count; i++)
                {
                    const auto& p = inOutPoints[startIdx + i];
                    float r = std::max(0.001f, p.radius);
                    cMin.x = std::min(cMin.x, p.position.x - r);
                    cMin.y = std::min(cMin.y, p.position.y - r);
                    cMin.z = std::min(cMin.z, p.position.z - r);

                    cMax.x = std::max(cMax.x, p.position.x + r);
                    cMax.y = std::max(cMax.y, p.position.y + r);
                    cMax.z = std::max(cMax.z, p.position.z + r);
                }

                MeshletChunkGPU& chunk = outMeshletChunks[c];
                chunk.center = XMFLOAT3(
                    (cMin.x + cMax.x) * 0.5f,
                    (cMin.y + cMax.y) * 0.5f,
                    (cMin.z + cMax.z) * 0.5f
                );
                chunk.aabbMin = cMin;
                chunk.aabbExtents = XMFLOAT3(cMax.x - cMin.x, cMax.y - cMin.y, cMax.z - cMin.z);
                chunk.surfelOffset = startIdx;
                chunk.surfelCount = count;

                float maxR2 = 0.0f;
                for (uint32_t i = 0; i < count; i++)
                {
                    const auto& p = inOutPoints[startIdx + i];
                    float dx = p.position.x - chunk.center.x;
                    float dy = p.position.y - chunk.center.y;
                    float dz = p.position.z - chunk.center.z;
                    float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 > maxR2) maxR2 = dist2;
                }
                chunk.boundingRadius = std::sqrt(maxR2) + 0.01f;
            }
        }
    };
}

