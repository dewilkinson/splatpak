#pragma once
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include "SpatialOctree.h"
#include "LiftingWavelet.h"
#include "Quantizer.h"
#include "ByteShuffle.h"

namespace Surfels
{
    class StreamPackager
    {
    public:
        static bool PackageDataset(
            const std::string& outputBasepath,
            std::vector<ChunkData>& chunks,
            uint32_t maxLODs = 4,
            float deadbandThreshold = 0.003f)
        {
            std::string sflwPath = outputBasepath + ".sflw";
            std::string jsonPath = outputBasepath + ".json";

            std::ofstream sflwOut(sflwPath, std::ios::binary);
            if (!sflwOut.is_open())
            {
                std::cerr << "Failed to open output file: " << sflwPath << std::endl;
                return false;
            }

            // Global Bounding Box
            XMFLOAT3 gMin(1e9f, 1e9f, 1e9f);
            XMFLOAT3 gMax(-1e9f, -1e9f, -1e9f);
            uint64_t totalSurfelsLOD0 = 0;

            for (const auto& c : chunks)
            {
                gMin.x = std::min(gMin.x, c.aabbMin.x);
                gMin.y = std::min(gMin.y, c.aabbMin.y);
                gMin.z = std::min(gMin.z, c.aabbMin.z);

                gMax.x = std::max(gMax.x, c.aabbMax.x);
                gMax.y = std::max(gMax.y, c.aabbMax.y);
                gMax.z = std::max(gMax.z, c.aabbMax.z);

                totalSurfelsLOD0 += c.surfels.size();
            }

            // Write File Header placeholder
            SFLWFileHeader header = {};
            header.magic = SFLW_MAGIC;
            header.version = SFLW_VERSION;
            header.numChunks = (uint32_t)chunks.size();
            header.maxLOD = maxLODs;
            header.totalSurfelsLOD0 = totalSurfelsLOD0;
            header.globalOriginX = 0.0;
            header.globalOriginY = 0.0;
            header.globalOriginZ = 0.0;
            header.globalBoundsMin = gMin;
            header.globalBoundsMax = gMax;

            sflwOut.write(reinterpret_cast<const char*>(&header), sizeof(SFLWFileHeader));

            std::vector<ChunkManifest> chunkManifests;
            chunkManifests.reserve(chunks.size());

            size_t totalRawBytes = 0;
            size_t totalCompressedBytes = 0;

            for (size_t i = 0; i < chunks.size(); i++)
            {
                auto& chunk = chunks[i];

                // 1. Decompose via Lifting Wavelet
                auto waveletResult = LiftingWavelet::DecomposeChunk(chunk.surfels, maxLODs, deadbandThreshold);

                ChunkManifest cm = {};
                cm.chunkId = chunk.chunkId;
                cm.aabbMin = chunk.aabbMin;
                cm.aabbMax = chunk.aabbMax;
                cm.center = chunk.center;
                cm.boundingRadius = chunk.boundingRadius;
                cm.numLODs = (uint32_t)waveletResult.lodLevels.size();

                for (const auto& lod : waveletResult.lodLevels)
                {
                    // 2. Quantize to 8-byte PackedSurfelGPU
                    auto packedSurfels = Quantizer::QuantizeSurfels(lod.surfels, chunk.aabbMin, chunk.aabbMax);

                    // 3. Byte-Shuffle
                    size_t uncompressedBytes = packedSurfels.size() * sizeof(PackedSurfelGPU);
                    auto shuffled = ByteShuffle::Shuffle(
                        reinterpret_cast<const uint8_t*>(packedSurfels.data()),
                        packedSurfels.size(),
                        sizeof(PackedSurfelGPU)
                    );

                    // 4. Compress
                    auto compressed = ByteShuffle::CompressShuffled(shuffled);

                    uint64_t currentFileOffset = (uint64_t)sflwOut.tellp();
                    sflwOut.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());

                    ChunkLODHeader lh = {};
                    lh.lodLevel = lod.level;
                    lh.surfelCount = (uint32_t)packedSurfels.size();
                    lh.uncompressedByteSize = (uint32_t)uncompressedBytes;
                    lh.compressedByteSize = (uint32_t)compressed.size();
                    lh.fileOffset = currentFileOffset;
                    lh.geometricError = lod.geometricError;

                    cm.lods.push_back(lh);

                    totalRawBytes += uncompressedBytes;
                    totalCompressedBytes += compressed.size();
                }

                chunkManifests.push_back(std::move(cm));
            }

            sflwOut.close();

            // Write manifest.json
            std::ofstream jsonOut(jsonPath);
            if (!jsonOut.is_open()) return false;

            jsonOut << "{\n";
            jsonOut << "  \"version\": " << SFLW_VERSION << ",\n";
            jsonOut << "  \"total_surfels_lod0\": " << totalSurfelsLOD0 << ",\n";
            jsonOut << "  \"num_chunks\": " << chunkManifests.size() << ",\n";
            jsonOut << "  \"max_lods\": " << maxLODs << ",\n";
            jsonOut << "  \"bounds_min\": [" << gMin.x << ", " << gMin.y << ", " << gMin.z << "],\n";
            jsonOut << "  \"bounds_max\": [" << gMax.x << ", " << gMax.y << ", " << gMax.z << "],\n";
            jsonOut << "  \"chunks\": [\n";

            for (size_t i = 0; i < chunkManifests.size(); i++)
            {
                const auto& c = chunkManifests[i];
                jsonOut << "    {\n";
                jsonOut << "      \"id\": " << c.chunkId << ",\n";
                jsonOut << "      \"bounds_min\": [" << c.aabbMin.x << ", " << c.aabbMin.y << ", " << c.aabbMin.z << "],\n";
                jsonOut << "      \"bounds_max\": [" << c.aabbMax.x << ", " << c.aabbMax.y << ", " << c.aabbMax.z << "],\n";
                jsonOut << "      \"center\": [" << c.center.x << ", " << c.center.y << ", " << c.center.z << "],\n";
                jsonOut << "      \"radius\": " << c.boundingRadius << ",\n";
                jsonOut << "      \"lods\": [\n";

                for (size_t j = 0; j < c.lods.size(); j++)
                {
                    const auto& l = c.lods[j];
                    jsonOut << "        { \"level\": " << l.lodLevel
                            << ", \"count\": " << l.surfelCount
                            << ", \"raw_bytes\": " << l.uncompressedByteSize
                            << ", \"compressed_bytes\": " << l.compressedByteSize
                            << ", \"offset\": " << l.fileOffset
                            << ", \"error\": " << l.geometricError << " }";
                    if (j + 1 < c.lods.size()) jsonOut << ",";
                    jsonOut << "\n";
                }

                jsonOut << "      ]\n";
                jsonOut << "    }";
                if (i + 1 < chunkManifests.size()) jsonOut << ",";
                jsonOut << "\n";
            }

            jsonOut << "  ]\n";
            jsonOut << "}\n";
            jsonOut.close();

            std::cout << "Successfully packaged " << totalSurfelsLOD0 << " surfels across " << chunks.size() << " chunks.\n";
            std::cout << "Uncompressed packed size: " << (totalRawBytes / 1024.0 / 1024.0) << " MB\n";
            std::cout << "Compressed package size:  " << (totalCompressedBytes / 1024.0 / 1024.0) << " MB\n";
            std::cout << "Compression ratio:        " << (totalRawBytes > 0 ? (float)totalRawBytes / totalCompressedBytes : 1.0f) << "x\n";

            return true;
        }
    };
}

