// TestSplatPlanes.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Compact splat planes (docs/design/compact-splat-planes.md) on real scans. For each .ply:
//   step 1, the position plane:
//   - the round-trip error of 21-bit fixed point against the loaded fp32 positions (max and RMS, in mm and in bounds);
//   - the bytes a geometry pass reads: the plane against SplatPoint arrays;
//   - the time of a bounds pass and of a Morton pass over the plane against the same passes over SplatPoint arrays
//     (single-threaded, the best of five);
//   - how many Morton codes change: the runtime's float curve from decoded positions (the quantisation alone), and the
//     integer curve against the float curve (the definitional difference);
//   step 2, the shape and colour planes:
//   - the round-trip errors of the log-scales (relative), opacity, rotation (degrees) and colour;
//   - the bake fed from the decoded planes against the bake from fp32: every level decomposed, merged and encoded
//     (SH degree 3) both ways with the same parameters, counting the package records that differ. Twice: shape and
//     colour decoded (step 2 alone), then positions as well.
//
//   TestSplatPlanes [--no-bake] scan.ply [...]   exit code 0 when every error is within its bound

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "../src/SplatCore/PLYLoader.h"
#include "../src/SplatCore/SpatialOctree.h"
#include "../src/SplatCore/SplatPlanes.h"
#include "../libs/gstx/LiftingWavelet.h"
#include "../libs/gstx/SplatCodec.h"

using namespace Splat;

template <class F>
static double BestOf5(F&& f)
{
    double best = 1e30;
    for (int r = 0; r < 5; r++)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        f();
        best = std::min(best, std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count());
    }
    return best;
}

// Every level of the package a bake makes from points and attributes: records and SH stream per level
struct BakedLevels
{
    std::vector<std::vector<PackedSplatGPU>> records;
    std::vector<std::vector<uint8_t>> sh;
    std::vector<uint32_t> order; // Level 0's points in the decomposition's (Morton) order, by sourceIndex
};

static constexpr uint32_t kBakeLevels = 5;
static constexpr float kBakeDeadbandMM = 1.353f;

static BakedLevels Bake(const SplatPointArray& points, const SplatAttributeArray& attrs, const SplatEncodeParams& params)
{
    WaveletDecompositionResult wavelet = LiftingWavelet::DecomposeChunk(points, kBakeLevels, kBakeDeadbandMM / 1000.0f);
    SplatCodec::MergeLevels(wavelet, attrs);
    BakedLevels out;
    for (const SplatPoint& p : wavelet.lodLevels[0].splats) out.order.push_back(p.sourceIndex);
    for (size_t l = 0; l < wavelet.lodLevels.size(); l++)
    {
        out.records.emplace_back();
        out.sh.emplace_back();
        SplatCodec::Encode(l == 0 ? points : wavelet.lodLevels[l].splats, attrs, (uint32_t)l, params, out.records.back(), out.sh.back(),
            &wavelet.mergedAttributes);
    }
    return out;
}

// A Gaussian's covariance R diag(s^2) R^T, as the 6 distinct entries
static void Covariance(const XMFLOAT3& s, const XMFLOAT4& q, double out[6])
{
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    const double r[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
                             { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
                             { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) } };
    const double s2[3] = { (double)s.x * s.x, (double)s.y * s.y, (double)s.z * s.z };
    int k = 0;
    for (int i = 0; i < 3; i++)
        for (int j = i; j < 3; j++) out[k++] = r[i][0] * r[j][0] * s2[0] + r[i][1] * r[j][1] * s2[1] + r[i][2] * r[j][2] * s2[2];
}

