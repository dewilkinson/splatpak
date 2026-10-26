// RdoAnalysis.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Rate-distortion analysis of a baked scan, phase 1 of docs/design/gstx-rdo.md: every spherical-harmonics
// encoding the package could use for the whole model (degree, and bits per band) is measured for its size and
// for its colour fidelity, so the Package Bake tab can draw the scan's own size-against-quality curve and pick
// the best encoding for a target.
//
// Rate. Each candidate's SH goes through what the packager does (StreamPackager::PackageDataset): requantised to
// its bits, coded as residuals from the next coarser level (SHCoding.h), the coarsest level as YCoCg, byte
// shuffled and compressed with Brotli. The streams are compressed in windows of kUnitSplats level 0 splats
// (Morton order, so a window is a compact patch of the model, about a chunk's worth) at a fast Brotli quality,
// scaled by the ratio that a few windows give at the package quality. The records (20 bytes per splat) are
// measured once: they do not depend on the SH choice beyond one byte.
//
// Distortion. Every 16th level 0 splat (kSampleStride): its colour error over kViewDirections view directions,
// the candidate's decoded harmonics against the source's full-precision ones (dropped bands count in full), weighted
// by how much the splat shows (opacity x the area of its two largest axes). Reported as a colour PSNR in dB over
// the [0, 1] colour range. It ranks the candidates as rendered views do but reads lower than an image PSNR, whose
// blending averages the per-splat error away (Cthulhu: 4 bits at 35 dB here, within the render's noise there).
//
// Streaming (the only bandwidth-related target): what a viewer downloads is the blocks a view needs, not the package, so the
// streaming measure is per view. A typical close view -- the model filling the screen, from kViewCount directions around it --
// needs the level 0 windows in its frustum on the model's near side plus that share of the coarser levels; its first view is
// that share of the two coarsest levels. RdoPoint::viewBytes and viewFirstBytes are the median over the views (Finish).
//
// Per-chunk estimate (for phase 2, format 15). The level 0 windows are also the units of a per-region choice: for
// a Lagrange multiplier each window takes the candidate minimising its distortion + lambda x its size, and sweeping
// lambda gives the per-region curve. PerRegionSaving() compares it with the whole-model candidate at the same
// distortion.

#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "../../libs/gstx/SplatCodec.h"
#include "../../libs/gstx/SHCoding.h"
#include "../../libs/gstx/ColourTransform.h"
#include "../../libs/gstx/ByteShuffle.h"
#include "PayloadCodec.h"

namespace Splat
{
    struct RdoSettings
    {
        uint32_t shDegree = 3;
        uint8_t  shBits[3] = { 8, 8, 8 }; // Bands 1, 2, 3 (bands above the degree are not stored)
        bool operator==(const RdoSettings& o) const
        {
            if (shDegree != o.shDegree) return false;
            for (uint32_t b = 0; b < shDegree && b < 3; b++) if (shBits[b] != o.shBits[b]) return false;
            return true;
        }
        std::string Describe() const
        {
            char buf[64];
            if (shDegree == 0) return "no harmonics";
            int n = snprintf(buf, sizeof(buf), "SH degree %u, ", shDegree);
            bool uniform = true;
            for (uint32_t b = 1; b < shDegree; b++) uniform &= (shBits[b] == shBits[0]);
            if (uniform) snprintf(buf + n, sizeof(buf) - n, "%u bits", (unsigned)shBits[0]);
            else if (shDegree == 2) snprintf(buf + n, sizeof(buf) - n, "%u/%u bits", (unsigned)shBits[0], (unsigned)shBits[1]);
            else snprintf(buf + n, sizeof(buf) - n, "%u/%u/%u bits", (unsigned)shBits[0], (unsigned)shBits[1], (unsigned)shBits[2]);
            return buf;
        }
    };

    struct RdoPoint
    {
        RdoSettings settings;
        double bytes = 0.0;        // Predicted package payload bytes (records + SH, every level)
        double coarseBytes = 0.0;  // Of which the two coarsest levels: what streams first
        double mse = 0.0;          // Weighted colour error (squared, [0, 1] colour)
        double psnr = 0.0;         // 10 log10(1 / mse)
        bool   frontier = false;   // On the size-against-quality frontier: no other candidate is both smaller and better
        double regionSaving = 0.0; // PerRegionSaving for this candidate (filled by Analyse; kept in a profile, unlike the unit tables)
        double viewBytes = 0.0;      // A typical close view's download to full detail (median over the views; Finish)
        double viewFirstBytes = 0.0; // and to its first, coarse picture
    };

