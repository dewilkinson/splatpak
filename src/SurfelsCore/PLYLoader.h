// PLYLoader.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Binary and ASCII .ply point cloud parser. Understands both plain colored point
// clouds and 3D Gaussian Splatting exports (f_dc_*/scale_*/rot_*/opacity properties),
// converting the latter's COLMAP coordinate convention and spherical-harmonic DC
// color term into this project's own SurfelVertex representation.

#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cmath>
#include "WaveletTypes.h"
#include "../../libs/bluesec-codec/SplatCodec.h"

namespace Surfels
{
    class PLYLoader
    {
    public:
        // Fallback splat scale/radius when a vertex has no scale_0..2 or radius/size property at all.
        static constexpr float kDefaultSplatScale = 0.02f;

        // 3DGS opacity/scale heuristics: cull true-zero-opacity splats outright, and separately cull
        // low-opacity, large-footprint splats (typically background "floater" noise from reconstruction).
        static constexpr float kMinOpacityToKeep = 0.005f;
        static constexpr float kFloaterOpacityThreshold = 0.15f;
        static constexpr float kFloaterScaleThreshold = 0.04f;

        // A splat's rendered radius is this factor times its median (2nd-smallest) Gaussian scale axis.
        static constexpr float kMedianScaleToRadiusFactor = 1.25f;

