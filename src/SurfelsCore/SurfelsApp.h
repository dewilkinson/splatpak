// SurfelsApp.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The shared app shell behind both executables. In Studio mode it is SplatLab: the whole
// offline preprocessing pipeline (load -> octree chunk -> wavelet decompose -> quantize ->
// export) plus the runtime streaming simulation and every ImGui panel. In Viewer mode it is
// the standalone Surfels viewer: the same class with the Surfel Generator tab, its menu items
// and its shortcuts hidden, so the viewer is SplatLab's Renderer and Streaming tabs by
// construction rather than a second copy of them. SurfelsRenderer handles the actual GPU work;
// this class is everything else -- UI, file I/O, and the streaming/decay/silhouette simulation.

#pragma once
#include "../../libs/bluesec-codec/OcclusionVolume.h"
#include "../../libs/bluesec-codec/DetailHeatmap.h"
#include "../../libs/bluesec-codec/StreamOrder.h"
#include "../../libs/bluesec-codec/CodecBuild.h"
#include "../../libs/bluesec-codec/SplatCodec.h"
#include "stdafx.h"
#include "SurfelsRenderer.h"
#include "SyntheticGenerator.h"
#include "PLYLoader.h"
#include "SPLATLoader.h"
#include "SpatialOctree.h"
#include "../../libs/bluesec-codec/LiftingWavelet.h"
#include "Quantizer.h"
#include "../../libs/bluesec-codec/ByteShuffle.h"
#include "StreamPackager.h"
#include "ImageCompare.h"
#include <commdlg.h>
#include <string>
#include <vector>

namespace Surfels
{
    void LogTransitionTrace(const char* fmt, ...);
    void LogD3D12Messages();

    class SurfelsApp : public CAULDRON_DX12::FrameworkWindows
    {
    public:
        // Which product this instance is (see the file header). Viewer hides everything that only
        // makes sense with a raw point cloud loaded: the generator tab, Open Raw / Generate Benchmark /
        // Save Package, and Ctrl+O opens a .sflw instead of a .ply.
        enum class Mode { Studio, Viewer };

        explicit SurfelsApp(LPCSTR name, Mode mode = Mode::Studio);

        Mode        GetMode() const      { return m_mode; }
        bool        IsViewerOnly() const { return m_mode == Mode::Viewer; }
        const char* AppTitle() const     { return m_mode == Mode::Viewer ? "Surfels Viewer" : "SplatLab"; } // Dialog titles

        // Splits a Win32 command line into arguments: whitespace-separated, double quotes group.
        static std::vector<std::string> SplitCommandLine(const char* cmdLine);
        // The options both executables accept (see AppEntry.cpp for the mode switches and --help).
        static const char* CommandLineUsage();

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

        Mode                      m_mode = Mode::Studio;
        SurfelsRenderer*          m_pRenderer = nullptr;
        SurfelsRenderer::State    m_state;
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
        std::string m_startupDatasetPath = "assets/dwarves/dwarves.sflw";

        // Interior Occlusion Volume: solid depth-writing cubes baked at preprocessing time so far-side
        // surfels can't show through gaps in the near side. Optional and disabled by default.
        bool  m_generateOcclusionVolume   = true;  // Preprocessor: bake a volume for this dataset on export (on by default)
        float m_occlusionShave            = 0.0f;  // Preprocessor: baked erosion beyond poke-through removal, 0 = none .. 10 = a thin core (see BuildOcclusionVolume)
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
        void     SetDetachCamera(bool on);   // Checkbox and Ctrl+D: freezes the culling camera where the view is and steps the view aside, or returns to it
        float    m_detachedYaw      = 0.6f;
        float    m_detachedPitch    = 0.35f;
        float    m_detachedDistance = 25.0f;
        XMFLOAT3 m_detachedTarget   = { 0.0f, 0.0f, 0.0f };