    struct RdoAnalysisResult
    {
        std::vector<RdoPoint> points;
        double recordBytes = 0.0, recordCoarseBytes = 0.0;
        double seconds = 0.0;
        // Per-region estimate: size and distortion of every level 0 window under every candidate, [unit * points + candidate]
        size_t units = 0;
        std::vector<double> unitBytes, unitError;
        // Where each unit is (x, y, z), its level 0 records' size, and the model's bounding sphere: for the per-view sizes
        std::vector<float>  unitCenter;
        std::vector<double> unitRecordBytes;
        double level0RecordBytes = 0.0;
        float  center[3] = { 0.0f, 0.0f, 0.0f };
        float  radius = 0.0f;

        bool Valid() const { return !points.empty(); }

        // The frontier in order of size
        std::vector<size_t> Frontier() const
        {
            std::vector<size_t> f;
            for (size_t i = 0; i < points.size(); i++) if (points[i].frontier) f.push_back(i);
            std::sort(f.begin(), f.end(), [&](size_t a, size_t b) { return points[a].bytes < points[b].bytes; });
            return f;
        }

        // How much smaller the level 0 harmonics would be with a per-region choice at the distortion of candidate c,
        // as a fraction of the whole package (0 when it would not help)
        double PerRegionSaving(size_t c) const
        {
            const size_t n = points.size();
            if (units == 0 || c >= n || unitBytes.size() != units * n) return 0.0;
            double globalBytes = 0.0, globalError = 0.0;
            for (size_t u = 0; u < units; u++) { globalBytes += unitBytes[u * n + c]; globalError += unitError[u * n + c]; }
            if (globalBytes <= 0.0) return 0.0;
            // lambda spans the trade-offs between the candidates' error per byte
            const double scale = (globalError > 0.0 ? globalError : 1e-12) / globalBytes;
            double bestBytes = globalBytes;
            for (int k = -120; k <= 120; k++)
            {
                const double lambda = scale * std::pow(10.0, k / 12.0);
                double r = 0.0, d = 0.0;
                for (size_t u = 0; u < units; u++)
                {
                    size_t pick = 0; double best = 1e300;
                    for (size_t j = 0; j < n; j++)
                    {
                        const double cost = unitError[u * n + j] + lambda * unitBytes[u * n + j];
                        if (cost < best) { best = cost; pick = j; }
                    }
                    r += unitBytes[u * n + pick];
                    d += unitError[u * n + pick];
                }
                if (d <= globalError * (1.0 + 1e-9)) bestBytes = std::min(bestBytes, r);
            }
            return std::max(0.0, (globalBytes - bestBytes) / std::max(1.0, points[c].bytes));
        }
    };

    namespace RdoAnalysis
    {
        constexpr size_t kUnitSplats = 8192;   // Level 0 splats per window (rate unit and per-region unit)
        constexpr size_t kSampleStride = 16;   // Every 16th level 0 splat is scored for distortion
        constexpr int    kViewDirections = 16;
        constexpr int    kFastQuality = 5;     // Brotli quality of the search; scaled to the package quality (11)
        constexpr int    kCalibrationWindows = 8;
        constexpr int    kViewCount = 8;         // Directions of the typical views (the --capture-views ones)
        constexpr float  kViewConeDegrees = 30.0f; // Half-angle of a view's cone (a 45-degree vertical field of view, wider across)  // Package-quality windows that scale a batch (spread over it)