// Prints, per level: the records of b that differ from a's at all (bytes), and those whose decoded Gaussian is more than a
// quantisation step from a's in some respect -- position (16-bit), opacity or colour (8-bit), the covariance (relative,
// against what one 8-bit log2 scale step and the rotation's 10 bits allow) or an SH coefficient (in steps of the larger
// of the two records' SH scales). The covariance compares shapes however the axes are labelled. Returns the records out.
static size_t CompareBakes(const char* label, const BakedLevels& a, const BakedLevels& b, const SplatEncodeParams& params, uint32_t shBytes)
{
    const double scaleStep = std::exp2((params.scaleLog2Max - params.scaleLog2Min) / 255.0);
    const double covTolerance = scaleStep * scaleStep - 1.0 + 4.0 * 1.4142 / 1023.0;
    size_t total = 0;
    printf("  bake, %s (records differing in bytes / more than a step out: any, position, covariance, opacity+colour, SH):\n", label);
    if (a.records.size() != b.records.size()) { printf("    %zu levels against %zu\n", b.records.size(), a.records.size()); return SIZE_MAX; }
    // The level 1 pairs: how many points have a different partner (the Morton order moved them across a pair boundary)
    if (a.order.size() == b.order.size())
    {
        std::vector<uint32_t> partner(a.order.size(), kNoSplatSource);
        for (size_t i = 0; i + 1 < a.order.size(); i += 2) { partner[a.order[i]] = a.order[i + 1]; partner[a.order[i + 1]] = a.order[i]; }
        size_t moved = 0;
        for (size_t i = 0; i + 1 < b.order.size(); i += 2) moved += partner[b.order[i]] != b.order[i + 1] ? 2 : 0;
        printf("    level 1 pairs with a different partner: %.3f%% of the points\n", 100.0 * moved / std::max<size_t>(1, a.order.size()));
    }
    for (size_t l = 0; l < a.records.size(); l++)
    {
        if (a.records[l].size() != b.records[l].size()) { printf("    L%zu %zu points against %zu\n", l, b.records[l].size(), a.records[l].size()); total += a.records[l].size(); continue; }
        size_t bytes = 0, over = 0, pos = 0, cov = 0, col = 0, sh = 0;
        const double posStep[3] = { (params.aabbMax.x - params.aabbMin.x) / 65535.0, (params.aabbMax.y - params.aabbMin.y) / 65535.0, (params.aabbMax.z - params.aabbMin.z) / 65535.0 };
        for (size_t i = 0; i < a.records[l].size(); i++)
        {
            if (memcmp(&a.records[l][i], &b.records[l][i], sizeof(PackedSplatGPU)) == 0 &&
                (!shBytes || memcmp(&a.sh[l][i * shBytes], &b.sh[l][i * shBytes], shBytes) == 0)) continue;
            bytes++;
            XMFLOAT3 pa, pb, sa, sb, ca, cb;
            XMFLOAT4 qa, qb;
            float oa, ob, ha, hb;
            SplatCodec::DecodeRecord(a.records[l][i], params, pa, sa, qa, oa, ca, ha);
            SplatCodec::DecodeRecord(b.records[l][i], params, pb, sb, qb, ob, cb, hb);
            const bool posOut = std::fabs(pa.x - pb.x) > 1.5 * posStep[0] || std::fabs(pa.y - pb.y) > 1.5 * posStep[1] || std::fabs(pa.z - pb.z) > 1.5 * posStep[2];
            double ma[6], mb[6], diff = 0.0, norm = 0.0;
            Covariance(sa, qa, ma);
            Covariance(sb, qb, mb);
            for (int k = 0; k < 6; k++) { const double wk = k == 0 || k == 3 || k == 5 ? 1.0 : 2.0; diff += wk * (ma[k] - mb[k]) * (ma[k] - mb[k]); norm += wk * ma[k] * ma[k]; }
            const bool covOut = std::sqrt(diff) > covTolerance * std::sqrt(norm);
            const double c8 = 1.5 / 255.0;
            const bool colOut = std::fabs(oa - ob) > c8 || std::fabs(ca.x - cb.x) > c8 || std::fabs(ca.y - cb.y) > c8 || std::fabs(ca.z - cb.z) > c8;
            bool shOut = false;
            const double hs = std::max(ha, hb);
            for (uint32_t k = 0; k < shBytes && hs > 0.0 && !shOut; k++)
                shOut = std::fabs((int8_t)a.sh[l][i * shBytes + k] * (double)ha - (int8_t)b.sh[l][i * shBytes + k] * (double)hb) > 1.5 * hs;
            pos += posOut; cov += covOut; col += colOut; sh += shOut;
            over += posOut || covOut || colOut || shOut ? 1 : 0;
        }
        const double m = 100.0 / std::max<size_t>(1, a.records[l].size());
        printf("    L%zu %8zu: %6.2f%% / %6.3f%%: %.3f%% %.3f%% %.3f%% %.3f%%\n", l, a.records[l].size(), bytes * m, over * m, pos * m, cov * m, col * m, sh * m);
        total += over;
    }
    return total;
}

