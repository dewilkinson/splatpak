#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include "../../src/DX12/Wavelet/WaveletTypes.h"

namespace Surfels
{
    struct QualityReport
    {
        double   geometricRmseMM     = 0.0; // Root Mean Square Error in mm
        double   hausdorffDistanceMM = 0.0; // Peak L-infinity deviation in mm
        double   geometricPsnrDb     = 0.0; // Geometric Peak Signal-to-Noise Ratio (dB)
        double   colorRmse           = 0.0; // Color Root Mean Square Error [0..1]
        double   colorPsnrDb         = 0.0; // Color Peak Signal-to-Noise Ratio (dB)
        double   colorDeltaE         = 0.0; // Mean Color Delta E
        double   meanNormalErrorDeg  = 0.0; // Mean Surface Normal Angular Error in degrees
        double   ssim                = 0.0; // Structural Similarity Index [0..1]
        uint64_t sampleCount         = 0;
        float    maxErrorThresholdMM = 1.0f; // Heatmap clamp range (default 1.0mm)
    };

    class QualityMetrics
    {
    public:
        // Evaluate Turbo Colormap (polynomial approximation for high performance)
        static XMFLOAT3 TurboColormap(float t)
        {
            t = std::max(0.0f, std::min(1.0f, t));
            const float kRedVec4[4]   = { 0.13572138f,  4.61539260f, -42.66032258f,  132.13108234f };
            const float kGreenVec4[4] = { 0.09140261f,  2.19418839f,   4.84296658f,  -14.18503333f };
            const float kBlueVec4[4]  = { 0.10667330f, 12.64194608f, -60.58204836f,  110.36276771f };
            const float kRedVec2[2]   = { -152.94239396f,  59.28637943f };
            const float kGreenVec2[2] = {    4.27729857f,   2.82956604f };
            const float kBlueVec2[2]  = {  -89.90310912f,  27.34824973f };

            float t2 = t * t;
            float t3 = t2 * t;
            float t4 = t3 * t;
            float t5 = t4 * t;

            float r = kRedVec4[0] + kRedVec4[1]*t + kRedVec4[2]*t2 + kRedVec4[3]*t3 + kRedVec2[0]*t4 + kRedVec2[1]*t5;
            float g = kGreenVec4[0] + kGreenVec4[1]*t + kGreenVec4[2]*t2 + kGreenVec4[3]*t3 + kGreenVec2[0]*t4 + kGreenVec2[1]*t5;
            float b = kBlueVec4[0] + kBlueVec4[1]*t + kBlueVec4[2]*t2 + kBlueVec4[3]*t3 + kBlueVec2[0]*t4 + kBlueVec2[1]*t5;

            return XMFLOAT3(
                std::max(0.0f, std::min(1.0f, r)),
                std::max(0.0f, std::min(1.0f, g)),
                std::max(0.0f, std::min(1.0f, b))
            );
        }

