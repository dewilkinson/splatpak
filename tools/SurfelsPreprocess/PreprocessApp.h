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
        bool LoadSFLWFile(const std::string& filepath);
        void GenerateSyntheticScene(uint32_t count = 300000);
        void ProcessAndExport(const std::string& outputPath);
        void CloseDataset();

        void LoadConfigFile();
        void SaveConfigFile();

    private:
        enum class PendingAction
        {
            None,
            OpenFile,
            OpenCompressedFile,
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

        std::string OpenFileDialog(const char* filter, const char* title = "Open File", const char* defaultExt = nullptr);
        std::string SaveFileDialog(const char* filter, const char* defaultExt = "sflw", const char* title = "Save File");

        PreprocessRenderer*       m_pRenderer = nullptr;
        PreprocessRenderer::State m_state;
        PendingAction             m_pendingAction = PendingAction::None;

        // Set once a GPU device-removed/suspended HRESULT is caught. Once true, every
        // frame stops attempting to render/Present entirely (see OnRender()) instead
        // of retrying and throwing the same ThrowIfFailed exception forever -- that
        // repeated throw is otherwise indistinguishable from a real hang if a debugger
        // is set to break on every C++ exception.
        bool m_deviceLost = false;

        // Preprocessor Working Structures (Source Model -> Wavelet Hierarchy)
        std::string               m_loadedFilePath = "No dataset loaded";
        std::vector<SurfelVertex> m_rawSurfels;
        std::vector<ChunkData>    m_chunks;
        WaveletDecompositionResult m_waveletResult;

        // Renderer Active Streaming Buffers (Decoupled Independent Copy for GPU & Viewport)
        bool                      m_isLoadedFromSFLW = false;
        std::string               m_rendererSourceDescription = "No model active";
        StreamPackager::SFLWPackageData m_loadedPackage;
        std::vector<PackedSurfelGPU>    m_rendererSurfels;
        std::vector<SurfelVertex>       m_rendererRawSurfels;
        std::vector<MeshletChunkGPU>    m_rendererMeshletChunks;
        std::vector<ChunkData>          m_rendererOctreeChunks;

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

        int   m_activeTab               = 0;   // 0 = Preprocessor Studio, 1 = Stream Renderer
        bool  m_showPreprocessorPane    = true;// Toggle preprocessor pane visibility to reduce clutter in viewer mode
        bool  m_pipelineNeedsUpdate     = false;// True when sliders/parameters change
        bool  m_isPipelineProcessing    = false;// True while pipeline execution is in progress
        bool  m_packageReadyToSave      = false;// True when pipeline processing has completed and is ready for export
        int   m_selectedPreviewLOD      = 0;
        int   m_maxPreviewLODs          = 4;
        float m_chunkSize               = 16.0f;
        int   m_maxLODLevels        = 4;
        float m_deadbandThresholdMM = 3.0f; // mm
        char  m_outputPathBuf[256]  = "scene";
        char  m_inputPathBuf[512]   = "";
        std::string m_statusMessage = "Ready. Load a PLY file or generate a synthetic benchmark.";
        bool  m_statusIsSuccess     = true;
        bool  m_showAboutDialog     = false;
        std::string m_benchmarkDatasetPath = "data/venus.ply";

        float m_yaw      = 0.6f;
        float m_pitch    = 0.35f;
        float m_distance = 25.0f;
        XMFLOAT3 m_target = { 0.0f, 0.0f, 0.0f };

        bool     m_detachCamera     = false; // Detach/freeze culling camera from viewing camera
        float    m_detachedYaw      = 0.6f;
        float    m_detachedPitch    = 0.35f;
        float    m_detachedDistance = 25.0f;
        XMFLOAT3 m_detachedTarget   = { 0.0f, 0.0f, 0.0f };

        bool  m_autoLOD              = true;  // Distance-adaptive dynamic LOD selection
        bool  m_autoRotate           = false; // Disabled by default
        bool  m_gpuRadixSort         = true;  // Checkbox: "GPU Radix Sort" under Accelerators (Enabled by default)
        bool  m_enableMortonOrder    = true;  // Checkbox: "Morton Spatial Curve Ordering" under Accelerators
        bool  m_enableConeCulling    = true;  // Checkbox: "Meshlet Backface Cone Culling" in Task Shader (mainAS)
        bool  m_useChunkedPipeline   = true;  // Micro-chunked meshlet pipeline with AS culling
        bool  m_useCopyQueue         = true;  // Dedicated DX12 Hardware DMA Copy Queue for asynchronous PCIe transfers
        bool  m_vsync                = false; // Uncapped framerate by default to expose true compute/render timings
        float m_uiScale              = 1.0f;  // Dynamic UI and font scaling factor (0.70x to 2.00x)
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
            XMFLOAT3 avgNormal;
        };

        std::vector<HeatmapClusterCube> m_heatmapClusterCubes;
        bool  m_devMode              = false;  // Loaded from config file (surfels_config.ini)
        bool  m_showClusterHeatmap   = false;  // Unchecked by default
        int   m_targetClusterCubes   = 4096;   // Target cluster cubes (e.g. 512, 1024, 4096, 16384)
        float m_heatmapOpacity       = 0.25f;  // Base alpha tint opacity for heatmap faces [0.05f, 1.0f]
        float m_wireframeOpacity     = 0.40f;  // Base opacity wireframe outlines [0.05f, 1.0f]
        float m_hotspotOpacityScale  = 2.0f;   // Opacity scale multiplier for dense/hot cubes
        bool  m_showHeatmapWireframe = false;  // Unchecked by default
        int   m_heatmapColorScheme   = 0;      // 0 = Turbo, 1 = Viridis, 2 = Plasma
        bool  m_showOctreeVisualizer = false;  // Unchecked by default
        bool  m_showCulledChunks     = false;  // Unchecked by default
        bool  m_showGlobalBounds     = false;  // Unchecked by default
        bool  m_cascadeLOD           = true;
        bool  m_showLODTint          = false;
        std::vector<SurfelVertex> m_previewLODSurfels;

        // Pre-cached resident GPU/CPU LOD buffers for zero-hitch instantaneous transitions
        struct ResidentLOD
        {
            std::vector<SurfelVertex> rawSurfels;
            std::vector<PackedSurfelGPU> packedSurfels;
            std::vector<MeshletChunkGPU> meshletChunks;
        };
        std::vector<ResidentLOD> m_residentLODs;
        void PrecacheResidentLODs();

        // Progressive Network Streaming & Bandwidth Throttle Simulator
        struct StreamChunk
        {
            int      lodLevel = 0;
            size_t   chunkIndex = 0;
            XMFLOAT3 center = { 0, 0, 0 };
            float    radius = 0.0f;
            XMFLOAT3 avgNormal = { 0, 1, 0 };      // Representative surface normal for silhouette edge testing
            float    normalSpread = 0.0f;          // Normal angular variation
            std::vector<SurfelVertex>   rawSurfels;
            std::vector<PackedSurfelGPU> packedSurfels;
            size_t   byteSize = 0;
            float    currentPriority = 0.0f;
            bool     isRequested = false;
            bool     isDelivered = false;
            bool     isResident = false;
            bool     isEvictionPending = false;    // Marked for eviction: waiting for parent demotion transition to complete
            bool     isLockedInTransition = false; // Locked against eviction while transition is running in either direction
            bool     isSilhouette = false;         // Active in-view silhouette edge chunk (locked against eviction)
            float    transitionProgress = 0.0f;   // 0.0 (Parent Level N Solid) <-> 1.0 (Children Level N-1 Solid)
            float    loadingHighlightTimer = 0.0f; // Countdown timer to highlight actively streaming/loading cluster in lavender
            XMFLOAT3 aabbMin = { 0, 0, 0 };
            XMFLOAT3 aabbMax = { 0, 0, 0 };
        };

        struct ChunkRequest
        {
            int    lodLevel = 0;
            size_t chunkIndex = 0;
            float  priority = 0.0f;
        };

        std::vector<ChunkRequest> m_demandRequestQueue;
        size_t                    m_demandRequestHead = 0; // O(1) queue consumption without array shifts
        void RequestChunk(int lodLevel, size_t chunkIndex, float priority);

        enum class StreamingPolicy
        {
            Conservative = 0, // Pulls only visible chunks + local neighbor buffer; stops when view is satisfied
            Greedy       = 1  // Refines visible chunks first, then continues pre-fetching remaining background chunks
        };

        StreamingPolicy m_streamingPolicy           = StreamingPolicy::Greedy; // Greedy (default) or Conservative
        float  m_conservativeNeighborBufferMargin   = 1.35f;  // Frustum margin for pre-fetching local neighbors in conservative mode
        bool   m_enableDitheredTransitions  = true;   // Stochastic screen-space Bayer dithering for smooth LOD transitions
        float  m_ditherTransitionDurationSec= 0.20f;  // Transition dissolve duration in seconds
        bool   m_enableStreamingSimulation  = false; // Simulated network connection
        bool   m_unthrottledBandwidth       = false; // Full uncapped bandwidth (removes throttle cap)
        bool   m_prioritizeFrustumAndProximity = true; // Stream view frustum & close proximity chunks first
        float  m_bandwidthThrottleMBps      = 10.0f;  // Simulated bandwidth in MB/s
        float  m_ringBufferCapacityMB       = 64.0f;  // GPU Ring Buffer capacity limit in MB
        bool   m_enableStreamDecay          = false;  // Toggle cache decay on/off
        float  m_streamDecayRate            = 0.50f;  // Decay rate (0.0 to 1.0) for memory reclamation
        bool   m_isStreamingPaused          = false;  // Pause/Resume packet streaming
        float  m_simulatedBytesDelivered    = 0.0f;   // Transferred bytes accumulator
        float  m_totalStreamBytes           = 0.0f;   // Total model transfer size
        float  m_streamRefinementProgress   = 1.0f;   // 0.0f to 1.0f
        size_t m_evictedSurfelCount         = 0;      // Count of earlier slots evicted from GPU Ring Buffer

        // Silhouette Edge Focused Reconstruction & Dilation Morphing
        bool   m_enableSilhouetteLOD0       = true;   // Refine silhouette edges using biased LOD levels
        int    m_silhouetteLODBias          = 2;      // Silhouette edge LOD bias (renders fine edges using Level N - 2, min value 0)
        float  m_silhouetteThreshold        = 0.25f;  // 2D screen-space grazing rim angle threshold (|N . V| <= threshold)
        float  m_dilationMorphAmount        = 0.40f;  // Geometric dilation morph factor during edge transitions
        bool   m_highlightSilhouetteChunks  = false;  // Highlight silhouette chunks in lavender semi-transparent effect
        bool   m_highlightLoadingClusters   = true;   // Highlight cluster groups currently being loaded in lavender

        std::vector<std::vector<StreamChunk>> m_lodStreamChunks; // Chunks grouped by LOD level for O(1) equalizer
        std::vector<StreamChunk*>             m_allStreamChunkPtrs; // Flat list of pointers for priority sorting
        std::vector<size_t>                   m_lodTotalSurfels;   // Total surfels per LOD level
        std::vector<size_t>                   m_lodResidentSurfels;// Resident surfels per LOD level

        // Throttling for frustum priority re-sorting & UI indicator stabilization
        XMFLOAT3 m_lastStreamCamPos = { 1e9f, 1e9f, 1e9f };
        float    m_lastStreamYaw = 1e9f;
        float    m_lastStreamPitch = 1e9f;
        float    m_priorityUpdateTimer = 0.0f;
        float    m_equalizerUpdateTimer = 0.0f;
        std::vector<float>    m_smoothedLodResidentPct;
        std::vector<uint32_t> m_smoothedLodResidentBlocks;
        bool     m_streamStateDirty = true;

        void   InitStreamingSimulation();
        void   UpdateStreamingSimulation(double dtSeconds);
        void   ResetStreamingSimulation();
        void   ClearResidentStream();
        void   TriggerSilhouetteEdgeMorphTest();

        void  DrawLODResidencyEqualizer();
        void  RebuildHeatmapClusterCubes();
        void  DrawOctreeVisualizer();
    };
}
