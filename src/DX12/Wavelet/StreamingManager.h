// StreamingManager.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Runtime-side dataset loader for the Surfels_DX12 viewer: opens a .sflw package,
// keeps a per-(chunk, LOD) decompressed cache (with the two coarsest LODs always pinned),
// and each frame turns the current LODSelector selection into a flat surfel buffer ready
// for GPU upload.

#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <atomic>
#include "WaveletTypes.h"
#include "ZstdDecompressor.h"
#include "LODSelector.h"

namespace Surfels
{
    // Telemetry's lodDistribution array only tracks this many levels -- plenty of headroom over the
    // preprocessor's practical max (SurfelLab's own LOD slider tops out well below this).
    static constexpr uint32_t kMaxTrackedLODLevels = 8;

    struct CachedChunkLOD
    {
        uint32_t chunkId;
        uint32_t lodLevel;
        std::vector<PackedSurfelGPU> surfels;
        uint64_t lastUsedFrame = 0;
    };

    struct StreamingTelemetry
    {
        uint32_t totalChunksInDataset = 0;
        uint32_t activeChunksRendered = 0;
        uint32_t totalSurfelsRendered = 0;
        uint32_t lodDistribution[kMaxTrackedLODLevels] = {};
        float    residentMemoryMB     = 0.0f;
        float    streamingBandwidthKB = 0.0f;
        uint32_t cacheHits            = 0;
        uint32_t cacheMisses          = 0;
    };

    class StreamingManager
    {
    public:
        StreamingManager() = default;
        ~StreamingManager() { Close(); }

        // Opens a .sflw package (searching a few relative paths under models/) and pins the two
        // coarsest LOD levels of every chunk into the cache so a base silhouette is always ready.
        // v4+ packages are a single file; a v1-v3 package needs its companion .json alongside.
        bool LoadDataset(const std::string& basepath)
        {
            Close();
            
            std::vector<std::string> searchBases = {
                basepath,
                "../" + basepath,
                "models/" + basepath,
                "../models/" + basepath,
                "../../models/" + basepath
            };

            bool packageFound = false;
            for (const auto& sb : searchBases)
            {
                std::string sflwTry = sb + ".sflw";
                std::ifstream test(sflwTry, std::ios::binary);
                if (test.is_open())
                {
                    test.close();
                    m_sflwPath = sflwTry;
                    packageFound = true;
                    break;
                }
            }

            if (!packageFound)
            {
                m_sflwPath = basepath + ".sflw";
            }

            m_sflwStream.open(m_sflwPath, std::ios::binary);
            if (!m_sflwStream.is_open())
            {
                std::cerr << "Failed to open stream file: " << m_sflwPath << std::endl;
                return false;
            }

            // Read header
            m_sflwStream.read(reinterpret_cast<char*>(&m_header), sizeof(SFLWFileHeader));
            if (m_header.magic != SFLW_MAGIC)
            {
                std::cerr << "Invalid SFLW magic header\n";
                return false;
            }
            if (m_header.version < 4)
            {
                m_header.manifestOffset = 0; // field didn't exist yet; holds misread payload bytes
            }
            if (m_header.version < 5)
            {
                m_header.sourceFileBytes = 0;
            }

            if (!ParseManifest())
            {
                std::cerr << "Failed to read manifest for: " << m_sflwPath << std::endl;
                Close();
                return false;
            }

            m_isLoaded = true;
            m_currentFrame = 0;

            // Pre-load and permanently pin the highest two mip levels (coarsest LODs) in resident memory
            for (size_t c = 0; c < m_chunks.size(); c++)
            {
                const auto& chunk = m_chunks[c];
                if (chunk.lods.empty()) continue;
                uint32_t coarsest = (uint32_t)chunk.lods.size() - 1;
                uint32_t minPinLOD = (coarsest > 0) ? (coarsest - 1) : coarsest;

                for (uint32_t lvl = minPinLOD; lvl <= coarsest; lvl++)
                {
                    uint64_t key = ((uint64_t)c << 32) | lvl;
                    if (m_cache.find(key) == m_cache.end())
                    {
                        CachedChunkLOD loadedChunk;
                        loadedChunk.chunkId = (uint32_t)c;
                        loadedChunk.lodLevel = lvl;
                        loadedChunk.lastUsedFrame = 0;
                        if (LoadChunkFromDisk(chunk, lvl, loadedChunk.surfels))
                        {
                            m_cache[key] = std::move(loadedChunk);
                        }
                    }
                }
            }

            return true;
        }

        // Releases the open file handle and drops the entire decompressed cache
        void Close()
        {
            if (m_sflwStream.is_open())
            {
                m_sflwStream.close();
            }
            m_cache.clear();
            m_chunks.clear();
            m_isLoaded = false;
        }