        bool  m_autoLOD              = true;  // Distance-adaptive dynamic LOD selection
        float m_autoLODCooldownTimer = 0.0f;  // Blocks auto-LOD from advancing another level until the in-flight dither transition has had time to settle
        // Auto LOD rule. Screen space (default): a level is used only once its point spacing projects to
        // at most m_lodPixelSpacing pixels at the model's near side, so a coarser level is never wider-spaced
        // on screen than the threshold. Legacy: the old linear map of distance / model size (1x..4x -> all
        // levels), kept for comparison (the LOD quality test measures both). Saved in config.json.
        bool  m_lodScreenSpace       = true;
        float m_lodPixelSpacing      = 1.0f;
        float LodEffectiveDistance(float cameraDistance) const;  // Camera to the model's near side, the distance the screen-space rule projects at
        float LodFocalPixels() const;                            // Pixels per unit of tangent at the render height (the renderer's 45-degree vertical field of view)
        float ContinuousLodAtDistance(float cameraDistance, bool screenSpace) const; // The level the rule wants at this distance, fractional
        float LodSwitchDistance(int level, bool screenSpace) const;                // Camera distance at which auto LOD steps from level - 1 to level
        bool  m_autoRotate           = false; // Disabled by default
        bool  m_showControlHints     = true;  // Viewport: draw the faint rotate/zoom/pan reminder at the bottom-right (see DrawControlHints); saved in config.json
        int         m_renderPathOverride = -1;     // "render_path" from config.json or --render-path: -1 auto, else SurfelsRenderer::RenderPath to force (testing)
        std::string m_renderPathConfig = "auto";  // The config value as written back (auto / mesh / vs6 / vs5); --render-path never changes it
        bool        m_renderPathFromCommandLine = false; // --render-path was given: it wins over config.json for this run only
        ImFont* m_pHintFont          = nullptr; // Proportional font for that reminder (Segoe UI, registered in OnCreate before the atlas is built); nullptr = default font
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
        int   m_heatmapSource        = 0;      // What the cluster cubes colour by: 0 = point density, 1 = detail heatmap (the streaming order)
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
            std::vector<PackedSplatGPU> splats;   // Splat mode: the level's 20-byte Gaussian records
            std::vector<uint8_t>        splatSH;  // Splat mode: the level's spherical-harmonics stream
            std::vector<MeshletChunkGPU> meshletChunks;
        };
        std::vector<ResidentLOD> m_residentLODs;
        XMFLOAT3 m_lodSortFrameMin = { 0, 0, 0 }; // The Morton frame every level is sorted in (PrecacheResidentLODs): the model's bounds
        XMFLOAT3 m_lodSortFrameMax = { 0, 0, 0 };
        void PrecacheResidentLODs();

        // Splat mode: the imported cloud's full Gaussians (SurfelVertex::sourceIndex refers here), the
        // bake settings, and the encode parameters every level shares (see SplatCodec).
        std::vector<SplatAttributes> m_splatAttributes;
        bool     m_sourceHasSplatAttributes = false; // The loaded source carried scales/rotations/opacities (a 3DGS .ply or .splat)
        bool     m_splatMode                = false; // Bake and draw as 3D Gaussians (surfel format 1) instead of 8-byte surfels
        int      m_splatSHQuality           = 2;     // Spherical-harmonics degree kept, minus one: 0 = low (degree 1), 1 = medium (degree 2), 2 = high (degree 3, the default); a loaded package sets it to its own degree
        int      m_splatBlendSpace          = 0;     // 0 = display-space blending (as the reference viewer), 1 = linear
        // Draw Splats as Surfels (Renderer tab, saved in config.json as splat_draw_as_surfels): a splat package is
        // drawn through the surfel pipeline as opaque, depth-tested discs (render mode 4; Auto Splat Size, Coverage,
        // Splat Radius Scale and Surfel Orientation apply, the dither dissolves between two surfaces). Off (the
        // default): the sorted Gaussian pipeline (render mode 3) with Splat Blending, Gaussians at level 0 and discs above.
        bool     m_splatDrawAsSurfels       = false;
        uint32_t SplatRenderMode() const { return m_splatDrawAsSurfels ? 4u : 3u; }
        SplatEncodeParams m_splatParams;
        uint32_t SplatSHDegree() const { return (uint32_t)std::clamp(m_splatSHQuality, 0, 2) + 1u; }
        // The bake keeps the SH Quality degree, but never more bands than the source carries: extra bands would
        // only store zeros (a source without harmonics, or a package baked at a lower degree).
        uint32_t m_sourceSHDegree = 3;               // Highest band the source's Gaussians carry (a package: its own degree)
        uint32_t BakeSHDegree() const { return std::min(SplatSHDegree(), m_sourceSHDegree); }
        void     DetectSourceSHDegree();            // Sets m_sourceSHDegree from m_splatAttributes (highest band with a non-zero coefficient)
        size_t   BytesPerRenderPoint() const { return m_splatMode ? (sizeof(PackedSplatGPU) + SplatCodec::SHRecordBytes(m_splatParams.shDegree)) : (m_enableQuantization ? sizeof(PackedSurfelGPU) : sizeof(SurfelVertex)); }
        void     EncodeSplatLevel(const std::vector<SurfelVertex>& surfels, uint32_t level, std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH) const;
        void     ApplySplatRenderState();

