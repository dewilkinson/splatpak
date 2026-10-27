// BackgroundEstimate.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The background a scan should be shown on, estimated when it is baked and kept in its view settings (PackageView,
// "background") as a recommendation a viewer applies unless its user chose their own. In order of trust:
//
//   training  A 3DGS training folder's cfg_args beside the .ply (or two folders up, from point_cloud/iteration_N/)
//             records white_background=True|False: the colour the scan was optimised against, so its semi-transparent
//             edges were tuned to blend into exactly that. Solid white or black.
//   shell     An environment: a vertical gradient from the colours of its own far shell (the farthest tenth of its
//             Gaussians) above the horizon to those below it, so where the capture runs out (sky gaps, the edge of the
//             scan) the backdrop continues it instead of showing black.
//   fringe    An object: floaters and edge Gaussians drift toward the colour they were trained against, so an outer
//             shell that is near-white or near-black and colourless gives the training background away.
//   contrast  An object with no such evidence: a neutral grey on the far side of the subject's own brightness.
//
// Colours are display-space 0..1 (SplatPoint::color is 0.5 + C0 * f_dc, the scan's own encoding).

#pragma once
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "GstxTypes.h"
#include "TrainingCameras.h"

namespace Splat
{
    struct PackageBackground
    {
        std::string mode;                          // "solid", "gradient", or empty (not stated)
        DirectX::XMFLOAT3 top = { 0, 0, 0 };       // Solid: both the same
        DirectX::XMFLOAT3 bottom = { 0, 0, 0 };
        std::string basis;                         // How it was chosen: training, shell, fringe, contrast (informative)

        bool Stated() const { return !mode.empty(); }
    };

    namespace BackgroundEstimate
    {
        // white_background from a 3DGS cfg_args near the .ply: 1 white, 0 black, -1 none found
        inline int TrainingBackground(const std::string& plyPath)
        {
            std::string dir = plyPath;
            for (int up = 0; up < 3; up++)
            {
                const size_t slash = dir.find_last_of("/\\");
                if (slash == std::string::npos) break;
                dir.resize(slash);
                std::ifstream f(dir + "/cfg_args");
                if (!f) continue;
                std::stringstream text;
                text << f.rdbuf();
                const std::string s = text.str();
                if (s.find("white_background=True") != std::string::npos) return 1;
                if (s.find("white_background=False") != std::string::npos) return 0;
            }
            return -1;
        }