        // The candidates: per degree the source carries, uniform bits and the tapers that keep the low bands finer
        inline std::vector<RdoSettings> Candidates(uint32_t sourceDegree)
        {
            static const uint8_t kSets3[][3] = { {8,8,8}, {6,6,6}, {5,5,5}, {4,4,4}, {8,6,5}, {8,6,4}, {6,5,4}, {6,4,4}, {5,4,4} };
            static const uint8_t kSets2[][3] = { {8,8,8}, {6,6,6}, {5,5,5}, {4,4,4}, {6,5,5}, {6,4,4}, {5,4,4} };
            static const uint8_t kSets1[][3] = { {8,8,8}, {6,6,6}, {5,5,5}, {4,4,4} };
            std::vector<RdoSettings> out;
            auto add = [&](uint32_t degree, const uint8_t (*sets)[3], size_t count)
            {
                for (size_t i = 0; i < count; i++)
                {
                    RdoSettings s; s.shDegree = degree;
                    for (int b = 0; b < 3; b++) s.shBits[b] = sets[i][b];
                    out.push_back(s);
                }
            };
            if (sourceDegree >= 3) add(3, kSets3, sizeof(kSets3) / sizeof(kSets3[0]));
            if (sourceDegree >= 2) add(2, kSets2, sizeof(kSets2) / sizeof(kSets2[0]));
            if (sourceDegree >= 1) add(1, kSets1, sizeof(kSets1) / sizeof(kSets1[0]));
            return out;
        }

        namespace Detail
        {
            // The real SH basis (bands 1 to 3, the 3D Gaussian Splatting order and constants) at a unit direction
            inline void Basis(float x, float y, float z, float* Y)
            {
                const float C1 = 0.4886025119029199f;
                const float C2[5] = { 1.0925484305920792f, -1.0925484305920792f, 0.31539156525252005f, -1.0925484305920792f, 0.5462742152960396f };
                const float C3[7] = { -0.5900435899266435f, 2.890611442640554f, -0.4570457994644658f, 0.3731763325901154f, -0.4570457994644658f, 1.445305721320277f, -0.5900435899266435f };
                const float xx = x * x, yy = y * y, zz = z * z;
                Y[0] = -C1 * y; Y[1] = C1 * z; Y[2] = -C1 * x;
                Y[3] = C2[0] * x * y; Y[4] = C2[1] * y * z; Y[5] = C2[2] * (2.0f * zz - xx - yy); Y[6] = C2[3] * x * z; Y[7] = C2[4] * (xx - yy);
                Y[8] = C3[0] * y * (3.0f * xx - yy); Y[9] = C3[1] * x * y * z; Y[10] = C3[2] * y * (4.0f * zz - xx - yy);
                Y[11] = C3[3] * z * (2.0f * zz - 3.0f * xx - 3.0f * yy); Y[12] = C3[4] * x * (4.0f * zz - xx - yy);
                Y[13] = C3[5] * z * (xx - yy); Y[14] = C3[6] * x * (xx - 3.0f * yy);
            }

            inline uint32_t Spread10(uint32_t v)
            {
                v &= 0x3FF;
                v = (v | (v << 16)) & 0x030000FF;
                v = (v | (v << 8)) & 0x0300F00F;
                v = (v | (v << 4)) & 0x030C30C3;
                v = (v | (v << 2)) & 0x09249249;
                return v;
            }

            // A stream cut into windows of `stride`-byte records, each window shuffled as the packager shuffles a payload
            inline void Windows(const uint8_t* data, size_t count, uint32_t stride, size_t perWindow, std::vector<std::vector<uint8_t>>& out)
            {
                for (size_t start = 0; start < count; start += perWindow)
                {
                    const size_t n = std::min(perWindow, count - start);
                    out.push_back(ByteShuffle::Shuffle(data + start * stride, n, stride));
                }
            }

            // Compressed size of every window at the search quality, and the ratio the package quality gives on a few
            inline double CompressedBytes(std::vector<std::vector<uint8_t>>& windows, std::vector<double>* perWindow)
            {
                if (windows.empty()) return 0.0;
                std::vector<std::vector<uint8_t>> calib;
                const size_t step = std::max<size_t>(1, windows.size() / kCalibrationWindows);
                for (size_t i = 0; i < windows.size() && calib.size() < (size_t)kCalibrationWindows; i += step) calib.push_back(windows[i]);
                std::vector<std::vector<uint8_t>*> fast, slow;
                for (auto& w : windows) fast.push_back(&w);
                std::vector<std::vector<uint8_t>> calibFast = calib;
                for (auto& w : calib) slow.push_back(&w);
                for (auto& w : calibFast) fast.push_back(&w);
                PayloadCodec::CompressInPlace(slow, PayloadCodec::kDefault, PayloadCodec::kPackageQuality);
                PayloadCodec::CompressInPlace(fast, PayloadCodec::kDefault, kFastQuality);
                double slowBytes = 0.0, fastCalib = 0.0;
                for (size_t i = 0; i < calib.size(); i++) { slowBytes += (double)calib[i].size(); fastCalib += (double)calibFast[i].size(); }
                const double ratio = fastCalib > 0.0 ? slowBytes / fastCalib : 1.0;
                double total = 0.0;
                if (perWindow) perWindow->assign(windows.size(), 0.0);
                for (size_t i = 0; i < windows.size(); i++)
                {
                    const double b = (double)windows[i].size() * ratio;
                    total += b;
                    if (perWindow) (*perWindow)[i] = b;
                }
                return total;
            }
        }