        // Progressive Network Streaming & Bandwidth Throttle Simulator
        struct StreamChunk
        {
            int      lodLevel = 0;
            size_t   chunkIndex = 0;
            XMFLOAT3 center = { 0, 0, 0 };
            float    radius = 0.0f;
            XMFLOAT3 avgNormal = { 0, 1, 0 };      // Representative surface normal for silhouette edge testing
            float    normalSpread = 0.0f;          // Normal angular variation
            float    detailScore = 0.0f;           // 0..1 from the package's detail grid over this block's bounds (DetailGrid::SampleBox); an input to the streaming order
            uint8_t  octahedronFace = 0;           // Face (0..7) of the bounding octahedron this block was assigned by StreamOrder::Build
            std::vector<SurfelVertex>   rawSurfels;
            std::vector<PackedSurfelGPU> packedSurfels;
            size_t   byteSize = 0;
            float    currentPriority = 0.0f;
            bool     isRequested = false;
            bool     isEdgeQueued = false;         // Has an entry in one of the per-face edge queues (m_faceEdgeQueue); cleared when that entry is consumed
            uint32_t lastRequestFrame = 0;         // m_streamFrame of the traversal's most recent request for this block; an edge-queue entry not re-requested for a while is stale and dropped
            uint32_t deliveryCount = 0;            // Deliveries since the last streaming reset (diagnostic: > 1 means the block was evicted and streamed again)
            bool     isDelivered = false;
            bool     isResident = false;
            bool     isEvictionPending = false;    // Marked for eviction: waiting for parent demotion transition to complete
            bool     isLockedInTransition = false; // Locked against eviction while transition is running in either direction
            bool     hasRefined = false;           // Has refined into its children (CASE 2) since they last arrived. Only then does dropping back to this node (CASE 1) evict them; children that came in ahead of need and were never shown stay resident for when the view gets closer
            bool     refinedBySilhouette = false;  // The refinement into its children was edge-driven (this node sits at or below the target level). When the flag drops and it demotes, the children are kept resident: evicting them only made the face lists re-stream them for the next flag, which flashed the visible faces while spinning
            bool     isSilhouette = false;         // Active in-view silhouette edge chunk (locked against eviction)
            float    transitionProgress = 0.0f;   // 0.0 (Parent Level N Solid) <-> 1.0 (Children Level N-1 Solid)
            float    streamWaveTimer = 0.0f;      // Arrival glow (Refinement Visualizer) countdown, m_chunkStreamDuration -> 0; started by the chunk's first draw after delivery (see streamWavePending)
            bool     streamWavePending = false;   // Delivered but not yet drawn: the arrival glow is armed and starts on the first draw, so a chunk whose refinement start the pacing defers (levels 1 and 0 at high bandwidth) still glows when it appears instead of expiring unseen
            uint32_t deliverySeq = 0;             // Order of delivery by the stream (1 = first delivered this session; 0 = resident at load). The refinement pacing admits waiting groups in this order so the model grows back in the scheduler's order, not the traversal's
            float    silhouetteHysteresisTimer = 0.0f; // Hysteresis hold time (seconds) to eliminate refinement/demotion thrashing
            bool     renderedLastFrame = false;    // Visited by the previous frame's traversal: rendered, or refined in place of by its children. Only such "active" chunks may keep an edge flag; anything the traversal never reached (a level outside the active range) is cleared before the next traversal. Clearing on "not rendered" alone made a refined edge parent lose its flag, re-render, get re-detected and refine again every other frame -- a whole-model flicker
            uint32_t globalSurfelOffset = 0;      // Zero-copy offset into m_unifiedPackedSurfels / m_unifiedRawSurfels
            XMFLOAT3 aabbMin = { 0, 0, 0 };
            XMFLOAT3 aabbMax = { 0, 0, 0 };
            size_t   childBegin = 0;              // The block's children at the next finer level: m_lodStreamChunks[lodLevel - 1][childBegin, childEnd). The finer
            size_t   parentIndex = SIZE_MAX;      // The block's parent at the next coarser level (SIZE_MAX at the coarsest level, or when the levels are not nested)
            bool     appendedLastFrame = false;
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
            Conservative = 0, // The view's own requests cover only visible chunks + a local neighbour buffer, and detail the view drops back from is evicted. The scheduler's face streams run in both policies (see StreamOrder.h)
            Greedy       = 1  // Refines visible chunks first, then continues pre-fetching remaining background chunks
        };

