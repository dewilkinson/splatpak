// SPLATLoader.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Reads and writes the compact 32-byte-per-splat binary ".splat" format (a fixed-layout
// alternative to .ply for 3D Gaussian Splatting data): position, scale, RGBA8 color,
// and an 8-bit-quantized orientation quaternion.

#pragma once
#include <fstream>
#include <vector>
#include <string>
#include <iostream>
#include <cmath>
#include "../../src/DX12/Wavelet/WaveletTypes.h"

namespace Surfels
{
    class SPLATLoader
    {
    public:
        // 8-bit quaternion quantization midpoint/scale: stored byte = round(component * kQuatQuantScale + kQuatQuantBias)
        static constexpr float kQuatQuantScale = 128.0f;
        static constexpr float kQuatQuantBias  = 128.0f;

        // Effective splat radius is clamped to this range after averaging the three Gaussian scale axes.
        static constexpr float kMinSplatRadius = 0.005f;
        static constexpr float kMaxSplatRadius = 1.0f;

        #pragma pack(push, 1)
        struct SplatRaw
        {
            float pos[3];       // 12 bytes
            float scale[3];     // 12 bytes
            uint8_t rgba[4];    // 4 bytes: r, g, b, a
            uint8_t rot[4];     // 4 bytes: q0, q1, q2, q3 (mapped [-1, 1] via (q*128)+128)
        };
        #pragma pack(pop)

        static_assert(sizeof(SplatRaw) == 32, "SplatRaw must be exactly 32 bytes");

        // Loads a binary .splat (3D Gaussian Splatting) file into SurfelVertex array
        static bool LoadSPLAT(const std::string& filepath, std::vector<SurfelVertex>& outSurfels, double originOut[3])
        {
            originOut[0] = 0.0;
            originOut[1] = 0.0;
            originOut[2] = 0.0;

            std::ifstream in(filepath, std::ios::binary | std::ios::ate);
            if (!in.is_open())
            {
                std::cerr << "Failed to open SPLAT file: " << filepath << std::endl;
                return false;
            }

            std::streamsize fileSize = in.tellg();
            in.seekg(0, std::ios::beg);

            if (fileSize < (std::streamsize)sizeof(SplatRaw) || (fileSize % sizeof(SplatRaw) != 0))
            {
                std::cerr << "Invalid SPLAT file size (not a multiple of 32 bytes): " << fileSize << " bytes\n";
                return false;
            }

            size_t splatCount = (size_t)(fileSize / sizeof(SplatRaw));
            std::vector<SplatRaw> rawSplats(splatCount);
            in.read(reinterpret_cast<char*>(rawSplats.data()), fileSize);
            in.close();

            outSurfels.clear();
            outSurfels.resize(splatCount);

            // Check for large origin offset on first splat
            if (splatCount > 0)
            {
                float fx = rawSplats[0].pos[0];
                float fy = rawSplats[0].pos[1];
                float fz = rawSplats[0].pos[2];
                if (std::abs(fx) > kGeospatialOffsetThreshold || std::abs(fy) > kGeospatialOffsetThreshold || std::abs(fz) > kGeospatialOffsetThreshold)
                {
                    originOut[0] = fx;
                    originOut[1] = fy;
                    originOut[2] = fz;
                }
            }

            for (size_t i = 0; i < splatCount; i++)
            {
                const auto& raw = rawSplats[i];
                SurfelVertex v;

                // 1. Position
                v.position.x = (float)(raw.pos[0] - originOut[0]);
                v.position.y = (float)(raw.pos[1] - originOut[1]);
                v.position.z = (float)(raw.pos[2] - originOut[2]);

                // 2. Color (RGB)
                v.color.x = raw.rgba[0] / 255.0f;
                v.color.y = raw.rgba[1] / 255.0f;
                v.color.z = raw.rgba[2] / 255.0f;

                // 3. Normal Vector from Quaternion Rotation
                // Unquantize quaternion from [0..255] back to [-1.0..1.0]
                float qr = (raw.rot[0] - kQuatQuantBias) / kQuatQuantScale;
                float qi = (raw.rot[1] - kQuatQuantBias) / kQuatQuantScale;
                float qj = (raw.rot[2] - kQuatQuantBias) / kQuatQuantScale;
                float qk = (raw.rot[3] - kQuatQuantBias) / kQuatQuantScale;

                float qLen = std::sqrt(qr * qr + qi * qi + qj * qj + qk * qk);
                if (qLen > 1e-6f)
                {
                    qr /= qLen; qi /= qLen; qj /= qLen; qk /= qLen;
                }
                else
                {
                    qr = 1.0f; qi = 0.0f; qj = 0.0f; qk = 0.0f;
                }

                // Transform local normal vector (0, 0, 1) by quaternion rotation matrix
                // R * [0, 0, 1]^T = (2*(qi*qk + qr*qj), 2*(qj*qk - qr*qi), 1 - 2*(qi^2 + qj^2))
                float nx = 2.0f * (qi * qk + qr * qj);
                float ny = 2.0f * (qj * qk - qr * qi);
                float nz = 1.0f - 2.0f * (qi * qi + qj * qj);

                float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (nLen > 1e-5f)
                {
                    v.normal = XMFLOAT3(nx / nLen, ny / nLen, nz / nLen);
                }
                else
                {
                    v.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
                }

                // 4. Effective splat radius (average of major scale axes)
                float avgScale = (std::abs(raw.scale[0]) + std::abs(raw.scale[1]) + std::abs(raw.scale[2])) / 3.0f;
                v.radius = std::max(kMinSplatRadius, std::min(kMaxSplatRadius, avgScale));

                outSurfels[i] = v;
            }

            return true;
        }