int main(int argc, char** argv)
{
    int failures = 0;
    bool bake = true;
    for (int a = 1; a < argc; a++)
    {
        if (std::string(argv[a]) == "--no-bake") { bake = false; continue; }
        SplatPointArray points;
        SplatAttributeArray attrs;
        double origin[3];
        if (!PLYLoader::LoadPLY(argv[a], points, origin, &attrs) || points.empty()) { printf("%s: cannot load\n", argv[a]); failures++; continue; }
        const size_t n = points.size();

        XMFLOAT3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (const SplatPoint& p : points)
        {
            mn.x = std::min(mn.x, p.position.x); mn.y = std::min(mn.y, p.position.y); mn.z = std::min(mn.z, p.position.z);
            mx.x = std::max(mx.x, p.position.x); mx.y = std::max(mx.y, p.position.y); mx.z = std::max(mx.z, p.position.z);
        }
        const PositionPlane::Frame frame = PositionPlane::FrameOf(mn, mx);
        std::vector<uint64_t> plane;
        PositionPlane::Build(points, frame, plane);

        // Round-trip error, against the fp32 positions
        double maxErr = 0.0, sumSq = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            const XMFLOAT3 d = PositionPlane::Decode(plane[i], frame);
            const double ex = d.x - points[i].position.x, ey = d.y - points[i].position.y, ez = d.z - points[i].position.z;
            const double e = std::sqrt(ex * ex + ey * ey + ez * ez);
            maxErr = std::max(maxErr, e);
            sumSq += e * e;
        }
        const double rms = std::sqrt(sumSq / (double)n);
        const double step = std::max(frame.extent.x, std::max(frame.extent.y, frame.extent.z)) / (double)PositionPlane::kMax;
        const double halfDiagonalStep = 0.5 * std::sqrt((double)frame.extent.x * frame.extent.x + (double)frame.extent.y * frame.extent.y +
                                                        (double)frame.extent.z * frame.extent.z) / (double)PositionPlane::kMax;
        // Bound: half a step per axis, plus the fp32 rounding of the decode at this distance from the origin
        const double maxAbs = std::max(std::max(std::fabs(mn.x), std::fabs(mx.x)), std::max(std::max(std::fabs(mn.y), std::fabs(mx.y)), std::max(std::fabs(mn.z), std::fabs(mx.z))));
        const bool withinBound = maxErr <= halfDiagonalStep * 1.01 + 4.0 * maxAbs * 1.2e-7;
        if (!withinBound) failures++;

        // A bounds pass: over the SplatPoint array (44-byte stride) and over the plane (8 bytes, integer min/max)
        volatile float sink = 0.0f;
        const double tBoundsPoints = BestOf5([&]()
        {
            XMFLOAT3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (const SplatPoint& p : points)
            {
                lo.x = std::min(lo.x, p.position.x); lo.y = std::min(lo.y, p.position.y); lo.z = std::min(lo.z, p.position.z);
                hi.x = std::max(hi.x, p.position.x); hi.y = std::max(hi.y, p.position.y); hi.z = std::max(hi.z, p.position.z);
            }
            sink = lo.x + hi.z;
        });
        const double tBoundsPlane = BestOf5([&]()
        {
            uint32_t lx = PositionPlane::kMax, ly = PositionPlane::kMax, lz = PositionPlane::kMax, hx = 0, hy = 0, hz = 0;
            for (uint64_t v : plane)
            {
                const uint32_t x = PositionPlane::AxisX(v), y = PositionPlane::AxisY(v), z = PositionPlane::AxisZ(v);
                lx = std::min(lx, x); ly = std::min(ly, y); lz = std::min(lz, z);
                hx = std::max(hx, x); hy = std::max(hy, y); hz = std::max(hz, z);
            }
            sink = (float)(lx + hz);
        });

        // A Morton pass: the runtime's float curve over the SplatPoint array; the same curve from decoded plane positions;
        // the integer curve over the plane
        std::vector<uint32_t> codesPoints(n), codesDecoded(n), codesInteger(n);
        const XMFLOAT3 frameExtent = SpatialOctree::MortonFrameExtent(mn, mx);
        const double tMortonPoints = BestOf5([&]()
        {
            for (size_t i = 0; i < n; i++) codesPoints[i] = SpatialOctree::MortonCodeInFrame(points[i].position, mn, frameExtent);
        });
        const double tMortonDecoded = BestOf5([&]()
        {
            for (size_t i = 0; i < n; i++) codesDecoded[i] = SpatialOctree::MortonCodeInFrame(PositionPlane::Decode(plane[i], frame), mn, frameExtent);
        });
        const double tMortonInteger = BestOf5([&]()
        {
            for (size_t i = 0; i < n; i++) codesInteger[i] = PositionPlane::Morton30(plane[i]);
        });
        size_t changedByQuantisation = 0, changedByCurve = 0;
        for (size_t i = 0; i < n; i++)
        {
            changedByQuantisation += codesDecoded[i] != codesPoints[i] ? 1 : 0;
            changedByCurve += codesInteger[i] != codesPoints[i] ? 1 : 0;
        }

        printf("%s: %zu points, bounds %.3f x %.3f x %.3f\n", argv[a], n, frame.extent.x, frame.extent.y, frame.extent.z);
        printf("  round trip: max %.4f mm, RMS %.4f mm (step %.4f mm, %.2e of the bounds) %s\n", maxErr * 1000.0, rms * 1000.0, step * 1000.0,
            step / std::max(frame.extent.x, std::max(frame.extent.y, frame.extent.z)), withinBound ? "within half a step" : "OUT OF BOUND");
        printf("  geometry bytes: plane %.1f MB, SplatPoint array %.1f MB (%.1fx)\n", n * 8.0 / 1048576.0, n * (double)sizeof(SplatPoint) / 1048576.0, (double)sizeof(SplatPoint) / 8.0);
        printf("  bounds pass: SplatPoint %.1f ms, plane %.1f ms (%.1fx)\n", tBoundsPoints * 1000.0, tBoundsPlane * 1000.0, tBoundsPoints / tBoundsPlane);
        printf("  Morton pass: SplatPoint %.1f ms, decoded plane %.1f ms, integer %.1f ms (%.1fx)\n", tMortonPoints * 1000.0, tMortonDecoded * 1000.0, tMortonInteger * 1000.0,
            tMortonPoints / tMortonInteger);
        printf("  Morton codes changed: by the quantisation %zu (%.4f%%), integer curve against float curve %zu (%.2f%%)\n",
            changedByQuantisation, 100.0 * changedByQuantisation / n, changedByCurve, 100.0 * changedByCurve / n);

        // Step 2: the shape and colour planes
        if (attrs.size() != n) continue;
        std::vector<ShapePlane::Record> shape;
        std::vector<ColourPlane::Record> colour;
        const auto tp0 = std::chrono::high_resolution_clock::now();
        ShapePlane::Build(attrs, shape);
        ColourPlane::Build(points, colour);
        const double tBuild = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - tp0).count();

        SplatAttributeArray attrsDecoded(attrs);
        SplatPointArray pointsDecoded(points), pointsAllDecoded(points);
        double maxScale = 0.0, sumScaleSq = 0.0, maxOpacity = 0.0, maxAngle = 0.0, sumAngleSq = 0.0, maxColour = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            ShapePlane::Decode(shape[i], attrsDecoded[i]);
            pointsDecoded[i].color = ColourPlane::Decode(colour[i]);
            pointsAllDecoded[i].color = pointsDecoded[i].color;
            pointsAllDecoded[i].position = PositionPlane::Decode(plane[i], frame);

            const SplatAttributes &s = attrs[i], &d = attrsDecoded[i];
            const double rs[3] = { d.scale.x / s.scale.x - 1.0, d.scale.y / s.scale.y - 1.0, d.scale.z / s.scale.z - 1.0 };
            for (double r : rs) { maxScale = std::max(maxScale, std::fabs(r)); sumScaleSq += r * r; }
            // Relative to the opacity and to the transmittance, whichever is smaller (opacities of exactly 0 or 1 decode exactly)
            const double so = std::min(1.0f, std::max(0.0f, s.opacity)), dop = std::fabs((double)d.opacity - so);
            maxOpacity = std::max(maxOpacity, so > 0.0 && so < 1.0 ? dop / std::min(so, 1.0 - so) : dop * 1e6);
            // The rotation angle between the two, 4 atan2(|a - b|, |a + b|) for unit quaternions a and b of the same sign
            // (acos of their dot product loses the angle to the dot's float rounding)
            double qa[4] = { s.rotation.x, s.rotation.y, s.rotation.z, s.rotation.w }, qb[4] = { d.rotation.x, d.rotation.y, d.rotation.z, d.rotation.w };
            double la = 0.0, lb = 0.0, dot = 0.0, dm = 0.0, dp = 0.0;
            for (int k = 0; k < 4; k++) { la += qa[k] * qa[k]; lb += qb[k] * qb[k]; dot += qa[k] * qb[k]; }
            for (int k = 0; k < 4; k++)
            {
                const double ak = qa[k] / std::sqrt(la), bk = (dot < 0.0 ? -qb[k] : qb[k]) / std::sqrt(lb);
                dm += (ak - bk) * (ak - bk);
                dp += (ak + bk) * (ak + bk);
            }
            const double angle = 4.0 * std::atan2(std::sqrt(dm), std::sqrt(dp)) * 57.29577951308232;
            maxAngle = std::max(maxAngle, angle);
            sumAngleSq += angle * angle;
            maxColour = std::max(maxColour, (double)std::max(std::fabs(pointsDecoded[i].color.x - points[i].color.x),
                std::max(std::fabs(pointsDecoded[i].color.y - points[i].color.y), std::fabs(pointsDecoded[i].color.z - points[i].color.z))));
        }
        // Bounds: fp16 rounds a log or logit to 2^-11 relative, about 2^-11 * |log| relative in the scale, opacity or
        // transmittance (|log| < 32 here); unorm16 half a step; the rotation's three kept components half a step each
        // over [-1/sqrt2, 1/sqrt2]
        const bool shapeOk = maxScale < 0.02 && maxOpacity < 0.02 && maxAngle < 0.01 && maxColour <= 0.5 / 65535.0 + 1e-7;
        if (!shapeOk) failures++;
        printf("  shape plane %.1f MB, colour plane %.1f MB (SplatAttributes %.1f MB); built in %.0f ms\n", n * 16.0 / 1048576.0, n * 6.0 / 1048576.0,
            n * (double)sizeof(SplatAttributes) / 1048576.0, tBuild * 1000.0);
        printf("  round trip: scale max %.4f%% RMS %.4f%%, opacity max %.4f%%, rotation max %.4f deg RMS %.4f deg, colour max %.2e %s\n",
            100.0 * maxScale, 100.0 * std::sqrt(sumScaleSq / (3.0 * n)), 100.0 * maxOpacity, maxAngle, std::sqrt(sumAngleSq / n), maxColour,
            shapeOk ? "within bounds" : "OUT OF BOUND");

        if (!bake) continue;
        SplatEncodeParams params;
        params.shDegree = 3;
        params.aabbMin = mn;
        params.aabbMax = mx;
        SplatCodec::ComputeScaleRange(attrs, kBakeLevels, params.scaleLog2Min, params.scaleLog2Max);
        const uint32_t shBytes = SplatCodec::SHRecordBytes(params.shDegree);
        const auto tb0 = std::chrono::high_resolution_clock::now();
        const BakedLevels reference = Bake(points, attrs, params);
        const double tBake = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - tb0).count();
        printf("  bake from fp32: %zu levels in %.1f s\n", reference.records.size(), tBake);
        if (CompareBakes("control, fp32 again", reference, Bake(points, attrs, params), params, shBytes) != 0) failures++;
        CompareBakes("shape and colour planes", reference, Bake(pointsDecoded, attrsDecoded, params), params, shBytes);
        CompareBakes("all three planes", reference, Bake(pointsAllDecoded, attrsDecoded, params), params, shBytes);
    }
    printf("TestSplatPlanes: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