        StreamingPolicy m_streamingPolicy           = StreamingPolicy::Greedy; // Greedy (default) or Conservative
        float  m_conservativeNeighborBufferMargin   = 1.35f;  // Frustum margin for pre-fetching local neighbors in conservative mode
        bool   m_enableDitheredTransitions  = true;   // Stochastic screen-space Bayer dithering for smooth LOD transitions
        bool   m_demoteChunks               = false;  // Off: a node that has refined into its children stays refined when the zoom target coarsens or its edge flag drops (only Decay-marked children demote). On: the v1.2.0 handshake, children cross-fade back to the parent and are evicted
        bool   m_autoSplatSize       = true;   // Auto Splat Size: disc radius from each level's point spacing (m_lodSpacing x m_autoSplatCoverage), not the size classes
        float  m_autoSplatCoverage   = 1.2f;   // Disc radius as a multiple of the level's typical point spacing
        std::vector<float> m_lodSpacing;       // Per LOD level: median nearest-neighbour distance of its points (world units), from ComputeLodSpacing
        void   ComputeLodSpacing();           // Fills m_lodSpacing from the resident level point sets (after a load or a preprocess)
        // Splat mode, Match Level 0 Softness (Renderer tab, saved in config.json): the coarse levels' discs take
        // the radius factor derived from the level 0 Gaussians (ComputeSplatSoftness) instead of the fixed 1.5,
        // and discs of level 1 and up take an opacity floor (State::splatDiscOpacityFloor). Off: 1.5, no floor.
        bool   m_splatDiscMatchLevel0 = true;
        float  m_splatLevel0Footprint = 0.0f;  // Median in-plane footprint sqrt(s_max x s_mid) of the level 0 records, world units (0 = not a splat package)
        float  m_splatSoftnessRatio   = 0.0f;  // m_splatLevel0Footprint / m_lodSpacing[0]: how soft the level 0 Gaussians are relative to their spacing
        float  m_splatDiscMatchFactor = 1.5f;  // clamp(sqrt(8) x ratio, 1.5, 4.0): the disc radius as a multiple of the level spacing that gives the same softness
        void   ComputeSplatSoftness();        // Fills the three values above from m_residentLODs[0].splats and m_lodSpacing[0] (right after ComputeLodSpacing)
        // Frame budget for the stats panel (CPU wall clock per frame, EMA-smoothed): where the frame time
        // actually goes, as opposed to the command-recording stage costs the renderer reports.
        float  m_frameSimMs = 0.0f, m_frameTraversalMs = 0.0f, m_frameUiMs = 0.0f, m_frameRenderMs = 0.0f, m_framePresentMs = 0.0f;
        // Displayed values: averaged over a 250 ms window and updated four times a second (like the FPS
        // counter), so the readout holds still instead of jittering with every frame.
        enum { kBudgetSim = 0, kBudgetTraversal, kBudgetUi, kBudgetGpuWait, kBudgetRecord, kBudgetPresent, kBudgetCount };
        float  m_budgetAccum[kBudgetCount] = {};
        float  m_budgetShown[kBudgetCount] = {};
        float  m_budgetAccumWallMs = 0.0f;
        uint32_t m_budgetAccumFrames = 0;
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
        static constexpr float kDecayFullDrainSecondsAtMaxRate  = 10.0f; // Full-drain time at rate 10, independent of bandwidth
        // Decay is a fixed outflow in bytes per second, set only when the toggle or the slider changes
        // (or a model loads): rate 10 = the whole evictable set in kDecayFullDrainSecondsAtMaxRate at the
        // bandwidth in force at that moment (the reference bandwidth, added to the outflow so the NET
        // drain at that bandwidth takes exactly that long; Full = no inflow term). Changing the bandwidth
        // afterwards does not touch it, so the throttle becomes the knob for the net eviction rate.
        float  m_decayBytesPerSec = 0.0f;
        float  m_decayReferenceBandwidthBps = 0.0f;                     // 0 = Full (uncapped) was the reference
        void   CalibrateDecay();
        float  EvictableStreamBytes() const;                            // Everything but the two pinned coarsest levels
        float  DecayFullDrainSeconds() const;                            // Implied full-drain time for the current slider value (0 when decay is off)
        bool   m_isStreamingPaused          = false;  // Pause/Resume packet streaming
        float  m_simulatedBytesDelivered    = 0.0f;   // Transferred bytes accumulator
        float  m_totalStreamBytes           = 0.0f;   // Total model transfer size
        float  m_streamRefinementProgress   = 1.0f;   // 0.0f to 1.0f
        size_t m_evictedSurfelCount         = 0;      // Count of earlier slots evicted from GPU Ring Buffer

