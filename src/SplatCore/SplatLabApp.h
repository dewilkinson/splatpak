// SplatLabApp.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The shared app shell behind both executables. In Studio mode it is SplatLab: the whole
// offline preprocessing pipeline (load -> octree chunk -> wavelet decompose -> quantize ->
// export) plus the runtime streaming simulation and every ImGui panel. In Viewer mode it is
// the standalone Splat viewer: the same class with the GSTx Bake tab, its menu items
// and its shortcuts hidden, so the viewer is SplatLab's Renderer and Streaming tabs by
// construction rather than a second copy of them. SplatRenderer handles the actual GPU work;
// this class is everything else -- UI, file I/O, and the streaming/decay/silhouette simulation.

#pragma once
#include "../../libs/gstx/OcclusionVolume.h"
#include "../../libs/gstx/DetailHeatmap.h"
#include "../../libs/gstx/StreamOrder.h"
#include "../../libs/gstx/CodecBuild.h"
#include "../../libs/gstx/SplatCodec.h"
#include "stdafx.h"
#include "SplatRenderer.h"
#include "SyntheticGenerator.h"
#include "PLYLoader.h"
#include "SPLATLoader.h"
#include "SpatialOctree.h"
#include "../../libs/gstx/LiftingWavelet.h"
#include "Quantizer.h"
#include "../../libs/gstx/ByteShuffle.h"
#include "StreamPackager.h"
#include "PointCloudGaussians.h"
#include "../../libs/gstx/StreamRuntime.h"
#include "ImageCompare.h"
#include <commdlg.h>
#include <string>
#include <vector>

namespace Splat
{
    void LogTransitionTrace(const char* fmt, ...);
    void LogD3D12Messages();

    class SplatLabApp : public CAULDRON_DX12::FrameworkWindows
    {
    public:
        // Which product this instance is (see the file header). Viewer hides everything that only
        // makes sense with a raw point cloud loaded: the generator tab, Open Raw / Generate Benchmark /
        // Save Package, and Ctrl+O opens a .gstx instead of a .ply.
        enum class Mode { Studio, Viewer };

        explicit SplatLabApp(LPCSTR name, Mode mode = Mode::Studio);

        Mode        GetMode() const      { return m_mode; }
        bool        IsViewerOnly() const { return m_mode == Mode::Viewer; }
        const char* AppTitle() const     { return m_mode == Mode::Viewer ? "Splat Viewer" : "SplatLab"; } // Dialog titles

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
        void UpdatePreviewSplats();
        void ReportDeviceLostAndExit(const std::string& message);

        std::string OpenFileDialog(const char* filter, const char* title = "Open File", const char* defaultExt = nullptr);
        std::string SaveFileDialog(const char* filter, const char* defaultExt = "gstx", const char* title = "Save File");

        Mode                      m_mode = Mode::Studio;
        SplatRenderer*          m_pRenderer = nullptr;
        SplatRenderer::State    m_state;
        PendingAction             m_pendingAction = PendingAction::None;

        // Set once a GPU device-removed/suspended HRESULT is caught. Once true, every
        // frame stops attempting to render/Present entirely (see OnRender()) instead
        // of retrying and throwing the same ThrowIfFailed exception forever -- that
        // repeated throw is otherwise indistinguishable from a real hang if a debugger
        // is set to break on every C++ exception.
        bool m_deviceLost = false;

        // Preprocessor Working Structures (Source Model -> Wavelet Hierarchy)
        std::string               m_loadedFilePath = "No dataset loaded";
        std::vector<SplatPoint> m_rawSplats;
        std::vector<ChunkData>    m_chunks;
        WaveletDecompositionResult m_waveletResult;

        // Renderer Active Streaming Buffers (Decoupled Independent Copy for GPU & Viewport)
        bool                      m_isLoadedFromSFLW = false;
        std::string               m_rendererSourceDescription = "No model active";
        StreamPackager::SFLWPackageData m_loadedPackage;
        std::vector<PackedPointGPU>    m_rendererSplats;
        std::vector<SplatPoint>       m_rendererRawSplats;
        std::vector<ChunkData>          m_rendererOctreeChunks;

        // Bounding box & stats
        XMFLOAT3 m_aabbMin = { 0, 0, 0 };
        XMFLOAT3 m_aabbMax = { 0, 0, 0 };
        XMFLOAT3 m_center  = { 0, 0, 0 };
        XMFLOAT3 m_extents = { 0, 0, 0 };
        uint64_t m_sourceFileBytes = 0;     // Size of the original .ply/.splat on disk; travels inside the .gstx (v5 header) so the ratio survives a reload
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
        // Configurable via config.json or config.ini ("startup_dataset") so a different default
        // can be swapped in without recompiling. Tried as-is and at a few relative CWD depths.
        std::string m_startupDatasetPath = "assets/dwarves/dwarves.gstx";