        // Compare raw uncompressed surfels against quantized/wavelet surfels
        static QualityReport ComputeQuality(
            const std::vector<SurfelVertex>& rawSurfels,
            const std::vector<SurfelVertex>& compSurfels,
            const XMFLOAT3& aabbMin,
            const XMFLOAT3& aabbMax,
            float maxErrorThresholdMM = 1.0f)
        {
            QualityReport report;
            report.maxErrorThresholdMM = maxErrorThresholdMM;
            size_t n = std::min(rawSurfels.size(), compSurfels.size());
            if (n == 0) return report;

            report.sampleCount = n;

            double sumSqGeomDist = 0.0;
            double maxGeomDist = 0.0;
            double sumSqColorDist = 0.0;
            double sumColorDist = 0.0;
            double sumNormalAngleDeg = 0.0;

            // Compute AABB bounding diagonal as peak signal reference for geometric PSNR
            float dx = aabbMax.x - aabbMin.x;
            float dy = aabbMax.y - aabbMin.y;
            float dz = aabbMax.z - aabbMin.z;
            double bboxDiagonal = std::sqrt((double)dx*dx + (double)dy*dy + (double)dz*dz);
            if (bboxDiagonal <= 1e-6) bboxDiagonal = 1.0;

            // Means and variances for SSIM
            double meanRawY = 0.0;
            double meanCompY = 0.0;

            for (size_t i = 0; i < n; i++)
            {
                const auto& r = rawSurfels[i];
                const auto& c = compSurfels[i];

                // 1. Geometric Euclidean Distance
                double gdx = (double)r.position.x - (double)c.position.x;
                double gdy = (double)r.position.y - (double)c.position.y;
                double gdz = (double)r.position.z - (double)c.position.z;
                double distSq = gdx*gdx + gdy*gdy + gdz*gdz;
                double dist = std::sqrt(distSq);

                sumSqGeomDist += distSq;
                if (dist > maxGeomDist) maxGeomDist = dist;

                // 2. Color Euclidean Distance (RGB)
                double cdx = (double)r.color.x - (double)c.color.x;
                double cdy = (double)r.color.y - (double)c.color.y;
                double cdz = (double)r.color.z - (double)c.color.z;
                double cdistSq = (cdx*cdx + cdy*cdy + cdz*cdz) / 3.0;
                sumSqColorDist += cdistSq;
                sumColorDist += std::sqrt(cdx*cdx + cdy*cdy + cdz*cdz) * 100.0; // Scaled Delta E

                // 3. Normal Angular Error (degrees)
                double dotN = (double)r.normal.x * c.normal.x + (double)r.normal.y * c.normal.y + (double)r.normal.z * c.normal.z;
                dotN = std::max(-1.0, std::min(1.0, dotN));
                double angleDeg = std::acos(dotN) * (180.0 / 3.14159265358979323846);
                sumNormalAngleDeg += angleDeg;

                // Luminance for SSIM (ITU-R BT.601)
                double lRaw = 0.299 * r.color.x + 0.587 * r.color.y + 0.114 * r.color.z;
                double lComp = 0.299 * c.color.x + 0.587 * c.color.y + 0.114 * c.color.z;
                meanRawY += lRaw;
                meanCompY += lComp;
            }

            meanRawY /= (double)n;
            meanCompY /= (double)n;

            double varRawY = 0.0;
            double varCompY = 0.0;
            double covY = 0.0;

            for (size_t i = 0; i < n; i++)
            {
                const auto& r = rawSurfels[i];
                const auto& c = compSurfels[i];
                double lRaw = 0.299 * r.color.x + 0.587 * r.color.y + 0.114 * r.color.z;
                double lComp = 0.299 * c.color.x + 0.587 * c.color.y + 0.114 * c.color.z;

                double dr = lRaw - meanRawY;
                double dc = lComp - meanCompY;
                varRawY += dr * dr;
                varCompY += dc * dc;
                covY += dr * dc;
            }

            varRawY /= (double)n;
            varCompY /= (double)n;
            covY /= (double)n;

            // SSIM Constants
            const double k1 = 0.01;
            const double k2 = 0.03;
            const double L = 1.0;
            const double c1 = (k1 * L) * (k1 * L);
            const double c2 = (k2 * L) * (k2 * L);

            double ssimNum = (2.0 * meanRawY * meanCompY + c1) * (2.0 * covY + c2);
            double ssimDen = (meanRawY * meanRawY + meanCompY * meanCompY + c1) * (varRawY + varCompY + c2);
            report.ssim = (ssimDen > 1e-9) ? std::max(0.0, std::min(1.0, ssimNum / ssimDen)) : 1.0;

            // Compute Final Metrics
            double geomMse = sumSqGeomDist / (double)n;
            double geomRmseMeters = std::sqrt(geomMse);
            report.geometricRmseMM = geomRmseMeters * 1000.0;
            report.hausdorffDistanceMM = maxGeomDist * 1000.0;

            if (geomRmseMeters > 1e-9)
            {
                report.geometricPsnrDb = 20.0 * std::log10(bboxDiagonal / geomRmseMeters);
            }
            else
            {
                report.geometricPsnrDb = 99.9; // Practically infinite / lossless
            }

            double colorMse = sumSqColorDist / (double)n;
            report.colorRmse = std::sqrt(colorMse);
            if (report.colorRmse > 1e-9)
            {
                report.colorPsnrDb = 20.0 * std::log10(1.0 / report.colorRmse);
            }
            else
            {
                report.colorPsnrDb = 99.9;
            }

            report.colorDeltaE = sumColorDist / (double)n;
            report.meanNormalErrorDeg = sumNormalAngleDeg / (double)n;

            return report;
        }

        // Generate a visual Diff Heatmap point cloud with colors mapped via Turbo colormap
        static std::vector<SurfelVertex> GenerateDiffHeatmap(
            const std::vector<SurfelVertex>& rawSurfels,
            const std::vector<SurfelVertex>& compSurfels,
            int diffMode = 0, // 0 = Geometric Error (mm), 1 = Color Delta E, 2 = Normal Angular Error
            float maxErrorThresholdMM = 1.0f)
        {
            size_t n = std::min(rawSurfels.size(), compSurfels.size());
            std::vector<SurfelVertex> heatmap(n);

            float thresholdMeters = std::max(0.0001f, maxErrorThresholdMM / 1000.0f);

            for (size_t i = 0; i < n; i++)
            {
                const auto& r = rawSurfels[i];
                const auto& c = compSurfels[i];

                heatmap[i].position = c.position;
                heatmap[i].normal = c.normal;
                heatmap[i].radius = c.radius;

                float t = 0.0f;

                if (diffMode == 0) // Geometric Error
                {
                    float dx = r.position.x - c.position.x;
                    float dy = r.position.y - c.position.y;
                    float dz = r.position.z - c.position.z;
                    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                    t = dist / thresholdMeters;
                }
                else if (diffMode == 1) // Color Delta
                {
                    float dr = r.color.x - c.color.x;
                    float dg = r.color.y - c.color.y;
                    float db = r.color.z - c.color.z;
                    float cdist = std::sqrt(dr*dr + dg*dg + db*db);
                    t = cdist / 0.25f; // Clamped at 25% color error
                }
                else if (diffMode == 2) // Normal Angular Error
                {
                    float dotN = r.normal.x * c.normal.x + r.normal.y * c.normal.y + r.normal.z * c.normal.z;
                    dotN = std::max(-1.0f, std::min(1.0f, dotN));
                    float angleDeg = std::acos(dotN) * (180.0f / 3.14159265f);
                    t = angleDeg / 15.0f; // Clamped at 15 degrees
                }

                heatmap[i].color = TurboColormap(t);
            }

            return heatmap;
        }
    };
}