        // Show Chunk Stream: Creeping Wavefront & Dissolving Alpha Wake
        bool   m_showChunkStream            = true;   // Streaming arrival visualizer: newly delivered chunks glow hot orange, settle to a regular orange tint and fade out (Renderer tab: "Show Streaming Arrivals")
        float  m_chunkStreamDuration        = 1.0f;   // Seconds a delivered chunk stays tinted (the glow is the first part of that); 1 s by default
        float  m_arrivalGlowIntensity       = 0.4f;   // Refinement visualizer strength (0..2); 0.4 by default
        float  m_arrivalGlowHue             = 0.0f;   // Refinement visualizer hue rotation in degrees (-180..180, 0 = orange)
        void   DrawRefinementVisualizerControls(const char* idSuffix); // Checkbox + duration/intensity/hue sliders (Renderer tab)

        // Silhouette Edge Focused Reconstruction & Dilation Morphing
        bool   m_enableSilhouetteLOD0       = false;   // Refine silhouette edges using biased LOD levels (Option 2 GPU Inversion)
        int    m_silhouetteLODBias          = 2;      // Silhouette edge LOD bias (renders fine edges using Level N - 2, min value 0)
        float  m_silhouetteThreshold        = 0.40f;  // Grazing rim angle threshold
        float  m_silhouetteDepthThreshold   = 0.05f;  // GPU depth step threshold for interior occlusion edges
        bool   m_silhouetteExteriorOnly     = true;   // 1 = only outer perimeter against background, 0 = include interior occlusion
        float  m_dilationMorphAmount        = 0.25f;  // Geometric dilation morph factor during silhouette edge transitions: 0 = off, 1 = full (1.5 radii outward, +40% disc size mid-dissolve). 0.25 covers the dissolve gap on the rim without a visible bulge
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
        // Streaming order (libs/bluesec-codec/StreamOrder.h): which face of the bounding octahedron each
        // block belongs to, the per-face delivery lists, face visibility, and the dealing of a frame's
        // bandwidth across faces. The app owns the blocks (handle = index into m_allStreamChunkPtrs) and
        // asks the scheduler for this frame's load list; the rules live in the codec library.
        StreamOrder               m_streamOrder;
        std::vector<uint32_t>     m_scratchLoadHandles;               // This frame's load list from the scheduler (block handles)
        std::vector<StreamChunk*> m_scratchLoadList;                  // The same list as chunk pointers, in delivery order
        std::vector<size_t>       m_faceDrainBuckets[kOctahedronFaces]; // Scratch: demand-queue entries of the tier being delivered, bucketed by face
        // TIER 1 (silhouette edges and the bootstrap envelope) bypasses the capped demand queue: those requests
        // go straight into a queue per face, and the delivery simulator serves the eight queues through the
        static constexpr size_t   kMaxEdgeQueuePerFace = 8192;
        std::vector<ChunkRequest> m_faceEdgeQueue[kOctahedronFaces];
        size_t                    m_faceEdgeHead[kOctahedronFaces] = {};
        uint32_t                  m_streamFrame = 0;                   // Counts UpdateStreamingSimulation calls (request staleness)
        uint32_t                  m_edgeRefineStartsThisFrame = 0;     // Edge-driven refinements into already-resident children begun this frame (capped, see kEdgeRefineStartsPerFrame)
        // Refinement pacing (see UpdateStreamingSimulation): new cross-fades started this frame, the frame's
        // budget of starts (UINT32_MAX = unlimited), and the starts deferred to a later frame by the budget.
        uint32_t                  m_refineStartsThisFrame = 0, m_refineStartBudget = 0, m_refineStartsDeferred = 0;
        uint32_t                  m_deliverySeqCounter = 0;            // Last StreamChunk::deliverySeq handed out
        std::vector<std::pair<uint32_t, uint32_t>> m_refineDeferred;  // Groups the pacing deferred this frame: (rank = the children's latest deliverySeq, start cost in chunks); sorted at the next frame's start to set m_refineAdmitSeqLimit
        uint32_t                  m_refineAdmitSeqLimit = 0;           // This frame a deferred group may start only if its rank is at or below this (delivery order); UINT32_MAX = no ordering (budget unlimited)
        float                     m_refinePacingTraceTimer = 0.0f;     // Once-per-second trace line while starts are being deferred
        uint32_t                  m_extraDeliveries = 0;               // Diagnostic: deliveries of blocks that had already been delivered since the last reset (re-streamed after an eviction)
        uint32_t                  m_redeliveredBlocks = 0;             // Diagnostic: distinct blocks delivered more than once since the last reset
        void  ClearFaceEdgeQueues();                                // Drops every queued edge request (streaming reset)
        void  HealStaleRequestFlags();                              // Periodic safety net: clears isRequested / isEvictionPending on non-resident blocks no queue holds, so a lost request cannot leave a permanent hole
        uint32_t m_residencyEpoch = 0;                              // Bumped whenever any block stops being resident (evict, reset, decay); the scheduler restarts its cursors on a change
        void  DrawOctahedronGlyph();                                   // Streaming tab: the octahedron's upper and lower faces, visible ones lit, each with its fill progress
        DetailGrid                            m_detailGrid;         // Detail grid for the loaded model: from the package (v7+) or built from LOD 0 on load / preprocess; written into exported packages
        std::vector<StreamChunk*>             m_rendererSourceChunks; // Source chunk pointers corresponding to m_rendererMeshletChunks
        std::vector<PackedSurfelGPU>          m_unifiedPackedSurfels; // Global zero-copy packed surfel buffer
        std::vector<SurfelVertex>            m_unifiedRawSurfels;    // Global zero-copy raw surfel buffer
        std::vector<PackedSplatGPU>          m_unifiedSplats;        // Splat mode: global zero-copy Gaussian records (same offsets as the packed buffer)
        std::vector<uint8_t>                 m_unifiedSH;            // Splat mode: their spherical-harmonics stream
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