        inline float Luma(const DirectX::XMFLOAT3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
        inline float Chroma(const DirectX::XMFLOAT3& c) { return std::max(c.x, std::max(c.y, c.z)) - std::min(c.x, std::min(c.y, c.z)); }

        // Per-channel median of the colours at the given indices
        inline DirectX::XMFLOAT3 MedianColour(const SplatPointArray& pts, const std::vector<size_t>& idx)
        {
            if (idx.empty()) return DirectX::XMFLOAT3(0, 0, 0);
            std::vector<float> ch(idx.size());
            float out[3];
            for (int c = 0; c < 3; c++)
            {
                for (size_t i = 0; i < idx.size(); i++)
                {
                    const DirectX::XMFLOAT3& col = pts[idx[i]].color;
                    ch[i] = c == 0 ? col.x : c == 1 ? col.y : col.z;
                }
                std::nth_element(ch.begin(), ch.begin() + ch.size() / 2, ch.end());
                out[c] = std::clamp(ch[ch.size() / 2], 0.0f, 1.0f);
            }
            return DirectX::XMFLOAT3(out[0], out[1], out[2]);
        }

        // centre: the scan's core centre (per-axis median); cameras: the training cameras in model space (their mean up
        // is the scene's up; +y without them)
        inline PackageBackground Estimate(const SplatPointArray& pts, const DirectX::XMFLOAT3& centre, bool environment,
                                          const std::vector<TrainingCamera>& cameras, const std::string& sourcePath)
        {
            PackageBackground bg;
            const int trained = TrainingBackground(sourcePath);
            if (trained >= 0)
            {
                const float v = trained ? 1.0f : 0.0f;
                bg.mode = "solid";
                bg.top = bg.bottom = DirectX::XMFLOAT3(v, v, v);
                bg.basis = "training";
                return bg;
            }
            if (pts.empty()) return bg;

            // A sample of up to 200k, with each one's distance from the centre
            const size_t step = std::max<size_t>(1, pts.size() / 200000);
            std::vector<size_t> sample;
            std::vector<float> dist;
            for (size_t i = 0; i < pts.size(); i += step)
            {
                const DirectX::XMFLOAT3& p = pts[i].position;
                sample.push_back(i);
                dist.push_back(sqrtf((p.x - centre.x) * (p.x - centre.x) + (p.y - centre.y) * (p.y - centre.y) + (p.z - centre.z) * (p.z - centre.z)));
            }
            std::vector<float> sorted = dist;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 9 / 10, sorted.end());
            const float r90 = sorted[sorted.size() * 9 / 10];
            std::vector<size_t> shell, inner;
            for (size_t k = 0; k < sample.size(); k++) (dist[k] >= r90 ? shell : inner).push_back(sample[k]);
            if (shell.empty()) return bg;

            if (environment)
            {
                // Up: the training cameras' mean up, else +y
                DirectX::XMFLOAT3 up(0, 1, 0);
                if (cameras.size() >= 3)
                {
                    DirectX::XMFLOAT3 s(0, 0, 0);
                    for (const TrainingCamera& c : cameras) { s.x += c.up.x; s.y += c.up.y; s.z += c.up.z; }
                    const float len = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z);
                    if (len > 1e-3f) up = DirectX::XMFLOAT3(s.x / len, s.y / len, s.z / len);
                }
                // Above: more than 15 degrees over the horizon seen from the centre; below: under it
                std::vector<size_t> above, below;
                for (size_t i : shell)
                {
                    const DirectX::XMFLOAT3& p = pts[i].position;
                    const float dx = p.x - centre.x, dy = p.y - centre.y, dz = p.z - centre.z;
                    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (len <= 0.0f) continue;
                    const float sinElev = (dx * up.x + dy * up.y + dz * up.z) / len;
                    if (sinElev > 0.26f) above.push_back(i);
                    else if (sinElev < 0.0f) below.push_back(i);
                }
                const size_t minCount = std::max<size_t>(50, shell.size() / 50);
                bg.mode = "gradient";
                bg.top = MedianColour(pts, above.size() >= minCount ? above : shell);
                bg.bottom = MedianColour(pts, below.size() >= minCount ? below : shell);
                bg.basis = "shell";
                return bg;
            }

            // An object: an outer shell that is near-white or near-black and colourless is the training background
            const DirectX::XMFLOAT3 fringe = MedianColour(pts, shell);
            const float fl = Luma(fringe), fc = Chroma(fringe);
            if (fc < 0.08f && (fl > 0.82f || fl < 0.06f))
            {
                const float v = fl > 0.5f ? 1.0f : 0.0f;
                bg.mode = "solid";
                bg.top = bg.bottom = DirectX::XMFLOAT3(v, v, v);
                bg.basis = "fringe";
                return bg;
            }
            // Otherwise a neutral grey against the subject's brightness: light behind a dark subject, a dark gradient
            // behind a bright or mid-toned one
            const float subject = Luma(MedianColour(pts, inner.empty() ? shell : inner));
            bg.basis = "contrast";
            if (subject < 0.3f)
            {
                bg.mode = "solid";
                bg.top = bg.bottom = DirectX::XMFLOAT3(0.72f, 0.72f, 0.72f);
            }
            else
            {
                bg.mode = "gradient";
                bg.top = DirectX::XMFLOAT3(0.20f, 0.20f, 0.21f);
                bg.bottom = DirectX::XMFLOAT3(0.06f, 0.06f, 0.065f);
            }
            return bg;
        }
    }
}
