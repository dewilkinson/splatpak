// TestShadowMesh.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Checks a package's shadow mesh (docs/requirements/gstx-shadow-mesh.md, criteria 1 to 3): the levels' triangle counts,
// that the finest level is closed, outward-facing and free of degenerate triangles, and that it follows the visible
// surface (distances from the opaque splats' centres to the mesh, against the bounding-box diagonal; translucent fuzz,
// opacity under 0.5, casts no hard shadow and is left out). A package without a mesh section has
// one built from its level 0 splats, so the builder can be checked on older packages too. Also round-trips the mesh
// through the section codec.
//
//   TestShadowMesh <package.gstx> [...]
//
// Exit code 0 when every package passes, 1 otherwise.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "../src/SplatCore/StreamPackager.h"
#include "../libs/gstx/SplatCodec.h"
#include "../libs/gstx/ShadowMeshBuilder.h"
#include "../libs/gstx/ShadowMeshCodec.h"

using namespace Splat;

namespace
{
    struct D3 { double x, y, z; };
    D3 Sub(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    D3 Add(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    D3 Mul(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
    double Dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    D3 Cross(D3 a, D3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    double Len(D3 a) { return std::sqrt(Dot(a, a)); }
    D3 P(const XMFLOAT3& p) { return { p.x, p.y, p.z }; }
    D3 P(const ShadowMeshVec3& p) { return { p.x, p.y, p.z }; }

    double PointTriangle(D3 p, D3 a, D3 b, D3 c)
    {
        const D3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
        const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) return Len(ap);
        const D3 bp = Sub(p, b); const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) return Len(bp);
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) return Len(Sub(p, Add(a, Mul(ab, d1 / (d1 - d3)))));
        const D3 cp = Sub(p, c); const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) return Len(cp);
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) return Len(Sub(p, Add(a, Mul(ac, d2 / (d2 - d6)))));
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return Len(Sub(p, Add(b, Mul(Sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))))));
        const double den = 1.0 / (va + vb + vc);
        return Len(Sub(p, Add(a, Add(Mul(ab, vb * den), Mul(ac, vc * den)))));
    }

    // Uniform bucket grid of triangles; distances capped at cap
    struct TriangleGrid
    {
        const ShadowMeshLevel& m; double size, cap; D3 origin; int n[3]; std::vector<std::vector<uint32_t>> cells;
        TriangleGrid(const ShadowMeshLevel& level, double bucket, double maxDistance) : m(level), size(bucket), cap(maxDistance)
        {
            D3 mn = { 1e30, 1e30, 1e30 }, mx = { -1e30, -1e30, -1e30 };
            for (const auto& p : m.positions) { mn = { std::min(mn.x, (double)p.x), std::min(mn.y, (double)p.y), std::min(mn.z, (double)p.z) }; mx = { std::max(mx.x, (double)p.x), std::max(mx.y, (double)p.y), std::max(mx.z, (double)p.z) }; }
            origin = Sub(mn, { cap, cap, cap });
            const double e[3] = { mx.x - mn.x, mx.y - mn.y, mx.z - mn.z };
            for (int k = 0; k < 3; k++) n[k] = (int)std::ceil((e[k] + 2 * cap) / size) + 1;
            cells.resize((size_t)n[0] * n[1] * n[2]);
            for (size_t t = 0; t < m.TriangleCount(); t++)
            {
                const D3 a = P(m.positions[m.indices[t * 3]]), b = P(m.positions[m.indices[t * 3 + 1]]), c = P(m.positions[m.indices[t * 3 + 2]]);
                const double lo[3] = { std::min({ a.x, b.x, c.x }), std::min({ a.y, b.y, c.y }), std::min({ a.z, b.z, c.z }) };
                const double hi[3] = { std::max({ a.x, b.x, c.x }), std::max({ a.y, b.y, c.y }), std::max({ a.z, b.z, c.z }) };
                const double o[3] = { origin.x, origin.y, origin.z };
                int l[3], h[3];
                for (int k = 0; k < 3; k++) { l[k] = std::clamp((int)((lo[k] - o[k]) / size), 0, n[k] - 1); h[k] = std::clamp((int)((hi[k] - o[k]) / size), 0, n[k] - 1); }
                for (int z = l[2]; z <= h[2]; z++) for (int y = l[1]; y <= h[1]; y++) for (int x = l[0]; x <= h[0]; x++)
                    cells[(size_t)x + (size_t)n[0] * ((size_t)y + (size_t)n[1] * z)].push_back((uint32_t)t);
            }
        }
        double Distance(D3 p) const
        {
            const int r = (int)std::ceil(cap / size);
            const int c[3] = { (int)((p.x - origin.x) / size), (int)((p.y - origin.y) / size), (int)((p.z - origin.z) / size) };
            double best = cap;
            for (int z = std::max(0, c[2] - r); z <= std::min(n[2] - 1, c[2] + r); z++)
                for (int y = std::max(0, c[1] - r); y <= std::min(n[1] - 1, c[1] + r); y++)
                    for (int x = std::max(0, c[0] - r); x <= std::min(n[0] - 1, c[0] + r); x++)
                        for (uint32_t t : cells[(size_t)x + (size_t)n[0] * ((size_t)y + (size_t)n[1] * z)])
                            best = std::min(best, PointTriangle(p, P(m.positions[m.indices[t * 3]]), P(m.positions[m.indices[t * 3 + 1]]), P(m.positions[m.indices[t * 3 + 2]])));
            return best;
        }
    };

    double Quantile(std::vector<double> v, double q)
    {
        const size_t k = std::min(v.size() - 1, (size_t)(q * v.size()));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    }

    int g_failures = 0;
    void Check(bool ok, const char* what, double got, double limit)
    {
        if (!ok) { printf("  FAIL: %s (%.4g, limit %.4g)\n", what, got, limit); g_failures++; }
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> paths;
    for (int i = 1; i < argc; i++) paths.push_back(argv[i]);
    if (paths.empty()) paths = { "assets/cthulu/cthulu.gstx", "assets/venus/venus.gstx", "assets/dwarves/dwarves.gstx", "assets/strawberry/strawberry.gstx" };

    for (const auto& path : paths)
    {
        printf("%s\n", path.c_str());
        StreamPackager::SFLWPackageData pkg;
        std::string reason;
        if (!StreamPackager::LoadPackage(path, pkg, &reason)) { printf("  FAIL: %s\n", reason.c_str()); g_failures++; continue; }

        // Level 0 splats and their attributes, decoded
        std::vector<SplatPoint> points;
        std::vector<SplatAttributes> attrs;
        for (size_t c = 0; c < pkg.chunkManifests.size(); c++)
        {
            if (!pkg.IsSplatPackage() || pkg.chunkLODSplats[c].empty()) continue;
            std::vector<SplatPoint> pts; std::vector<SplatAttributes> at;
            const auto& sh = pkg.chunkLODSH[c][0];
            SplatCodec::Decode(pkg.chunkLODSplats[c][0], sh.empty() ? nullptr : sh.data(), sh.size(), pkg.splatParams, 0, 0, pts, at);
            for (size_t i = 0; i < pts.size(); i++) { pts[i].sourceIndex = (uint32_t)(attrs.size() + i); }
            points.insert(points.end(), pts.begin(), pts.end());
            attrs.insert(attrs.end(), at.begin(), at.end());
        }
        if (points.empty()) { printf("  FAIL: no level 0 splats\n"); g_failures++; continue; }
        const XMFLOAT3 mn = pkg.header.globalBoundsMin, mx = pkg.header.globalBoundsMax;
        const double diag = Len(Sub(P(mx), P(mn)));

        ShadowMesh mesh = pkg.shadowMesh;
        const char* source = "from the package";
        if (mesh.Empty())
        {
            std::string trace;
            const auto t0 = std::chrono::steady_clock::now();
            if (!ShadowMeshBuilder::Build(points, &attrs, mn, mx, mesh, trace)) { printf("  FAIL: no mesh could be built\n"); g_failures++; continue; }
            printf("%s  built in %.2f s\n", trace.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            source = "built here";
        }
        printf("  %zu levels %s:", mesh.levels.size(), source);
        for (const auto& l : mesh.levels) printf(" %zu", l.TriangleCount());
        printf(" triangles\n");

        // Criterion 1: three levels, the finest 8k to 20k triangles (more allowed only when thin parts need it), halving
        Check(mesh.levels.size() == 3, "three levels", (double)mesh.levels.size(), 3);
        Check(mesh.levels[0].TriangleCount() >= 8000, "finest level has at least 8,000 triangles", (double)mesh.levels[0].TriangleCount(), 8000);
        for (size_t l = 1; l < mesh.levels.size(); l++)
            Check(mesh.levels[l].TriangleCount() * 2 <= mesh.levels[l - 1].TriangleCount() + 2, "each level at most half the previous", (double)mesh.levels[l].TriangleCount(), mesh.levels[l - 1].TriangleCount() / 2.0);

        // Criterion 2: closed (every edge shared by exactly two triangles, in opposite directions), no degenerate triangles
        const ShadowMeshLevel& m = mesh.levels[0];
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        size_t degenerate = 0;
        double volume = 0;
        for (size_t t = 0; t < m.TriangleCount(); t++)
        {
            const uint32_t i[3] = { m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2] };
            for (int k = 0; k < 3; k++) edges[{ i[k], i[(k + 1) % 3] }]++;
            const D3 a = P(m.positions[i[0]]), b = P(m.positions[i[1]]), c = P(m.positions[i[2]]);
            if (i[0] == i[1] || i[1] == i[2] || i[0] == i[2] || Len(Cross(Sub(b, a), Sub(c, a))) < 1e-14 * diag * diag) degenerate++;
            volume += Dot(a, Cross(b, c)) / 6.0;
        }
        size_t open = 0;
        for (const auto& e : edges)
        {
            auto it = edges.find({ e.first.second, e.first.first });
            if (e.second != 1 || it == edges.end() || it->second != 1) open++;
        }
        // Euler characteristic V - E + F = 2 (components - genus) on a closed manifold
        const long long euler = (long long)m.positions.size() - (long long)(edges.size() / 2) + (long long)m.TriangleCount();
        printf("  finest level: %zu vertices, %zu edges not shared by exactly two opposite triangles, %zu degenerate triangles, enclosed volume %.4g, Euler characteristic %lld\n",
               m.positions.size(), open, degenerate, volume, euler);
        Check(open == 0, "closed", (double)open, 0);
        Check(degenerate == 0, "no degenerate triangles", (double)degenerate, 0);
        Check(volume > 0, "faces point outward (positive enclosed volume)", volume, 0);

        // Criterion 3: splat centres to the mesh, and the mesh's vertices to the splat centres. Splats are found through a
        // bucket grid of points.
        std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
        const double bs = 0.005 * diag;
        auto key = [](int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 1048576) << 42) | ((uint64_t)(uint32_t)(y + 1048576) << 21) | (uint64_t)(uint32_t)(z + 1048576); };
        for (uint32_t i = 0; i < points.size(); i++)
            buckets[key((int)std::floor(points[i].position.x / bs), (int)std::floor(points[i].position.y / bs), (int)std::floor(points[i].position.z / bs))].push_back(i);
        // Nearest splat centre to p, other than skip, searched out to maxRings buckets
        auto nearest = [&](D3 p, size_t skip, int maxRings)
        {
            const int c[3] = { (int)std::floor(p.x / bs), (int)std::floor(p.y / bs), (int)std::floor(p.z / bs) };
            double best = 1e30;
            for (int r = 1; r <= maxRings && best > (r - 1) * bs; r++)
                for (int z = c[2] - r; z <= c[2] + r; z++) for (int y = c[1] - r; y <= c[1] + r; y++) for (int x = c[0] - r; x <= c[0] + r; x++)
                {
                    auto it = buckets.find(key(x, y, z));
                    if (it == buckets.end()) continue;
                    for (uint32_t i : it->second) if (i != skip) best = std::min(best, Len(Sub(P(points[i].position), p)));
                }
            return best;
        };

        // Measured: the splats that make up the visible surface. Left out: translucent haze (the summed opacity of the splats
        // within 0.4% of the diagonal, about one grid cell of the builder, under 1: no hard shadow) and isolated splats (no
        // other splat within 1% of the diagonal: sampling too sparse to form a surface, such as a scan's underside)
        auto support = [&](size_t i)
        {
            const D3 p = P(points[i].position);
            const double r = 0.004 * diag;
            const int c[3] = { (int)std::floor(p.x / bs), (int)std::floor(p.y / bs), (int)std::floor(p.z / bs) };
            double s = 0;
            for (int z = c[2] - 1; z <= c[2] + 1; z++) for (int y = c[1] - 1; y <= c[1] + 1; y++) for (int x = c[0] - 1; x <= c[0] + 1; x++)
            {
                auto it = buckets.find(key(x, y, z));
                if (it == buckets.end()) continue;
                for (uint32_t j : it->second) if (Len(Sub(P(points[j].position), p)) <= r) s += attrs[points[j].sourceIndex].opacity;
            }
            return s;
        };
        TriangleGrid tg(m, 0.01 * diag, 0.05 * diag);
        const size_t stride = std::max<size_t>(1, points.size() / 300000);
        std::vector<double> d;
        std::vector<size_t> measured;
        size_t translucent = 0, isolated = 0, sampled = 0;
        for (size_t i = 0; i < points.size(); i += stride)
        {
            sampled++;
            if (support(i) < 1.0) { translucent++; continue; }
            if (nearest(P(points[i].position), i, 2) > 0.01 * diag) { isolated++; continue; }
            d.push_back(tg.Distance(P(points[i].position)) / diag);
            measured.push_back(i);
        }
        const double q50 = Quantile(d, 0.5), q95 = Quantile(d, 0.95), q999 = Quantile(d, 0.999);
        printf("  measured %zu of %zu sampled splats (%zu in translucent haze, %zu isolated left out)\n", measured.size(), sampled, translucent, isolated);
        if (getenv("SHADOWMESH_DIAG"))
        {
            int shown = 0;
            for (size_t j = 0; j < measured.size() && shown < 12; j++)
                if (d[j] > 0.02)
                {
                    const size_t i = measured[j];
                    const auto& a = attrs[points[i].sourceIndex];
                    printf("    far splat at (%.3f %.3f %.3f): %.2f%% from the mesh, opacity %.2f, scale %.4f %.4f %.4f, nearest splat %.2f%% of the diagonal\n",
                           points[i].position.x, points[i].position.y, points[i].position.z, 100 * d[j], a.opacity, a.scale.x, a.scale.y, a.scale.z,
                           100 * nearest(P(points[i].position), i, 8) / diag);
                    shown++;
                }
        }
        std::vector<double> dv;
        for (const auto& v : m.positions) dv.push_back(std::min(nearest(P(v), (size_t)-1, 8), 0.05 * diag) / diag);
        const double v50 = Quantile(dv, 0.5);
        printf("  splat to mesh: median %.3f%%, 95th %.3f%%, 99.9th %.3f%% of the diagonal; mesh vertex to splat: median %.3f%%\n",
               100 * q50, 100 * q95, 100 * q999, 100 * v50);
        Check(q50 <= 0.005, "median splat distance <= 0.5%", q50, 0.005);
        Check(q95 <= 0.01, "95th percentile <= 1%", q95, 0.01);
        Check(q999 <= 0.02, "99.9th percentile <= 2%", q999, 0.02);
        Check(v50 <= 0.005, "median vertex distance <= 0.5%", v50, 0.005);

        // The section codec round-trips the mesh (positions within the 16-bit quantisation)
        std::vector<uint8_t> section;
        ShadowMesh back;
        std::string error;
        const bool encoded = ShadowMeshCodec::Encode(mesh, section);
        const bool decoded = encoded && ShadowMeshCodec::Decode(section.data(), section.size(), back, error);
        double maxErr = 0;
        bool same = decoded && back.levels.size() == mesh.levels.size();
        for (size_t l = 0; same && l < mesh.levels.size(); l++)
        {
            same = back.levels[l].indices == mesh.levels[l].indices && back.levels[l].positions.size() == mesh.levels[l].positions.size();
            for (size_t i = 0; same && i < mesh.levels[l].positions.size(); i++)
                maxErr = std::max(maxErr, Len(Sub(P(back.levels[l].positions[i]), P(mesh.levels[l].positions[i]))));
            same = same && back.levels[l].colours == mesh.levels[l].colours;
        }
        printf("  section %.1f KB, round trip %s (position error %.2g of the diagonal)\n", section.size() / 1024.0, same ? "exact" : "FAILED", maxErr / diag);
        Check(same, "codec round trip", 0, 0);
        Check(maxErr <= 2e-5 * diag + 1e-9, "round-trip position error within 16-bit quantisation", maxErr / diag, 2e-5);
        Check(section.size() <= (1u << 20), "section at most 1 MB", (double)section.size(), 1 << 20);
    }
    printf("TestShadowMesh: %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