        // Exports a vector of surfels to the binary 32-byte .splat format
        static bool ExportToSPLAT(const std::string& filepath, const std::vector<SurfelVertex>& surfels)
        {
            std::ofstream out(filepath, std::ios::binary);
            if (!out.is_open()) return false;

            std::vector<SplatRaw> buffer(surfels.size());
            for (size_t i = 0; i < surfels.size(); i++)
            {
                const auto& s = surfels[i];
                SplatRaw r = {};

                r.pos[0] = s.position.x;
                r.pos[1] = s.position.y;
                r.pos[2] = s.position.z;

                r.scale[0] = s.radius;
                r.scale[1] = s.radius;
                r.scale[2] = s.radius * 0.25f; // Flatter disk profile

                r.rgba[0] = (uint8_t)std::max(0.0f, std::min(255.0f, s.color.x * 255.0f));
                r.rgba[1] = (uint8_t)std::max(0.0f, std::min(255.0f, s.color.y * 255.0f));
                r.rgba[2] = (uint8_t)std::max(0.0f, std::min(255.0f, s.color.z * 255.0f));
                r.rgba[3] = 255;

                // Construct rotation quaternion aligning (0, 0, 1) with normal vector
                XMFLOAT3 n = s.normal;
                float dot = n.z; // dot((0,0,1), n)

                float qw = 1.0f + dot;
                float qx = -n.y;
                float qy = n.x;
                float qz = 0.0f;

                float len = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
                if (len > 1e-5f)
                {
                    qw /= len; qx /= len; qy /= len; qz /= len;
                }
                else
                {
                    qw = 1.0f; qx = 0.0f; qy = 0.0f; qz = 0.0f;
                }

                r.rot[0] = (uint8_t)std::max(0, std::min(255, (int)std::round(qw * kQuatQuantScale + kQuatQuantBias)));
                r.rot[1] = (uint8_t)std::max(0, std::min(255, (int)std::round(qx * kQuatQuantScale + kQuatQuantBias)));
                r.rot[2] = (uint8_t)std::max(0, std::min(255, (int)std::round(qy * kQuatQuantScale + kQuatQuantBias)));
                r.rot[3] = (uint8_t)std::max(0, std::min(255, (int)std::round(qz * kQuatQuantScale + kQuatQuantBias)));

                buffer[i] = r;
            }

            out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(SplatRaw));
            return out.good();
        }
    };
}