        // Interior Occlusion Volume: solid depth-writing cubes baked at preprocessing time so far-side
        // splats can't show through gaps in the near side. Optional and disabled by default.
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
        uint32_t m_occlusionSafetyNetVersion = UINT32_MAX; // m_occlusionVoxelsVersion right after the GSTx Bake tab's fallback bake last fired (see BuildUI): equal = already tried for this volume state
        std::vector<OcclusionVoxelGPU> m_occlusionVoxels; // Baked result -- from BuildOcclusionVolume() or loaded from an .gstx's package
        OcclusionMipTable m_occlusionMips;       // Mip layout of m_occlusionVoxels (see OcclusionMipTable): from Bake, or the loaded package's header
        int   m_occlusionMipOverride = -1;       // Viewer: -1 = renderer picks the mip from projected cell size; 0.. = force that mip (debug / side-by-side compare)
        bool  m_enableOcclusionCulling    = true;  // Viewer: "Show Occlusion Volume" -- draw the volume and depth-test splats against it (on by default)
        bool  m_showOcclusionSetting      = true;  // The user's choice for the toggle above (the checkbox, config show_occlusion_volume): a load or a bake shows the volume only if this is on
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
        bool  m_lodPerBlock          = false;                    // Screen-space LOD per block: each block's level from its own distance to the eye (BlockTargetLOD); off = one level for the whole model from its near side. Off by default until the pixel threshold is calibrated for it: at 1 px it draws 57% fewer splats than the model-wide level but is visibly softer, since a coarse level drawn as merged Gaussians does not match a finer one even at sub-pixel point spacing
        bool  m_autoRotate           = false; // Disabled by default
        bool  m_showControlHints     = true;  // Viewport: draw the faint rotate/zoom/pan reminder at the bottom-right (see DrawControlHints); saved in config.json
        int         m_renderPathOverride = -1;     // "render_path" from config.json or --render-path: -1 auto, else SplatRenderer::RenderPath to force (testing)
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
        bool  m_devMode              = false;  // Loaded from config file (config.json)
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
        std::vector<SplatPoint> m_previewLODSplats;

        // Pre-cached resident GPU/CPU LOD buffers for zero-hitch instantaneous transitions
        std::vector<ResidentLOD> m_residentLODs;
        // The streaming runtime (libs/gstx/StreamRuntime.h): it owns the blocks, residency, delivery and
        // the render list; the app feeds it the levels above, its settings and the camera, and draws its output
        StreamRuntime m_stream;
        void SyncStreamInputs();   // The UI settings and the camera, into m_stream (every frame, and before any call into it)
        void ApplyStreamFrame();   // The runtime's render list and buffers, into the renderer state
        XMFLOAT3 m_lodSortFrameMin = { 0, 0, 0 }; // The Morton frame every level is sorted in (PrecacheResidentLODs): the model's bounds
        XMFLOAT3 m_lodSortFrameMax = { 0, 0, 0 };
        void PrecacheResidentLODs();