        float  m_morphTestDebounceTimer = 0.0f;

        void   InitStreamingSimulation();
        void   UpdateStreamingSimulation(double dtSeconds);
        void   ResetStreamingSimulation();
        void   ClearResidentStream();
        void   TriggerSilhouetteEdgeMorphTest();

        void  DrawLODResidencyEqualizer();
        void  DrawOcclusionVolumeControls();
        void  DrawRenderModeBanner(float leftPanelWidth, float rightPanelWidth); // "[X Mode Enabled]" overlay for every active toggle that alters the rendered model (see BuildUI)
        void  DrawControlHints(float leftPanelWidth, float rightPanelWidth);     // Faint rotate / zoom / pan reminder at the bottom-right of the viewport (mirrors UpdateCamera's bindings)
        void  DrawResetViewButton(float leftPanelWidth);                          // "Reset View" overlay button at the bottom-left of the viewport: default angle, centred, fitted
        bool  m_fitViewPending = false;                                           // Set when a load framed the camera before the window had a size; the first sized frame re-fits once
        float m_lastFitDistance = 0.0f;                                           // Fit distance for the current window size; on resize the camera distance is rescaled by newFit / this so the model keeps its relative size
        void  ApplyFitDistance();                                                 // m_distance = FitDistanceForViewport(), and remember it for resize rescaling
        float FitDistanceForViewport() const;                                     // Camera distance at which the model's bounding sphere fits neatly in the viewport strip between the two control panels
        void  RebuildHeatmapClusterCubes();
        void  DrawOctreeVisualizer();

