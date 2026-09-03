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
        uint32_t lodDistribution[8]   = {};
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

        bool LoadDataset(const std::string& basepath)
        {
            Close();
            
            std::vector<std::string> searchBases = {
                basepath,
                "../datasets/" + basepath,
                "datasets/" + basepath,
                "../../datasets/" + basepath
            };

            bool manifestFound = false;
            for (const auto& sb : searchBases)
            {
                std::string sflwTry = sb + ".sflw";
                std::string jsonTry = sb + ".json";

                std::ifstream test(jsonTry);
                if (test.is_open())
                {
                    test.close();
                    m_sflwPath = sflwTry;
                    m_jsonPath = jsonTry;
                    manifestFound = true;
                    break;
                }
            }

            if (!manifestFound)
            {
                m_sflwPath = basepath + ".sflw";
                m_jsonPath = basepath + ".json";
            }

            if (!ParseManifest(m_jsonPath))
            {
                std::cerr << "Failed to parse manifest: " << m_jsonPath << std::endl;
                return false;
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
                    if (targetLOD < 8) m_telemetry.lodDistribution[targetLOD]++;
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
                        if (targetLOD < 8) m_telemetry.lodDistribution[targetLOD]++;
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

        bool ParseManifest(const std::string& jsonPath)
        {
            std::ifstream in(jsonPath);
            if (!in.is_open()) return false;

            m_chunks.clear();

            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();

            // Simple fast JSON parser for our specific manifest schema
            size_t pos = content.find("\"chunks\":");
            if (pos == std::string::npos) return false;

            while ((pos = content.find("\"id\":", pos)) != std::string::npos)
            {
                ChunkManifest cm = {};
                sscanf_s(content.c_str() + pos, "\"id\": %u,", &cm.chunkId);

                size_t bminPos = content.find("\"bounds_min\": [", pos);
                if (bminPos != std::string::npos)
                {
                    sscanf_s(content.c_str() + bminPos, "\"bounds_min\": [%f, %f, %f],", &cm.aabbMin.x, &cm.aabbMin.y, &cm.aabbMin.z);
                }

                size_t bmaxPos = content.find("\"bounds_max\": [", pos);
                if (bmaxPos != std::string::npos)
                {
                    sscanf_s(content.c_str() + bmaxPos, "\"bounds_max\": [%f, %f, %f],", &cm.aabbMax.x, &cm.aabbMax.y, &cm.aabbMax.z);
                }

                size_t ctrPos = content.find("\"center\": [", pos);
                if (ctrPos != std::string::npos)
                {
                    sscanf_s(content.c_str() + ctrPos, "\"center\": [%f, %f, %f],", &cm.center.x, &cm.center.y, &cm.center.z);
                }

                size_t radPos = content.find("\"radius\":", pos);
                if (radPos != std::string::npos)
                {
                    sscanf_s(content.c_str() + radPos, "\"radius\": %f,", &cm.boundingRadius);
                }

                size_t lodsPos = content.find("\"lods\": [", pos);
                size_t lodsEnd = content.find("]", lodsPos);
                if (lodsPos != std::string::npos && lodsEnd != std::string::npos)
                {
                    size_t curLod = lodsPos;
                    while ((curLod = content.find("{\"level\":", curLod)) != std::string::npos || (curLod = content.find("{ \"level\":", curLod)) != std::string::npos)
                    {
                        if (curLod > lodsEnd) break;
                        ChunkLODHeader lh = {};
                        sscanf_s(content.c_str() + curLod, "%*[^{]{%*[^0-9]%u%*[^0-9]%u%*[^0-9]%u%*[^0-9]%u%*[^0-9]%llu%*[^0-9]%f",
                            &lh.lodLevel, &lh.surfelCount, &lh.uncompressedByteSize, &lh.compressedByteSize, &lh.fileOffset, &lh.geometricError);
                        
                        // Parse individual fields with robust fallback
                        size_t lPos = content.find("\"level\":", curLod);
                        if (lPos != std::string::npos && lPos < lodsEnd) sscanf_s(content.c_str() + lPos, "\"level\": %u", &lh.lodLevel);

                        size_t cntPos = content.find("\"count\":", curLod);
                        if (cntPos != std::string::npos && cntPos < lodsEnd) sscanf_s(content.c_str() + cntPos, "\"count\": %u", &lh.surfelCount);

                        size_t rawPos = content.find("\"raw_bytes\":", curLod);
                        if (rawPos != std::string::npos && rawPos < lodsEnd) sscanf_s(content.c_str() + rawPos, "\"raw_bytes\": %u", &lh.uncompressedByteSize);

                        size_t cmpPos = content.find("\"compressed_bytes\":", curLod);
                        if (cmpPos != std::string::npos && cmpPos < lodsEnd) sscanf_s(content.c_str() + cmpPos, "\"compressed_bytes\": %u", &lh.compressedByteSize);

                        size_t offPos = content.find("\"offset\":", curLod);
                        if (offPos != std::string::npos && offPos < lodsEnd) sscanf_s(content.c_str() + offPos, "\"offset\": %llu", &lh.fileOffset);

                        size_t errPos = content.find("\"error\":", curLod);
                        if (errPos != std::string::npos && errPos < lodsEnd) sscanf_s(content.c_str() + errPos, "\"error\": %f", &lh.geometricError);

                        cm.lods.push_back(lh);
                        curLod += 10;
                    }
                }

                cm.numLODs = (uint32_t)cm.lods.size();
                m_chunks.push_back(std::move(cm));
                pos += 10;
            }

            return !m_chunks.empty();
        }

        std::string m_sflwPath;
        std::string m_jsonPath;
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

