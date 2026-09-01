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
                    // 2. Quantize to 8-byte PackedSurfelGPU using dataset Global Bounding Box
                    // This guarantees that all streamed chunks unpack with the exact same global AABB in Mesh Shaders & GPU Sorting!
                    auto packedSurfels = Quantizer::QuantizeSurfels(lod.surfels, gMin, gMax);

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

        struct SFLWPackageData
        {
            SFLWFileHeader header = {};
            std::vector<ChunkManifest> chunkManifests;
            std::vector<std::vector<PackedSurfelGPU>> chunkLOD0Surfels;
            uint64_t totalSurfels = 0;
            size_t totalCompressedBytes = 0;
        };

        static bool LoadPackage(const std::string& inputPath, SFLWPackageData& outPackage)
        {
            std::string sflwPath = inputPath;
            std::string jsonPath = inputPath;

            std::string lowerPath = inputPath;
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
            if (lowerPath.size() >= 5 && lowerPath.substr(lowerPath.size() - 5) == ".sflw")
            {
                sflwPath = inputPath;
                jsonPath = inputPath.substr(0, inputPath.size() - 5) + ".json";
            }
            else if (lowerPath.size() >= 5 && lowerPath.substr(lowerPath.size() - 5) == ".json")
            {
                jsonPath = inputPath;
                sflwPath = inputPath.substr(0, inputPath.size() - 5) + ".sflw";
            }
            else
            {
                sflwPath = inputPath + ".sflw";
                jsonPath = inputPath + ".json";
            }

            std::ifstream sflwIn(sflwPath, std::ios::binary);
            if (!sflwIn.is_open())
            {
                std::cerr << "Failed to open .sflw file: " << sflwPath << std::endl;
                return false;
            }

            sflwIn.read(reinterpret_cast<char*>(&outPackage.header), sizeof(SFLWFileHeader));
            if (outPackage.header.magic != SFLW_MAGIC)
            {
                std::cerr << "Invalid SFLW magic header in " << sflwPath << std::endl;
                return false;
            }

            std::ifstream jsonIn(jsonPath);
            if (!jsonIn.is_open())
            {
                std::cerr << "Failed to open companion .json manifest: " << jsonPath << std::endl;
                return false;
            }

            std::string content((std::istreambuf_iterator<char>(jsonIn)), std::istreambuf_iterator<char>());
            jsonIn.close();

            outPackage.chunkManifests.clear();
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
                outPackage.chunkManifests.push_back(std::move(cm));
                pos += 10;
            }

            outPackage.chunkLOD0Surfels.resize(outPackage.chunkManifests.size());
            outPackage.totalSurfels = 0;
            outPackage.totalCompressedBytes = 0;

            for (size_t c = 0; c < outPackage.chunkManifests.size(); c++)
            {
                const auto& cm = outPackage.chunkManifests[c];
                if (cm.lods.empty()) continue;

                const auto& lod0 = cm.lods[0];
                outPackage.totalCompressedBytes += lod0.compressedByteSize;
                std::vector<uint8_t> compressedBytes(lod0.compressedByteSize);
                sflwIn.seekg(lod0.fileOffset, std::ios::beg);
                sflwIn.read(reinterpret_cast<char*>(compressedBytes.data()), lod0.compressedByteSize);

                std::vector<uint8_t> shuffled(lod0.uncompressedByteSize);
                if (ByteShuffle::DecompressShuffled(compressedBytes.data(), compressedBytes.size(), shuffled.data(), lod0.uncompressedByteSize))
                {
                    outPackage.chunkLOD0Surfels[c].resize(lod0.surfelCount);
                    ByteShuffle::Unshuffle(
                        shuffled.data(),
                        reinterpret_cast<uint8_t*>(outPackage.chunkLOD0Surfels[c].data()),
                        lod0.surfelCount,
                        sizeof(PackedSurfelGPU)
                    );
                    outPackage.totalSurfels += lod0.surfelCount;
                }
            }

            sflwIn.close();
            return true;
        }
    };
}

