#pragma once
#include <vector>
#include <cmath>
#include <random>
#include <fstream>
#include <iostream>
#include "../../src/DX12/Wavelet/WaveletTypes.h"

namespace Surfels
{
    class SyntheticGenerator
    {
    public:
        // Generates a synthetic urban city street scene with buildings, roads, sidewalks, and procedural details
        static std::vector<SurfelVertex> GenerateUrbanStreetScene(uint32_t targetPointCount = 500000)
        {
            std::vector<SurfelVertex> surfels;
            surfels.reserve(targetPointCount);

            std::mt19937 rng(42);
            std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
            std::uniform_real_distribution<float> jitter(-0.015f, 0.015f);

            // Scene bounds: Length 160m (Z: -80 to +80), Width 40m (X: -20 to +20)
            const float streetLength = 160.0f;
            const float streetWidth = 14.0f;
            const float sidewalkWidth = 4.0f;
            const float buildingHeight = 24.0f;

            // 1. Asphalt Road Surface (Flat plane at Y = 0.0)
            uint32_t roadPoints = targetPointCount / 4;
            for (uint32_t i = 0; i < roadPoints; i++)
            {
                float x = (dist01(rng) - 0.5f) * streetWidth;
                float z = (dist01(rng) - 0.5f) * streetLength;
                float y = 0.0f + jitter(rng) * 0.2f;

                SurfelVertex v;
                v.position = XMFLOAT3(x, y, z);
                v.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);

                // Road markings (yellow centerline / white borders)
                if (std::abs(x) < 0.25f && std::fmod(std::abs(z), 6.0f) < 3.0f)
                {
                    v.color = XMFLOAT3(0.95f, 0.85f, 0.15f); // Yellow centerline
                }
                else if (std::abs(std::abs(x) - (streetWidth * 0.5f - 0.4f)) < 0.2f)
                {
                    v.color = XMFLOAT3(0.95f, 0.95f, 0.95f); // White lane boundary
                }
                else
                {
                    float shade = 0.15f + dist01(rng) * 0.05f;
                    v.color = XMFLOAT3(shade, shade, shade * 1.05f); // Dark asphalt
                }
                v.radius = 0.06f;
                surfels.push_back(v);
            }

            // 2. Sidewalks (Elevated at Y = 0.15m on both sides)
            uint32_t sidewalkPoints = targetPointCount / 6;
            for (uint32_t i = 0; i < sidewalkPoints; i++)
            {
                int side = (dist01(rng) > 0.5f) ? 1 : -1;
                float x = side * (streetWidth * 0.5f + dist01(rng) * sidewalkWidth);
                float z = (dist01(rng) - 0.5f) * streetLength;
                float y = 0.15f + jitter(rng) * 0.1f;

                SurfelVertex v;
                v.position = XMFLOAT3(x, y, z);
                v.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);

                // Sidewalk concrete tile pattern
                float tileX = std::fmod(std::abs(x), 1.5f);
                float tileZ = std::fmod(std::abs(z), 1.5f);
                bool isSeam = (tileX < 0.05f || tileZ < 0.05f);

                float gray = isSeam ? 0.35f : (0.6f + dist01(rng) * 0.08f);
                v.color = XMFLOAT3(gray, gray * 0.98f, gray * 0.95f);
                v.radius = 0.05f;
                surfels.push_back(v);
            }

            // 3. Building Facades (Vertical planes on left and right)
            uint32_t buildingPoints = targetPointCount / 2;
            for (uint32_t i = 0; i < buildingPoints; i++)
            {
                int side = (dist01(rng) > 0.5f) ? 1 : -1;
                float x = side * (streetWidth * 0.5f + sidewalkWidth);
                float z = (dist01(rng) - 0.5f) * streetLength;
                float y = 0.15f + dist01(rng) * buildingHeight;

                SurfelVertex v;
                v.position = XMFLOAT3(x + jitter(rng) * 0.1f, y, z);
                v.normal = XMFLOAT3(-side * 1.0f, 0.0f, 0.0f); // Pointing towards street

                // Architectural windows & brickwork patterns
                float winFloor = std::fmod(y, 3.5f);
                float winCol = std::fmod(std::abs(z), 4.0f);
                bool isWindow = (winFloor > 1.2f && winFloor < 2.8f && winCol > 1.0f && winCol < 3.0f);

                if (isWindow)
                {
                    v.color = XMFLOAT3(0.15f, 0.25f, 0.4f); // Reflective window blue
                    v.radius = 0.04f;
                }
                else
                {
                    // Brick / concrete facades with variation per building block
                    int bldgIdx = (int)((z + streetLength * 0.5f) / 20.0f);
                    float rTint = (bldgIdx % 3 == 0) ? 0.75f : ((bldgIdx % 3 == 1) ? 0.65f : 0.85f);
                    float gTint = (bldgIdx % 3 == 0) ? 0.45f : ((bldgIdx % 3 == 1) ? 0.55f : 0.75f);
                    float bTint = (bldgIdx % 3 == 0) ? 0.35f : ((bldgIdx % 3 == 1) ? 0.45f : 0.65f);
                    float noise = dist01(rng) * 0.1f;
                    v.color = XMFLOAT3(rTint + noise, gTint + noise, bTint + noise);
                    v.radius = 0.055f;
                }
                surfels.push_back(v);
            }