        // The frontier and every candidate's per-region saving, once every candidate is measured (Analyse does it for a
        // whole analysis; a dispatcher merging workers' parts calls it after Merge)
        inline void Finish(RdoAnalysisResult& out)
        {
            const size_t nc = out.points.size();
            for (size_t i = 0; i < nc; i++)
            {
                bool dominated = false;
                for (size_t j = 0; j < nc && !dominated; j++)
                    if (j != i && out.points[j].bytes <= out.points[i].bytes && out.points[j].psnr >= out.points[i].psnr &&
                        (out.points[j].bytes < out.points[i].bytes || out.points[j].psnr > out.points[i].psnr)) dominated = true;
                out.points[i].frontier = !dominated;
            }
            for (size_t i = 0; i < nc; i++) out.points[i].regionSaving = out.PerRegionSaving(i);

            // The typical views' downloads (see the file comment)
            if (out.units > 0 && out.unitCenter.size() == out.units * 3 && out.unitBytes.size() == out.units * nc && out.radius > 0.0f)
            {
                const float distance = out.radius / std::tan(22.5f * 3.14159265f / 180.0f);
                const float cosCone = std::cos(kViewConeDegrees * 3.14159265f / 180.0f);
                std::vector<std::vector<size_t>> visible(kViewCount);
                for (int v = 0; v < kViewCount; v++)
                {
                    const float yaw = 6.2831853f * v / kViewCount, pitch = 0.3f;
                    const float d[3] = { std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw) };
                    const float eye[3] = { out.center[0] + d[0] * distance, out.center[1] + d[1] * distance, out.center[2] + d[2] * distance };
                    for (size_t u = 0; u < out.units; u++)
                    {
                        const float* c = &out.unitCenter[u * 3];
                        const float tc[3] = { c[0] - eye[0], c[1] - eye[1], c[2] - eye[2] };
                        const float len = std::sqrt(tc[0] * tc[0] + tc[1] * tc[1] + tc[2] * tc[2]);
                        const bool inCone = len > 0.0f && -(tc[0] * d[0] + tc[1] * d[1] + tc[2] * d[2]) / len >= cosCone;
                        const bool nearSide = (c[0] - out.center[0]) * d[0] + (c[1] - out.center[1]) * d[1] + (c[2] - out.center[2]) * d[2] >= 0.0f;
                        if (inCone && nearSide) visible[v].push_back(u);
                    }
                }
                for (size_t c = 0; c < nc; c++)
                {
                    RdoPoint& p = out.points[c];
                    double level0SH = 0.0;
                    for (size_t u = 0; u < out.units; u++) level0SH += out.unitBytes[u * nc + c];
                    const double coarser = std::max(0.0, p.bytes - level0SH - out.level0RecordBytes);
                    std::vector<double> full(kViewCount), first(kViewCount);
                    for (int v = 0; v < kViewCount; v++)
                    {
                        double bytes = 0.0;
                        for (size_t u : visible[v]) bytes += out.unitBytes[u * nc + c] + (u < out.unitRecordBytes.size() ? out.unitRecordBytes[u] : 0.0);
                        const double share = (double)visible[v].size() / (double)out.units;
                        full[v] = bytes + share * coarser;
                        first[v] = share * p.coarseBytes;
                    }
                    std::nth_element(full.begin(), full.begin() + kViewCount / 2, full.end());
                    std::nth_element(first.begin(), first.begin() + kViewCount / 2, first.end());
                    p.viewBytes = full[kViewCount / 2];
                    p.viewFirstBytes = first[kViewCount / 2];
                }
            }
        }

