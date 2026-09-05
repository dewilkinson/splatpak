// PreprocessApp.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The SplatLab app shell: owns the whole offline preprocessing pipeline (load ->
// octree chunk -> wavelet decompose -> quantize -> export) plus the runtime streaming
// simulation and every ImGui panel. PreprocessRenderer handles the actual GPU work; this
// class is everything else -- UI, file I/O, and the streaming/decay/silhouette simulation.

#pragma once
#include "../../libs/bluesec-codec/OcclusionVolume.h"
#include "../../src/DX12/stdafx.h"
#include "PreprocessRenderer.h"
#include "SyntheticGenerator.h"
#include "PLYLoader.h"
#include "SPLATLoader.h"
#include "SpatialOctree.h"
#include "../../libs/bluesec-codec/LiftingWavelet.h"
#include "Quantizer.h"
#include "../../libs/bluesec-codec/ByteShuffle.h"
#include "StreamPackager.h"
#include <commdlg.h>
#include <string>
#include <vector>

namespace Surfels
{
    void LogTransitionTrace(const char* fmt, ...);
    void LogD3D12Messages();

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

        void  LoadConfigFile();
        std::vector<std::string> ResolveRelativeCandidates(const std::string& relativePath) const; // cwd- and exe-relative places to look for a repo file
        std::string GetProjectRootFolder() const;   // The repo root (exe lives in <root>/bin), used as the default Open/Save folder
        std::string GetDialogDefaultFolder() const; // m_lastDialogFolder if it still exists on disk, else GetProjectRootFolder()
        void RememberDialogFolder(const std::string& filePath); // Called after a successful Open/Save; persists the folder to config.json if it changed
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
        void ReportDeviceLostAndExit(const std::string& message);

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
        uint64_t m_sourceFileBytes = 0;     // Size of the original .ply/.splat on disk; travels inside the .sflw (v5 header) so the ratio survives a reload
        float    m_rawFileSizeMB = 0.0f;
        float    m_compressedSizeMB = 0.0f;
        float    m_compressionRatio = 1.0f;
        float    m_deadbandZeroPercent = 0.0f;

        // GUI Options & Parameters
        bool  m_enableWavelet      = true; // Checkbox: "Wavelet Transform" (Enabled by default)
        bool  m_enableQuantization = true; // Checkbox: "Apply Quantization" (Enabled by default)

        int   m_activeTab               = 0;   // 0 = Preprocessor Studio, 1 = Stream Renderer
        bool  m_showPreprocessorPane    = true;// Toggle preprocessor pane visibility to reduce clutter in viewer mode
        bool  m_pipelineNeedsUpdate     = false;// A chunking/wavelet parameter changed; the pipeline re-runs as soon as the slider is released
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
        std::string m_benchmarkDatasetPath = "cthulu.ply"; // Secondary fallback only -- the built-in
                                                            // assets/cthulu/ copy (see GenerateSyntheticScene)
                                                            // is always tried first.

        // Dataset auto-loaded on launch when no file is passed on the command line (see OnCreate()).
        // Configurable via config.json/surfels_config.ini ("startup_dataset") so a different default
        // can be swapped in without recompiling. Tried as-is and at a few relative CWD depths.
        std::string m_startupDatasetPath = "assets/cthulu/cthulu.sflw";

        // Interior Occlusion Volume: solid depth-writing cubes baked at preprocessing time so far-side
        // surfels can't show through gaps in the near side. Optional and disabled by default.
        bool  m_generateOcclusionVolume   = true;  // Preprocessor: bake a volume for this dataset on export (on by default)
        float m_occlusionShaveBiasCells   = 0.0f;  // From config.json "occlusion_shave_bias": extra cells added to the unconditional poke-through cull band (see OcclusionVolume::BuildGrid)
        std::string m_lastDialogFolder;             // From config.json "last_dialog_folder": Open/Save dialogs start here instead of the project root once the user has browsed elsewhere
        float m_occlusionHueShift         = 0.0f;  // Preprocessor: baked colour grade of the volume -- hue rotation in degrees (-180..180)
        float m_occlusionSaturation       = 1.0f;  // Preprocessor: baked colour grade -- saturation multiplier (0 = greyscale, 1 = as sampled)
        float m_occlusionBrightness       = 1.0f;  // Preprocessor: baked colour grade -- value/brightness multiplier (1 = as sampled)
        XMFLOAT3 GradeOcclusionColor(const XMFLOAT3& rgb) const; // Applies the three sliders above