        // LOD quality test (--lod-quality-test <folder>): once the whole stream is resident, renders the model
        // at each LOD switch distance -- of both auto LOD rules -- at level 0, the level before the switch and
        // the level after it, compares each against level 0 at the same distance (PSNR, SSIM) and writes the
        // images and a report to the folder, then quits. Coarse levels are measured both as discs (the
        // current splat-mode drawing) and as their own Gaussians.
        struct LodTestShot
        {
            bool        screenSpaceRule = true; // Which rule's switch distance this shot sits at
            int         switchLevel = 0;        // The level the rule switches to at that distance
            float       distance = 0.0f;
            int         level = 0;              // Level drawn
            bool        coarseAsGaussians = false;
            float       coarseGaussianCap = 0.0f; // With coarseAsGaussians: the scale cap, x level spacing (0 = uncapped)
            bool        asSurfels = false;      // Splat mode drawn as opaque surfel discs (mode 4) instead of sorted Gaussians (mode 3)
            int         repeat = 0;             // 1 = a second level 0 capture, for the TAA noise floor
            RgbaImage   image;
        };
        std::string              m_lodTestFolder;            // Empty = no test this run
        bool                     m_lodTestRunning = false;
        std::vector<LodTestShot> m_lodTestShots;
        size_t                   m_lodTestIndex = 0;
        int                      m_lodTestSettledFrames = 0;
        bool                     m_lodTestCaptureRequested = false;
        double                   m_lodTestShotSeconds = 0.0;
        // Splat mode: draw levels 1+ as their own Gaussians, scales capped at m_splatCoarseGaussianCap x the level spacing,
        // instead of discs. On by default: the LOD quality test (2026-10-24, cthulu) put capped Gaussians at or above discs at
        // every level -- 25.4 vs 18.2 dB at level 1 -- while uncapped ones bloom from level 2 up. Renderer option, saved in config.json.
        bool                     m_splatCoarseAsGaussians = true;
        static constexpr float   kLodTestGaussianCaps[2] = { 0.75f, 1.5f }; // Scale caps the LOD quality test measures, x level spacing
        float                    m_splatCoarseGaussianCap = 1.5f;  // With Coarse Levels as Gaussians: cap each axis's scale at this x the level spacing (0 = no cap)
        bool                     m_lodTestSavedCoarseAsGaussians = false;
        float                    m_lodTestSavedCoarseGaussianCap = 0.0f;
        bool                     m_lodTestSavedDrawAsSurfels = false;
        std::vector<std::string> m_lodTestMontagesWritten;   // The report is rewritten after every finished group; montages are written once
        void  SetSplatDrawAsSurfels(bool asSurfels);        // Draw Splats as Surfels, as the checkbox applies it (re-stamps the chunks' fade value)
        // Loupe (L): a box that follows the mouse over the viewport, drawn again
        // magnified into an inset at the viewport's top right (SurfelsRenderer::State::loupeEnabled). W/S
        // change the magnification while it is on.
        // W/S zoom: kLoupeZoomRate e-folds per second at first, growing by kLoupeZoomAccel per second held up to
        // kLoupeZoomRateMax, so a tap nudges and a long hold dives deep.
        static constexpr float kLoupeZoomMin = 2.0f, kLoupeZoomMax = 256.0f;
        static constexpr float kLoupeZoomRate = 0.8f, kLoupeZoomAccel = 1.5f, kLoupeZoomRateMax = 6.0f;
        float  m_loupeZoomHoldSec = 0.0f;            // How long W or S has been held (drives the acceleration)
        // Inset placement: opens at the top right of the viewport, then a left-button drag inside it moves it and one
        // on an edge or corner resizes it (UpdateCamera). Drag modes combine: two edge bits = a corner.
        static constexpr int kLoupeDragLeft = 1, kLoupeDragBottom = 2, kLoupeDragRight = 4, kLoupeDragTop = 8, kLoupeDragMove = 16;
        ImVec2 m_loupeSize = ImVec2((float)kLoupeWidth, (float)kLoupeHeight);
        ImVec2 m_loupePos = ImVec2(-1.0f, -1.0f);   // Inset top-left, window pixels; negative = not placed yet (top right of the viewport). Size and place are saved in config.json (loupe_size, loupe_pos) when a drag ends
        ImVec4 m_loupeRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // Inset x0, y0, x1, y1 last frame (for grabbing it)
        int    m_loupeDrag = 0;                      // The drag in progress (kLoupeDrag* bits); 0 = none
        int    m_loupeHover = 0;                     // What a drag would do from where the mouse is (highlighted)
        bool   m_loupeEnabled = false;
        float  m_loupeZoom = 8.0f;
        int    m_loupeViz = 0;                       // Inset display: 0 normal, 1 splat outlines, 2 random colour per splat, 3 centres (Shift+V cycles it)
        int    m_splatViz = 0;                       // The same display for the whole model (Renderer tab "Splat Display"; V cycles it)
        ImVec2 m_loupeCenter = ImVec2(-1.0f, -1.0f); // Back-buffer pixels; negative = not placed yet (centre of the view)
        bool   m_loupePinned = false;                // The box stays put instead of following the mouse (a click inside it, or Space, toggles)
        bool   m_loupeBoxPressed = false;            // The left button went down inside the box (a click toggles the pin on release)
        ImVec2 m_loupeBoxPressPos = ImVec2(0.0f, 0.0f);
        float  m_rightPanelWidth = 0.0f;             // From BuildUI, for placing the inset
        ImVec4 m_leftPanelRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);  // Side panels' x0, y0, x1, y1 this frame (from BuildUI):
        ImVec4 m_rightPanelRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // the loupe's box is kept out of them
        float  m_bannerBottomY = 0.0f;               // Bottom edge of the render-mode banner last frame (0 = none); the inset sits below it
        void   DrawLoupeOverlay();                   // The box and the inset's frame and zoom label (the inset itself is the renderer's)

        // Right panel sections: drawn in m_rightPanelOrder (section ids, see BuildUI), each open or collapsed per
        // m_rightPanelOpen. Dragging a section's title bar up or down moves it; order and open state are saved in
        // config.json (right_panel_order, right_panel_open). The panel's height fits whatever is open.
        static constexpr int kRightPerformance = 0, kRightCompression = 1, kRightResidency = 2, kRightPyramid = 3, kRightModel = 4, kRightCulling = 5, kRightSpatial = 6;
        static constexpr int kRightSectionCount = 7;
        int    m_rightPanelOrder[kRightSectionCount] = { 0, 1, 2, 3, 4, 5, 6 };
        bool   m_rightPanelOpen[kRightSectionCount] = { true, true, true, true, true, true, true };
        bool   m_rightDragMoved = false;             // The title bar being dragged has moved its section: its click must not toggle it
        bool   RightPanelSectionHeader(int slot, int section, int& swapSlotA, int& swapSlotB); // Title bar of the section at this slot: open state, drag to reorder; true = draw the body
        void  StepLodQualityTest();     // Once per frame, before the UI: moves the test along
        void  FinishLodQualityTest();   // Compares the captures and writes the report
    };
}
