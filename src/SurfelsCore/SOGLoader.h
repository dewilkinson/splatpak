// SOGLoader.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Reads PlayCanvas ".sog" (Spatially Ordered Gaussians) packages: a ZIP container of a
// meta.json descriptor plus several WebP-encoded texture channels (position/quaternion/
// scale/color, each one Gaussian per pixel). Includes a small self-contained ZIP/DEFLATE
// decompressor (RFC 1951/1950) since the format is just a standard ZIP archive, and uses
// Windows Imaging Component (WIC) to decode the WebP textures themselves.

#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <wincodec.h>
#include <shlwapi.h>
#include "WaveletTypes.h"

#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace Surfels
{
    class SOGLoader
    {
    private:
        // Minimal Deflate / Inflate decompressor for ZIP archives (RFC 1951)
        struct BitStream
        {
            const uint8_t* data;
            size_t size;
            size_t bytePos = 0;
            uint32_t bitBuf = 0;
            int bitCount = 0;

            BitStream(const uint8_t* d, size_t s) : data(d), size(s) {}

            uint32_t GetBits(int n)
            {
                while (bitCount < n)
                {
                    if (bytePos < size)
                    {
                        bitBuf |= ((uint32_t)data[bytePos++]) << bitCount;
                        bitCount += 8;
                    }
                    else
                    {
                        break;
                    }
                }
                uint32_t res = bitBuf & ((1u << n) - 1);
                bitBuf >>= n;
                bitCount -= n;
                return res;
            }
        };

        // Simple Huffman Table for Deflate
        struct HuffmanTable
        {
            uint16_t counts[16] = {};
            uint16_t symbols[288] = {};

            bool Build(const uint8_t* lengths, size_t numSymbols)
            {
                std::fill(std::begin(counts), std::end(counts), (uint16_t)0);
                for (size_t i = 0; i < numSymbols; i++)
                {
                    if (lengths[i] > 0 && lengths[i] < 16)
                        counts[lengths[i]]++;
                }

                uint16_t offsets[16] = {};
                uint16_t code = 0;
                for (int i = 1; i < 16; i++)
                {
                    code = (code + counts[i - 1]) << 1;
                    offsets[i] = (i == 1) ? 0 : (offsets[i - 1] + counts[i - 1]);
                }

                uint16_t nextCode[16] = {};
                std::copy(std::begin(offsets), std::end(offsets), std::begin(nextCode));

                for (size_t i = 0; i < numSymbols; i++)
                {
                    uint8_t len = lengths[i];
                    if (len > 0 && len < 16)
                    {
                        symbols[nextCode[len]++] = (uint16_t)i;
                    }
                }
                return true;
            }

            int Decode(BitStream& bs) const
            {
                uint16_t code = 0;
                uint16_t count = 0;
                uint16_t first = 0;
                uint16_t index = 0;

                for (int i = 1; i < 16; i++)
                {
                    code = (code << 1) | (uint16_t)bs.GetBits(1);
                    count = counts[i];
                    if (code < first + count)
                    {
                        return symbols[index + (code - first)];
                    }
                    index += count;
                    first += count;
                    first <<= 1;
                }
                return -1;
            }
        };

        static bool InflateRaw(const uint8_t* inData, size_t inSize, std::vector<uint8_t>& out)
        {
            BitStream bs(inData, inSize);
            bool isFinal = false;

            static const uint16_t lenBase[29] = {
                3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
            };
            static const uint8_t lenExtra[29] = {
                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
            };
            static const uint16_t distBase[30] = {
                1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
            };
            static const uint8_t distExtra[30] = {
                0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
            };
            static const uint8_t clOrder[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };

            while (!isFinal)
            {
                isFinal = (bs.GetBits(1) != 0);
                uint32_t bType = bs.GetBits(2);

                if (bType == 0) // Uncompressed
                {
                    bs.bitBuf = 0;
                    bs.bitCount = 0;
                    if (bs.bytePos + 4 > inSize) return false;
                    uint16_t len = (uint16_t)(bs.data[bs.bytePos] | (bs.data[bs.bytePos + 1] << 8));
                    bs.bytePos += 4;
                    if (bs.bytePos + len > inSize) return false;
                    out.insert(out.end(), bs.data + bs.bytePos, bs.data + bs.bytePos + len);
                    bs.bytePos += len;
                }
                else if (bType == 1 || bType == 2) // Fixed or Dynamic Huffman
                {
                    HuffmanTable litTable, distTable;

                    if (bType == 1) // Fixed
                    {
                        uint8_t lits[288];
                        for (int i = 0; i <= 143; i++) lits[i] = 8;
                        for (int i = 144; i <= 255; i++) lits[i] = 9;
                        for (int i = 256; i <= 279; i++) lits[i] = 7;
                        for (int i = 280; i <= 287; i++) lits[i] = 8;
                        litTable.Build(lits, 288);

                        uint8_t dists[32];
                        for (int i = 0; i < 32; i++) dists[i] = 5;
                        distTable.Build(dists, 32);
                    }
                    else // Dynamic
                    {
                        uint32_t hLit = bs.GetBits(5) + 257;
                        uint32_t hDist = bs.GetBits(5) + 1;
                        uint32_t hCLen = bs.GetBits(4) + 4;

                        uint8_t clLengths[19] = {};
                        for (uint32_t i = 0; i < hCLen; i++)
                        {
                            clLengths[clOrder[i]] = (uint8_t)bs.GetBits(3);
                        }

                        HuffmanTable clTable;
                        clTable.Build(clLengths, 19);

                        std::vector<uint8_t> codeLengths(hLit + hDist, 0);
                        size_t idx = 0;
                        while (idx < hLit + hDist)
                        {
                            int sym = clTable.Decode(bs);
                            if (sym < 0) return false;
                            if (sym < 16)
                            {
                                codeLengths[idx++] = (uint8_t)sym;
                            }
                            else if (sym == 16)
                            {
                                if (idx == 0) return false;
                                uint8_t prev = codeLengths[idx - 1];
                                uint32_t repeat = bs.GetBits(2) + 3;
                                while (repeat-- && idx < hLit + hDist) codeLengths[idx++] = prev;
                            }
                            else if (sym == 17)
                            {
                                uint32_t repeat = bs.GetBits(3) + 3;
                                while (repeat-- && idx < hLit + hDist) codeLengths[idx++] = 0;
                            }
                            else if (sym == 18)
                            {
                                uint32_t repeat = bs.GetBits(7) + 11;
                                while (repeat-- && idx < hLit + hDist) codeLengths[idx++] = 0;
                            }
                        }

                        litTable.Build(codeLengths.data(), hLit);
                        distTable.Build(codeLengths.data() + hLit, hDist);
                    }

                    while (true)
                    {
                        int sym = litTable.Decode(bs);
                        if (sym < 0) return false;
                        if (sym < 256)
                        {
                            out.push_back((uint8_t)sym);
                        }
                        else if (sym == 256)
                        {
                            break; // End of block
                        }
                        else if (sym <= 285)
                        {
                            uint32_t lenIdx = sym - 257;
                            uint32_t length = lenBase[lenIdx] + bs.GetBits(lenExtra[lenIdx]);
                            int distSym = distTable.Decode(bs);
                            if (distSym < 0 || distSym >= 30) return false;
                            uint32_t distance = distBase[distSym] + bs.GetBits(distExtra[distSym]);

                            if (out.size() < distance) return false;
                            size_t srcOffset = out.size() - distance;
                            for (uint32_t k = 0; k < length; k++)
                            {
                                out.push_back(out[srcOffset + k]);
                            }
                        }
                        else
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    return false; // Reserved block type
                }
            }
            return true;
        }

        // Extracts files from in-memory ZIP container
        static bool ExtractZip(const std::vector<uint8_t>& zipData, std::unordered_map<std::string, std::vector<uint8_t>>& outFiles)
        {
            if (zipData.size() < 22) return false;

            size_t pos = 0;
            while (pos + 30 <= zipData.size())
            {
                uint32_t sig = *reinterpret_cast<const uint32_t*>(&zipData[pos]);
                if (sig != 0x04034b50) // Local file header signature
                    break;

                uint16_t method = *reinterpret_cast<const uint16_t*>(&zipData[pos + 8]);
                uint32_t compSize = *reinterpret_cast<const uint32_t*>(&zipData[pos + 18]);
                uint32_t uncompSize = *reinterpret_cast<const uint32_t*>(&zipData[pos + 22]);
                uint16_t nameLen = *reinterpret_cast<const uint16_t*>(&zipData[pos + 26]);
                uint16_t extraLen = *reinterpret_cast<const uint16_t*>(&zipData[pos + 28]);

                size_t namePos = pos + 30;
                size_t dataPos = namePos + nameLen + extraLen;
                if (dataPos + compSize > zipData.size()) return false;

                std::string filename(reinterpret_cast<const char*>(&zipData[namePos]), nameLen);
                
                // Normalize slashes
                for (char& c : filename) { if (c == '\\') c = '/'; }
                // Strip directory prefixes if present
                size_t lastSlash = filename.find_last_of('/');
                std::string baseFilename = (lastSlash != std::string::npos) ? filename.substr(lastSlash + 1) : filename;

                std::vector<uint8_t> uncompressed;
                if (method == 0) // Stored
                {
                    uncompressed.assign(zipData.begin() + dataPos, zipData.begin() + dataPos + compSize);
                }
                else if (method == 8) // Deflated
                {
                    uncompressed.reserve(uncompSize);
                    if (!InflateRaw(zipData.data() + dataPos, compSize, uncompressed))
                    {
                        std::cerr << "Failed to deflate entry: " << filename << std::endl;
                    }
                }

                if (!baseFilename.empty() && !uncompressed.empty())
                {
                    outFiles[baseFilename] = std::move(uncompressed);
                }

                pos = dataPos + compSize;
            }
            return !outFiles.empty();
        }

        // Decodes WebP/PNG image buffer to 32bpp RGBA pixel buffer using Windows Imaging Component (WIC)
        struct DecodedImage
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> pixels; // RGBA 32-bit (width * height * 4)
        };

        static bool DecodeImageWIC(const std::vector<uint8_t>& imgData, DecodedImage& outImg)
        {
            if (imgData.empty()) return false;

            HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            bool needUninit = SUCCEEDED(hr);

            IWICImagingFactory* pFactory = nullptr;
            hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&pFactory);
            if (FAILED(hr) || !pFactory)
            {
                if (needUninit) CoUninitialize();
                return false;
            }

            IStream* pStream = SHCreateMemStream(imgData.data(), (UINT)imgData.size());
            if (!pStream)
            {
                pFactory->Release();
                if (needUninit) CoUninitialize();
                return false;
            }

            IWICBitmapDecoder* pDecoder = nullptr;
            hr = pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDecoder);
            if (FAILED(hr) || !pDecoder)
            {
                pStream->Release();
                pFactory->Release();
                if (needUninit) CoUninitialize();
                return false;
            }

            IWICBitmapFrameDecode* pFrame = nullptr;
            hr = pDecoder->GetFrame(0, &pFrame);
            if (FAILED(hr) || !pFrame)
            {
                pDecoder->Release();
                pStream->Release();
                pFactory->Release();
                if (needUninit) CoUninitialize();
                return false;
            }

            UINT w = 0, h = 0;
            pFrame->GetSize(&w, &h);
            outImg.width = w;
            outImg.height = h;

            IWICFormatConverter* pConverter = nullptr;
            hr = pFactory->CreateFormatConverter(&pConverter);
            if (SUCCEEDED(hr) && pConverter)
            {
                hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeCustom);
                if (SUCCEEDED(hr))
                {
                    outImg.pixels.resize((size_t)w * h * 4);
                    hr = pConverter->CopyPixels(NULL, w * 4, (UINT)outImg.pixels.size(), outImg.pixels.data());
                }
                pConverter->Release();
            }

            pFrame->Release();
            pDecoder->Release();
            pStream->Release();
            pFactory->Release();
            if (needUninit) CoUninitialize();

            return SUCCEEDED(hr) && !outImg.pixels.empty();
        }

        // Lightweight JSON string parser for SOG meta.json
        static float ParseFloat(const std::string& str, size_t& pos)
        {
            while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\r' || str[pos] == '\n' || str[pos] == ',' || str[pos] == '[' || str[pos] == ']'))
                pos++;
            size_t start = pos;
            if (pos < str.size() && (str[pos] == '-' || str[pos] == '+')) pos++;
            while (pos < str.size() && ((str[pos] >= '0' && str[pos] <= '9') || str[pos] == '.' || str[pos] == 'e' || str[pos] == 'E' || str[pos] == '-' || str[pos] == '+'))
                pos++;
            if (start == pos) return 0.0f;
            try { return std::stof(str.substr(start, pos - start)); } catch (...) { return 0.0f; }
        }

        static void ParseVec3(const std::string& json, const std::string& key, XMFLOAT3& outVec)
        {
            size_t p = json.find(key);
            if (p != std::string::npos)
            {
                p = json.find('[', p);
                if (p != std::string::npos)
                {
                    p++;
                    outVec.x = ParseFloat(json, p);
                    outVec.y = ParseFloat(json, p);
                    outVec.z = ParseFloat(json, p);
                }
            }
        }

        static uint32_t ParseCount(const std::string& json)
        {
            size_t p = json.find("\"count\"");
            if (p != std::string::npos)
            {
                p = json.find(':', p);
                if (p != std::string::npos)
                {
                    p++;
                    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
                    size_t start = p;
                    while (p < json.size() && json[p] >= '0' && json[p] <= '9') p++;
                    if (start < p)
                    {
                        try { return (uint32_t)std::stoul(json.substr(start, p - start)); } catch (...) {}
                    }
                }
            }
            return 0;
        }

    public:
        // Loads a PlayCanvas .sog (Spatially Ordered Gaussians) ZIP package
        static bool LoadSOG(const std::string& filepath, std::vector<SurfelVertex>& outSurfels, double originOut[3])
        {
            originOut[0] = 0.0;
            originOut[1] = 0.0;
            originOut[2] = 0.0;

            std::ifstream file(filepath, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                std::cerr << "Failed to open .sog file: " << filepath << std::endl;
                return false;
            }

            std::streamsize fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            if (fileSize < 30) return false;

            std::vector<uint8_t> zipBuffer((size_t)fileSize);
            file.read(reinterpret_cast<char*>(zipBuffer.data()), fileSize);
            file.close();

            std::unordered_map<std::string, std::vector<uint8_t>> files;
            if (!ExtractZip(zipBuffer, files))
            {
                std::cerr << "Failed to extract .sog ZIP archive: " << filepath << std::endl;
                return false;
            }

            // 1. Parse meta.json
            auto itMeta = files.find("meta.json");
            if (itMeta == files.end())
            {
                itMeta = files.find("lod-meta.json");
            }
            if (itMeta == files.end())
            {
                std::cerr << ".sog missing meta.json descriptor" << std::endl;
                return false;
            }

            std::string metaJson(reinterpret_cast<const char*>(itMeta->second.data()), itMeta->second.size());
            uint32_t splatCount = ParseCount(metaJson);

            XMFLOAT3 meansMins(-1.0f, -1.0f, -1.0f);
            XMFLOAT3 meansMaxs(1.0f, 1.0f, 1.0f);
            ParseVec3(metaJson, "\"mins\"", meansMins);
            ParseVec3(metaJson, "\"maxs\"", meansMaxs);

            XMFLOAT3 scalesMins(-10.0f, -10.0f, -10.0f);
            XMFLOAT3 scalesMaxs(0.0f, 0.0f, 0.0f);
            size_t scalesPos = metaJson.find("\"scales\"");
            if (scalesPos != std::string::npos)
            {
                std::string scalesSub = metaJson.substr(scalesPos, 300);
                ParseVec3(scalesSub, "\"mins\"", scalesMins);
                ParseVec3(scalesSub, "\"maxs\"", scalesMaxs);
            }

            // 2. Decode WebP Textures
            DecodedImage imgMeansU, imgMeansL, imgQuats, imgScales, imgColors;

            auto itMeansU = files.find("means_u.webp");
            auto itMeansL = files.find("means_l.webp");
            auto itMeansSingle = files.find("means.webp");

            if (itMeansU != files.end()) DecodeImageWIC(itMeansU->second, imgMeansU);
            if (itMeansL != files.end()) DecodeImageWIC(itMeansL->second, imgMeansL);
            if (itMeansSingle != files.end() && imgMeansU.pixels.empty()) DecodeImageWIC(itMeansSingle->second, imgMeansU);

            auto itQuats = files.find("quats.webp");
            if (itQuats != files.end()) DecodeImageWIC(itQuats->second, imgQuats);

            auto itScales = files.find("scales.webp");
            if (itScales != files.end()) DecodeImageWIC(itScales->second, imgScales);

            auto itColors = files.find("colors.webp");
            if (itColors == files.end()) itColors = files.find("sh0.webp");
            if (itColors != files.end()) DecodeImageWIC(itColors->second, imgColors);

            if (imgMeansU.pixels.empty())
            {
                std::cerr << "Failed to decode position textures from .sog" << std::endl;
                return false;
            }

            size_t maxPossibleSplats = (size_t)imgMeansU.width * imgMeansU.height;
            if (splatCount == 0 || splatCount > maxPossibleSplats)
            {
                splatCount = (uint32_t)maxPossibleSplats;
            }

            outSurfels.clear();
            outSurfels.resize(splatCount);

            XMFLOAT3 meansRange(meansMaxs.x - meansMins.x, meansMaxs.y - meansMins.y, meansMaxs.z - meansMins.z);
            XMFLOAT3 scalesRange(scalesMaxs.x - scalesMins.x, scalesMaxs.y - scalesMins.y, scalesMaxs.z - scalesMins.z);

            bool hasMeansL = !imgMeansL.pixels.empty() && (imgMeansL.pixels.size() == imgMeansU.pixels.size());
            bool hasQuats = !imgQuats.pixels.empty() && (imgQuats.pixels.size() >= splatCount * 4);
            bool hasScales = !imgScales.pixels.empty() && (imgScales.pixels.size() >= splatCount * 4);
            bool hasColors = !imgColors.pixels.empty() && (imgColors.pixels.size() >= splatCount * 4);

            #pragma omp parallel for
            for (int i = 0; i < (int)splatCount; i++)
            {
                size_t pIdx = (size_t)i * 4;
                SurfelVertex v;

                // 1. Reconstruct 16-bit Positions
                uint16_t qx = 0, qy = 0, qz = 0;
                if (hasMeansL)
                {
                    qx = ((uint16_t)imgMeansU.pixels[pIdx + 0] << 8) | imgMeansL.pixels[pIdx + 0];
                    qy = ((uint16_t)imgMeansU.pixels[pIdx + 1] << 8) | imgMeansL.pixels[pIdx + 1];
                    qz = ((uint16_t)imgMeansU.pixels[pIdx + 2] << 8) | imgMeansL.pixels[pIdx + 2];
                }
                else
                {
                    qx = (uint16_t)imgMeansU.pixels[pIdx + 0] * 257;
                    qy = (uint16_t)imgMeansU.pixels[pIdx + 1] * 257;
                    qz = (uint16_t)imgMeansU.pixels[pIdx + 2] * 257;
                }

                v.position.x = meansMins.x + (qx / 65535.0f) * meansRange.x;
                v.position.y = meansMins.y + (qy / 65535.0f) * meansRange.y;
                v.position.z = meansMins.z + (qz / 65535.0f) * meansRange.z;

                // 2. Decode RGB Colors
                if (hasColors)
                {
                    v.color.x = imgColors.pixels[pIdx + 0] / 255.0f;
                    v.color.y = imgColors.pixels[pIdx + 1] / 255.0f;
                    v.color.z = imgColors.pixels[pIdx + 2] / 255.0f;
                }
                else
                {
                    v.color = XMFLOAT3(0.9f, 0.9f, 0.9f);
                }

                // 3. Decode Rotation Quaternion -> Normal Vector
                if (hasQuats)
                {
                    float qx_f = (imgQuats.pixels[pIdx + 0] - 128.0f) / 128.0f;
                    float qy_f = (imgQuats.pixels[pIdx + 1] - 128.0f) / 128.0f;
                    float qz_f = (imgQuats.pixels[pIdx + 2] - 128.0f) / 128.0f;
                    float qw_f = (imgQuats.pixels[pIdx + 3] - 128.0f) / 128.0f;

                    float qLen = std::sqrt(qx_f * qx_f + qy_f * qy_f + qz_f * qz_f + qw_f * qw_f);
                    if (qLen > 1e-5f)
                    {
                        qx_f /= qLen; qy_f /= qLen; qz_f /= qLen; qw_f /= qLen;
                    }
                    else
                    {
                        qw_f = 1.0f; qx_f = 0.0f; qy_f = 0.0f; qz_f = 0.0f;
                    }

                    // Rotate local vector (0, 0, 1) by quaternion
                    float nx = 2.0f * (qx_f * qz_f + qw_f * qy_f);
                    float ny = 2.0f * (qy_f * qz_f - qw_f * qx_f);
                    float nz = 1.0f - 2.0f * (qx_f * qx_f + qy_f * qy_f);

                    float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
                    v.normal = (nLen > 1e-5f) ? XMFLOAT3(nx / nLen, ny / nLen, nz / nLen) : XMFLOAT3(0.0f, 1.0f, 0.0f);
                }
                else
                {
                    v.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
                }

                // 4. Decode Scales -> Surfel Radius
                if (hasScales)
                {
                    float s0 = scalesMins.x + (imgScales.pixels[pIdx + 0] / 255.0f) * scalesRange.x;
                    float s1 = scalesMins.y + (imgScales.pixels[pIdx + 1] / 255.0f) * scalesRange.y;
                    float s2 = scalesMins.z + (imgScales.pixels[pIdx + 2] / 255.0f) * scalesRange.z;

                    // Log scale to linear if negative range
                    float r0 = (scalesMins.x < 0.0f) ? std::exp(s0) : s0;
                    float r1 = (scalesMins.y < 0.0f) ? std::exp(s1) : s1;
                    float r2 = (scalesMins.z < 0.0f) ? std::exp(s2) : s2;

                    v.radius = std::max(0.001f, std::min(5.0f, (r0 + r1 + r2) / 3.0f));
                }
                else
                {
                    v.radius = 0.02f;
                }

                outSurfels[i] = v;
            }

            return true;
        }
    };
}