        // Cached voxelization of the current raw points, shared by every shave value so dragging the
        // shave/colour sliders only re-runs the (cheap) erosion + octree stage. Invalidated by
        // RecomputeWaveletHierarchy; see OcclusionVolume.h.
        OcclusionVolume::Grid m_occlusionGrid;
        void  BuildOcclusionGrid();
        uint32_t m_occlusionVoxelsVersion = 0;   // Incremented whenever m_occlusionVoxels is rebuilt or cleared (renderer re-upload trigger)
        uint32_t m_occlusionSafetyNetVersion = UINT32_MAX; // m_occlusionVoxelsVersion right after the Surfel Generator tab's fallback bake last fired (see BuildUI): equal = already tried for this volume state
        std::vector<OcclusionVoxelGPU> m_occlusionVoxels; // Baked result -- from BuildOcclusionVolume() or loaded from an .sflw's package
        OcclusionMipTable m_occlusionMips;       // Mip layout of m_occlusionVoxels (see OcclusionMipTable): from Bake, or the loaded package's header
        int   m_occlusionMipOverride = -1;       // Viewer: -1 = renderer picks the mip from projected cell size; 0.. = force that mip (debug / side-by-side compare)
        bool  m_enableOcclusionCulling    = true;  // Viewer: "Show Occlusion Volume" -- draw the volume and depth-test splats against it (on by default)
        bool  m_showOcclusionVolumeOnly   = false; // Viewer: debug view -- render only the occluder geometry
        void  BuildOcclusionVolume();
        void  RefreshOcclusionVolume(); // Rebuild (or clear) from the current checkbox/sliders and make it visible

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
        float m_autoLODCooldownTimer = 0.0f;  // Blocks auto-LOD from advancing another level until the in-flight dither transition has had time to settle
        bool  m_autoRotate           = false; // Disabled by default
        bool  m_gpuRadixSort         = true;  // Checkbox: "GPU Radix Sort" under Accelerators (Enabled by default)
        bool  m_enableMortonOrder    = true;  // Checkbox: "Morton Spatial Curve Ordering" under Accelerators
        bool  m_enableConeCulling    = true;  // Checkbox: "Meshlet Backface Cone Culling" in Task Shader (mainAS)
        bool  m_useChunkedPipeline   = true;  // Micro-chunked meshlet pipeline with AS culling
        bool  m_useCopyQueue         = true;  // Dedicated DX12 copy queue for async PCIe uploads (on by default; off = direct-queue uploads, the fallback for cross-queue sync problems)
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
        float m_heatmapOpacity       = 0.02f;  // Base alpha tint opacity for heatmap faces [0.0f, 1.0f]
        float m_wireframeOpacity     = 0.08f;  // Base opacity wireframe outlines [0.0f, 1.0f]
        float m_hotspotOpacityScale  = 2.0f;   // Opacity scale multiplier for dense/hot cubes
        bool  m_showHeatmapWireframe = false;  // Unchecked by default
        int   m_heatmapColorScheme   = 0;      // 0 = Turbo, 1 = Viridis, 2 = Plasma
        bool  m_showOctreeVisualizer = false;  // Unchecked by default
        bool  m_showCulledChunks     = false;  // Unchecked by default
        bool  m_showGlobalBounds     = false;  // Unchecked by default
        bool  m_cascadeLOD           = false;
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
            float    streamWaveTimer = 0.0f;      // Active chunk streaming lavender wavefront timer (3.0s -> 0.0s)
            float    silhouetteHysteresisTimer = 0.0f; // Hysteresis hold time (seconds) to eliminate refinement/demotion thrashing
            uint32_t globalSurfelOffset = 0;      // Zero-copy offset into m_unifiedPackedSurfels / m_unifiedRawSurfels
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
        float  m_ditherTransitionDurationSec= 0.75f;  // Transition dissolve duration in seconds (slider 0.05..5 s)
        bool   m_enableStreamingSimulation  = true;  // Hierarchical streaming simulation & LOD refinement
        bool   m_unthrottledBandwidth       = true;  // Full uncapped bandwidth (removes throttle cap) -- the default; pick a network profile to throttle
        bool   m_prioritizeFrustumAndProximity = true; // Stream view frustum & close proximity chunks first
        float  m_bandwidthThrottleMBps      = 10.0f;  // Simulated bandwidth in MB/s
        float  m_ringBufferCapacityMB       = 512.0f; // GPU Ring Buffer capacity limit in MB. Defaults to the slider's ceiling (see MaxRingBufferMB) whenever a dataset's stream is built
        float  MaxRingBufferMB() const;               // Ring buffer slider ceiling: 512 MB, or twice the loaded dataset's total stream size, whichever is larger
        bool   m_enableStreamDecay          = false;  // Toggle cache decay on/off
        float  m_streamDecayRate            = 5.00f;  // Decay rate (0.0 = no decay .. 10.0 = max) for memory reclamation.
                                                        // At 10.0 a full drain (everything but the two pinned coarsest
                                                        // levels) completes in kDecayFullDrainSecondsAtMaxRate on EVERY
                                                        // bandwidth setting: the drain budget is a multiple of the
                                                        // evictable byte total, not of the bandwidth, so the throttle
                                                        // cancels out. Scales linearly below max (5.0 = twice as long).
                                                        // See the decay budget calculation in UpdateStreamingSimulation.
        static constexpr float kMaxDecayRate                    = 10.0f; // Slider maximum
        static constexpr float kDecayFullDrainSecondsAtMaxRate  = 3.0f;  // Full-drain time at rate 10, independent of bandwidth
        float  DecayFullDrainSeconds() const;                            // Implied full-drain time for the current slider value (0 when decay is off)
        bool   m_isStreamingPaused          = false;  // Pause/Resume packet streaming
        float  m_simulatedBytesDelivered    = 0.0f;   // Transferred bytes accumulator
        float  m_totalStreamBytes           = 0.0f;   // Total model transfer size
        float  m_streamRefinementProgress   = 1.0f;   // 0.0f to 1.0f
        size_t m_evictedSurfelCount         = 0;      // Count of earlier slots evicted from GPU Ring Buffer