        // Parts measured apart (a worker's job each, the same scan and pyramid) as one result: the candidates in order,
        // the per-region tables re-laid for all of them. Finish it afterwards.
        inline RdoAnalysisResult Merge(const std::vector<RdoAnalysisResult>& parts)
        {
            RdoAnalysisResult out;
            size_t total = 0;
            for (const auto& p : parts)
            {
                total += p.points.size();
                out.units = std::max(out.units, p.units);
                if (out.recordBytes == 0.0)
                {
                    out.recordBytes = p.recordBytes; out.recordCoarseBytes = p.recordCoarseBytes;
                    out.unitCenter = p.unitCenter; out.unitRecordBytes = p.unitRecordBytes; out.level0RecordBytes = p.level0RecordBytes;
                    for (int a = 0; a < 3; a++) out.center[a] = p.center[a];
                    out.radius = p.radius;
                }
                out.seconds = std::max(out.seconds, p.seconds); // The parts run side by side
            }
            out.unitBytes.assign(out.units * total, 0.0);
            out.unitError.assign(out.units * total, 0.0);
            size_t c0 = 0;
            for (const auto& p : parts)
            {
                const size_t n = p.points.size();
                for (size_t c = 0; c < n; c++)
                {
                    out.points.push_back(p.points[c]);
                    if (p.unitBytes.size() == p.units * n)
                        for (size_t u = 0; u < p.units && u < out.units; u++)
                        {
                            out.unitBytes[u * total + c0 + c] = p.unitBytes[u * n + c];
                            out.unitError[u * total + c0 + c] = p.unitError[u * n + c];
                        }
                }
                c0 += n;
            }
            return out;
        }

