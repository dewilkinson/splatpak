// SplatPakApp.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The shared app shell behind both executables. In Studio mode it is SplatPak: the whole
// offline preprocessing pipeline (load -> octree chunk -> wavelet decompose -> quantize ->
// export) plus the runtime streaming simulation and every ImGui panel. In Viewer mode it is
// the standalone Splat viewer: the same class with the Package Bake tab, its menu items
// and its shortcuts hidden, so the viewer is SplatPak's Renderer and Streaming tabs by
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
#include "BackgroundOp.h"
#include "../../libs/gstx/ShadowMeshBuilder.h"
#include <commdlg.h>
#include <condition_variable>
#include <string>
#include <map>
#include <unordered_map>
#include "ShadowMeshDisplay.h"
#include "RdoProfile.h"
#include "EntropyProfile.h"
#include "TrainingCameras.h"
#include "PackageView.h"
#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace Splat
{
    void LogTransitionTrace(const char* fmt, ...);
    void LogD3D12Messages();

    class SplatPakApp : public CAULDRON_DX12::FrameworkWindows
    {
    public:
        // Which product this instance is (see the file header). Viewer hides everything that only
        // makes sense with a raw point cloud loaded: the generator tab, Open Raw / Generate Benchmark /
        // Save Package, and Ctrl+O opens a .gstx instead of a .ply.
        enum class Mode { Studio, Viewer };

        explicit SplatPakApp(LPCSTR name, Mode mode = Mode::Studio);

        Mode        GetMode() const      { return m_mode; }
        bool        IsViewerOnly() const { return m_mode == Mode::Viewer; }
        const char* AppTitle() const     { return m_mode == Mode::Viewer ? "Splat Viewer" : "SplatPak"; } // Dialog titles

        // Splits a Win32 command line into arguments: whitespace-separated, double quotes group.
        static std::vector<std::string> SplitCommandLine(const char* cmdLine);
        // The options both executables accept (see AppEntry.cpp for the mode switches and --help).
        static const char* CommandLineUsage();

        void OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight) override;
        void OnCreate() override;
        void OnDestroy() override;
        bool OnCloseRequested(bool escape);         // Asks to save an unsaved bake; waits for a save in progress (the window's close and Esc, through CloseGuardProc)
        // The close prompt for an unsaved bake (DrawClosePrompt): Open asks it to appear, Shown while it is up, Cancel = Esc
        enum class ClosePrompt { None, Open, Shown, Cancel };
        ClosePrompt m_closePrompt = ClosePrompt::None;
        void DrawClosePrompt();
        bool SaveBakeFromPrompt(bool quitAfter);
        bool m_bakeUnsaved = false;                 // A bake on screen that no .gstx holds yet (cleared by a save or a package open)
        bool m_quitAfterSave = false;               // Close once the save under way has written its package
        // Mouse-look (Tab; Esc or Tab again, or losing focus, leaves it): the cursor is hidden and held at the window's
        // centre, and every movement turns the view about the eye, all the way round; WASD/QE fly where it looks
        bool m_mouseLook = false;
        void SetMouseLook(bool on);
        // Navigation (the viewport's Orbit / Fly switch, or O): Orbit turns the view about a pivot on the model (left-drag,
        // A/D/Q/E circle it, W/S and the wheel move in and out, right-drag pans); Fly moves the eye (W/S/A/D/Q/E, drags look,
        // the wheel sets the speed). A package's view settings (navigation) choose the mode it opens in; without them an
        // object orbits and an environment flies. Mouse-look (Tab) is Fly with the mouse captured.
        enum NavMode { NavOrbit = 0, NavFly = 1 };
        int  m_navMode = NavOrbit;
        void SetNavMode(int mode);
        void RecenterCamera(); // The nav switch's Recenter button
        void DrawNavSwitch();
        // Mouse-look's movement: the mouse's own relative motion (raw input, WM_INPUT in CloseGuardProc), summed until the
        // camera takes it each frame. Warping the cursor back to the centre and reading its offset spun the view without
        // end whenever the warp did not take.
        long m_lookDX = 0, m_lookDY = 0;
        // Fly speed, metres a second: the scene's own (an environment: its core radius x 0.3, 0.5 .. 10 m/s; an object:
        // half its largest dimension a second), times m_flySpeedScale (the wheel in mouse-look, x1.25 a notch)
        float m_flySpeedScale = 1.0f;
        float FlySpeed() const
        {
            const float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
            const float base = m_sceneIsEnvironment ? std::clamp(m_coreRadius50 * 0.3f, 0.5f, 10.0f) : std::max(0.05f, maxDim * 0.5f);
            return base * m_flySpeedScale;
        }
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
        void UnloadModel();   // CloseDataset, and every resource of the model released (CPU memory and GPU buffers)

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
            UnloadModel,           // The Package/Bake tab's Unload Model: the model and its resources freed (UnloadModel)
            ExitApp,
            LocateReencodeSource,  // Re-encode: the package's source scan was not found beside it
            ReencodeSaveAs         // Re-encode: where to write the package
        };

        void BuildUI();
        void ExecutePendingAction();
        void UpdateCamera(const ImGuiIO& io);
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
        SplatPointArray m_rawSplats;
        std::vector<ChunkData>    m_chunks;
        WaveletDecompositionResult m_waveletResult;

        // Renderer Active Streaming Buffers (Decoupled Independent Copy for GPU & Viewport)
        bool                      m_isLoadedFromSFLW = false;
        std::string               m_rendererSourceDescription = "No model active";
        StreamPackager::SFLWPackageData m_loadedPackage;
        std::vector<PackedPointGPU>    m_rendererSplats;
        SplatPointArray       m_rendererRawSplats;
        std::vector<ChunkData>          m_rendererOctreeChunks;

        // Bounding box & stats
        XMFLOAT3 m_aabbMin = { 0, 0, 0 };
        XMFLOAT3 m_aabbMax = { 0, 0, 0 };
        XMFLOAT3 m_center  = { 0, 0, 0 };
        XMFLOAT3 m_extents = { 0, 0, 0 };
        uint64_t m_sourceFileBytes = 0;     // Size of the original .ply/.splat on disk; travels inside the .gstx (v5 header) so the ratio survives a reload
        float    m_rawFileSizeMB = 0.0f;
        float    m_compressedSizeMB = 0.0f;
        float    m_transformedMB = 0.0f;     // Tier 4: every level's records and SH as the codec receives them (uncompressed)
        float    m_transformedSHMB = 0.0f;   // Of which the spherical harmonics
        float    m_compressionRatio = 0.0f;
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
        static constexpr const char* kBuiltInDataset = "assets/cthulu/cthulu.gstx"; // Opened when the configured one is not there
        std::string m_startupDatasetPath = kBuiltInDataset;

        // Interior Occlusion Volume: solid depth-writing cubes baked at preprocessing time so far-side
        // splats can't show through gaps in the near side. Optional and disabled by default.
        bool  m_generateOcclusionVolume   = true;  // Preprocessor: bake a volume for this dataset on export (on by default)
        float m_occlusionShaveBiasCells   = 0.0f;  // From config.json "occlusion_shave_bias": extra cells added to the unconditional poke-through cull band (see OcclusionVolume::BuildGrid)
        std::string m_lastDialogFolder;             // From config.json "last_dialog_folder": Open/Save dialogs start here instead of the project root once the user has browsed elsewhere

        // Cached voxelization of the current raw points (the bake's erosion + octree stage runs on it). Invalidated by
        // RecomputeWaveletHierarchy; see OcclusionVolume.h.
        OcclusionVolume::Grid m_occlusionGrid;
        void  BuildOcclusionGrid();
        uint32_t m_occlusionVoxelsVersion = 0;   // Incremented whenever m_occlusionVoxels is rebuilt or cleared (renderer re-upload trigger)
        uint32_t m_occlusionSafetyNetVersion = UINT32_MAX; // m_occlusionVoxelsVersion right after the Package Bake tab's fallback bake last fired (see BuildUI): equal = already tried for this volume state
        std::vector<OcclusionVoxelGPU> m_occlusionVoxels; // Baked result -- from BuildOcclusionVolume() or loaded from an .gstx's package
        OcclusionMipTable m_occlusionMips;       // Mip layout of m_occlusionVoxels (see OcclusionMipTable): from Bake, or the loaded package's header
        int   m_occlusionMipOverride = -1;       // Viewer: -1 = renderer picks the mip from projected cell size; 0.. = force that mip (debug / side-by-side compare)
        bool  m_enableOcclusionCulling    = true;  // Viewer: "Enable Occlusion Volume" -- depth-test the splats against the volume, so the far side does not show through (on by default)
        bool  m_showOcclusionSetting      = true;  // The user's choice for the toggle above (the checkbox, config enable_occlusion_volume): a load or a bake enables the volume only if this is on
        bool  m_showOcclusionVolumeOnly   = false; // Viewer: debug view -- render only the occluder geometry
        // Shadow mesh (ShadowMesh.h): from the package, or built in the background after a bake of a raw scan. Shared and
        // immutable once published, so a save on the worker can hold it while the UI replaces it.
        struct ShadowMeshJob
        {
            std::thread thread;
            std::atomic<bool> cancel{ false }, done{ false };
            std::shared_ptr<const ShadowMesh> result; // Set (or left empty on failure) before done
            std::shared_ptr<const ShadowMesh> occluder; // The occluder mesh built from the bake's volume (format 13)
            std::string trace;
            double seconds = 0.0;
        };
        std::shared_ptr<const ShadowMesh> m_shadowMesh;  // Empty: no mesh (an older package, a point-format bake)
        std::shared_ptr<const ShadowMesh> m_occluderMesh; // Format 13: the occlusion volume as a mesh, used in place of the cubes
        ShadowMeshDisplay m_occluderMeshGpu; // Smooth normals, not subdivided: it must stay inside the cubes
        float m_occluderInsetEdges = 1.0f; // How far inside the splats the occluder is drawn and tested, in its median edges (SetOccluderMesh; config occluder_inset_edges)
        uint32_t m_occluderMeshVersion = 0;
        bool HasOccluderMesh() const { return m_occluderMesh && !m_occluderMesh->Empty(); }
        void SetOccluderMesh(std::shared_ptr<const ShadowMesh> mesh);
        std::shared_ptr<ShadowMeshJob>    m_shadowMeshJob;
        // The package compressed in the background after a bake (StartPrecompress), into a temporary file, so Save only has to
        // copy it into place. It reads the displayed model, so whatever replaces the model stops it first (StopPrecompress, beside
        // CancelShadowMeshBuild); its threads run at the lowest priority, behind drawing and streaming.
        struct PrecompressJob
        {
            std::thread thread;
            std::atomic<bool> cancel{ false }, done{ false };
            bool ok = false;                     // Set before done
            std::atomic<float> fraction{ 0.0f };
            std::string base;                    // The temporary package, without .gstx
            uint64_t generation = 0;             // m_bakeGeneration it was made for
            float splatRadius = 0.0f, deadbandMM = 0.0f;
            int maxLODs = 0;
            TransformStats stats;
            std::shared_ptr<const ShadowMesh> mesh, occluder;
            std::vector<OcclusionVoxelGPU> voxels;
            OcclusionMipTable mips;
            DetailGrid detailGrid;
            SplatBakeInput splat;
            bool sizeShown = false;              // Its exact size has replaced the estimate (UI thread)
        };
        std::shared_ptr<PrecompressJob> m_precompress;
        uint64_t m_bakeGeneration = 0;           // Counts bakes applied: a precompressed package belongs to one
        void     StartPrecompress();
        void     StopPrecompress();
        bool     PrecompressMatches() const;     // The precompressed package is of the model on screen, with today's settings
        bool     m_showShadowMesh = false;   // View Shadow Mesh (config show_shadow_mesh)
        bool     m_showGrid = true;          // Renderer tab, Show Grid: the editor grid at y = 0, this session (an environment starts with it off)
        bool     m_showGridSetting = true;   // The saved choice (config show_grid): what objects load with
        int      m_shadowMeshLevel = 0;      // Level drawn by the view
        uint32_t m_shadowMeshVersion = 0;    // Bumped when the mesh or the drawn level changes (renderer re-upload)
        float    m_shadowMeshMedianEdge = 0.0f; // Level 0's median edge: the unit of the shadow-mesh occlusion test's margin
        float    m_occlusionMarginEdges = 1.0f; // The shadow-mesh test's margin in those edges (--occlusion-margin)
        bool     m_solidDepthOn = false;    // Solid splat depth (SplatRenderer::State::solidDepth), experimental; --solid-alpha
        float    m_solidAlpha = 0.3f;       // Its core opacity threshold
        float    m_solidMargin = 0.02f;     // Its depth push-back in metres; --solid-margin
        bool     m_opaqueSurface = true;    // Opaque Front Surface (SplatRenderer::State::opaqueSurface); config opaque_front_surface, --opaque-front on|off
        float    m_opaqueLo = 0.3f, m_opaqueHi = 0.8f; // Its coverage ramp; --opaque-ramp lo,hi
        float    m_occluderPushEdges = 0.0f; // The occluder mesh pushed back this many median edges under the splats (experimental); --occluder-push
        bool     m_farTint = false;         // The far side drawn magenta, a see-through test (SplatRenderer::State::farTint); --far-tint
        bool     m_vizShowThrough = false;  // Over the occlusion volume, how much of it shows through the splats, in red (--opaque-debug)
        bool     m_vizCoverage = false;     // Elsewhere, the splats' coverage in green (--opaque-debug)
        int      m_occlusionTest = 0;       // The sort's occlusion test: 0 the occluder mesh, 1 the shadow mesh's depth (experimental; SplatRenderer::State::occlusionTest); config occlusion_test, --occlusion-test
        ShadowMeshDisplay m_shadowMeshGpu;   // The drawn level, subdivided and smooth-shaded (ShadowMeshDisplay.h)
        int      m_shadowMeshGpuLevel = -1;
        int      m_shadowMeshGpuPasses = -1;  // Its subdivision passes: 2 for View Shadow Mesh, none as the gap fill under the splats
        uint32_t m_shadowMeshSerial = 0;              // Bumped by every SetShadowMesh: a new mesh may reuse a freed one's address
        uint32_t m_shadowMeshGpuSerial = UINT32_MAX;  // The serial m_shadowMeshGpu was built from; UINT32_MAX = none
        void ReleaseModelMeshes();                    // Cancels the build and drops the shadow and occluder meshes of the model being replaced
        void StartShadowMeshBuild();
        void CancelShadowMeshBuild();
        void PollShadowMeshBuild();
        void SetShadowMesh(std::shared_ptr<const ShadowMesh> mesh);
        std::string ShadowMeshSummary() const; // "10000 / 5000 / 2500 triangles", "building", or "none"
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
        bool  m_lodPerBlock          = true;                     // Screen-space LOD per block (on by default): each block's level from its own distance to the eye (BlockTargetLOD); off = one level for the whole model from its near side. At 1 px it draws 57% fewer splats than the model-wide level but is visibly softer, since a coarse level drawn as merged Gaussians does not match a finer one even at sub-pixel point spacing; a lower pixel threshold trades some of the saving back
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

        // ---- Bake (Package Bake tab) and long operations (BackgroundOp.h) -------------------------------------
        // The tab's bake parameters. The displayed model was baked with m_bakedParams; while the tab's values
        // differ the changes are pending: Bake is enabled, Save is not, and the model stays as it was.
        struct BakeParams
        {
            float chunkSize  = 16.0f;
            int   maxLODs    = 4;
            int   shQuality  = 2;
            int   shPrecision = 0; // Index into kSHPrecisionBits: 0 = 8 bits (lossless)
            uint8_t shBandBits[3] = { 8, 8, 8 }; // Bits per SH coefficient for bands 1-3: the RDO choice, or SH Precision's bits for all three
            float deadbandMM = 3.0f;
            bool  occlusion  = true;
            bool operator==(const BakeParams& o) const
            {
                return chunkSize == o.chunkSize && maxLODs == o.maxLODs && shQuality == o.shQuality && shPrecision == o.shPrecision &&
                       memcmp(shBandBits, o.shBandBits, 3) == 0 && deadbandMM == o.deadbandMM && occlusion == o.occlusion;
            }
            bool operator!=(const BakeParams& o) const { return !(*this == o); }
        };
        // The scan a bake reads: the loaded one, or one just read from disk (an open bakes before it replaces anything)
        struct BakeSource
        {
            const SplatPointArray*      points = nullptr;
            const SplatAttributeArray* attributes = nullptr;
            bool     splatMode = false;
            uint32_t shDegree = 0; // The degree the bake keeps (BakeSHDegree for this source)
            uint8_t  shBits[3] = { 8, 8, 8 }; // Bits per SH coefficient per band (StreamPackager.h RequantiseSH); 8 = as encoded
        };
        // What the v11 colour transform (YCoCg-R) does to a bake's data, from the size estimate's sample: entropy
        // (mean order-0 over the byte planes) and Brotli size, as percentage changes against plain RGB
        struct ColourEstimate
        {
            struct Stream { double entropyChangePercent = 0.0, sizeChangePercent = 0.0; };
            Stream records, sh;
            double entropyChangePercent = 0.0, sizeChangePercent = 0.0; // Both streams together
            bool valid = false;                                          // Splat-mode bakes only
        };
        ColourEstimate m_colourEstimate;
        EntropyProfile m_entropy;                    // Tiers 2-4 in bits per byte (EntropyProfile.h): from the bake, or measured on a loaded package
        static EntropyProfile MeasurePackageEntropy(const StreamPackager::SFLWPackageData& pkg); // A package's entropy, on a sample of its chunks
        // Everything a .gstx load produces: read, decoded and built on the worker (BuildPackageLoad), swapped in by
        // ApplyPackageLoad on the UI thread, so only a package that loads replaces the model on screen
        struct PackageLoad
        {
            StreamPackager::SFLWPackageData pkg;
            std::string reason;                         // Why the package could not be read
            SplatAttributeArray attributes;             // Every level's Gaussians: level 0 of every chunk, then level 1, ...
            std::vector<ChunkData> chunks;              // Level 0 per chunk, for a re-save
            std::vector<ChunkData> octreeChunks;        // The chunks' bounds, for the visualiser
            SplatPointArray rawSplats;                  // Level 0, meshlet-ordered
            std::vector<PackedPointGPU> packedSplats;   // The same, quantised
            DetailGrid detailGrid;
            bool detailFromPackage = false;
            WaveletDecompositionResult wavelet;
            std::vector<ResidentLOD> residentLODs;
            StreamRuntime stream;
            EntropyProfile entropy;
            uint64_t packageBytes = 0, recordBytes = 0, shBytes = 0;
        };
        bool        BuildPackageLoad(const std::string& filepath, const StreamSettings& streamSettings, const StreamCamera& streamCamera,
                                     PackageLoad& out, Progress& progress) const;
        void        ApplyPackageLoad(const std::string& filepath, PackageLoad& loaded);
        // Everything a bake produces: built on the worker, swapped in by ApplyBake on the UI thread
        struct BakeResult
        {
            XMFLOAT3 aabbMin = {}, aabbMax = {}, center = {}, extents = {};
            DetailGrid detailGrid;
            std::vector<ChunkData> chunks;
            WaveletDecompositionResult waveletResult;
            SplatEncodeParams splatParams;
            float compressedSizeMB = 0.0f;
            float transformedMB = 0.0f, transformedSHMB = 0.0f; // Tier 4 (see m_transformedMB)
            ColourEstimate colourEstimate;
            EntropyProfile entropy;
            std::vector<ResidentLOD> residentLODs;
            StreamRuntime stream;
            OcclusionVolume::Grid occlusionGrid;
            std::vector<OcclusionVoxelGPU> occlusionVoxels;
            OcclusionMipTable occlusionMips;
            std::vector<HeatmapClusterCube> heatmapCubes;
        };
        BakeParams  CurrentBakeParams() const;
        bool        BakePending() const { return m_hasBakedParams && CurrentBakeParams() != m_bakedParams; }
        std::string PendingBakeChanges() const;                    // The changed parameters by name, for the tab
        static BakeParams DefaultBakeParams(const SplatPointArray& points, BakeParams keep); // Chunk size, levels and deadband from the scan's size (an environment's from its core)
        static void MeasureCore(const SplatPointArray& points, XMFLOAT3& centre, float& radius50, float& radius99);
        BakeSource  LoadedBakeSource() const;
        void        StartBake(bool fitCamera);                     // Bakes the loaded scan with the tab's parameters
        bool        RunBake(const BakeParams& params, const BakeSource& source, const StreamSettings& streamSettings,
                            const StreamCamera& streamCamera, BakeResult& out, Progress& progress) const;
        void        ApplyBake(BakeResult& result, const BakeParams& params, bool fitCamera);
        bool        StartOpenScan(const std::string& filepath, bool splatFile); // .ply / .splat: read and bake, then replace the model
        void        BuildResidentLODs(const BakeSource& source, const WaveletDecompositionResult& wavelet, const XMFLOAT3& aabbMin,
                                      const XMFLOAT3& aabbMax, const SplatEncodeParams& splatParams, std::vector<ResidentLOD>& out,
                                      const std::function<void(float)>& onProgress = nullptr) const; // onProgress: the share of levels done
        void        BuildHeatmapClusterCubes(const SplatPointArray& points, const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax,
                                             const DetailGrid& detail, std::vector<HeatmapClusterCube>& out) const;
        bool        UseBackgroundOps() const;                      // False for scripted runs and before the first frames: those run inline
        bool        RunOperation(const std::string& title, BackgroundOp::Work work, BackgroundOp::Done done, bool quiet = false);
        void        DrawOperationDialog();
        unsigned    BusyThreads();                                   // The dialog's live core count (see the .cpp)
        std::chrono::steady_clock::time_point m_busySampleTime;
        std::unordered_map<DWORD, uint64_t>   m_busyThreadTimes;     // Each thread's CPU time at the last sample
        unsigned    m_busyThreads = 0;
        BackgroundOp m_op;
        BakeParams   m_bakedParams;
        bool         m_hasBakedParams = false;
        uint64_t     m_framesRendered = 0;
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
        SplatPointArray m_previewLODSplats;

        // Pre-cached resident GPU/CPU LOD buffers for zero-hitch instantaneous transitions
        std::vector<ResidentLOD> m_residentLODs;
        // The streaming runtime (libs/gstx/StreamRuntime.h): it owns the blocks, residency, delivery and
        // the render list; the app feeds it the levels above, its settings and the camera, and draws its output
        StreamRuntime m_stream;
        void SyncStreamInputs();   // The UI settings and the camera, into m_stream (every frame, and before any call into it)
        void ApplyStreamFrame();   // The runtime's render list and buffers, into the renderer state
        // The streaming update runs on its own thread (UpdateCamera): one at a time, started at most kStreamUpdateHz times a
        // second with that frame's settings and camera, while the frames go on drawing the last finished render list
        // (m_drawChunks). On the main thread it took 5-10 ms of one frame in four while moving, which showed as hitches at
        // 300 fps. The main thread changes the runtime only between updates: everything that does waits for the running one
        // first (WaitStreamJob, called by SyncStreamInputs and so by every call into the runtime). The UI reads its figures
        // while one runs: single values, and the blocks, which an update changes but never adds or removes.
        std::thread             m_streamThread;
        std::mutex              m_streamMutex;
        std::condition_variable m_streamCv;
        bool                    m_streamJobQueued = false, m_streamJobRunning = false, m_streamJobFinished = false, m_streamThreadQuit = false;
        double                  m_streamJobDt = 0.0;
        float                   m_streamJobMs = 0.0f;      // How long the last update took on its thread
        bool                    m_streamDirtyPending = false; // MarkStreamDirty: handed to the runtime with the next update
        std::vector<MeshletChunkGPU> m_drawChunks, m_drawChunksNext;   // The render list drawn, and the one the running update fills
        std::vector<StreamChunk*>    m_drawSources, m_drawSourcesNext; // Their blocks (m_rendererSourceChunks), for the UI and the click test
        void StartStreamJob(double dtSeconds);
        void StreamThreadMain();
        bool StreamJobBusy();      // An update is queued or running
        void CollectStreamJob();   // A finished update's render list, into the renderer state
        void WaitStreamJob();      // Waits for the running update and collects it
        void StopStreamThread();
        void MarkStreamDirty() { m_streamDirtyPending = true; } // The next update rebuilds the render list
        void SnapshotStreamFrame(std::vector<MeshletChunkGPU>& chunks, std::vector<StreamChunk*>& sources) const;
        XMFLOAT3 m_lodSortFrameMin = { 0, 0, 0 }; // The Morton frame every level is sorted in (PrecacheResidentLODs): the model's bounds
        XMFLOAT3 m_lodSortFrameMax = { 0, 0, 0 };
        void PrecacheResidentLODs();

        // Splat mode: the imported cloud's full Gaussians (SplatPoint::sourceIndex refers here), the
        // bake settings, and the encode parameters every level shares (see SplatCodec).
        SplatAttributeArray m_splatAttributes;
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
        // SH precision (bake parameter): bits per spherical-harmonics coefficient. Fewer bits snap the coefficients
        // to coarser steps inside their 8-bit slots (lossy, no format change), so Brotli packs them tighter.
        static constexpr uint8_t kSHPrecisionBits[4] = { 8, 6, 5, 4 };
        int      m_shPrecision = 0;
        uint8_t  m_shBandBits[3] = { 8, 8, 8 };     // What the bake requantises the SH to, per band: SH Precision's bits, or the RDO choice
        void     SetSHPrecision(int precision) { m_shPrecision = std::clamp(precision, 0, 3); for (uint8_t& b : m_shBandBits) b = SHBitsFor(m_shPrecision); }

        // ---- Rate Distortion Optimization: docs/design/gstx-rdo.md, phase 1 -------------------------------------------------------
        RdoAnalysisResult   m_rdo;                  // The last analysis (or a loaded profile's)
        uint64_t            m_rdoHash = 0;          // The scan it measured (BakeTuningCache hash)
        BakeParams          m_rdoParams;            // The bake it measured (levels and deadband decide its pyramid)
        RdoAnalysis::Target m_rdoTarget;
        int                 m_rdoChoice = -1;       // Index into m_rdo.points
        bool                m_rdoOverride = false;  // The SH settings are set by hand instead of by the target
        int                 m_rdoNetwork = 1;       // Streaming target's link: 0 3G, 1 4G, 2 5G, 3 custom
        std::unique_ptr<RdoProfile> m_pendingRdoProfile; // A profile waiting for its scan to finish opening
        bool   RdoStale() const { return !m_rdo.Valid() || m_rdoHash == 0 || m_rdoHash != m_sourceHash || m_rdoParams.maxLODs != m_bakedParams.maxLODs || m_rdoParams.deadbandMM != m_bakedParams.deadbandMM; }
        // The analysis an opened package carries (PackageRdoFooter): its scan is not loaded, so RdoStale cannot match the hash,
        // but it is the curve this package was encoded from
        bool   RdoFromPackage() const { return m_rdo.Valid() && !m_rdoPackagePath.empty() && m_rdoParams.maxLODs == m_bakedParams.maxLODs; }
        void   DrawRdoMenu();
        void   DrawRdoPlot();
        // scanPath: a .ply to analyse in place of the loaded scan (a package's own scan, read from disk by the workers and never
        // loaded into the view); its curve then belongs to the package on screen
        void   StartRdoAnalysis(bool quiet = false, const std::string& scanPath = std::string());
        void   ResetRdoAnalysis();                 // The graph's Reset Analysis: the analysis deleted, stored profiles too
        std::string FindPackageScan();             // The package's .ply in its folder, or one the user picks; empty = none
        bool   IsPackageOnScreen() const { return m_isLoadedFromSFLW && !m_packagePath.empty(); }
        // A target no encoding meets: Pick's closest (the fastest or smallest), or the best fidelity within half as much
        // again (RdoAnalysis::PickKeepQuality). -1 = not chosen yet: the menu warns, and Bake or Save asks before going on.
        int    m_rdoUnreachablePolicy = -1;
        bool   m_rdoAskUnreachable = false;             // Bake or Save is waiting on that choice (DrawRdoUnreachablePopup)
        int    PickFor(const RdoAnalysis::Target& t) const;
        int    PickForTarget() const { return PickFor(m_rdoTarget); }
        bool   TargetReachable() const { return RdoAnalysis::Reachable(m_rdo, m_rdoTarget, (double)m_sourceFileBytes); }
        std::string DescribeRdoPoint(int index) const; // "degree 1, 4 bits: full detail 7.6 s, 16.3 dB" for the target's mode
        void   DrawRdoUnreachableChoice();
        void   DrawRdoUnreachablePopup();
        // (An analysis started by itself on load was tried and dropped, 2026-10-27: it loads every core for about a
        // minute. Analyse, Bake and Save start it; a quiet analysis, StartRdoAnalysis(true), is kept for later use.)
        bool   PyramidPending() const;                   // Levels, deadband or chunk size changed since the bake (what the analysis measures)
        void   ApplyRdoChoice(int choice);   // The chosen candidate's SH settings become the bake's
        void   SaveRdoProfile();
        // Every analysis is kept in <root>/rdo-profiles: one identical to what would be measured (the scan's hash, the levels
        // and deadband, the analysis constants) is used instead of analysing; a new one is stored under a unique name
        RdoProfile  MakeRdoProfile() const;          // What Save Profile writes: the analysis, target and choice of the scan open
        std::string RdoProfileFolder() const { return GetProjectRootFolder() + "\\rdo-profiles"; }
        static bool FindRdoProfile(const std::string& folder, uint64_t hash, int maxLODs, float deadbandMM, RdoProfile& out, std::string& path);
        void   StoreRdoProfile();
        std::string m_lastRdoProfilePath;             // The profile last stored or reused (StoreRdoProfile, StartRdoAnalysis)
        std::string m_rdoPackagePath;                 // The package whose footer m_rdo came from (PackageRdoFooter); empty = not from a package
        void   AdoptRdoAnalysis(const RdoProfile& p); // A stored analysis becomes the curve; the target and SH settings stay as they are
        static std::string NewRdoProfilePath(const std::string& folder, const std::string& scan, int maxLODs, float deadbandMM); // <scan>_L<levels>_D<deadband>, numbered when taken
        // Re-encode's confirmation: which profile it will store or reuse, worked out in the background (the scan's hash)
        struct ReencodeProfilePlan { std::string path; bool reuse = false; };
        std::future<ReencodeProfilePlan> m_reencodeProfilePlan;
        ReencodeProfilePlan m_reencodeProfile;
        bool m_reencodeProfileReady = false;
        void StartReencodeProfilePlan();
        void   LoadRdoProfile();
        void   ApplyRdoProfile(const RdoProfile& profile);
        void   StepPendingRdoProfile();
        std::string m_rdoSourcePath;                // The scan m_rdo measured: a package is matched to its analysis through its source scan

        // ---- Streaming tab: the encoding on screen, checked against the selected network profile, and Re-encode -------
        // Each row of the Encoding panel may carry a warning (a triangle, with why and what a re-encode changes). Without an
        // RDO analysis of the scan, times are the whole model's; with one, a typical close view's, as the RDO target uses.
        enum EncodingRow { kEncDegree, kEncBits, kEncLevels, kEncChunks, kEncOcclusion, kEncFirstView, kEncFullDetail, kEncRowCount };
        struct EncodingReport
        {
            bool     valid = false, package = false, viewBased = false;
            uint32_t shDegree = 0;
            uint8_t  shBits[3] = { 8, 8, 8 };
            int      levels = 0;
            size_t   blocks = 0;               // Level 0 blocks: what view-first streaming can choose between
            bool     occlusion = false;
            double   packageBytes = 0.0, firstBytes = 0.0, fullBytes = 0.0;
            double   bandwidthMBps = 0.0;      // 0 = Full (uncapped)
            std::string profile;               // "4G (2 MB/s)"
            std::string warn[kEncRowCount];    // Empty = nothing to fix on that row
            // The row's severity, which colours its triangle and text: 1 a recommendation (yellow), 2 a suboptimal setting
            // (orange), 3 incompatible with the network profile's bitrate -- the time target is missed (red)
            int      severity[kEncRowCount] = {};
            // What a re-encode would change: -1 / 0 = keep
            int      recMaxLODs = -1;
            float    recChunk = 0.0f;
            bool     recOcclusion = false;
            int      rdoPick = -1;             // Index into m_rdo.points: the RDO choice for this network (analysis available)
            bool     needsAnalysis = false;    // No RDO analysis yet, and a warning that one would settle
            bool     Any() const { for (const auto& w : warn) if (!w.empty()) return true; return false; }
            // Whether a re-encode would write a different package. A warning no encoding of the scan can clear (the RDO
            // choice for this network is already the one on screen) is information only: re-encoding writes the same bits.
            bool     Actionable() const { return needsAnalysis || rdoPick >= 0 || recMaxLODs > 0 || recChunk > 0.0f || recOcclusion; }
        };
        EncodingReport AssessEncoding() const;
        void   DrawEncodingPanel();
        std::string m_packagePath;                  // The .gstx on screen (opened, or saved this session); empty = none
        mutable std::string m_packageSourceFor, m_packageSource; // FindSourceScan's cache: the package and its source scan
        std::string FindSourceScan() const;         // The package's source scan: beside it (.ply / .splat), or remembered in bake_tunings.json
        enum class Reencode { Idle, Confirm, Open, Structure, Analyse, Bake, Save, Reload };
        Reencode    m_reencode = Reencode::Idle;
        EncodingReport m_reencodeReport;            // What the re-encode fixes (taken when it was confirmed)
        std::string m_reencodeSource, m_reencodeOutput; // The scan it bakes from, and the package it writes (without .gstx)
        bool        m_reencodeReload = false;       // A package was on screen: open the new one when written
        void   StepReencode();
        // Bake or Save with a target (Rate Distortion Optimization) and no current analysis: bake, analyse, apply the target's pick,
        // bake again if that changed the harmonics, then write the package when Save started it. Analyse is never required.
        enum class AutoRdo { Idle, Bake, Analyse, Apply, Save };
        AutoRdo     m_autoRdo = AutoRdo::Idle;
        std::string m_autoRdoSavePath;                  // The package to write at the end; empty for a Bake
        bool   TargetUnmet() const;                     // A target governs the harmonics but the curve is not measured for this scan and bake
        void   StartAutoRdo(const std::string& savePath);
        void   StepAutoRdo();
        void   DrawRdoPlaceholder(bool canAnalyse, bool analysing, bool scanLoaded);
        void   FailReencode(const std::string& why);
        static uint8_t SHBitsFor(int precision) { return kSHPrecisionBits[std::clamp(precision, 0, 3)]; }
        // The bake keeps the SH Quality degree, but never more bands than the source carries: extra bands would
        // only store zeros (a source without harmonics, or a package baked at a lower degree).
        uint32_t m_sourceSHDegree = 3;               // Highest band the source's Gaussians carry (a package: its own degree)
        uint32_t BakeSHDegree() const { return std::min(SplatSHDegree(), m_sourceSHDegree); }
        static uint32_t SourceSHDegreeOf(const SplatAttributeArray& attributes); // Highest band with a non-zero coefficient
        void     DetectSourceSHDegree();            // Sets m_sourceSHDegree from m_splatAttributes (highest band with a non-zero coefficient)
        size_t   BytesPerRenderPoint() const { return m_splatMode ? (sizeof(PackedSplatGPU) + SplatCodec::SHRecordBytes(m_splatParams.shDegree)) : (m_enableQuantization ? sizeof(PackedPointGPU) : sizeof(SplatPoint)); }
        void     EncodeSplatLevel(const SplatPointArray& splats, uint32_t level, std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH) const;
        void     ApplySplatRenderState();

        // Progressive Network Streaming & Bandwidth Throttle Simulator




        float  m_neighborBufferMargin               = 1.35f;  // Frustum margin for pre-fetching local neighbours with Release Unused LOD Segments
        bool   m_enableDitheredTransitions  = true;   // Stochastic screen-space Bayer dithering for smooth LOD transitions
        int    m_streamingPolicy            = 0;      // Streaming policy: 0 Conservative (the default), 1 Greedy (StreamSettings::Policy); radio buttons under Release Unused LOD Segments
        int    m_objectStreamingPolicy      = 0;      // The policy last chosen while viewing an object: what objects load with (an environment loads Conservative)
        bool   m_releaseUnusedSegments      = true;   // On by default (user, 2026-10-26); scripted runs turn it off (they wait for the whole model). Streaming tab "Release Unused LOD Segments" (StreamRuntime::Settings::releaseUnusedSegments)
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
        // The slowest frame of each logged second (wall clock, and its stages), and how many frames ran over 8 and 16 ms:
        // hitches the means hide
        float  m_budgetWorstMs = 0.0f, m_budgetWorst[kBudgetCount] = {};
        float  m_budgetWorstPreUiMs = 0.0f, m_budgetWorstRenderMs = 0.0f, m_budgetWorstOwnMs = 0.0f; // Its time before the UI, in the renderer, in OnRender
        float  m_budgetWorstGapMs = 0.0f; // The longest time outside OnRender between two frames this second
        uint32_t m_budgetSlowFrames = 0, m_budgetVerySlowFrames = 0;
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
        float  m_arrivalGlowIntensity       = 0.25f;  // Refinement visualizer strength (0..2); 0.25 by default
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
        void  ResetView();                                                       // The nav switch's Reset View: default angle, centred, fitted
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
        // A scripted run (a test, a calibration, --bake-to, --capture-views): nobody is there to answer a dialog
        uint64_t m_sourceHash = 0;                  // Hash of the scan file loaded (BakeTuningCache): its bakes are remembered against it; 0 = a package, or nothing loaded
        bool   Unattended() const { return !m_lodTestFolder.empty() || !m_lodCalFolder.empty() || m_streamTest || !m_cliBakeOutput.empty() || !m_viewCapFolder.empty() || !m_camCapFolder.empty() || m_ringTest; }
        // --capture-views: level 0 from eight directions (plus the first again: the noise floor), written as .rgba files
        // and, with --reference, compared with another package's capture (StepViewCapture)
        std::string              m_viewCapFolder;
        std::string              m_viewCapReference;
        bool                     m_viewCapRunning = false;
        size_t                   m_viewCapIndex = 0;
        bool                     m_viewCapRequested = false;
        double                   m_viewCapStableSeconds = 0.0;
        uint64_t                 m_viewCapLastSplats = 0;
        float                    m_viewCapFit = 0.0f;
        std::vector<RgbaImage>   m_viewCapImages;
        void   StepViewCapture();
        void   FinishViewCapture();
        // Training cameras (TrainingCameras.h): the scan's cameras.json (beside the file loaded, or --cameras), viewed
        // through the renderer's pose camera. Any orbit input leaves the pose and orbits on from where it was.
        std::vector<TrainingCamera> m_trainingCameras;
        std::string m_trainingCamerasFor;           // The m_loadedFilePath they were looked up for
        std::string m_camerasOverride;              // --cameras <cameras.json>
        int      m_poseCameraIndex = -1;            // The training camera on screen; -1 = the orbit camera
        int      m_trainingCameraPick = 0;          // The Renderer tab's selection
        float    m_poseOrbitYaw = 0.0f, m_poseOrbitPitch = 0.0f, m_poseOrbitDistance = 0.0f;
        XMFLOAT3 m_poseOrbitTarget = { 0.0f, 0.0f, 0.0f };
        void   ViewTrainingCamera(int index);
        // The kind of scan loaded, decided once per file from level 0 (docs/design/environment-scenes.md): an environment
        // (a 3DGS capture of a place: a dense core in a far wider shell) or an object. An environment loads with the
        // occlusion volume off (for the session: enable_occlusion_volume stays the choice for objects), opens at training
        // camera 0 or inside its core, and does not auto-rotate.
        PackageView m_packageView;                  // The opened package's view settings (GSTXVEW1), when it carries them
        std::string m_sceneOverride;                // --scene environment|object: overrides the detection and the package
        bool        m_sceneIsEnvironment = false;
        std::string m_sceneClassFor;                // The m_loadedFilePath it was decided for
        XMFLOAT3    m_coreCentre = { 0.0f, 0.0f, 0.0f };
        float       m_coreRadius50 = 0.0f, m_coreRadius99 = 0.0f; // Radii about the core centre holding half and 99% of level 0
        // What an environment is of (PackageView's subject): where its training cameras converge, out to where level 0's
        // density falls to a tenth of the centre's; streamed first. Radius 0 = none.
        XMFLOAT3    m_subjectCentre = { 0.0f, 0.0f, 0.0f };
        float       m_subjectRadius = 0.0f;
        bool        m_noSubject = false;            // --no-subject: no boost (the measurement's baseline)
        int         m_cliOcclusion = -1;            // --occlusion on|off: the volume culls and depth-tests (or not) this run; -1 = the setting
        int         m_cliOpaqueSurface = -1;        // --opaque-front on|off: Opaque Front Surface this run; -1 = the setting
        // The background behind the model (Renderer tab, Background; config background_*). Recommended takes the package's
        // own (PackageView background) or, for a scan, BackgroundEstimate's; the others are the user's own and win over it.
        enum BackgroundChoice { BgRecommended = 0, BgBlack, BgStudioWhite, BgLightGrey, BgDarkGrey, BgCustom, BgGradient, BgImage, BgChoiceCount };
        int         m_backgroundChoice = BgRecommended;
        XMFLOAT3    m_bgCustom = { 0.5f, 0.5f, 0.5f };
        XMFLOAT3    m_bgGradientTop = { 0.45f, 0.62f, 0.85f };     // Sky
        XMFLOAT3    m_bgGradientBottom = { 0.85f, 0.88f, 0.92f };
        std::string m_bgImagePath;
        PackageBackground m_recommendedBackground;  // For the loaded file (ApplySceneDefaults); written into the package on save
        void        ApplyBackground();              // m_state's backdrop from the choice, every frame
        // The loading veil (State::loadingBlurPx): armed by a load, an Evict or a jump to a training camera; while armed,
        // the scene is blurred by up to 0.8% of the window height, easing out as the fill (an environment: its residency
        // ring at its levels; an object: the drawn blocks at theirs) goes from 40% to 75%, then disarmed until the next.
        bool        m_loadingBlurEnabled = false;  // Renderer tab, Loading Blur (config loading_blur); off by default
        bool        m_loadingBlurArmed = false;
        float       m_loadFill = 1.0f;              // 0..1, refreshed four times a second while armed
        double      m_loadFillTimer = 0.0;
        void        ArmLoadingBlur() { m_loadingBlurArmed = true; m_loadFill = 0.0f; m_loadFillTimer = 1.0; }
        void        UpdateLoadingBlur(const XMFLOAT3& eye);
        // --ring-test: an environment is evicted and refills at 5G for 30 s at its view, then the view turns a full circle in place over 24 s
        // (the eye still, as a right-drag look); every second the trace gives the residency ring's blocks resident at
        // their distance level, of all of them. Conservative with Release Unused, as an environment loads. Quits after.
        bool        m_ringTest = false;
        double      m_ringTestSeconds = 0.0;
        double      m_ringTestReport = 0.0;
        void        StepRingTest();
        static float SubjectRadius(const SplatPointArray& points, const XMFLOAT3& centre, float maxRadius);
        void   ApplySceneDefaults();                // Once per frame, after UpdatePoseCamera
        void   UpdatePoseCamera();                  // Once per frame, before the camera reaches the renderer and the stream
        // --camera-capture <folder> [--camera-list i,j,...]: once fully resident, each training camera as camera_NNN.png
        std::string      m_camCapFolder;
        std::vector<int> m_camCapList;
        size_t   m_camCapIndex = 0;
        bool     m_camCapRunning = false, m_camCapRequested = false;
        double   m_camCapStableSeconds = -1.0;
        uint64_t m_camCapLastSplats = 0;
        RgbaImage m_camCapImage;
        int      m_cliStartCamera = -1;             // --camera N: start at training camera N
        float    m_cliSpinDegPerSec = 0.0f;         // --spin D: the orbit turns D degrees a second on its own (motion benchmark)
        float    m_cliZoomPeriodSec = 0.0f;         // --zoom P: the orbit distance swings in and out once every P seconds (motion benchmark)
        float    m_cliZoomBaseDistance = 0.0f, m_cliZoomPhase = 0.0f;
        float    m_cliDistanceFactor = 0.0f;        // --distance F: the orbit distance is set to F x the framing distance once the model shows
        bool     m_cliViewSet = false;              // --view yaw,pitch[,lift] (degrees; lift x the model's height raises the target), once the model shows
        float    m_cliViewYaw = 0.0f, m_cliViewPitch = 0.0f, m_cliViewLift = 0.0f;
        int      m_cliOptLevel = -1;                // --opt-level N: the optimization slider's position (-1 = as it is)
        double   m_cliShownSeconds = 0.0;           // How long the fitted model has been on screen (--distance and --view wait for 2 s)
        bool     m_modelClickArmed = false;         // A left press began over the viewport: its release, if not a drag, may be a click on the model
        bool     ClickHitsModel(float x, float y) const;
        double   m_streamDtAccumMs = 0.0;           // Time since the stream's last update (it runs at most kStreamUpdateHz a second)
        bool     m_camCapLevel0 = false;            // --level0: the capture draws level 0 everywhere
        void   StepCameraCapture();
        // A level's refinement-visualizer tint (Splat.hlsl kArrivalGlowLevelHue, rotated by Glow Hue) at the given HSV
        // saturation and value: the Level tint legend and the residency graph share it
        ImVec4 LevelTint(int lod, float saturation, float value) const;
        std::string m_cliBakeOutput;         // --bake-to: bake the command-line scan, save the package here and quit (StepCliBake)
        bool   m_cliRemember = false;            // --remember-settings: the bake's settings are also remembered for the scan (BakeTuningCache)
        bool   m_noPrecompress = false;          // --no-precompress: no background compression after a bake (a measurement's baseline)
        void   StepCliBake();
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
        bool   m_loupeEnabled = false;               // Off by default (user, 2026-10-26); L toggles it, remembered in config (loupe_enabled)
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
        bool   m_rightPanelCollapsed = false;
        // Presentation Mode: its own panel under the analytics panel, folded like the side panels (present_panel_collapsed).
        // Each subsection is a list of presets, each setting the view, the visualizers and the loupe to demo one feature
        // (DrawPresentationPanel); the choice is shown but nothing is held: a preset applies once, when it is picked.
        ImVec4 m_presentPanelRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        bool   m_presentPanelCollapsed = false;
        static constexpr int kPresetNone = -1, kPresetClear = 0, kPresetPoints = 1, kPresetOutlines = 2, kPresetFull = 3, kPresetCulling = 4, kPresetBackOfHead = 5;
        int    m_presentSplatPreset = kPresetNone;
        int    m_presentStreamPreset = kPresetNone;
        int    m_presentOcclusionPreset = kPresetNone;
        void   ApplyBackOfHeadView();                // Occlusion - Back of the Head: the crown from behind, close up
        // Optimizations: a slider from brute force (0: every splat of the finest level, sorted every frame, full quads, no
        // culling, all on the main thread) to everything on (5), each step adding one group (ApplyOptimizationLevel)
        static constexpr int kOptLevels = 6;
        int    m_optLevel = kOptLevels - 1;
        bool   m_trimQuads = true, m_sortOnlyWhenMoved = true, m_frustumCull = true; // SplatRenderer::State's, set by the slider
        bool   m_streamOnThread = true;              // The streaming update on its own thread (off: on the main thread, as scripted runs)
        void   ApplyOptimizationLevel(int level);
        void   DrawPresentationPanel(float x, float width, float maxHeight);
        void   ApplyPresentationClear();             // Clear: no model, the launch view, the visualizers back to normal, no loupe
        void   ResetPresentationCamera();            // Live camera, no auto-rotate: what every preset but Culling starts from
        void   PlaceLoupeBottomRight();              // The loupe in the bottom-right corner of the viewport
        // Every preset switch saves a screenshot of the window once it has settled (a backup should a demo crash):
        // Pictures\SplatPak Presentation\<time> <subsection> - <preset>.png
        std::string m_presentShotName;
        double m_presentShotTimer = -1.0;            // Seconds until the screenshot is taken; negative = none pending
        bool   m_presentShotRequested = false;
        void   SchedulePresentationShot(const std::string& name) { m_presentShotName = name; m_presentShotTimer = 1.5; m_presentShotRequested = false; }
        void   StepPresentationShot();               // Once per frame, before the UI
        void   SavePresentationShot();               // After the frame's render, once the capture is in         // right_panel_collapsed): the space goes to the viewport and the loupe
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
        // Every other collapsing header's open state, by its fixed id (titles can change), saved in config.json (menu_open)
        // so menus reopen as left
        std::map<std::string, bool> m_menuOpen;
        bool   MenuHeader(const char* id, const char* title, bool defaultOpen = true); // CollapsingHeader with an open state remembered under id
        void  StepLodQualityTest();     // Once per frame, before the UI: moves the test along
        void  FinishLodQualityTest();   // Compares the captures and writes the report
    };
}
