#pragma once
#include "../../src/DX12/stdafx.h"
#include "PreprocessRenderer.h"
#include "SyntheticGenerator.h"
#include "PLYLoader.h"
#include "SPLATLoader.h"
#include "SpatialOctree.h"
#include "LiftingWavelet.h"
#include "Quantizer.h"
#include "ByteShuffle.h"
#include "StreamPackager.h"
#include <commdlg.h>
#include <string>
#include <vector>

namespace Surfels
{
    class PreprocessApp : public CAULDRON_DX12::FrameworkWindows
    {
    public:
        explicit PreprocessApp(LPCSTR name);

        void OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight) override;
        void OnCreate() override;
        void OnDestroy() override;
        void OnRender() override;
        bool OnEvent(MSG msg) override;
        void OnResize(bool resizeRender) override;
        void OnUpdateDisplay() override;

        bool LoadFile(const std::string& filepath);
        bool LoadPLYFile(const std::string& filepath);
        bool LoadSPLATFile(const std::string& filepath);
        void GenerateSyntheticScene(uint32_t count = 300000);
        void ProcessAndExport(const std::string& outputPath);
        void CloseDataset();

    private:
        enum class PendingAction
        {
            None,
            OpenFile,
            GenerateBenchmark,
            ExportStream,
            ExportPLY,
            ExportSPLAT,
            CloseDataset,
            ExitApp
        };

        void BuildUI();
        void ExecutePendingAction();
        void UpdateCamera(const ImGuiIO& io);
        void RecomputeWaveletHierarchy();
        void UpdatePreviewSurfels();

        std::string OpenFileDialog(const char* filter);
        std::string SaveFileDialog(const char* filter, const char* defaultExt);

        PreprocessRenderer*       m_pRenderer = nullptr;
        PreprocessRenderer::State m_state;
        PendingAction             m_pendingAction = PendingAction::None;

        // Set once a GPU device-removed/suspended HRESULT is caught. Once true, every
        // frame stops attempting to render/Present entirely (see OnRender()) instead
        // of retrying and throwing the same ThrowIfFailed exception forever -- that
        // repeated throw is otherwise indistinguishable from a real hang if a debugger
        // is set to break on every C++ exception.
        bool m_deviceLost = false;

        // Current loaded model data
        std::string               m_loadedFilePath = "No dataset loaded";
        std::vector<SurfelVertex> m_rawSurfels;
        std::vector<ChunkData>    m_chunks;
        std::vector<MeshletChunkGPU> m_meshletChunks;
        WaveletDecompositionResult m_waveletResult;

        // Packed preview surfels for currently selected preview LOD
        std::vector<PackedSurfelGPU> m_previewSurfels;

        // Bounding box & stats
        XMFLOAT3 m_aabbMin = { 0, 0, 0 };
        XMFLOAT3 m_aabbMax = { 0, 0, 0 };
        XMFLOAT3 m_center  = { 0, 0, 0 };
        XMFLOAT3 m_extents = { 0, 0, 0 };
        float    m_rawFileSizeMB = 0.0f;
        float    m_compressedSizeMB = 0.0f;
        float    m_compressionRatio = 1.0f;
        float    m_deadbandZeroPercent = 0.0f;

        // GUI Options & Parameters
        bool  m_enableWavelet      = true; // Checkbox: "Wavelet Transform" (Enabled by default)
        bool  m_enableQuantization = true; // Checkbox: "Apply Quantization" (Enabled by default)

        int   m_selectedPreviewLOD  = 0;
        int   m_maxPreviewLODs      = 4;
        float m_chunkSize           = 16.0f;
        int   m_maxLODLevels        = 4;
        float m_deadbandThresholdMM = 3.0f; // mm
        char  m_outputPathBuf[256]  = "scene";
        char  m_inputPathBuf[512]   = "";
        std::string m_statusMessage = "Ready. Load a PLY file or generate a synthetic benchmark.";
        bool  m_statusIsSuccess     = true;
        bool  m_showAboutDialog     = false;

        float m_yaw      = 0.6f;
        float m_pitch    = 0.35f;
        float m_distance = 25.0f;
        XMFLOAT3 m_target = { 0.0f, 0.0f, 0.0f };

        bool  m_autoLOD              = true;  // Distance-adaptive dynamic LOD selection
        bool  m_autoRotate           = true;
        bool  m_gpuRadixSort         = true;  // Checkbox: "GPU Radix Sort" under Accelerators (Enabled by default)
        bool  m_useChunkedPipeline   = true;  // Micro-chunked meshlet pipeline with AS culling
        struct HeatmapClusterCube
        {
            XMFLOAT3 aabbMin;
            XMFLOAT3 aabbMax;
            XMFLOAT3 center;
            float    boundingRadius;
            uint32_t pointCount;
            float    volume;
            float    density;     // pointCount / volume
            float    normDensity; // 0.0 to 1.0 (heatmap parameter t)
        };

        std::vector<HeatmapClusterCube> m_heatmapClusterCubes;
        bool  m_showClusterHeatmap   = true;   // Visualizes cluster cubes with heatmap point density fill
        int   m_targetClusterCubes   = 4096;   // Target cluster cubes (e.g. 512, 1024, 4096, 16384)
        float m_heatmapOpacity       = 0.20f;  // 20% alpha tint opacity for heatmap faces
        float m_wireframeOpacity     = 0.70f;  // 70% opacity wireframe outlines
        bool  m_showHeatmapWireframe = true;   // Draw wireframe outlines around cluster cubes
        int   m_heatmapColorScheme   = 0;      // 0 = Turbo, 1 = Viridis, 2 = Plasma
        bool  m_showOctreeVisualizer = false;  // Show streaming octree chunks (amber boxes)
        bool  m_showCulledChunks     = true;   // Visualizes frustum-culled chunks in darker shade
        bool  m_showGlobalBounds     = false;
        bool  m_cascadeLOD           = true;
        bool  m_showLODTint          = false;
        std::vector<SurfelVertex> m_previewLODSurfels;
        void  RebuildHeatmapClusterCubes();
        void  DrawOctreeVisualizer();
    };
}