        // levels: every level's points, finest first (level 0 = the scan's points); attributes: the source Gaussians,
        // merged: the coarse levels' merged Gaussians (WaveletDecompositionResult::mergedAttributes); baseParams: the
        // bake's encoding frame (bounds, scale range). progress(fraction, stage) returns false to cancel. only: the
        // candidates to measure (a worker's share); null = every one, finished (Finish). A share is left unfinished.
        inline bool Analyse(const std::vector<const std::vector<SplatPoint>*>& levels, const std::vector<SplatAttributes>& attributes,
                            const std::vector<SplatAttributes>* merged, const SplatEncodeParams& baseParams, uint32_t sourceDegree,
                            const std::function<bool(float, const char*)>& progress, RdoAnalysisResult& out,
                            const std::vector<RdoSettings>* only = nullptr)
        {
            using namespace Detail;
            const auto t0 = std::chrono::high_resolution_clock::now();
            out = RdoAnalysisResult{};
            if (levels.empty() || !levels[0] || levels[0]->empty() || sourceDegree == 0) return false;
            std::vector<RdoSettings> candidates = only ? *only : Candidates(std::min(sourceDegree, 3u));
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const RdoSettings& s) { return s.shDegree < 1 || s.shDegree > std::min(sourceDegree, 3u); }), candidates.end());
            if (candidates.empty()) return false;
            const size_t nc = candidates.size();
            const size_t numLevels = levels.size();
            const uint32_t recordBytes = (uint32_t)sizeof(PackedSplatGPU);

            // Level 0 in Morton order, so the windows are compact patches
            std::vector<SplatPoint> level0(*levels[0]);
            {
                const XMFLOAT3 mn = baseParams.aabbMin, mx = baseParams.aabbMax;
                auto key = [&](const SplatPoint& p)
                {
                    auto q = [](float v, float a, float b) { return (uint32_t)std::clamp((v - a) / std::max(1e-9f, b - a) * 1023.0f, 0.0f, 1023.0f); };
                    return Spread10(q(p.position.x, mn.x, mx.x)) | (Spread10(q(p.position.y, mn.y, mx.y)) << 1) | (Spread10(q(p.position.z, mn.z, mx.z)) << 2);
                };
                std::stable_sort(level0.begin(), level0.end(), [&](const SplatPoint& a, const SplatPoint& b) { return key(a) < key(b); });
            }
            std::vector<const std::vector<SplatPoint>*> lv(levels);
            lv[0] = &level0;
            const size_t coarseFrom = numLevels >= 2 ? numLevels - 2 : 0;

            // The scored sample: index into level 0, weight, and the view-direction basis
            std::vector<size_t> sample;
            for (size_t i = 0; i < level0.size(); i += kSampleStride) sample.push_back(i);
            std::vector<double> weight(sample.size(), 0.0);
            for (size_t s = 0; s < sample.size(); s++)
            {
                const SplatPoint& p = level0[sample[s]];
                if (p.sourceIndex == kNoSplatSource || p.sourceIndex >= attributes.size()) continue;
                const SplatAttributes& a = attributes[p.sourceIndex];
                float sc[3] = { a.scale.x, a.scale.y, a.scale.z };
                std::sort(sc, sc + 3);
                weight[s] = (double)a.opacity * (double)sc[1] * (double)sc[2];
            }
            double weightSum = 0.0;
            for (double w : weight) weightSum += w;
            if (weightSum <= 0.0) { for (double& w : weight) w = 1.0; weightSum = (double)weight.size(); }
            float basis[kViewDirections][15];
            for (int d = 0; d < kViewDirections; d++)
            {
                const float t = (d + 0.5f) / kViewDirections, z = 1.0f - 2.0f * t, r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                const float phi = 2.399963229728653f * d;
                Basis(r * std::cos(phi), r * std::sin(phi), z, basis[d]);
            }

            const size_t units = (level0.size() + kUnitSplats - 1) / kUnitSplats;
            out.units = units;
            out.unitCenter.assign(units * 3, 0.0f);
            for (size_t u = 0; u < units; u++)
            {
                double s[3] = {}; size_t n = 0;
                for (size_t i = u * kUnitSplats; i < std::min(level0.size(), (u + 1) * kUnitSplats); i++, n++)
                { s[0] += level0[i].position.x; s[1] += level0[i].position.y; s[2] += level0[i].position.z; }
                for (int a = 0; a < 3; a++) out.unitCenter[u * 3 + a] = n ? (float)(s[a] / n) : 0.0f;
            }
            out.center[0] = 0.5f * (baseParams.aabbMin.x + baseParams.aabbMax.x);
            out.center[1] = 0.5f * (baseParams.aabbMin.y + baseParams.aabbMax.y);
            out.center[2] = 0.5f * (baseParams.aabbMin.z + baseParams.aabbMax.z);
            {
                const float ex = baseParams.aabbMax.x - baseParams.aabbMin.x, ey = baseParams.aabbMax.y - baseParams.aabbMin.y, ez = baseParams.aabbMax.z - baseParams.aabbMin.z;
                out.radius = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
            }
            out.unitBytes.assign(units * nc, 0.0);
            out.unitError.assign(units * nc, 0.0);
            out.points.resize(nc);

            size_t done = 0;
            for (uint32_t degree = std::min(sourceDegree, 3u); degree >= 1; degree--)
            {
                std::vector<size_t> mine;
                for (size_t c = 0; c < nc; c++) if (candidates[c].shDegree == degree) mine.push_back(c);
                if (mine.empty()) continue;
                if (progress && !progress((float)done / (float)nc, "Encoding the levels")) return false;

                // Every level encoded at this degree, at 8 bits
                SplatEncodeParams params = baseParams;
                params.shDegree = degree;
                const uint32_t shBytes = SplatCodec::SHRecordBytes(degree);
                std::vector<std::vector<PackedSplatGPU>> recs(numLevels);
                std::vector<std::vector<uint8_t>> sh8(numLevels);
                for (size_t l = 0; l < numLevels; l++)
                    SplatCodec::Encode(*lv[l], attributes, (uint32_t)l, params, recs[l], sh8[l], merged);

                // The records' size, once (they barely depend on the SH choice)
                if (out.recordBytes == 0.0)
                {
                    if (progress && !progress((float)done / (float)nc, "Measuring the records")) return false;
                    std::vector<std::vector<uint8_t>> w;
                    std::vector<size_t> firstWindow(numLevels + 1, 0);
                    for (size_t l = 0; l < numLevels; l++)
                    {
                        std::vector<PackedSplatGPU> r = recs[l];
                        ColourTransform::ApplyRecords(reinterpret_cast<uint8_t*>(r.data()), r.size(), recordBytes);
                        firstWindow[l] = w.size();
                        Windows(reinterpret_cast<const uint8_t*>(r.data()), r.size(), recordBytes, kUnitSplats, w);
                    }
                    firstWindow[numLevels] = w.size();
                    std::vector<double> perWindow;
                    CompressedBytes(w, &perWindow);
                    out.unitRecordBytes.assign(units, 0.0);
                    for (size_t l = 0; l < numLevels; l++)
                        for (size_t i = firstWindow[l]; i < firstWindow[l + 1]; i++)
                        {
                            out.recordBytes += perWindow[i];
                            if (l >= coarseFrom) out.recordCoarseBytes += perWindow[i];
                            if (l == 0 && i < units) { out.unitRecordBytes[i] = perWindow[i]; out.level0RecordBytes += perWindow[i]; }
                        }
                }

                // Where each level's points take their SH prediction from: the same for every candidate of this degree
                std::vector<std::vector<uint32_t>> parentMaps;
                {
                    std::vector<const uint8_t*> rp(numLevels);
                    std::vector<size_t> counts(numLevels);
                    for (size_t l = 0; l < numLevels; l++) { rp[l] = reinterpret_cast<const uint8_t*>(recs[l].data()); counts[l] = recs[l].size(); }
                    SHCoding::ParentMaps(rp, counts, recordBytes, parentMaps);
                }

                // The sample's SH scale per record at this degree
                std::vector<float> shScale(sample.size(), 0.0f);
                for (size_t s = 0; s < sample.size(); s++)
                {
                    XMFLOAT3 pos, scale, colour; XMFLOAT4 rot; float opacity = 0.0f, sscale = 0.0f;
                    SplatCodec::DecodeRecord(recs[0][sample[s]], params, pos, scale, rot, opacity, colour, sscale);
                    shScale[s] = sscale;
                }

                for (size_t c : mine)
                {
                    const RdoSettings& cand = candidates[c];
                    if (progress && !progress((float)done / (float)nc, "Measuring the candidates")) return false;
                    std::vector<std::vector<uint8_t>> sh(sh8);
                    for (size_t l = 0; l < numLevels; l++) RequantiseSH(sh[l].data(), recs[l].size(), shBytes, cand.shBits);
                    if (progress && !progress((float)done / (float)nc, "Scoring the colour")) return false;

                    // Distortion over the sample, per unit
                    const uint32_t coeffs = SplatCodec::SHCoefficientsPerChannel(degree), sourceCoeffs = SplatCodec::SHCoefficientsPerChannel(std::min(sourceDegree, 3u));
                    double mse = 0.0;
                    for (size_t s = 0; s < sample.size(); s++)
                    {
                        const SplatPoint& p = level0[sample[s]];
                        if (p.sourceIndex == kNoSplatSource || p.sourceIndex >= attributes.size()) continue;
                        const SplatAttributes& a = attributes[p.sourceIndex];
                        const int8_t* q = reinterpret_cast<const int8_t*>(sh[0].data() + sample[s] * shBytes);
                        float diff[45] = {};
                        for (uint32_t k = 0; k < sourceCoeffs; k++)
                            for (int ch = 0; ch < 3; ch++)
                            {
                                const float decoded = k < coeffs ? (float)q[3 * k + ch] * shScale[s] : 0.0f;
                                diff[3 * k + ch] = a.sh[3 * k + ch] - decoded;
                            }
                        double se = 0.0;
                        for (int d = 0; d < kViewDirections; d++)
                            for (int ch = 0; ch < 3; ch++)
                            {
                                float e = 0.0f;
                                for (uint32_t k = 0; k < sourceCoeffs; k++) e += basis[d][k] * diff[3 * k + ch];
                                se += (double)e * e;
                            }
                        se /= (double)(kViewDirections * 3);
                        mse += weight[s] * se;
                        out.unitError[(sample[s] / kUnitSplats) * nc + c] += weight[s] * se;
                    }
                    mse /= weightSum;
                    out.points[c].settings = cand;
                    out.points[c].mse = mse;
                    out.points[c].psnr = mse > 0.0 ? 10.0 * std::log10(1.0 / mse) : 99.0;

                    // Rate: the packager's SH path
                    if (progress && !progress((float)done / (float)nc, "Coding SH residuals")) return false;
                    if (numLevels >= 2)
                    {
                        std::vector<const uint8_t*> rp(numLevels);
                        std::vector<size_t> counts(numLevels);
                        std::vector<std::vector<uint8_t>*> shp(numLevels);
                        for (size_t l = 0; l < numLevels; l++) { rp[l] = reinterpret_cast<const uint8_t*>(recs[l].data()); counts[l] = recs[l].size(); shp[l] = &sh[l]; }
                        SHCoding::EncodeResiduals(rp, counts, recordBytes, shp, shBytes, cand.shBits, &parentMaps);
                    }
                    ColourTransform::ApplySH(sh[numLevels - 1].data(), sh[numLevels - 1].size()); // Only the coarsest level is absolute
                    // Every level's windows in one batch, compressed across the cores with one calibration
                    if (progress && !progress((float)done / (float)nc, "Compressing")) return false;
                    std::vector<std::vector<uint8_t>> w;
                    std::vector<size_t> firstWindow(numLevels + 1, 0);
                    for (size_t l = 0; l < numLevels; l++)
                    {
                        firstWindow[l] = w.size();
                        Windows(sh[l].data(), recs[l].size(), shBytes, kUnitSplats, w);
                    }
                    firstWindow[numLevels] = w.size();
                    std::vector<double> perWindow;
                    CompressedBytes(w, &perWindow);
                    double shTotal = 0.0, shCoarse = 0.0;
                    for (size_t l = 0; l < numLevels; l++)
                        for (size_t i = firstWindow[l]; i < firstWindow[l + 1]; i++)
                        {
                            shTotal += perWindow[i];
                            if (l >= coarseFrom) shCoarse += perWindow[i];
                            if (l == 0 && i < units) out.unitBytes[i * nc + c] = perWindow[i];
                        }
                    out.points[c].bytes = out.recordBytes + shTotal;
                    out.points[c].coarseBytes = out.recordCoarseBytes + shCoarse;
                    done++;
                }
                if (degree == 1) break;
            }

            if (!only) Finish(out);
            out.seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
            if (progress) progress(1.0f, "Done");
            return true;
        }

        // The candidate a target picks (index into result.points, or -1): the best quality within a size; the smallest
        // that reaches a quality; or, for streaming, the best whose two coarsest levels and whole package arrive in time.
        enum class TargetMode : int { Size = 0, Ratio = 1, Quality = 2, Streaming = 3 };
        struct Target
        {
            TargetMode mode = TargetMode::Size;
            double sizeMB = 0.0;           // Size: megabytes (1024 x 1024)
            double ratio = 4.0;            // Ratio: source bytes / package bytes
            double qualityDb = 40.0;       // Quality: minimum colour PSNR
            double bandwidthMBps = 2.0;      // Streaming: the link
            double firstViewSeconds = 1.0;   //   a typical view's first, coarse picture in this time
            double fullDetailSeconds = 10.0; //   and its full detail in this
        };
        inline int Pick(const RdoAnalysisResult& r, const Target& t, double sourceBytes)
        {
            int best = -1;
            const double mb = 1024.0 * 1024.0;
            for (size_t i = 0; i < r.points.size(); i++)
            {
                const RdoPoint& p = r.points[i];
                bool ok = true;
                switch (t.mode)
                {
                    case TargetMode::Size:      ok = p.bytes <= t.sizeMB * mb; break;
                    case TargetMode::Ratio:     ok = t.ratio > 0.0 && p.bytes <= sourceBytes / t.ratio; break;
                    case TargetMode::Quality:   ok = p.psnr >= t.qualityDb; break;
                    case TargetMode::Streaming: ok = p.viewFirstBytes <= t.bandwidthMBps * mb * t.firstViewSeconds && p.viewBytes <= t.bandwidthMBps * mb * t.fullDetailSeconds; break;
                }
                if (!ok) continue;
                if (best < 0) { best = (int)i; continue; }
                const RdoPoint& b = r.points[best];
                const bool better = (t.mode == TargetMode::Quality) ? (p.bytes < b.bytes || (p.bytes == b.bytes && p.psnr > b.psnr))
                                                                    : (p.psnr > b.psnr || (p.psnr == b.psnr && p.bytes < b.bytes));
                if (better) best = (int)i;
            }
            if (best < 0) // Nothing meets it: the closest -- the smallest, or for a quality target the best
            {
                for (size_t i = 0; i < r.points.size(); i++)
                {
                    if (best < 0) { best = (int)i; continue; }
                    const RdoPoint& p = r.points[i]; const RdoPoint& b = r.points[best];
                    if (t.mode == TargetMode::Quality ? p.psnr > b.psnr : p.bytes < b.bytes) best = (int)i;
                }
            }
            return best;
        }
    }
}