        // outSplat (optional): when the file carries 3D Gaussian Splatting fields (f_dc_*, scale_*, rot_*,
        // opacity), every kept point's Gaussian is appended here and the point's sourceIndex refers to
        // it -- the input of a splat-mode bake. With outSplat given the floater cull is skipped, so the
        // large soft Gaussians that fill smooth surfaces survive. Left empty for plain point clouds.
        static bool LoadPLY(const std::string& filepath, std::vector<SurfelVertex>& outSurfels, double originOut[3],
                            std::vector<SplatAttributes>* outSplat = nullptr)
        {
            originOut[0] = 0.0;
            originOut[1] = 0.0;
            originOut[2] = 0.0;

            std::ifstream in(filepath, std::ios::binary);
            if (!in.is_open())
            {
                std::cerr << "Failed to open PLY file: " << filepath << std::endl;
                return false;
            }

            std::string line;
            if (!std::getline(in, line)) return false;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

            if (line != "ply")
            {
                std::cerr << "Invalid PLY header magic: '" << line << "'\n";
                return false;
            }

            bool isBinary = false;
            uint32_t vertexCount = 0;
            std::vector<std::string> propertyNames;
            std::vector<std::string> propertyTypes;

            while (std::getline(in, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line == "end_header") break;

                std::istringstream iss(line);
                std::string token;
                iss >> token;

                if (token == "format")
                {
                    std::string fmt;
                    iss >> fmt;
                    if (fmt == "binary_little_endian") isBinary = true;
                }
                else if (token == "element")
                {
                    std::string elemType;
                    iss >> elemType;
                    if (elemType == "vertex")
                    {
                        iss >> vertexCount;
                    }
                }
                else if (token == "property")
                {
                    std::string propType, propName;
                    iss >> propType >> propName;
                    propertyTypes.push_back(propType);
                    propertyNames.push_back(propName);
                }
            }

            if (vertexCount == 0)
            {
                std::cerr << "No vertex element found in PLY\n";
                return false;
            }

            outSurfels.clear();
            outSurfels.resize(vertexCount);

            if (isBinary)
            {
                // Property offsets and types
                int xOffset = -1, yOffset = -1, zOffset = -1;
                bool isXDouble = false, isYDouble = false, isZDouble = false;

                int nxOffset = -1, nyOffset = -1, nzOffset = -1;
                bool isNXDouble = false, isNYDouble = false, isNZDouble = false;

                int rOffset = -1, gOffset = -1, bOffset = -1;
                bool isRFloat = false, isGFloat = false, isBFloat = false;

                int fdc0Offset = -1, fdc1Offset = -1, fdc2Offset = -1;
                int scale0Offset = -1, scale1Offset = -1, scale2Offset = -1;
                int rot0Offset = -1, rot1Offset = -1, rot2Offset = -1, rot3Offset = -1;
                int opacityOffset = -1;
                int radOffset = -1;
                int frestOffset[kSplatSHMaxCoefficients];
                for (int k = 0; k < (int)kSplatSHMaxCoefficients; k++) frestOffset[k] = -1;

                int currentByte = 0;
                for (size_t i = 0; i < propertyNames.size(); i++)
                {
                    const auto& name = propertyNames[i];
                    const auto& type = propertyTypes[i];
                    int propSize = 1;

                    if (type == "float" || type == "float32" || type == "int" || type == "uint" || type == "int32" || type == "uint32")
                        propSize = 4;
                    else if (type == "double" || type == "float64" || type == "int64" || type == "uint64")
                        propSize = 8;
                    else if (type == "short" || type == "ushort" || type == "int16" || type == "uint16")
                        propSize = 2;
                    else
                        propSize = 1; // uchar, uint8, char, int8

                    bool isDbl = (propSize == 8);
                    bool isFlt = (type == "float" || type == "float32");

                    if (name == "x") { xOffset = currentByte; isXDouble = isDbl; }
                    else if (name == "y") { yOffset = currentByte; isYDouble = isDbl; }
                    else if (name == "z") { zOffset = currentByte; isZDouble = isDbl; }
                    else if (name == "nx") { nxOffset = currentByte; isNXDouble = isDbl; }
                    else if (name == "ny") { nyOffset = currentByte; isNYDouble = isDbl; }
                    else if (name == "nz") { nzOffset = currentByte; isNZDouble = isDbl; }
                    else if (name == "red" || name == "r" || name == "diffuse_red") { rOffset = currentByte; isRFloat = isFlt; }
                    else if (name == "green" || name == "g" || name == "diffuse_green") { gOffset = currentByte; isGFloat = isFlt; }
                    else if (name == "blue" || name == "b" || name == "diffuse_blue") { bOffset = currentByte; isBFloat = isFlt; }
                    else if (name == "f_dc_0") { fdc0Offset = currentByte; }
                    else if (name == "f_dc_1") { fdc1Offset = currentByte; }
                    else if (name == "f_dc_2") { fdc2Offset = currentByte; }
                    else if (name == "scale_0") { scale0Offset = currentByte; }
                    else if (name == "scale_1") { scale1Offset = currentByte; }
                    else if (name == "scale_2") { scale2Offset = currentByte; }
                    else if (name == "rot_0") { rot0Offset = currentByte; }
                    else if (name == "rot_1") { rot1Offset = currentByte; }
                    else if (name == "rot_2") { rot2Offset = currentByte; }
                    else if (name == "rot_3") { rot3Offset = currentByte; }
                    else if (name == "opacity") { opacityOffset = currentByte; }
                    else if (name == "radius" || name == "size") { radOffset = currentByte; }
                    else if (name.rfind("f_rest_", 0) == 0)
                    {
                        const int k = std::atoi(name.c_str() + 7);
                        if (k >= 0 && k < (int)kSplatSHMaxCoefficients && propSize == 4) frestOffset[k] = currentByte;
                    }

                    currentByte += propSize;
                }

                int vertexStride = currentByte;
                if (vertexStride <= 0) return false;

                std::vector<uint8_t> rawData((size_t)vertexCount * vertexStride);
                in.read(reinterpret_cast<char*>(rawData.data()), rawData.size());
                if (!in && in.gcount() < (std::streamsize)rawData.size())
                {
                    // Partial read fallback
                    vertexCount = (uint32_t)(in.gcount() / vertexStride);
                    outSurfels.resize(vertexCount);
                }

                // Check for large geospatial offset on first vertex
                double firstX = 0, firstY = 0, firstZ = 0;
                if (vertexCount > 0)
                {
                    const uint8_t* ptr0 = &rawData[0];
                    firstX = (xOffset >= 0) ? (isXDouble ? *reinterpret_cast<const double*>(ptr0 + xOffset) : *reinterpret_cast<const float*>(ptr0 + xOffset)) : 0.0;
                    firstY = (yOffset >= 0) ? (isYDouble ? *reinterpret_cast<const double*>(ptr0 + yOffset) : *reinterpret_cast<const float*>(ptr0 + yOffset)) : 0.0;
                    firstZ = (zOffset >= 0) ? (isZDouble ? *reinterpret_cast<const double*>(ptr0 + zOffset) : *reinterpret_cast<const float*>(ptr0 + zOffset)) : 0.0;

                    if (fdc0Offset >= 0)
                    {
                        firstY = -firstY;
                        firstZ = -firstZ;
                    }

                    if (std::abs(firstX) > kGeospatialOffsetThreshold || std::abs(firstY) > kGeospatialOffsetThreshold || std::abs(firstZ) > kGeospatialOffsetThreshold)
                    {
                        originOut[0] = firstX;
                        originOut[1] = firstY;
                        originOut[2] = firstZ;
                    }
                }

                outSurfels.clear();
                outSurfels.reserve(vertexCount);

                // Splat mode input: the full Gaussian per point, when asked for and the file has one.
                const bool keepSplat = outSplat != nullptr && fdc0Offset >= 0 && scale0Offset >= 0 && scale1Offset >= 0 && scale2Offset >= 0
                                       && rot0Offset >= 0 && rot1Offset >= 0 && rot2Offset >= 0 && rot3Offset >= 0;
                if (outSplat != nullptr) { outSplat->clear(); if (keepSplat) outSplat->reserve(vertexCount); }
                int frestCount = 0;
                while (frestCount < (int)kSplatSHMaxCoefficients && frestOffset[frestCount] >= 0) frestCount++;
                const int shPerChannel = frestCount / 3; // 3DGS lays f_rest out channel-major: all of R, then all of G, then all of B

                for (uint32_t i = 0; i < vertexCount; i++)
                {
                    const uint8_t* ptr = &rawData[i * vertexStride];
                    SurfelVertex v;

                    // 3DGS Opacity filter: only cull true zero opacities
                    float opacity = 1.0f;
                    if (opacityOffset >= 0)
                    {
                        float opLogit = *reinterpret_cast<const float*>(ptr + opacityOffset);
                        opacity = 1.0f / (1.0f + std::exp(-opLogit));
                        if (opacity < kMinOpacityToKeep)
                            continue;
                    }

                    double px = (xOffset >= 0) ? (isXDouble ? *reinterpret_cast<const double*>(ptr + xOffset) : *reinterpret_cast<const float*>(ptr + xOffset)) : 0.0;
                    double py = (yOffset >= 0) ? (isYDouble ? *reinterpret_cast<const double*>(ptr + yOffset) : *reinterpret_cast<const float*>(ptr + yOffset)) : 0.0;
                    double pz = (zOffset >= 0) ? (isZDouble ? *reinterpret_cast<const double*>(ptr + zOffset) : *reinterpret_cast<const float*>(ptr + zOffset)) : 0.0;

                    // Convert COLMAP / 3DGS coordinate system (+Y down, +Z forward) to DirectX 12 (+Y up)
                    if (fdc0Offset >= 0)
                    {
                        py = -py;
                        pz = -pz;
                    }

                    v.position.x = (float)(px - originOut[0]);
                    v.position.y = (float)(py - originOut[1]);
                    v.position.z = (float)(pz - originOut[2]);

                    float s0 = kDefaultSplatScale, s1 = kDefaultSplatScale, s2 = kDefaultSplatScale;
                    if (scale0Offset >= 0 && scale1Offset >= 0 && scale2Offset >= 0)
                    {
                        s0 = std::exp(*reinterpret_cast<const float*>(ptr + scale0Offset));
                        s1 = std::exp(*reinterpret_cast<const float*>(ptr + scale1Offset));
                        s2 = std::exp(*reinterpret_cast<const float*>(ptr + scale2Offset));

                        float sortedS[3] = { s0, s1, s2 };
                        std::sort(sortedS, sortedS + 3);

                        // Cull large low-opacity air floaters / background noise. Not in splat mode: those
                        // large soft Gaussians are what fills smooth surfaces and backgrounds in a 3DGS scene.
                        if (!keepSplat && opacity < kFloaterOpacityThreshold && sortedS[1] > kFloaterScaleThreshold)
                            continue;

                        // Continuous surface footprint: median scale times kMedianScaleToRadiusFactor
                        v.radius = std::max(1e-5f, sortedS[1] * kMedianScaleToRadiusFactor);
                    }
                    else if (radOffset >= 0)
                    {
                        v.radius = std::max(1e-5f, *reinterpret_cast<const float*>(ptr + radOffset));
                        s0 = s1 = s2 = v.radius;
                    }
                    else
                    {
                        v.radius = kDefaultSplatScale;
                        s0 = s1 = s2 = kDefaultSplatScale;
                    }

                    if (nxOffset >= 0)
                    {
                        v.normal.x = isNXDouble ? (float)*reinterpret_cast<const double*>(ptr + nxOffset) : *reinterpret_cast<const float*>(ptr + nxOffset);
                        v.normal.y = isNYDouble ? (float)*reinterpret_cast<const double*>(ptr + nyOffset) : *reinterpret_cast<const float*>(ptr + nyOffset);
                        v.normal.z = isNZDouble ? (float)*reinterpret_cast<const double*>(ptr + nzOffset) : *reinterpret_cast<const float*>(ptr + nzOffset);
                    }
                    else if (rot0Offset >= 0 && rot1Offset >= 0 && rot2Offset >= 0 && rot3Offset >= 0)
                    {
                        float qw = *reinterpret_cast<const float*>(ptr + rot0Offset);
                        float qx = *reinterpret_cast<const float*>(ptr + rot1Offset);
                        float qy = *reinterpret_cast<const float*>(ptr + rot2Offset);
                        float qz = *reinterpret_cast<const float*>(ptr + rot3Offset);
                        float qlen = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
                        if (qlen > 1e-6f) { qw /= qlen; qx /= qlen; qy /= qlen; qz /= qlen; }

                        // In 3DGS, the surface normal corresponds to the thinnest/smallest scale dimension
                        int minIdx = 0;
                        if (s1 <= s0 && s1 <= s2) minIdx = 1;
                        else if (s2 <= s0 && s2 <= s1) minIdx = 2;

                        if (minIdx == 0) // Column 0 (Local X axis)
                        {
                            v.normal.x = 1.0f - 2.0f * (qy * qy + qz * qz);
                            v.normal.y = 2.0f * (qx * qy + qw * qz);
                            v.normal.z = 2.0f * (qx * qz - qw * qy);
                        }
                        else if (minIdx == 1) // Column 1 (Local Y axis)
                        {
                            v.normal.x = 2.0f * (qx * qy - qw * qz);
                            v.normal.y = 1.0f - 2.0f * (qx * qx + qz * qz);
                            v.normal.z = 2.0f * (qy * qz + qw * qx);
                        }
                        else // Column 2 (Local Z axis)
                        {
                            v.normal.x = 2.0f * (qx * qz + qw * qy);
                            v.normal.y = 2.0f * (qy * qz - qw * qx);
                            v.normal.z = 1.0f - 2.0f * (qx * qx + qy * qy);
                        }
                    }
                    else
                    {
                        v.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
                    }

                    if (fdc0Offset >= 0)
                    {
                        v.normal.y = -v.normal.y;
                        v.normal.z = -v.normal.z;
                    }
                    float nlen = std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
                    if (nlen > 1e-6f) { v.normal.x /= nlen; v.normal.y /= nlen; v.normal.z /= nlen; }

                    // Color extraction
                    if (rOffset >= 0)
                    {
                        v.color.x = isRFloat ? *reinterpret_cast<const float*>(ptr + rOffset) : *(ptr + rOffset) / 255.0f;
                        v.color.y = isGFloat ? *reinterpret_cast<const float*>(ptr + gOffset) : *(ptr + gOffset) / 255.0f;
                        v.color.z = isBFloat ? *reinterpret_cast<const float*>(ptr + bOffset) : *(ptr + bOffset) / 255.0f;
                    }
                    else if (fdc0Offset >= 0 && fdc1Offset >= 0 && fdc2Offset >= 0)
                    {
                        // 3D Gaussian Splatting Spherical Harmonics DC color (SH degree 0)
                        static const float SH_C0 = 0.28209479177387814f;
                        float f0 = *reinterpret_cast<const float*>(ptr + fdc0Offset);
                        float f1 = *reinterpret_cast<const float*>(ptr + fdc1Offset);
                        float f2 = *reinterpret_cast<const float*>(ptr + fdc2Offset);

                        v.color.x = std::max(0.0f, std::min(1.0f, 0.5f + SH_C0 * f0));
                        v.color.y = std::max(0.0f, std::min(1.0f, 0.5f + SH_C0 * f1));
                        v.color.z = std::max(0.0f, std::min(1.0f, 0.5f + SH_C0 * f2));
                    }
                    else
                    {
                        v.color = XMFLOAT3(0.85f, 0.85f, 0.85f);
                    }

                    if (keepSplat)
                    {
                        SplatAttributes a;
                        a.scale = XMFLOAT3(s0, s1, s2);
                        float qw = *reinterpret_cast<const float*>(ptr + rot0Offset);
                        float qx = *reinterpret_cast<const float*>(ptr + rot1Offset);
                        float qy = *reinterpret_cast<const float*>(ptr + rot2Offset);
                        float qz = *reinterpret_cast<const float*>(ptr + rot3Offset);
                        const float ql = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
                        if (ql > 1e-8f) { qw /= ql; qx /= ql; qy /= ql; qz /= ql; } else { qw = 1.0f; qx = qy = qz = 0.0f; }
                        // The positions above were turned by a half turn about X (+Y down / +Z forward to +Y up);
                        // the Gaussian's frame and its harmonics take the same turn.
                        a.rotation = SplatCodec::RotateQuaternionHalfTurnX(XMFLOAT4(qx, qy, qz, qw));
                        a.opacity = opacity;
                        a.bakedLevel = 0;
                        for (int c = 0; c < 3; c++)
                            for (int k = 0; k < 15; k++)
                            {
                                const int idx = c * shPerChannel + k;
                                a.sh[3 * k + c] = (k < shPerChannel && idx < frestCount) ? *reinterpret_cast<const float*>(ptr + frestOffset[idx]) : 0.0f;
                            }
                        SplatCodec::ApplyAxisFlipToSH(a.sh);
                        v.sourceIndex = (uint32_t)outSplat->size();
                        outSplat->push_back(a);
                    }

                    outSurfels.push_back(v);
                }
            }
            else
            {
                // ASCII parsing
                for (uint32_t i = 0; i < vertexCount; i++)
                {
                    SurfelVertex v;
                    double px = 0, py = 0, pz = 0;
                    in >> px >> py >> pz;
                    if (i == 0 && (std::abs(px) > kGeospatialOffsetThreshold || std::abs(py) > kGeospatialOffsetThreshold || std::abs(pz) > kGeospatialOffsetThreshold))
                    {
                        originOut[0] = px;
                        originOut[1] = py;
                        originOut[2] = pz;
                    }
                    v.position.x = (float)(px - originOut[0]);
                    v.position.y = (float)(py - originOut[1]);
                    v.position.z = (float)(pz - originOut[2]);
                    v.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
                    v.color = XMFLOAT3(0.85f, 0.85f, 0.85f);
                    v.radius = 0.05f;
                    outSurfels[i] = v;
                }
            }

            return !outSurfels.empty();
        }
    };
}