        // Splat mode: the imported cloud's full Gaussians (SplatPoint::sourceIndex refers here), the
        // bake settings, and the encode parameters every level shares (see SplatCodec).
        std::vector<SplatAttributes> m_splatAttributes;
        bool     m_sourceHasSplatAttributes = false; // The loaded source carried scales/rotations/opacities (a 3DGS .ply or .splat)
        bool     m_splatMode                = false; // Bake and draw as 3D Gaussians (splat format 1) instead of 8-byte splats
        int      m_splatSHQuality           = 2;     // Spherical-harmonics degree kept, minus one: 0 = low (degree 1), 1 = medium (degree 2), 2 = high (degree 3, the default); a loaded package sets it to its own degree
        int      m_splatBlendSpace          = 0;     // 0 = display-space blending (as the reference viewer), 1 = linear
        // Draw Splats as Splat (Renderer tab, saved in config.json as splat_draw_as_splats): a splat package is
        // drawn through the splat pipeline as opaque, depth-tested discs (render mode 4; Auto Splat Size, Coverage,
        // Splat Radius Scale and Splat Orientation apply, the dither dissolves between two surfaces). Off (the
        // default): the sorted Gaussian pipeline (render mode 3) with Splat Blending, Gaussians at level 0 and discs above.
        bool     m_splatDrawAsDiscs       = false;
        uint32_t SplatRenderMode() const { return m_splatDrawAsDiscs ? 4u : 3u; }
        SplatEncodeParams m_splatParams;
        uint32_t SplatSHDegree() const { return (uint32_t)std::clamp(m_splatSHQuality, 0, 2) + 1u; }
        // The bake keeps the SH Quality degree, but never more bands than the source carries: extra bands would
        // only store zeros (a source without harmonics, or a package baked at a lower degree).
        uint32_t m_sourceSHDegree = 3;               // Highest band the source's Gaussians carry (a package: its own degree)
        uint32_t BakeSHDegree() const { return std::min(SplatSHDegree(), m_sourceSHDegree); }
        void     DetectSourceSHDegree();            // Sets m_sourceSHDegree from m_splatAttributes (highest band with a non-zero coefficient)
        size_t   BytesPerRenderPoint() const { return m_splatMode ? (sizeof(PackedSplatGPU) + SplatCodec::SHRecordBytes(m_splatParams.shDegree)) : (m_enableQuantization ? sizeof(PackedPointGPU) : sizeof(SplatPoint)); }
        void     EncodeSplatLevel(const std::vector<SplatPoint>& splats, uint32_t level, std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH) const;
        void     ApplySplatRenderState();

        // Progressive Network Streaming & Bandwidth Throttle Simulator




        StreamingPolicy m_streamingPolicy           = StreamingPolicy::Greedy; // Greedy (default) or Conservative
        float  m_conservativeNeighborBufferMargin   = 1.35f;  // Frustum margin for pre-fetching local neighbors in conservative mode
        bool   m_enableDitheredTransitions  = true;   // Stochastic screen-space Bayer dithering for smooth LOD transitions
        bool   m_demoteChunks               = false;  // Demote Chunks on Zoom Out. Off: detail already shown stays when the view coarsens (decay can still drain it). On: it fades back to the coarser level and is dropped
        bool   m_autoSplatSize       = true;   // Auto Splat Size: disc radius from each level's point spacing (m_lodSpacing x m_autoSplatCoverage), not the size classes
        float  m_autoSplatCoverage   = 1.2f;   // Disc radius as a multiple of the level's typical point spacing
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
        bool   m_enableStreamDecay          = false;  // Toggle cache decay on/off
        float  m_streamDecayRate            = 5.00f;  // Decay rate (0.0 = no decay .. 10.0 = max) for memory reclamation.
                                                        // At 10.0 a full drain (everything but the two pinned coarsest
                                                        // levels) completes in kDecayFullDrainSecondsAtMaxRate on EVERY
                                                        // bandwidth setting: the drain budget is a multiple of the
                                                        // evictable byte total, not of the bandwidth, so the throttle
                                                        // cancels out. Scales linearly below max (5.0 = twice as long).
                                                        // See the decay budget calculation in UpdateStreamingSimulation.
        // Decay is a fixed outflow in bytes per second, set only when the toggle or the slider changes
        // (or a model loads): rate 10 = the whole evictable set in kDecayFullDrainSecondsAtMaxRate at the
        // bandwidth in force at that moment (the reference bandwidth, added to the outflow so the NET
        // drain at that bandwidth takes exactly that long; Full = no inflow term). Changing the bandwidth
        // afterwards does not touch it, so the throttle becomes the knob for the net eviction rate.
        void   CalibrateDecay();
        bool   m_isStreamingPaused          = false;  // Pause/Resume packet streaming

        // Show Chunk Stream: Creeping Wavefront & Dissolving Alpha Wake
        bool   m_showChunkStream            = true;   // Streaming arrival visualizer: newly delivered chunks glow white-gold, cool to gold and fade out (Renderer tab: "Show Streaming Arrivals")
        float  m_chunkStreamDuration        = 2.5f;   // Seconds a delivered chunk glows, fading out over that time; 2.5 s by default
        float  m_arrivalGlowIntensity       = 1.0f;   // Refinement visualizer strength (0..2); 1 by default
        float  m_arrivalGlowHue             = 0.0f;   // Refinement visualizer hue rotation in degrees (-180..180, 0 = gold)
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

        // Streaming tab: the streaming order's bounding octahedron (libs/gstx/StreamOrder.h, owned by m_stream)
        void  DrawOctahedronGlyph();                                   // Streaming tab: the octahedron's upper and lower faces, visible ones lit, each with its fill progress
        DetailGrid                            m_detailGrid;         // Detail grid for the loaded model: from the package (v7+) or built from LOD 0 on load / preprocess; written into exported packages

