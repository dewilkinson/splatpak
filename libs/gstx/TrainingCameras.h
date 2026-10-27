// TrainingCameras.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The training cameras of a 3D Gaussian Splatting scan: the cameras.json the reference trainer writes beside its
// point_cloud.ply (one entry per photo: name, image size, position, camera-to-world rotation, focal lengths in pixels).
// Viewing a scan from one of them shows it as the trainer saw it, the view its published renders are made from.
// Converted to SplatPak's model space here: the import's half turn about X (PLYLoader: +Y down / +Z forward to +Y up)
// applied to the position and to the camera's axes. A COLMAP camera looks along +Z with +Y down its image.

#pragma once
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace Splat
{
    struct TrainingCamera
    {
        std::string name;                      // The photo's name (img_name)
        uint32_t width = 0, height = 0;        // The photo's size in pixels
        DirectX::XMFLOAT3 eye = { 0, 0, 0 };   // In model space
        DirectX::XMFLOAT3 forward = { 0, 0, -1 };
        DirectX::XMFLOAT3 up = { 0, 1, 0 };
        float fovY = 0.785398f;                // Vertical field of view from fy and the height
        float aspect = 1.0f;                   // width / height
    };

    struct TrainingCameras
    {
        // Reads cameras.json; false if it is missing or holds no camera. A small reader for this one file's layout: a
        // flat array of objects whose numbers and nested arrays it picks out by key.
        static bool Load(const std::string& path, std::vector<TrainingCamera>& out)
        {
            out.clear();
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string s = ss.str();
            size_t at = 0;
            while ((at = s.find("\"img_name\"", at)) != std::string::npos)
            {
                const size_t end = s.find('}', s.find("\"fx\"", at));
                if (end == std::string::npos) break;
                const std::string e = s.substr(at, end - at);
                at = end;
                TrainingCamera c;
                const size_t q0 = e.find('"', e.find(':') + 1), q1 = e.find('"', q0 + 1);
                if (q0 != std::string::npos && q1 != std::string::npos) c.name = e.substr(q0 + 1, q1 - q0 - 1);
                double pos[3], rot[9], width = 0, height = 0, fy = 0;
                if (!Numbers(e, "\"position\"", pos, 3) || !Numbers(e, "\"rotation\"", rot, 9) || !Numbers(e, "\"width\"", &width, 1) ||
                    !Numbers(e, "\"height\"", &height, 1) || !Numbers(e, "\"fy\"", &fy, 1) || height <= 0 || fy <= 0)
                    continue;
                c.width = (uint32_t)width;
                c.height = (uint32_t)height;
                c.aspect = (float)(width / height);
                c.fovY = (float)(2.0 * std::atan(0.5 * height / fy));
                // rotation is camera-to-world, row-major: its columns are the camera's right, down and forward
                auto toModel = [](double x, double y, double z) { return DirectX::XMFLOAT3((float)x, (float)-y, (float)-z); };
                c.eye = toModel(pos[0], pos[1], pos[2]);
                c.forward = toModel(rot[2], rot[5], rot[8]);
                const DirectX::XMFLOAT3 down = toModel(rot[1], rot[4], rot[7]);
                c.up = DirectX::XMFLOAT3(-down.x, -down.y, -down.z);
                out.push_back(c);
            }
            return !out.empty();
        }

        // The cameras.json beside a scan: in its folder, or the folder above (a 3DGS output keeps it beside
        // point_cloud/iteration_N/point_cloud.ply)
        static std::string FindFor(const std::string& scanPath)
        {
            const size_t slash = scanPath.find_last_of("\\/");
            std::string folder = slash == std::string::npos ? std::string(".") : scanPath.substr(0, slash);
            for (int up = 0; up < 3; up++)
            {
                const std::string candidate = folder + "\\cameras.json";
                if (std::ifstream(candidate).good()) return candidate;
                const size_t s = folder.find_last_of("\\/");
                if (s == std::string::npos) break;
                folder = folder.substr(0, s);
            }
            return std::string();
        }

        // Where the cameras look: the point nearest every camera's view axis in the least-squares sense (sum over cameras of
        // (I - f f^T)(p - eye) = 0). A capture walked around its subject converges on it. False with fewer than two
        // cameras or when their axes are near parallel (a fly-through, a panorama).
        static bool Converge(const std::vector<TrainingCamera>& cams, DirectX::XMFLOAT3& out)
        {
            if (cams.size() < 2) return false;
            double a[3][3] = {}, b[3] = {};
            for (const TrainingCamera& c : cams)
            {
                const double f[3] = { c.forward.x, c.forward.y, c.forward.z }, e[3] = { c.eye.x, c.eye.y, c.eye.z };
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                    {
                        const double m = (i == j ? 1.0 : 0.0) - f[i] * f[j];
                        a[i][j] += m;
                        b[i] += m * e[j];
                    }
            }
            const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
            if (std::fabs(det) < 1e-6 * (double)cams.size() * (double)cams.size() * (double)cams.size()) return false;
            double p[3];
            for (int k = 0; k < 3; k++)
            {
                double m[3][3];
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) m[i][j] = j == k ? b[i] : a[i][j];
                p[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
            }
            out = DirectX::XMFLOAT3((float)p[0], (float)p[1], (float)p[2]);
            return true;
        }

    private:
        // The first `count` numbers after key (in nested brackets or not)
        static bool Numbers(const std::string& e, const char* key, double* out, int count)
        {
            size_t p = e.find(key);
            if (p == std::string::npos) return false;
            p = e.find(':', p) + 1;
            for (int i = 0; i < count; i++)
            {
                while (p < e.size() && !(isdigit((unsigned char)e[p]) || e[p] == '-' || e[p] == '.')) p++;
                if (p >= e.size()) return false;
                char* endp = nullptr;
                out[i] = strtod(e.c_str() + p, &endp);
                p = (size_t)(endp - e.c_str());
            }
            return true;
        }
    };
}
