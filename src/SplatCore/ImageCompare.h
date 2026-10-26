// ImageCompare.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Screen-image comparison for the LOD quality test (SplatPakApp::StepLodQualityTest): RGBA8 images,
// PSNR and SSIM over a crop, an error heatmap, and a dependency-free PNG writer (stored deflate
// blocks, so the files are large but need no zlib).

#pragma once
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace Splat
{
    struct RgbaImage
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> pixels; // width * height * 4, row-major, top row first
        bool Empty() const { return width == 0 || height == 0 || pixels.size() < (size_t)width * height * 4; }
    };

    struct PixelRect { uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; }; // Half-open [x0, x1) x [y0, y1)

    namespace ImageCompare
    {
        // Bounding box of the pixels brighter than the background in any of the images, grown by a margin.
        inline PixelRect ContentBounds(const std::vector<const RgbaImage*>& images, uint32_t margin = 8, int threshold = 6)
        {
            PixelRect r{ UINT32_MAX, UINT32_MAX, 0, 0 };
            uint32_t w = 0, h = 0;
            for (const RgbaImage* img : images)
            {
                if (!img || img->Empty()) continue;
                w = img->width; h = img->height;
                for (uint32_t y = 0; y < img->height; y++)
                {
                    const uint8_t* row = img->pixels.data() + (size_t)y * img->width * 4;
                    for (uint32_t x = 0; x < img->width; x++)
                    {
                        if (row[x * 4] > threshold || row[x * 4 + 1] > threshold || row[x * 4 + 2] > threshold)
                        {
                            r.x0 = std::min(r.x0, x); r.y0 = std::min(r.y0, y);
                            r.x1 = std::max(r.x1, x + 1); r.y1 = std::max(r.y1, y + 1);
                        }
                    }
                }
            }
            if (r.x0 == UINT32_MAX) return PixelRect{ 0, 0, w, h };
            r.x0 = (r.x0 > margin) ? r.x0 - margin : 0;
            r.y0 = (r.y0 > margin) ? r.y0 - margin : 0;
            r.x1 = std::min(w, r.x1 + margin);
            r.y1 = std::min(h, r.y1 + margin);
            return r;
        }

        // PSNR over the RGB channels inside rect (dB; 99 for identical images).
        inline double Psnr(const RgbaImage& a, const RgbaImage& b, const PixelRect& rect)
        {
            double se = 0.0; uint64_t n = 0;
            for (uint32_t y = rect.y0; y < rect.y1; y++)
                for (uint32_t x = rect.x0; x < rect.x1; x++)
                {
                    const size_t i = ((size_t)y * a.width + x) * 4;
                    for (int c = 0; c < 3; c++) { const double d = (double)a.pixels[i + c] - (double)b.pixels[i + c]; se += d * d; }
                    n += 3;
                }
            if (n == 0) return 0.0;
            const double mse = se / (double)n;
            return (mse <= 1e-12) ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
        }

        // Mean SSIM of the luma inside rect: 8x8 windows on a 4-pixel stride, the standard constants.
        inline double Ssim(const RgbaImage& a, const RgbaImage& b, const PixelRect& rect)
        {
            auto luma = [](const RgbaImage& img, uint32_t x, uint32_t y)
            {
                const uint8_t* p = img.pixels.data() + ((size_t)y * img.width + x) * 4;
                return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            };
            const double c1 = (0.01 * 255.0) * (0.01 * 255.0), c2 = (0.03 * 255.0) * (0.03 * 255.0);
            const uint32_t win = 8, step = 4;
            double sum = 0.0; uint64_t count = 0;
            for (uint32_t y = rect.y0; y + win <= rect.y1; y += step)
                for (uint32_t x = rect.x0; x + win <= rect.x1; x += step)
                {
                    double ma = 0, mb = 0;
                    for (uint32_t j = 0; j < win; j++) for (uint32_t i = 0; i < win; i++) { ma += luma(a, x + i, y + j); mb += luma(b, x + i, y + j); }
                    const double n = (double)(win * win);
                    ma /= n; mb /= n;
                    double va = 0, vb = 0, cov = 0;
                    for (uint32_t j = 0; j < win; j++) for (uint32_t i = 0; i < win; i++)
                    {
                        const double da = luma(a, x + i, y + j) - ma, db = luma(b, x + i, y + j) - mb;
                        va += da * da; vb += db * db; cov += da * db;
                    }
                    va /= (n - 1); vb /= (n - 1); cov /= (n - 1);
                    sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
                    count++;
                }
            return count ? sum / (double)count : 0.0;
        }

        inline RgbaImage Crop(const RgbaImage& src, const PixelRect& rect)
        {
            RgbaImage out;
            out.width = rect.x1 - rect.x0; out.height = rect.y1 - rect.y0;
            out.pixels.resize((size_t)out.width * out.height * 4);
            for (uint32_t y = 0; y < out.height; y++)
                std::copy_n(src.pixels.data() + ((size_t)(rect.y0 + y) * src.width + rect.x0) * 4, (size_t)out.width * 4, out.pixels.data() + (size_t)y * out.width * 4);
            return out;
        }

        // Per-pixel RGB error, amplified 4x, as a grey-to-red ramp (black = identical).
        inline RgbaImage ErrorMap(const RgbaImage& a, const RgbaImage& b)
        {
            RgbaImage out; out.width = a.width; out.height = a.height;
            out.pixels.resize(a.pixels.size());
            for (size_t i = 0; i + 3 < a.pixels.size(); i += 4)
            {
                int e = 0;
                for (int c = 0; c < 3; c++) e = std::max(e, std::abs((int)a.pixels[i + c] - (int)b.pixels[i + c]));
                const int v = std::min(255, e * 4);
                out.pixels[i] = (uint8_t)v; out.pixels[i + 1] = (uint8_t)(v / 3); out.pixels[i + 2] = (uint8_t)(v / 6); out.pixels[i + 3] = 255;
            }
            return out;
        }

        // Places images side by side with a gap (all the same height).
        inline RgbaImage SideBySide(const std::vector<const RgbaImage*>& images, uint32_t gap = 6)
        {
            RgbaImage out;
            for (const RgbaImage* img : images) { out.width += img->width; out.height = std::max(out.height, img->height); }
            if (!images.empty()) out.width += gap * (uint32_t)(images.size() - 1);
            out.pixels.assign((size_t)out.width * out.height * 4, 0);
            for (size_t i = 3; i < out.pixels.size(); i += 4) { out.pixels[i - 3] = out.pixels[i - 2] = out.pixels[i - 1] = 60; out.pixels[i] = 255; }
            uint32_t xOff = 0;
            for (const RgbaImage* img : images)
            {
                for (uint32_t y = 0; y < img->height; y++)
                    std::copy_n(img->pixels.data() + (size_t)y * img->width * 4, (size_t)img->width * 4, out.pixels.data() + ((size_t)y * out.width + xOff) * 4);
                xOff += img->width + gap;
            }
            return out;
        }

        inline bool WritePng(const std::string& path, const RgbaImage& img)
        {
            if (img.Empty()) return false;
            static uint32_t crcTable[256];
            static bool crcInit = false;
            if (!crcInit)
            {
                for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTable[n] = c; }
                crcInit = true;
            }
            auto crc = [&](const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) { for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c; };
            auto be32 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); };
            std::vector<uint8_t> file = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
            auto chunk = [&](const char* type, const std::vector<uint8_t>& data)
            {
                be32(file, (uint32_t)data.size());
                std::vector<uint8_t> body(type, type + 4);
                body.insert(body.end(), data.begin(), data.end());
                file.insert(file.end(), body.begin(), body.end());
                be32(file, crc(body.data(), body.size()) ^ 0xFFFFFFFFu);
            };
            std::vector<uint8_t> ihdr;
            be32(ihdr, img.width); be32(ihdr, img.height);
            ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB
            chunk("IHDR", ihdr);

            std::vector<uint8_t> raw; raw.reserve((size_t)img.height * (img.width * 3 + 1));
            for (uint32_t y = 0; y < img.height; y++)
            {
                raw.push_back(0);
                const uint8_t* row = img.pixels.data() + (size_t)y * img.width * 4;
                for (uint32_t x = 0; x < img.width; x++) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
            }
            std::vector<uint8_t> z = { 0x78, 0x01 };
            for (size_t pos = 0; pos < raw.size() || pos == 0; )
            {
                const size_t len = std::min<size_t>(65535, raw.size() - pos);
                const bool last = (pos + len >= raw.size());
                z.push_back(last ? 1 : 0);
                z.push_back((uint8_t)len); z.push_back((uint8_t)(len >> 8));
                z.push_back((uint8_t)~len); z.push_back((uint8_t)(~len >> 8));
                z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
                pos += len;
                if (last) break;
            }
            uint32_t s1 = 1, s2 = 0;
            for (uint8_t b : raw) { s1 = (s1 + b) % 65521u; s2 = (s2 + s1) % 65521u; }
            be32(z, (s2 << 16) | s1);
            chunk("IDAT", z);
            chunk("IEND", {});

            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
            const bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
            fclose(f);
            return ok;
        }
    }
}
