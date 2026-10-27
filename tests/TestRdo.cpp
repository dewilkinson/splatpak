// TestRdo.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// RDO analysis (RdoAnalysis.h, docs/design/gstx-rdo.md phase 1) on a scan: prints every candidate's predicted
// package size and colour PSNR, the frontier, the per-region estimate, and what a few targets pick. With
// "measured" package sizes given as degree:bits=bytes (bits uniform), checks the predictions against them.
//
//   TestRdo [input.ply] [levels] [deadband-mm] [d:b=bytes ...]
//
// Exit code 0 on success, 1 when a prediction is more than kTolerance off a measured size or the analysis fails.

#include <chrono>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <string>
#include <vector>
#include "../src/SplatCore/PLYLoader.h"
#include "../src/SplatCore/RdoAnalysis.h"
#include "../src/SplatCore/RdoJob.h"
#include <fstream>
#include "../libs/gstx/LiftingWavelet.h"

using namespace Splat;

// --merge part.json ...: the parts workers measured (SplatPakWorker), merged and finished as a dispatcher does
static int MergeParts(int argc, char** argv)
{
    std::vector<RdoAnalysisResult> parts;
    for (int i = 2; i < argc; i++)
    {
        std::ifstream in(argv[i]);
        RdoPart part; std::string error;
        if (!in || !part.Parse(in, error) || !part.error.empty()) { fprintf(stderr, "%s: %s%s\n", argv[i], error.c_str(), part.error.c_str()); return 1; }
        parts.push_back(part.result);
    }
    RdoAnalysisResult r = RdoAnalysis::Merge(parts);
    RdoAnalysis::Finish(r);
    printf("%zu parts, %zu candidates, %zu units\n", parts.size(), r.points.size(), r.units);
    for (const RdoPoint& p : r.points)
        printf("%-26s %10.1f %10.1f %8.2f %9.1f %s\n", p.settings.Describe().c_str(), p.bytes / 1e6, p.coarseBytes / 1e6, p.psnr, 100.0 * p.regionSaving, p.frontier ? "*" : "");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--merge") return MergeParts(argc, argv);
    const std::string input = argc > 1 ? argv[1] : "assets/cthulu/cthulu.ply";
    const uint32_t maxLODs = argc > 2 ? (uint32_t)atoi(argv[2]) : 5;
    const float deadbandMM = argc > 3 ? (float)atof(argv[3]) : 1.353f;
    constexpr double kTolerance = 0.05;

    SplatPointArray points;
    SplatAttributeArray attrs;
    double origin[3] = {};
    if (!PLYLoader::LoadPLY(input, points, origin, &attrs) || attrs.empty())
    {
        fprintf(stderr, "Cannot load %s as a Gaussian splat scan\n", input.c_str());
        return 1;
    }
    uint32_t sourceDegree = 0;
    for (const auto& a : attrs)
        for (int k = 44; k >= 0; k--)
            if (a.sh[k] != 0.0f) { sourceDegree = std::max(sourceDegree, k >= 24 ? 3u : k >= 9 ? 2u : 1u); break; }

    WaveletDecompositionResult wavelet = LiftingWavelet::DecomposeChunk(points, maxLODs, deadbandMM / 1000.0f);
    SplatCodec::MergeLevels(wavelet, attrs);
    SplatEncodeParams params;
    XMFLOAT3 mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
    for (const auto& p : points)
    {
        mn.x = std::min(mn.x, p.position.x); mn.y = std::min(mn.y, p.position.y); mn.z = std::min(mn.z, p.position.z);
        mx.x = std::max(mx.x, p.position.x); mx.y = std::max(mx.y, p.position.y); mx.z = std::max(mx.z, p.position.z);
    }
    params.aabbMin = mn; params.aabbMax = mx;
    SplatCodec::ComputeScaleRange(attrs, maxLODs, params.scaleLog2Min, params.scaleLog2Max);

    std::vector<const SplatPointArray*> levels;
    levels.push_back(&points);
    for (size_t l = 1; l < wavelet.lodLevels.size(); l++) levels.push_back(&wavelet.lodLevels[l].splats);

    RdoAnalysisResult result;
    const bool ok = RdoAnalysis::Analyse(levels, attrs, &wavelet.mergedAttributes, params, sourceDegree,
        [](float f, const char* stage)
        {
            // With RDO_TIMING set, the time spent in each stage: where the analysis goes
            static std::string last;
            static auto since = std::chrono::high_resolution_clock::now();
            static std::map<std::string, double> total;
            const auto now = std::chrono::high_resolution_clock::now();
            if (!last.empty()) total[last] += std::chrono::duration<double>(now - since).count();
            last = stage; since = now;
            if (!getenv("RDO_TIMING")) fprintf(stderr, "\r%3.0f%% %-28s", f * 100.0f, stage);
            else if (std::string(stage) == "Done") for (const auto& kv : total) fprintf(stderr, "%-28s %7.1f s\n", kv.first.c_str(), kv.second);
            return true;
        }, result);
    fprintf(stderr, "\n");
    if (!ok) { fprintf(stderr, "Analysis failed\n"); return 1; }

    printf("%s: %zu splats, %zu levels, source SH degree %u; analysis %.1f s, %zu units\n", input.c_str(), points.size(), levels.size(), sourceDegree, result.seconds, result.units);
    printf("records %.1f MB (coarse %.1f MB)\n", result.recordBytes / 1048576.0, result.recordCoarseBytes / 1048576.0);
    printf("%-26s %10s %10s %8s %9s %s\n", "candidate", "MB", "coarse MB", "dB", "region %", "frontier");
    for (size_t i = 0; i < result.points.size(); i++)
    {
        const RdoPoint& p = result.points[i];
        printf("%-26s %10.1f %10.1f %8.2f %9.1f %s\n", p.settings.Describe().c_str(), p.bytes / 1e6, p.coarseBytes / 1e6, p.psnr,
            100.0 * result.PerRegionSaving(i), p.frontier ? "*" : "");
    }

    int failures = 0;
    for (int a = 4; a < argc; a++)
    {
        unsigned d = 0, b = 0; double bytes = 0.0;
        if (sscanf(argv[a], "%u:%u=%lf", &d, &b, &bytes) != 3) continue;
        for (const RdoPoint& p : result.points)
        {
            if (p.settings.shDegree != d || p.settings.shBits[0] != b || p.settings.shBits[1] != b || p.settings.shBits[2] != b) continue;
            const double err = p.bytes / bytes - 1.0;
            printf("measured degree %u, %u bits: %.1f MB, predicted %.1f MB (%+.1f%%)%s\n", d, b, bytes / 1e6, p.bytes / 1e6, 100.0 * err,
                std::fabs(err) > kTolerance ? "  FAIL" : "");
            if (std::fabs(err) > kTolerance) failures++;
        }
    }

    RdoAnalysis::Target t;
    const double sourceBytes = 784007638.0;
    for (double mb : { 120.0, 160.0, 200.0, 280.0 })
    {
        t.mode = RdoAnalysis::TargetMode::Size; t.sizeMB = mb;
        const int c = RdoAnalysis::Pick(result, t, sourceBytes);
        if (c >= 0) printf("target %.0f MB -> %s, %.1f MB, %.2f dB\n", mb, result.points[c].settings.Describe().c_str(), result.points[c].bytes / 1048576.0, result.points[c].psnr);
    }
    t.mode = RdoAnalysis::TargetMode::Streaming; t.bandwidthMBps = 2.0; t.firstViewSeconds = 2.0; t.fullDetailSeconds = 90.0;
    const int s = RdoAnalysis::Pick(result, t, sourceBytes);
    if (s >= 0) printf("target 4G, first view 2 s, full detail 90 s -> %s, %.1f MB\n", result.points[s].settings.Describe().c_str(), result.points[s].bytes / 1048576.0);
    printf("TestRdo: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