        bool IsLoaded() const { return m_isLoaded; }
        const SFLWFileHeader& GetHeader() const { return m_header; }
        const std::vector<ChunkManifest>& GetChunks() const { return m_chunks; }
        const StreamingTelemetry& GetTelemetry() const { return m_telemetry; }

        // Updates active chunks based on LOD selection and collects all active surfels for GPU upload
        void Update(const std::vector<ActiveChunkSelection>& selections, std::vector<PackedSurfelGPU>& outActiveSurfels)
        {
            m_currentFrame++;
            outActiveSurfels.clear();
            memset(&m_telemetry, 0, sizeof(StreamingTelemetry));
            m_telemetry.totalChunksInDataset = (uint32_t)m_chunks.size();

            if (!m_isLoaded || selections.empty()) return;

            for (const auto& sel : selections)
            {
                if (sel.chunkId >= m_chunks.size()) continue;

                const auto& chunk = m_chunks[sel.chunkId];
                uint32_t targetLOD = std::min(sel.selectedLOD, (uint32_t)chunk.lods.size() - 1);

                uint64_t key = ((uint64_t)sel.chunkId << 32) | targetLOD;

                auto it = m_cache.find(key);
                if (it != m_cache.end())
                {
                    it->second.lastUsedFrame = m_currentFrame;
                    outActiveSurfels.insert(outActiveSurfels.end(), it->second.surfels.begin(), it->second.surfels.end());
                    m_telemetry.cacheHits++;
                    m_telemetry.activeChunksRendered++;
                    if (targetLOD < kMaxTrackedLODLevels) m_telemetry.lodDistribution[targetLOD]++;
                }
                else
                {
                    // Cache miss: Load and decompress from disk
                    m_telemetry.cacheMisses++;
                    CachedChunkLOD loadedChunk;
                    loadedChunk.chunkId = sel.chunkId;
                    loadedChunk.lodLevel = targetLOD;
                    loadedChunk.lastUsedFrame = m_currentFrame;

                    if (LoadChunkFromDisk(chunk, targetLOD, loadedChunk.surfels))
                    {
                        outActiveSurfels.insert(outActiveSurfels.end(), loadedChunk.surfels.begin(), loadedChunk.surfels.end());
                        m_cache[key] = std::move(loadedChunk);
                        m_telemetry.activeChunksRendered++;
                        if (targetLOD < kMaxTrackedLODLevels) m_telemetry.lodDistribution[targetLOD]++;
                    }
                }
            }

            m_telemetry.totalSurfelsRendered = (uint32_t)outActiveSurfels.size();

            // Compute resident cache size
            size_t totalCachedSurfels = 0;
            for (const auto& pair : m_cache)
            {
                totalCachedSurfels += pair.second.surfels.size();
            }
            m_telemetry.residentMemoryMB = (totalCachedSurfels * sizeof(PackedSurfelGPU)) / (1024.0f * 1024.0f);
        }

    private:
        // Seeks to and reads one chunk's LOD payload from the open .sflw stream, then decompresses it
        bool LoadChunkFromDisk(const ChunkManifest& chunk, uint32_t lodLevel, std::vector<PackedSurfelGPU>& outSurfels)
        {
            if (lodLevel >= chunk.lods.size()) return false;
            const auto& lodInfo = chunk.lods[lodLevel];

            std::vector<uint8_t> compressedBuffer(lodInfo.compressedByteSize);
            {
                std::lock_guard<std::mutex> lock(m_streamMutex);
                m_sflwStream.seekg(lodInfo.fileOffset, std::ios::beg);
                m_sflwStream.read(reinterpret_cast<char*>(compressedBuffer.data()), lodInfo.compressedByteSize);
            }

            return ZstdDecompressor::DecompressChunk(
                compressedBuffer.data(),
                compressedBuffer.size(),
                lodInfo.surfelCount,
                outSurfels
            );
        }

        // Minimal hand-rolled parser for this project's own .json manifest schema (not general JSON)
        // Fills m_chunks from the open .sflw: the embedded manifest table on v4+ files, or the legacy
        // companion .json for packages written before the manifest moved inside the container.
        bool ParseManifest()
        {
            m_chunks.clear();
            if (m_header.version >= 4)
            {
                return ReadEmbeddedManifest(m_sflwStream, m_header, m_chunks) && !m_chunks.empty();
            }

            std::string legacyJson = m_sflwPath;
            if (legacyJson.size() >= 5) legacyJson = legacyJson.substr(0, legacyJson.size() - 5);
            legacyJson += ".json";
            return ParseLegacyJsonManifest(legacyJson, m_chunks);
        }

        std::string m_sflwPath;
        std::ifstream m_sflwStream;
        std::mutex m_streamMutex;

        SFLWFileHeader m_header = {};
        std::vector<ChunkManifest> m_chunks;
        std::unordered_map<uint64_t, CachedChunkLOD> m_cache;

        StreamingTelemetry m_telemetry;
        uint64_t m_currentFrame = 0;
        bool m_isLoaded = false;
    };
}