            // 4. Street Trees / Sphere Foliage (placed periodically along sidewalks)
            uint32_t foliagePoints = targetPointCount - (uint32_t)surfels.size();
            const float treeInterval = 20.0f;
            int numTrees = (int)(streetLength / treeInterval);

            for (uint32_t i = 0; i < foliagePoints; i++)
            {
                int treeIdx = (int)(dist01(rng) * numTrees);
                int side = (dist01(rng) > 0.5f) ? 1 : -1;
                float treeZ = -streetLength * 0.5f + (treeIdx + 0.5f) * treeInterval;
                float treeX = side * (streetWidth * 0.5f + 1.2f);
                float treeY = 4.5f;

                // Sphere distribution for canopy
                float u = dist01(rng) * 2.0f - 1.0f;
                float phi = dist01(rng) * 6.2831853f;
                float r = 1.8f + dist01(rng) * 0.4f;
                float sq = std::sqrt(std::max(0.0f, 1.0f - u * u));

                XMFLOAT3 localDir(sq * std::cos(phi), u, sq * std::sin(phi));
                SurfelVertex v;
                v.position = XMFLOAT3(treeX + localDir.x * r, treeY + localDir.y * r, treeZ + localDir.z * r);
                v.normal = localDir;
                v.color = XMFLOAT3(0.12f + dist01(rng) * 0.08f, 0.55f + dist01(rng) * 0.2f, 0.15f + dist01(rng) * 0.08f);
                v.radius = 0.065f;
                surfels.push_back(v);
            }

            return surfels;
        }

        // Exports a vector of surfels to a standard binary PLY file
        static bool ExportToPLY(const std::string& filepath, const std::vector<SurfelVertex>& surfels)
        {
            std::ofstream out(filepath, std::ios::binary);
            if (!out.is_open()) return false;

            // Write ASCII Header
            out << "ply\n";
            out << "format binary_little_endian 1.0\n";
            out << "comment Generated by Surfels Synthetic Benchmark Generator\n";
            out << "element vertex " << surfels.size() << "\n";
            out << "property float x\n";
            out << "property float y\n";
            out << "property float z\n";
            out << "property float nx\n";
            out << "property float ny\n";
            out << "property float nz\n";
            out << "property uchar red\n";
            out << "property uchar green\n";
            out << "property uchar blue\n";
            out << "property float radius\n";
            out << "end_header\n";

            #pragma pack(push, 1)
            struct PLYVertexBinary
            {
                float x, y, z;
                float nx, ny, nz;
                uint8_t r, g, b;
                float radius;
            };
            #pragma pack(pop)

            std::vector<PLYVertexBinary> buffer(surfels.size());
            for (size_t i = 0; i < surfels.size(); i++)
            {
                const auto& s = surfels[i];
                PLYVertexBinary b;
                b.x = s.position.x;
                b.y = s.position.y;
                b.z = s.position.z;
                b.nx = s.normal.x;
                b.ny = s.normal.y;
                b.nz = s.normal.z;
                b.r = (uint8_t)std::max(0.0f, std::min(255.0f, s.color.x * 255.0f));
                b.g = (uint8_t)std::max(0.0f, std::min(255.0f, s.color.y * 255.0f));
                b.b = (uint8_t)std::max(0.0f, std::min(255.0f, s.color.z * 255.0f));
                b.radius = s.radius;
                buffer[i] = b;
            }

            out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(PLYVertexBinary));
            return out.good();
        }
    };
}