        // Show Chunk Stream: Creeping Wavefront & Dissolving Alpha Wake
        bool   m_showChunkStream            = false;  // Chunk-arrival wave sweep visualizer. No UI toggle any more; left off
        float  m_chunkStreamDuration        = 3.0f;   // Duration in seconds of advancing wave crest & trailing alpha dissipation

        // Silhouette Edge Focused Reconstruction & Dilation Morphing
        bool   m_enableSilhouetteLOD0       = true;   // Refine silhouette edges using biased LOD levels (Option 2 GPU Inversion)
        int    m_silhouetteLODBias          = 2;      // Silhouette edge LOD bias (renders fine edges using Level N - 2, min value 0)
        float  m_silhouetteThreshold        = 0.40f;  // Grazing rim angle threshold
        float  m_silhouetteDepthThreshold   = 0.05f;  // GPU depth step threshold for interior occlusion edges
        bool   m_silhouetteExteriorOnly     = true;   // 1 = only outer perimeter against background, 0 = include interior occlusion
        float  m_dilationMorphAmount        = 0.0f;   // Geometric dilation morph factor during edge transitions
        bool   m_highlightSilhouetteChunks  = false;  // Visualizer toggle for edge chunks (lavender)
        bool   m_showOnlyLockedChunks       = false;  // Isolate dynamic workload: show ONLY locked chunks (transition or edge)
        bool   m_freezeRenderingAndMemory   = false;  // Freeze streaming simulation, memory management, and edge updates to remove flickering when paused

        // Temporal Anti-Aliasing (TAA) & Dither Transition Resolver
        bool   m_enableTemporalFiltering    = true;   // Enable Temporal Accumulation & Dither Resolver (on by default)
        float  m_temporalBlendWeight        = 0.15f;  // History blend weight (0.05 = maximum smoothness, 0.50 = responsive)
        bool   m_enableSubpixelJitter       = true;   // 8-phase Halton(2,3) sub-pixel camera jitter
        bool   m_enableVarianceClamping     = true;   // 3x3 YCoCg neighborhood variance color box clamping (anti-ghosting)

        std::vector<std::vector<StreamChunk>> m_lodStreamChunks; // Chunks grouped by LOD level for O(1) equalizer
        std::vector<StreamChunk*>             m_allStreamChunkPtrs; // Flat list of pointers for priority sorting
        std::vector<StreamChunk*>             m_rendererSourceChunks; // Source chunk pointers corresponding to m_rendererMeshletChunks
        std::vector<PackedSurfelGPU>          m_unifiedPackedSurfels; // Global zero-copy packed surfel buffer
        std::vector<SurfelVertex>            m_unifiedRawSurfels;    // Global zero-copy raw surfel buffer
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
        uint32_t m_lastActiveTransitions = 0; // Simultaneously mid-transition chunk count from the previous frame; forces instant transition completion under overload (see UpdateStreamingSimulation)

        std::string m_integrityReport;
        void   RunMemoryAndLODIntegrityTest();
        float  m_morphTestDebounceTimer = 0.0f;

        void   InitStreamingSimulation();
        void   UpdateStreamingSimulation(double dtSeconds);
        void   ResetStreamingSimulation();
        void   ClearResidentStream();
        void   TriggerSilhouetteEdgeMorphTest();

        void  DrawLODResidencyEqualizer();
        void  DrawOcclusionVolumeControls();
        void  DrawRenderModeBanner(float leftPanelWidth, float rightPanelWidth); // "[X Mode Enabled]" overlay for every active toggle that alters the rendered model (see BuildUI)
        void  RebuildHeatmapClusterCubes();
        void  DrawOctreeVisualizer();
    };
}