        // Throttling for frustum priority re-sorting & UI indicator stabilization


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
            bool        asSplats = false;      // Splat mode drawn as opaque splat discs (mode 4) instead of sorted Gaussians (mode 3)
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
        bool                     m_lodTestSavedDrawAsDiscs = false;
        std::vector<std::string> m_lodTestMontagesWritten;   // The report is rewritten after every finished group; montages are written once
        // LOD calibration (--lod-calibration <folder>): with the whole stream resident, streaming on and Demote Chunks on,
        // renders the launch view at a few distances with level 0 everywhere (the reference), the model-wide rule at 1 px,
        // and per-block levels at a range of pixel thresholds; each shot waits for the render list to settle, then is
        // compared with the reference (PSNR, SSIM) and its splat count recorded. Writes report.md, results.csv and crops.
        struct LodCalShot
        {
            float     distanceFactor = 1.0f;  // x the fitted launch distance
            int       kind = 0;               // 0 level 0 reference, 1 level 0 again (TAA noise floor), 2 model-wide rule, 3 per-block levels
            float     pixelSpacing = 1.0f;    // The rule's threshold (kinds 2 and 3)
            RgbaImage image;
            uint64_t  splats = 0;
            uint32_t  chunks = 0;
            uint64_t  perLevel[8] = {};
            float     seconds = 0.0f;
            bool      settled = true;
        };
        std::string              m_lodCalFolder;            // Empty = no calibration this run
        bool                     m_lodCalRunning = false;
        std::vector<LodCalShot>  m_lodCalShots;
        size_t                   m_lodCalIndex = 0;
        int                      m_lodCalStableFrames = -1;
        uint64_t                 m_lodCalLastSplats = 0;
        bool                     m_lodCalCaptureRequested = false;
        double                   m_lodCalShotSeconds = 0.0;
        double                   m_lodCalStableSeconds = 0.0; // How long the render list has been unchanged with no cross-fade running
        float                    m_lodCalFit = 0.0f;
        void  StepLodCalibration();    // Once per frame, before the UI
        bool   m_streamTest = false;         // --stream-test: a scripted Evict at 5G, for streaming baselines (StepStreamTest)
        int    m_streamTestPhase = 0;
        double m_streamTestSeconds = 0.0;
        void   StepStreamTest();
        void  FinishLodCalibration();  // Compares the captures and writes the report
        void  SetSplatDrawAsDiscs(bool asSplats);        // Draw Splats as Splat, as the checkbox applies it (re-stamps the chunks' fade value)
        // Loupe (L): a box that follows the mouse over the viewport, drawn again
        // magnified into an inset at the viewport's top right (SplatRenderer::State::loupeEnabled). W/S
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
        bool   m_leftPanelCollapsed = false;          // Side panels folded to their title bar (the arrow on it; config left_panel_collapsed,
        bool   m_rightPanelCollapsed = false;         // right_panel_collapsed): the space goes to the viewport and the loupe
        void   RecordSidePanel(ImVec4& rect, bool& collapsed); // Inside a side panel: its on-screen rectangle (the title bar while folded), and remembers a fold
        void   SetNextSidePanelCollapse(const ImVec4& rect, bool collapsed); // Before a side panel: a click on its title bar's arrow folds or unfolds it
        float  m_bannerBottomY = 0.0f;               // Bottom edge of the render-mode banner last frame (0 = none); the inset sits below it
        void   DrawLoupeOverlay();                   // The box and the inset's frame and zoom label (the inset itself is the renderer's)
        // A pinned box is anchored to the surface under it: the point and the patch's half-height in world units
        // at pin time (at m_loupeAnchorZoom), re-projected every frame so zoom and orbit keep the same subregion
        XMFLOAT3 m_loupeAnchorWorld = { 0, 0, 0 };
        float  m_loupeAnchorHalfWorld = 0.0f;
        float  m_loupeAnchorZoom = 8.0f;
        bool   m_loupeAnchorValid = false;
        float  m_loupeEffectiveZoom = 8.0f;          // The magnification drawn last frame (m_loupeZoom, or the anchored box's)
        void   SetLoupePinned(bool pinned);          // Pins (capturing the anchor) or releases the box
        bool   ProjectLiveCamera(const XMFLOAT3& world, ImVec2& screen, float& viewDepth) const; // World point -> window pixels and view depth, live camera

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
