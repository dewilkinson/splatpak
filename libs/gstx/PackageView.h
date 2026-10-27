// PackageView.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// A package's view settings: how a viewing app should open it (docs/GSTX_FORMAT_SPECIFICATION.md, clause 14.2), kept in
// the .gstx as the trailing section GSTXVEW1 (PackageFooters.h). Written by the bake from what SplatPak knows of the scan:
// whether it is an environment or an object (and its core), whether the occlusion volume should start on, the training
// cameras (cameras.json, converted to model space) and the one to open at. Text, one setting per line, a keyword and its
// values; a reader ignores keywords it does not know, so settings can be added without a version change.
//
//   gstx-view 1
//   scene environment|object
//   core <cx> <cy> <cz> <radius50> <radius99>      model space, metres
//   occlusion 0|1                                   show the occlusion volume on open
//   grid 0|1                                        show the editor grid on open
//   start_camera <n>                                index into the cameras below; absent = the app's own framing
//   subject <cx> <cy> <cz> <radius>                 what the scan is of: where the training cameras converge, out to where
//                                                   the density falls to a tenth (model space, metres); streamed first
//   background solid|gradient <tr> <tg> <tb> <br> <bg> <bb> <basis>   the recommended backdrop: top and bottom colours,
//                                                   display-space 0..1 (solid: both the same); basis is how the bake chose
//                                                   it (BackgroundEstimate.h). A viewer applies it unless its user chose their own
//   navigation orbit|fly                            how the viewer's camera starts: orbiting the model (an object) or flying
//                                                   through it (an environment)
//   camera <ex> <ey> <ez> <fx> <fy> <fz> <ux> <uy> <uz> <fovY> <width> <height> <name>   eye, unit view, unit up, radians, photo size

#pragma once
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "PackageFooters.h"
#include "TrainingCameras.h"
#include "BackgroundEstimate.h"

namespace Splat
{
    struct PackageView
    {
        static constexpr char kMarker[8] = { 'G', 'S', 'T', 'X', 'V', 'E', 'W', '1' };

        std::string scene;                        // "environment", "object", or empty (not stated)
        DirectX::XMFLOAT3 coreCentre = { 0, 0, 0 };
        float coreRadius50 = 0.0f, coreRadius99 = 0.0f;
        int occlusion = -1;                       // -1 not stated, 0 off, 1 on
        int grid = -1;                            // The editor grid on open: -1 not stated, 0 off, 1 on
        int startCamera = -1;
        DirectX::XMFLOAT3 subjectCentre = { 0, 0, 0 };
        float subjectRadius = 0.0f;               // 0 = not stated
        std::string navigation;                   // "orbit", "fly", or empty (not stated)
        PackageBackground background;             // The recommended backdrop; mode empty = not stated
        std::vector<TrainingCamera> cameras;

        std::string Text() const
        {
            std::ostringstream o;
            o << "gstx-view 1\n";
            char line[512];
            if (!scene.empty()) o << "scene " << scene << "\n";
            if (coreRadius50 > 0.0f)
            {
                snprintf(line, sizeof(line), "core %.6g %.6g %.6g %.6g %.6g\n", coreCentre.x, coreCentre.y, coreCentre.z, coreRadius50, coreRadius99);
                o << line;
            }
            if (occlusion >= 0) o << "occlusion " << occlusion << "\n";
            if (grid >= 0) o << "grid " << grid << "\n";
            if (startCamera >= 0 && startCamera < (int)cameras.size()) o << "start_camera " << startCamera << "\n";
            if (subjectRadius > 0.0f)
            {
                snprintf(line, sizeof(line), "subject %.6g %.6g %.6g %.6g\n", subjectCentre.x, subjectCentre.y, subjectCentre.z, subjectRadius);
                o << line;
            }
            if (!navigation.empty()) o << "navigation " << navigation << "\n";
            if (background.Stated())
            {
                const PackageBackground& b = background;
                snprintf(line, sizeof(line), "background %s %.4g %.4g %.4g %.4g %.4g %.4g %s\n", b.mode.c_str(), b.top.x, b.top.y, b.top.z,
                    b.bottom.x, b.bottom.y, b.bottom.z, b.basis.empty() ? "-" : b.basis.c_str());
                o << line;
            }
            for (const TrainingCamera& c : cameras)
            {
                snprintf(line, sizeof(line), "camera %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %u %u %s\n", c.eye.x, c.eye.y, c.eye.z,
                    c.forward.x, c.forward.y, c.forward.z, c.up.x, c.up.y, c.up.z, c.fovY, c.width, c.height, c.name.empty() ? "-" : c.name.c_str());
                o << line;
            }
            return o.str();
        }

        bool Parse(const std::string& text)
        {
            std::istringstream in(text);
            std::string line;
            bool seen = false;
            while (std::getline(in, line))
            {
                std::istringstream l(line);
                std::string key;
                l >> key;
                if (key == "gstx-view") seen = true;
                else if (key == "scene") l >> scene;
                else if (key == "core") l >> coreCentre.x >> coreCentre.y >> coreCentre.z >> coreRadius50 >> coreRadius99;
                else if (key == "occlusion") l >> occlusion;
                else if (key == "grid") l >> grid;
                else if (key == "start_camera") l >> startCamera;
                else if (key == "subject") l >> subjectCentre.x >> subjectCentre.y >> subjectCentre.z >> subjectRadius;
                else if (key == "navigation") { std::string v; l >> v; if (v == "orbit" || v == "fly") navigation = v; }
                else if (key == "background")
                {
                    PackageBackground b;
                    l >> b.mode >> b.top.x >> b.top.y >> b.top.z >> b.bottom.x >> b.bottom.y >> b.bottom.z >> b.basis;
                    if (!l.fail() && (b.mode == "solid" || b.mode == "gradient")) background = b;
                }
                else if (key == "camera")
                {
                    TrainingCamera c;
                    l >> c.eye.x >> c.eye.y >> c.eye.z >> c.forward.x >> c.forward.y >> c.forward.z >> c.up.x >> c.up.y >> c.up.z >> c.fovY >> c.width >> c.height >> c.name;
                    if (c.height > 0 && c.fovY > 0.0f) // A line cut short leaves these unset
                    {
                        c.aspect = c.height > 0 ? (float)c.width / (float)c.height : 1.0f;
                        cameras.push_back(c);
                    }
                }
            }
            return seen;
        }

        bool Append(const std::string& packagePath) const { return PackageFooters::Append(packagePath, kMarker, Text()); }
        bool Write(const std::string& packagePath) const { return PackageFooters::Replace(packagePath, kMarker, Text()); } // In place of any it has
        bool Read(const std::string& packagePath)
        {
            std::string text;
            return PackageFooters::Read(packagePath, kMarker, text) && Parse(text);
        }
        bool ReadMemory(const uint8_t* data, size_t size)
        {
            std::string text;
            return PackageFooters::ReadMemory(data, size, kMarker, text) && Parse(text);
        }
    };
}
