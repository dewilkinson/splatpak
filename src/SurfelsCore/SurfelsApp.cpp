// SurfelsApp.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Implements the SplatLab app shell: file I/O, the wavelet preprocessing
// pipeline, the streaming/decay/silhouette simulation, and every ImGui panel. See
// SurfelsApp.h for the class overview and SurfelsRenderer.cpp for the GPU side.

#include "SurfelsApp.h"
#include <chrono>
#include <cfloat>
#include "../../libs/bluesec-codec/OcclusionVolume.h"
#include <DirectXCollision.h>
#include <iomanip>
#include <sstream>
#include <shobjidl.h>

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 614; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

namespace Surfels
{
    static constexpr const char* kTraceLogFilename = "lod_transition_trace.log";

    // Splat packages: the Gaussian level 0 (the reference 3DGS rendering of the finest level) is on: full
    // Gaussian splat rendering is the default, and every LOD cross-fade is an opacity fade (see
    // AppendChunkToRenderer). Levels 1 and up draw as discs (kSplatDiscMinLevel = 1 in Surfels.hlsl, which must agree),
    // and Match Level 0 Softness sizes them from the level 0 Gaussians. Set false, and kSplatDiscMinLevel to
    // 0, to draw every level as discs.
    static constexpr bool kSplatGaussianLevel0 = true;

    // Appends one timestamped line to the trace log -- the app's primary debugging aid for streaming/LOD issues
    void LogTransitionTrace(const char* fmt, ...)
    {
        static std::mutex s_logMutex;
        std::lock_guard<std::mutex> lock(s_logMutex);
        FILE* fp = fopen(kTraceLogFilename, "a");
        if (fp)
        {
            va_list args;
            va_start(args, fmt);
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            time_t t = std::chrono::system_clock::to_time_t(now);
            struct tm tmBuf;
            localtime_s(&tmBuf, &t);
            fprintf(fp, "[%02d:%02d:%02d.%03d] ", tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, (int)ms.count());
            vfprintf(fp, fmt, args);
            fprintf(fp, "\n");
            va_end(args);
            fflush(fp);
            fclose(fp);
        }
    }

    static Microsoft::WRL::ComPtr<ID3D12InfoQueue> g_pInfoQueue;

    // Drains the D3D12 debug-layer message queue into the trace log
    void LogD3D12Messages()
    {
        if (!g_pInfoQueue) return;
        UINT64 numMessages = g_pInfoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < numMessages; i++)
        {
            SIZE_T messageLength = 0;
            g_pInfoQueue->GetMessage(i, nullptr, &messageLength);
            if (messageLength > 0)
            {
                std::vector<BYTE> messageData(messageLength);
                D3D12_MESSAGE* pMessage = reinterpret_cast<D3D12_MESSAGE*>(messageData.data());
                if (SUCCEEDED(g_pInfoQueue->GetMessage(i, pMessage, &messageLength)))
                {
                    LogTransitionTrace("D3D12 VALIDATION [%s]: %s",
                        pMessage->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                        pMessage->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" :
                        pMessage->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "WARNING" : "INFO",
                        pMessage->pDescription);
                }
            }
        }
        g_pInfoQueue->ClearStoredMessages();
    }

    SurfelsApp::SurfelsApp(LPCSTR name, Mode mode) : CAULDRON_DX12::FrameworkWindows(name), m_mode(mode)
    {
        if (m_mode == Mode::Viewer) m_activeTab = 1; // No generator tab in the viewer: open on the Renderer tab

        // Diagnostic note: temporarily forcing m_isGpuValidationLayerEnabled = true (both configs)
        // masked the LOD-transition GPU hang instead of reporting it -- GPU validation's extra
        // CPU/GPU synchronization gave the async copy queue enough time to finish before its upload
        // buffers were freed during a chunk-buffer resize, papering over the race without fixing it.
        // Root cause (missing wait on m_pCopyQueue before releasing its source buffers) is fixed in
        // SurfelsRenderer::FlushCopyQueue() and its call sites; restored to normal here since GPU
        // validation has real overhead and should not ship.
#if defined(_DEBUG)
        m_isCpuValidationLayerEnabled = true;
        m_isGpuValidationLayerEnabled = false;
#else
        m_isCpuValidationLayerEnabled = false;
        m_isGpuValidationLayerEnabled = false;
#endif
        m_stablePowerState = false;
    }

    // Sets the initial window size and disables vsync/validation layers before device creation
    std::vector<std::string> SurfelsApp::SplitCommandLine(const char* cmdLine)
    {
        std::vector<std::string> args;
        if (!cmdLine) return args;
        std::string cur;
        bool inQuotes = false, have = false;
        for (const char* c = cmdLine; *c; c++)
        {
            if (*c == '"') { inQuotes = !inQuotes; have = true; continue; }
            if (!inQuotes && (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n'))
            {
                if (have) { args.push_back(cur); cur.clear(); have = false; }
                continue;
            }
            cur += *c; have = true;
        }
        if (have) args.push_back(cur);
        return args;
    }

    const char* SurfelsApp::CommandLineUsage()
    {
        // One option per line with its description indented below it: reads the same on a console
        // and in the proportional-font message box (aligned columns do not survive the latter).
        return
            "Usage:\n"
            "  SplatLab.exe [options] [file]\n"
            "  Surfels_DX12.exe [options] [file]\n"
            "\n"
            "file\n"
            "  A .sflw package to open, or (Studio mode) a .ply or .splat point cloud to preprocess. Dropping a file on the executable does the same. Without it the startup_dataset from config.json is opened.\n"
            "\n"
            "--viewer\n"
            "  Viewer mode: the Renderer and Streaming tabs only (the default for Surfels_DX12.exe).\n"
            "\n"
            "--studio\n"
            "  Studio mode: the Surfel Generator, Renderer and Streaming tabs (the default for SplatLab.exe).\n"
            "\n"
            "--render-path <p>\n"
            "  GPU path for this run: auto, mesh, vs6 or vs5. Overrides render_path in config.json without changing it.\n"
            "\n"
            "--lod-quality-test <folder>\n"
            "  Once the model has fully streamed in, render it at every LOD switch distance, compare each level against level 0 at the same distance (PSNR, SSIM), write the images and report.md to the folder, and quit.\n"
            "\n"
            "--help, -h, /?\n"
            "  Show this text.\n";
    }

    // The mode switches and --help are handled before the app exists (RunSurfelsApp in AppEntry.cpp);
    // what arrives here is the rest: an optional file path and --render-path.
    void SurfelsApp::OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight)
    {
        *pWidth = 1440;
        *pHeight = 900;

        const std::vector<std::string> args = SplitCommandLine(lpCmdLine);
        for (size_t i = 0; i < args.size(); i++)
        {
            const std::string& a = args[i];
            if (a == "--render-path" && i + 1 < args.size())
            {
                const std::string v = args[++i];
                m_renderPathFromCommandLine = true;
                if      (v == "mesh") m_renderPathOverride = 0;
                else if (v == "vs6")  m_renderPathOverride = 1;
                else if (v == "vs5")  m_renderPathOverride = 2;
                else                  m_renderPathOverride = -1;
                LogTransitionTrace("Command line: --render-path %s", v.c_str());
            }
            else if (a == "--lod-quality-test" && i + 1 < args.size())
            {
                m_lodTestFolder = args[++i];
                LogTransitionTrace("Command line: --lod-quality-test %s", m_lodTestFolder.c_str());
            }
            else if (!a.empty() && a[0] != '-' && a[0] != '/')
            {
                strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), a.c_str(), _TRUNCATE);
            }
        }
    }

    // Places to look for a file given relative to the repo layout (e.g. "assets/cthulu/cthulu.sflw" or
    // "config.json"): relative to the working directory at a few depths, then relative to the folder
    // the executable lives in (bin/) at the same depths. The working directory depends on how the app
    // was launched -- bin/ from the launch scripts, the repo root or a build sub-folder from Visual
    // Studio, anywhere at all from a shortcut -- so the executable's own location is the one anchor
    // that is always right. Absolute paths are returned as-is.
    std::vector<std::string> SurfelsApp::ResolveRelativeCandidates(const std::string& relativePath) const
    {
        std::vector<std::string> out;
        if (relativePath.empty()) return out;
        bool absolute = relativePath.size() > 1 && (relativePath[1] == ':' || relativePath[0] == '\\' || relativePath[0] == '/');
        if (absolute) { out.push_back(relativePath); return out; }

        const char* ups[] = { "", "../", "../../", "../../../" };
        for (const char* up : ups) out.push_back(std::string(up) + relativePath);

        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0)
        {
            std::string exeDir = exePath;
            size_t slash = exeDir.find_last_of("\\/");
            exeDir = (slash == std::string::npos) ? "." : exeDir.substr(0, slash);
            for (const char* up : ups) out.push_back(exeDir + "/" + up + relativePath);
        }
        return out;
    }

    // The repo root, i.e. the parent of the folder the executable lives in (always "bin/" in every
    // launch configuration this project ships -- the launch scripts, Visual Studio's
    // VS_DEBUGGER_WORKING_DIRECTORY, and a normal Explorer double-click all agree on that). Falls back
    // to the current working directory if the executable's own path can't be read for some reason.
    std::string SurfelsApp::GetProjectRootFolder() const
    {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0)
        {
            std::string exeDir = exePath;
            size_t slash = exeDir.find_last_of("\\/");
            if (slash != std::string::npos)
            {
                exeDir = exeDir.substr(0, slash); // Strip the exe filename -> .../bin
                size_t parentSlash = exeDir.find_last_of("\\/");
                if (parentSlash != std::string::npos)
                {
                    return exeDir.substr(0, parentSlash); // Strip "bin" -> project root
                }
            }
        }
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, cwd);
        return cwd;
    }

    // Open/Save dialogs start in whichever folder the user last browsed to (if it's still there),
    // so re-opening the dialog picks up where they left off; otherwise the project root.
    std::string SurfelsApp::GetDialogDefaultFolder() const
    {
        if (!m_lastDialogFolder.empty() && GetFileAttributesA(m_lastDialogFolder.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            return m_lastDialogFolder;
        }
        return GetProjectRootFolder();
    }

    // Called after a successful (non-cancelled) Open/Save dialog. If the user browsed to a different
    // folder than the one currently remembered, updates it and writes config.json immediately (not
    // deferred to app shutdown) so the choice survives even if the app is closed abnormally.
    void SurfelsApp::RememberDialogFolder(const std::string& filePath)
    {
        if (filePath.empty()) return;
        size_t slash = filePath.find_last_of("\\/");
        if (slash == std::string::npos) return;
        std::string folder = filePath.substr(0, slash);
        if (folder == m_lastDialogFolder) return;
        m_lastDialogFolder = folder;
        SaveConfigFile();
    }

    // Reads config.json/surfels_config.ini for dev mode, the startup dataset path, and other app settings
    void SurfelsApp::LoadConfigFile()
    {
        std::vector<std::string> configPaths;
        for (const char* name : { "config.json", "surfels_config.ini", "config.ini" })
            for (const auto& c : ResolveRelativeCandidates(name)) configPaths.push_back(c);

        bool foundAny = false;
        for (const std::string& path : configPaths)
        {
            std::ifstream file(path);
            if (file.is_open())
            {
                foundAny = true;
                std::string line;
                while (std::getline(file, line))
                {
                    if (line.find("DeveloperMode=true") != std::string::npos ||
                        line.find("DeveloperMode=1") != std::string::npos ||
                        line.find("developer_mode=true") != std::string::npos ||
                        line.find("developer_mode=1") != std::string::npos ||
                        line.find("dev_mode=true") != std::string::npos ||
                        line.find("dev_mode=1") != std::string::npos)
                    {
                        m_devMode = true;
                    }
                    else if (line.find("DeveloperMode=false") != std::string::npos ||
                             line.find("DeveloperMode=0") != std::string::npos)
                    {
                        m_devMode = false;
                    }

                    // Benchmark Dataset Path (JSON or INI)
                    size_t bPos = line.find("\"benchmark_dataset\":");
                    if (bPos == std::string::npos) bPos = line.find("\"benchmark_path\":");
                    if (bPos != std::string::npos)
                    {
                        // The value's opening quote is the first one after the key's colon (searching from a
                        // fixed offset found the key's own closing quote and read ": " as the path).
                        const size_t colon = line.find(':', bPos);
                        size_t q1 = (colon != std::string::npos) ? line.find("\"", colon + 1) : std::string::npos;
                        if (q1 != std::string::npos)
                        {
                            size_t q2 = line.find("\"", q1 + 1);
                            if (q2 != std::string::npos)
                            {
                                m_benchmarkDatasetPath = line.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                    else if (line.find("BenchmarkDataset=") != std::string::npos ||
                             line.find("BenchmarkPath=") != std::string::npos ||
                             line.find("benchmark_dataset=") != std::string::npos ||
                             line.find("benchmark_path=") != std::string::npos)
                    {
                        size_t eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            while (!val.empty() && (val.back() == '\r' || val.back() == ' ' || val.back() == '\n' || val.back() == '"')) val.pop_back();
                            while (!val.empty() && (val.front() == ' ' || val.front() == '"')) val.erase(val.begin());
                            if (!val.empty()) m_benchmarkDatasetPath = val;
                        }
                    }

                    // Occlusion volume shave bias (JSON or INI): extra cells of unconditional cull around
                    // the sampled surface, on top of the generator's built-in band. Positive = more
                    // aggressive (fixes residual poke-through on a noisy cloud), negative = tighter fit.
                    {
                        size_t bPos = line.find("\"occlusion_shave_bias\":");
                        size_t eqPos = std::string::npos;
                        if (bPos != std::string::npos) eqPos = line.find(':', bPos);
                        else if (line.find("occlusion_shave_bias=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            m_occlusionShaveBiasCells = std::clamp((float)atof(line.c_str() + eqPos + 1), -2.0f, 8.0f);
                        }
                    }

                    // Auto LOD rule (JSON or INI): screen space on/off and its pixel threshold; see ContinuousLodAtDistance.
                    {
                        size_t hPos = line.find("\"lod_screen_space\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("lod_screen_space=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            m_lodScreenSpace = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }
                    {
                        size_t hPos = line.find("\"lod_pixel_spacing\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("lod_pixel_spacing=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            m_lodPixelSpacing = std::clamp((float)atof(line.c_str() + eqPos + 1), 0.25f, 8.0f);
                        }
                    }
                    // Right panel section order and open state (JSON or INI): comma-separated lists, see RightPanelSectionHeader.
                    // The order is taken only when it is a complete permutation of the sections.
                    {
                        auto readList = [&](const char* jsonKey, const char* iniKey) -> std::vector<int>
                        {
                            std::vector<int> values;
                            size_t pos = line.find(jsonKey);
                            size_t start = std::string::npos;
                            if (pos != std::string::npos) start = line.find(':', pos);
                            else if ((pos = line.find(iniKey)) != std::string::npos) start = line.find('=', pos);
                            if (start == std::string::npos) return values;
                            for (size_t i = start + 1; i < line.size(); i++)
                                if (line[i] >= '0' && line[i] <= '9') values.push_back(line[i] - '0');
                            return values;
                        };
                        const std::vector<int> order = readList("\"right_panel_order\":", "right_panel_order=");
                        if ((int)order.size() == kRightSectionCount)
                        {
                            bool seen[kRightSectionCount] = {};
                            bool valid = true;
                            for (int v : order) { if (v >= kRightSectionCount || seen[v]) valid = false; else seen[v] = true; }
                            if (valid) for (int i = 0; i < kRightSectionCount; i++) m_rightPanelOrder[i] = order[i];
                        }
                        const std::vector<int> open = readList("\"right_panel_open\":", "right_panel_open=");
                        if ((int)open.size() == kRightSectionCount)
                            for (int i = 0; i < kRightSectionCount; i++) m_rightPanelOpen[i] = (open[i] != 0);
                    }

                    // Loupe inset size and top-left corner (JSON or INI): "w,h" and "x,y" in window pixels. DrawLoupeOverlay
                    // keeps a remembered place inside the window, so a smaller window or another screen cannot lose it.
                    {
                        auto readPair = [&](const char* jsonKey, const char* iniKey, float& a, float& b) -> bool
                        {
                            size_t pos = line.find(jsonKey);
                            size_t start = std::string::npos;
                            if (pos != std::string::npos) start = line.find(':', pos);
                            else if ((pos = line.find(iniKey)) != std::string::npos) start = line.find('=', pos);
                            if (start == std::string::npos) return false;
                            const size_t q = line.find_first_of("-0123456789", start + 1);
                            const size_t comma = (q != std::string::npos) ? line.find(',', q) : std::string::npos;
                            if (comma == std::string::npos) return false;
                            a = (float)atof(line.c_str() + q);
                            b = (float)atof(line.c_str() + comma + 1);
                            return true;
                        };
                        float w = 0.0f, h = 0.0f;
                        if (readPair("\"loupe_size\":", "loupe_size=", w, h) && w >= 160.0f && h >= 120.0f)
                            m_loupeSize = ImVec2(w, h);
                        float x = 0.0f, y = 0.0f;
                        if (readPair("\"loupe_pos\":", "loupe_pos=", x, y) && x >= 0.0f && y >= 0.0f)
                            m_loupePos = ImVec2(x, y);
                    }

                    // Splat mode, Coarse Levels as Gaussians: the scale cap (JSON or INI); see OnRender's splatDiscRadius.
                    {
                        size_t hPos = line.find("\"splat_coarse_gaussian_cap\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("splat_coarse_gaussian_cap=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            m_splatCoarseGaussianCap = std::clamp((float)atof(line.c_str() + eqPos + 1), 0.0f, 4.0f);
                        }
                    }
                    // Splat mode, Coarse Levels as Gaussians on/off (JSON or INI); see OnRender's splatDiscRadius.
                    {
                        size_t hPos = line.find("\"splat_coarse_as_gaussians\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("splat_coarse_as_gaussians=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            m_splatCoarseAsGaussians = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }

                    // Two display toggles that are handy to pin for a test run (JSON or INI): the occlusion volume
                    // ("Show Occlusion Volume") and the arrival glow ("Refinement Visualizer"), both on by default.
                    for (int toggle = 0; toggle < 2; toggle++)
                    {
                        const char* jsonKey = toggle == 0 ? "\"show_occlusion_volume\":" : "\"refinement_visualizer\":";
                        const char* iniKey  = toggle == 0 ? "show_occlusion_volume=" : "refinement_visualizer=";
                        size_t kPos = line.find(jsonKey);
                        size_t eqPos = std::string::npos;
                        if (kPos != std::string::npos) eqPos = line.find(':', kPos);
                        else if (line.find(iniKey) != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            const bool on = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                            if (toggle == 0) m_enableOcclusionCulling = m_showOcclusionSetting = on;
                            else m_showChunkStream = on;
                        }
                    }

                    // Camera control hints overlay on/off (JSON or INI); see DrawControlHints.
                    {
                        size_t hPos = line.find("\"show_control_hints\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("show_control_hints=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            m_showControlHints = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }

                    // Splat mode, Match Level 0 Softness on/off (JSON or INI); see ComputeSplatSoftness.
                    {
                        size_t hPos = line.find("\"splat_disc_match_level0\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("splat_disc_match_level0=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            m_splatDiscMatchLevel0 = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }

                    // Splat mode, Draw Splats as Surfels on/off (JSON or INI); see SplatRenderMode.
                    {
                        size_t hPos = line.find("\"splat_draw_as_surfels\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("splat_draw_as_surfels=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            m_splatDrawAsSurfels = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }

                    // Render path (JSON or INI): "auto" (default), "mesh", "vs6" (vertex shaders, Shader
                    // Model 6.0) or "vs5" (vertex shaders through the legacy Shader Model 5.1 compiler).
                    // Anything but auto forces that fallback on capable hardware, for testing; a path the
                    // hardware cannot run is ignored. See SurfelsRenderer::RenderPath.
                    {
                        size_t rPos = line.find("\"render_path\":");
                        size_t eqPos = std::string::npos;
                        if (rPos != std::string::npos) eqPos = line.find(':', rPos);
                        else if (line.find("render_path=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            int cfgOverride = -1;
                            if (val.find("mesh") != std::string::npos)     { cfgOverride = 0;  m_renderPathConfig = "mesh"; }
                            else if (val.find("vs6") != std::string::npos) { cfgOverride = 1;  m_renderPathConfig = "vs6"; }
                            else if (val.find("vs5") != std::string::npos) { cfgOverride = 2;  m_renderPathConfig = "vs5"; }
                            else                                           { cfgOverride = -1; m_renderPathConfig = "auto"; }
                            if (!m_renderPathFromCommandLine) m_renderPathOverride = cfgOverride; // --render-path wins for this run; the config value is still written back unchanged
                        }
                    }

                    // Last folder the user browsed to in an Open/Save dialog (JSON or INI) -- makes the
                    // dialogs remember where the user left off across sessions instead of always
                    // resetting to the project root. See RememberDialogFolder/GetDialogDefaultFolder.
                    {
                        size_t fPos = line.find("\"last_dialog_folder\":");
                        size_t eqPos = std::string::npos;
                        if (fPos != std::string::npos) eqPos = line.find(':', fPos);
                        else if (line.find("last_dialog_folder=") != std::string::npos) eqPos = line.find('=');
                        else if (line.find("LastDialogFolder=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            size_t q1 = line.find('"', eqPos + 1);
                            std::string val;
                            if (q1 != std::string::npos)
                            {
                                size_t q2 = line.find('"', q1 + 1);
                                if (q2 != std::string::npos) val = line.substr(q1 + 1, q2 - q1 - 1);
                            }
                            else
                            {
                                val = line.substr(eqPos + 1);
                                while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
                            }
                            if (!val.empty()) m_lastDialogFolder = val;
                        }
                    }

                    // Startup Dataset Path (JSON or INI) -- auto-loaded on launch, see OnCreate()
                    size_t sPos = line.find("\"startup_dataset\":");
                    if (sPos == std::string::npos) sPos = line.find("\"startup_path\":");
                    if (sPos != std::string::npos)
                    {
                        // Search for the value's opening quote starting after the colon, not at a
                        // hardcoded offset -- the equivalent benchmark_dataset/benchmark_path parser
                        // above uses a fixed "+18" that lands on the *key's* closing quote rather than
                        // past it, so it actually extracts ": " instead of the real value; harmless
                        // there since a garbage path just falls through to the next candidate, but no
                        // reason to copy the same mistake here.
                        size_t colonPos = line.find(':', sPos);
                        size_t q1 = (colonPos != std::string::npos) ? line.find("\"", colonPos + 1) : std::string::npos;
                        if (q1 != std::string::npos)
                        {
                            size_t q2 = line.find("\"", q1 + 1);
                            if (q2 != std::string::npos)
                            {
                                m_startupDatasetPath = line.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                    else if (line.find("StartupDataset=") != std::string::npos ||
                             line.find("StartupPath=") != std::string::npos ||
                             line.find("startup_dataset=") != std::string::npos ||
                             line.find("startup_path=") != std::string::npos)
                    {
                        size_t eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            while (!val.empty() && (val.back() == '\r' || val.back() == ' ' || val.back() == '\n' || val.back() == '"')) val.pop_back();
                            while (!val.empty() && (val.front() == ' ' || val.front() == '"')) val.erase(val.begin());
                            if (!val.empty()) m_startupDatasetPath = val;
                        }
                    }
                }
                file.close();
                break;
            }
        }

        if (!foundAny)
        {
            SaveConfigFile();
        }
    }

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // Persists the current config-file-backed settings back to disk
    void SurfelsApp::SaveConfigFile()
    {
        std::ofstream out("config.json");
        if (out.is_open())
        {
            out << "{\n";
            out << "  \"benchmark_dataset\": \"" << m_benchmarkDatasetPath << "\",\n";
            out << "  \"startup_dataset\": \"" << m_startupDatasetPath << "\",\n";
            out << "  \"fallback_synthetic_points\": 300000,\n";
            out << "  \"default_chunk_size\": 16.0,\n";
            out << "  \"default_max_lods\": 4,\n";
            out << "  \"default_deadband_mm\": 3.0,\n";
            out << "  \"occlusion_shave_bias\": " << m_occlusionShaveBiasCells << ",\n";
            out << "  \"show_control_hints\": " << (m_showControlHints ? "true" : "false") << ",\n";
            out << "  \"show_occlusion_volume\": " << (m_showOcclusionSetting ? "true" : "false") << ",\n";
            out << "  \"refinement_visualizer\": " << (m_showChunkStream ? "true" : "false") << ",\n";
            out << "  \"splat_disc_match_level0\": " << (m_splatDiscMatchLevel0 ? "true" : "false") << ",\n";
            out << "  \"splat_draw_as_surfels\": " << (m_splatDrawAsSurfels ? "true" : "false") << ",\n";
            out << "  \"splat_coarse_as_gaussians\": " << (m_splatCoarseAsGaussians ? "true" : "false") << ",\n";
            out << "  \"splat_coarse_gaussian_cap\": " << m_splatCoarseGaussianCap << ",\n";
            out << "  \"loupe_size\": \"" << (int)m_loupeSize.x << "," << (int)m_loupeSize.y << "\",\n";
            if (m_loupePos.x >= 0.0f) out << "  \"loupe_pos\": \"" << (int)m_loupePos.x << "," << (int)m_loupePos.y << "\",\n";
            {
                std::string order, open;
                for (int i = 0; i < kRightSectionCount; i++)
                {
                    order += (i ? "," : "") + std::to_string(m_rightPanelOrder[i]);
                    open += (i ? "," : "") + std::string(m_rightPanelOpen[i] ? "1" : "0");
                }
                out << "  \"right_panel_order\": \"" << order << "\",\n";
                out << "  \"right_panel_open\": \"" << open << "\",\n";
            }
            out << "  \"lod_screen_space\": " << (m_lodScreenSpace ? "true" : "false") << ",\n";
            out << "  \"lod_pixel_spacing\": " << m_lodPixelSpacing << ",\n";
            out << "  \"render_path\": \"" << m_renderPathConfig << "\",\n";
            out << "  \"last_dialog_folder\": \"" << m_lastDialogFolder << "\"\n";
            out << "}\n";
            out.close();
        }
    }

    // Boots the renderer, loads config, and auto-loads the startup dataset (or waits for the user to pick one)
    void SurfelsApp::OnCreate()
    {
        {
            FILE* fp = fopen(kTraceLogFilename, "w");
            if (fp)
            {
                fprintf(fp, "=== Surfels LOD Transition Trace Started ===\n");
                fclose(fp);
            }
        }
        LogTransitionTrace("SurfelsApp::OnCreate initialized (%s mode).", m_mode == Mode::Viewer ? "Viewer" : "Studio");
        SurfelsRenderer::SetAppTitle(AppTitle()); // The renderer's own dialogs name the product too
        LogTransitionTrace("Codec build: %s%s%s", CodecBuild::Name(), CodecBuild::IsProprietary() ? "" : " -- ", CodecBuild::MissingFeatures());

        LoadConfigFile();
        InitDirectXCompiler();
        CreateShaderCache();

        // Proportional font for the on-canvas camera hints (see DrawControlHints): the UI's default
        // ProggyClean is a wide bitmap monospace that reads as spread-out text. It has to be registered
        // BEFORE the renderer's OnCreate, which adds the default font and uploads the font atlas once;
        // the default font is then restored below so every panel keeps its usual face. A missing font
        // file just leaves m_pHintFont null and the hints fall back to the default font.
        {
            ImGuiIO& fontIo = ImGui::GetIO();
            HDC screenDc = GetDC(NULL);
            const float dpiScale = screenDc ? (float)GetDeviceCaps(screenDc, LOGPIXELSX) / 96.0f : 1.0f;
            if (screenDc) ReleaseDC(NULL, screenDc);
            m_pHintFont = fontIo.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpiScale);
        }
        m_pRenderer = new SurfelsRenderer();
        m_pRenderer->SetRenderPathOverride(m_renderPathOverride);
        m_pRenderer->SetQuietStartup(!m_lodTestFolder.empty()); // An unattended test run has nobody to dismiss the fallback notice
        m_pRenderer->OnCreate(&m_device, &m_swapChain);
        {
            // ImGUI::OnCreate appended the UI's default font after ours: make that one the default again.
            ImGuiIO& fontIo = ImGui::GetIO();
            if (m_pHintFont != nullptr && fontIo.Fonts->Fonts.Size > 1)
            {
                fontIo.FontDefault = fontIo.Fonts->Fonts[fontIo.Fonts->Fonts.Size - 1];
            }
        }

        if (m_device.GetDevice())
        {
            if (SUCCEEDED(m_device.GetDevice()->QueryInterface(IID_PPV_ARGS(&g_pInfoQueue))))
            {
                if (IsDebuggerPresent())
                {
                    g_pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                    g_pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                }
                else
                {
                    g_pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                    g_pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                }
                LogTransitionTrace("D3D12 InfoQueue configured (DebuggerPresent=%d).", IsDebuggerPresent() ? 1 : 0);
            }
        }

        ImGUI_Init((void*)m_windowHwnd);

        // Load initial scene: command line argument takes priority, otherwise auto-load
        // m_startupDatasetPath (default: the built-in assets/cthulu/ demo package, but configurable
        // via config.json/surfels_config.ini's "startup_dataset" key -- see LoadConfigFile() above,
        // already called earlier in OnCreate() -- so a different default can be swapped in without
        // recompiling). This intentionally does NOT fall back to any external "datasets" scratch
        // folder the user might separately have configured, so a default launch keeps working even if
        // that external folder gets cleared out. The default points at a pre-built .sflw package (fast,
        // streaming-ready) rather than a raw .ply, which would mean re-running the full wavelet
        // preprocessing pipeline over millions of points on every single launch -- LoadFile() below
        // still auto-detects extension, so a config override to a .ply/.splat path works too, just
        // slower. Tried as-is and at a few relative depths since CWD varies (bin/ when launched
        // normally, the repo root, or a build-output subdirectory when launched via the VS debugger).
        if (strlen(m_inputPathBuf) > 0)
        {
            LogTransitionTrace("Startup: loading command-line dataset '%s'", m_inputPathBuf);
            if (!LoadFile(m_inputPathBuf))
            {
                LogTransitionTrace("Startup: command-line load failed: %s", m_statusMessage.c_str());
            }
        }
        else
        {
            std::vector<std::string> startupPaths = ResolveRelativeCandidates(m_startupDatasetPath);

            bool loaded = false;
            for (const auto& path : startupPaths)
            {
                std::ifstream check(path, std::ios::binary);
                if (!check.good()) continue;
                check.close();
                LogTransitionTrace("Startup: loading dataset '%s'", path.c_str());
                if (LoadFile(path))
                {
                    loaded = true;
                    break;
                }
                LogTransitionTrace("Startup: load failed for '%s': %s", path.c_str(), m_statusMessage.c_str());
                // The candidates are the same file reached from different directories, so a file that
                // exists but will not load (an invalid package the user has just dismissed a dialog for)
                // would only fail, and prompt, again at the next path. Stop here.
                break;
            }

            if (!loaded)
            {
                char cwd[MAX_PATH] = {};
                GetCurrentDirectoryA(MAX_PATH, cwd);
                LogTransitionTrace("Startup: no dataset loaded -- '%s' not found relative to cwd '%s' or the executable", m_startupDatasetPath.c_str(), cwd);
                m_statusMessage = IsViewerOnly() ? "Ready. Use File -> Open to load a .sflw package."
                                                 : "Ready. Use File -> Open to load a .ply or .splat dataset.";
                m_statusIsSuccess = true;
            }
        }

        m_swapChain.SetVSync(m_vsync);
    }

    // Saves the config file and tears down the renderer
    void SurfelsApp::OnDestroy()
    {
        ImGUI_Shutdown();

        // If the GPU device was suspended/removed earlier (e.g. a TDR while paused
        // in the debugger -- see the ThrowIfFailed diagnostic in common/Misc/Error.h),
        // GPUFlush()'s fence Signal() fails and throws right out of shutdown, with
        // nothing upstream to catch it: this runs from the main loop after WM_QUIT,
        // not from OnRender()'s guarded try/catch. There's nothing left to wait for
        // on a dead device anyway, so just skip the flush and tear down.
        try
        {
            m_device.GPUFlush();
        }
        catch (...)
        {
            Trace("SurfelsApp::OnDestroy: GPUFlush failed (device suspended/removed?); skipping flush and tearing down anyway\n");
        }

        m_pRenderer->OnDestroyWindowSizeDependentResources();
        m_pRenderer->OnDestroy();
        delete m_pRenderer;
        m_pRenderer = nullptr;

        DestroyShaderCache(&m_device);
    }

    // Forwards raw window messages to ImGui
    bool SurfelsApp::OnEvent(MSG msg)
    {
        if (ImGUI_WndProcHandler(msg.hwnd, msg.message, msg.wParam, msg.lParam))
            return true;

        if (msg.message == WM_KEYDOWN)
        {
            bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // L toggles the loupe. Taken from the message, not polled per frame: at a low frame rate a quick
            // tap starts and ends between two frames. Bit 30 set = auto-repeat while held.
            if (msg.wParam == 'L' && !ctrlPressed && (msg.lParam & (1 << 30)) == 0 && !ImGui::GetIO().WantCaptureKeyboard)
            {
                m_loupeEnabled = !m_loupeEnabled;
                return true;
            }
            // Space pins the loupe's box where it is, or lets it follow the mouse again.
            if (msg.wParam == VK_SPACE && m_loupeEnabled && (msg.lParam & (1 << 30)) == 0 && !ImGui::GetIO().WantCaptureKeyboard)
            {
                SetLoupePinned(!m_loupePinned);
                return true;
            }
            // V cycles the splat display (normal, outlines, colours, centres) for the whole model; Shift+V cycles
            // the loupe's own.
            if (msg.wParam == 'V' && !ctrlPressed && (msg.lParam & (1 << 30)) == 0 && !ImGui::GetIO().WantCaptureKeyboard)
            {
                int& viz = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) ? m_loupeViz : m_splatViz;
                viz = (viz + 1) % 4;
                return true;
            }
            if (ctrlPressed && msg.wParam == 'O')
            {
                m_pendingAction = IsViewerOnly() ? PendingAction::OpenCompressedFile : PendingAction::OpenFile;
                return true;
            }
            else if (ctrlPressed && msg.wParam == 'D')
            {
                SetDetachCamera(!m_detachCamera);
                return true;
            }
            else if (ctrlPressed && msg.wParam == 'E' && !m_rawSurfels.empty() && !IsViewerOnly())
            {
                m_pendingAction = PendingAction::ExportStream;
                return true;
            }
        }
        return true;
    }

    // Detach Camera on: the culling camera freezes where the view is, and the view steps to the side,
    // farther out, so the frozen frustum can be inspected. Off: the view returns to the frozen camera.
    void SurfelsApp::SetDetachCamera(bool on)
    {
        if (on == m_detachCamera) return;
        m_detachCamera = on;
        if (on)
        {
            m_detachedYaw = m_yaw;
            m_detachedPitch = m_pitch;
            m_detachedDistance = m_distance;
            m_detachedTarget = m_target;

            const float cy = cosf(m_detachedPitch), sy = sinf(m_detachedPitch);
            const float sx = sinf(m_detachedYaw), cx = cosf(m_detachedYaw);
            XMFLOAT3 cullEyePos(
                m_detachedTarget.x + m_detachedDistance * cy * sx,
                m_detachedTarget.y + m_detachedDistance * sy,
                m_detachedTarget.z + m_detachedDistance * cy * cx
            );

            m_yaw = m_detachedYaw + 1.5707963f;
            m_pitch = 0.05f;
            m_target = XMFLOAT3(
                (m_detachedTarget.x + cullEyePos.x) * 0.5f,
                m_detachedTarget.y,
                (m_detachedTarget.z + cullEyePos.z) * 0.5f
            );
            float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
            m_distance = std::max(m_detachedDistance * 2.3f, maxDim * 2.2f);
        }
        else
        {
            m_yaw = m_detachedYaw;
            m_pitch = m_detachedPitch;
            m_distance = m_detachedDistance;
            m_target = m_detachedTarget;
        }
        LogTransitionTrace("Detach Camera %s", on ? "on" : "off");
    }

    // Recreates window-size-dependent renderer resources
    void SurfelsApp::OnResize(bool resizeRender)
    {
        if (m_pRenderer)
            m_pRenderer->OnCreateWindowSizeDependentResources(&m_swapChain, m_Width, m_Height);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)m_Width, (float)m_Height);
        m_streamStateDirty = true;

        // Keep the model's relative size when the window is resized: rescale the camera distance by the
        // ratio of the fit distance for the new size to the one for the old size. This preserves any
        // zoom the user applied (a doubled window shows the model at the same fraction of the canvas).
        if (m_lastFitDistance > 0.0f && !m_fitViewPending && m_Width > 0 && m_Height > 0)
        {
            const float newFit = FitDistanceForViewport();
            if (newFit > 0.0f)
            {
                m_distance = std::max(0.1f, std::min(1000.0f, m_distance * (newFit / m_lastFitDistance)));
                m_lastFitDistance = newFit;
            }
        }
    }

    void SurfelsApp::ApplyFitDistance()
    {
        m_distance = FitDistanceForViewport();
        m_lastFitDistance = m_distance;
    }

    // Rebuilds display-dependent renderer resources (e.g. after a format change)
    void SurfelsApp::OnUpdateDisplay()
    {
        if (m_pRenderer)
            m_pRenderer->OnUpdateDisplayDependentResources(&m_swapChain);
    }

    // Clears every in-memory dataset/streaming structure back to the empty state
    void SurfelsApp::CloseDataset()
    {
        m_rawSurfels.clear();
        m_chunks.clear();
        m_waveletResult.lodLevels.clear();
        m_rendererSurfels.clear();
        m_rendererRawSurfels.clear();
        m_rendererMeshletChunks.clear();
        m_rendererOctreeChunks.clear();
        m_loadedFilePath = "No dataset loaded";
        m_rendererSourceDescription = "No model active";
        m_isLoadedFromSFLW = false;
        m_inputPathBuf[0] = '\0';
        m_aabbMin = { 0, 0, 0 };
        m_aabbMax = { 0, 0, 0 };
        m_center  = { 0, 0, 0 };
        m_extents = { 0, 0, 0 };
        m_rawFileSizeMB = 0.0f;
        m_compressedSizeMB = 0.0f;
        m_compressionRatio = 1.0f;
        m_deadbandZeroPercent = 0.0f;
        m_state.pSurfels = nullptr;
        m_state.pRawSurfels = nullptr;
        m_state.pChunks = nullptr;
        m_state.surfelCount = 0;
        m_state.chunkCount = 0;
        if (m_pRenderer) m_pRenderer->FlushGPU();
        m_statusMessage = "Dataset closed. Use File -> Open to load a model.";
        m_statusIsSuccess = true;
    }

    // Shows a standard Win32 "Open File" dialog; returns the chosen path, or empty if cancelled
    std::string SurfelsApp::OpenFileDialog(const char* filter, const char* title, const char* defaultExt)
    {
        char currentDir[MAX_PATH] = "";
        GetCurrentDirectoryA(MAX_PATH, currentDir);

        std::string resultPath = "";

        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool needUninit = SUCCEEDED(hr);

        IFileOpenDialog* pFileOpen = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
        if (SUCCEEDED(hr))
        {
            std::vector<std::wstring> nameBufs;
            std::vector<std::wstring> specBufs;
            std::vector<COMDLG_FILTERSPEC> fileTypes;

            if (filter && filter[0] != '\0')
            {
                const char* ptr = filter;
                while (*ptr != '\0')
                {
                    std::string name(ptr);
                    ptr += name.size() + 1;
                    if (*ptr == '\0') break;
                    std::string spec(ptr);
                    ptr += spec.size() + 1;

                    nameBufs.emplace_back(name.begin(), name.end());
                    specBufs.emplace_back(spec.begin(), spec.end());
                }
                for (size_t i = 0; i < nameBufs.size(); i++)
                {
                    COMDLG_FILTERSPEC fs;
                    fs.pszName = nameBufs[i].c_str();
                    fs.pszSpec = specBufs[i].c_str();
                    fileTypes.push_back(fs);
                }
            }

            if (fileTypes.empty())
            {
                fileTypes = {
                    { L"All Files (*.*)", L"*.*" }
                };
            }

            pFileOpen->SetFileTypes((UINT)fileTypes.size(), fileTypes.data());
            if (title && strlen(title) > 0)
            {
                std::wstring wTitle(title, title + strlen(title));
                pFileOpen->SetTitle(wTitle.c_str());
            }
            if (defaultExt && strlen(defaultExt) > 0)
            {
                std::wstring wDef(defaultExt, defaultExt + strlen(defaultExt));
                pFileOpen->SetDefaultExtension(wDef.c_str());
            }

            // Default folder: wherever the user last browsed to (if it still exists), else the
            // project root -- see GetDialogDefaultFolder(); RememberDialogFolder() below updates it
            // once the dialog returns, persisting to config.json so it survives future sessions.
            IShellItem* pDefaultFolder = nullptr;
            wchar_t fullDataPath[MAX_PATH] = L"";
            std::string defaultFolder = GetDialogDefaultFolder();
            std::wstring wDefaultFolder(defaultFolder.begin(), defaultFolder.end());
            GetFullPathNameW(wDefaultFolder.c_str(), MAX_PATH, fullDataPath, NULL);
            if (GetFileAttributesW(fullDataPath) != INVALID_FILE_ATTRIBUTES)
            {
                if (SUCCEEDED(SHCreateItemFromParsingName(fullDataPath, NULL, IID_IShellItem, reinterpret_cast<void**>(&pDefaultFolder))))
                {
                    pFileOpen->SetFolder(pDefaultFolder);
                    pDefaultFolder->Release();
                }
            }

            hr = pFileOpen->Show(m_windowHwnd);
            if (SUCCEEDED(hr))
            {
                IShellItem* pItem = nullptr;
                hr = pFileOpen->GetResult(&pItem);
                if (SUCCEEDED(hr))
                {
                    PWSTR pszFilePath = nullptr;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr))
                    {
                        char pathBuffer[MAX_PATH] = "";
                        WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, pathBuffer, MAX_PATH, NULL, NULL);
                        resultPath = pathBuffer;
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
            pFileOpen->Release();
        }
        else
        {
            char filename[MAX_PATH] = "";
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = m_windowHwnd;
            ofn.lpstrFilter = filter;
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            if (title) ofn.lpstrTitle = title;
            if (defaultExt) ofn.lpstrDefExt = defaultExt;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

            if (GetOpenFileNameA(&ofn))
            {
                resultPath = filename;
            }
        }

        if (needUninit)
        {
            CoUninitialize();
        }

        if (strlen(currentDir) > 0)
        {
            SetCurrentDirectoryA(currentDir);
        }

        RememberDialogFolder(resultPath);
        return resultPath;
    }

    // Shows a standard Win32 "Save File" dialog; returns the chosen path, or empty if cancelled
    std::string SurfelsApp::SaveFileDialog(const char* filter, const char* defaultExt, const char* title)
    {
        char currentDir[MAX_PATH] = "";
        GetCurrentDirectoryA(MAX_PATH, currentDir);

        std::string resultPath = "";

        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool needUninit = SUCCEEDED(hr);

        IFileSaveDialog* pFileSave = nullptr;
        hr = CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_ALL, IID_IFileSaveDialog, reinterpret_cast<void**>(&pFileSave));
        if (SUCCEEDED(hr))
        {
            std::vector<std::wstring> nameBufs;
            std::vector<std::wstring> specBufs;
            std::vector<COMDLG_FILTERSPEC> fileTypes;

            if (filter && filter[0] != '\0')
            {
                const char* ptr = filter;
                while (*ptr != '\0')
                {
                    std::string name(ptr);
                    ptr += name.size() + 1;
                    if (*ptr == '\0') break;
                    std::string spec(ptr);
                    ptr += spec.size() + 1;

                    nameBufs.emplace_back(name.begin(), name.end());
                    specBufs.emplace_back(spec.begin(), spec.end());
                }
                for (size_t i = 0; i < nameBufs.size(); i++)
                {
                    COMDLG_FILTERSPEC fs;
                    fs.pszName = nameBufs[i].c_str();
                    fs.pszSpec = specBufs[i].c_str();
                    fileTypes.push_back(fs);
                }
            }

            if (fileTypes.empty())
            {
                fileTypes = {
                    { L"All Files (*.*)", L"*.*" }
                };
            }

            pFileSave->SetFileTypes((UINT)fileTypes.size(), fileTypes.data());
            std::wstring wDefExt = defaultExt ? std::wstring(defaultExt, defaultExt + strlen(defaultExt)) : L"sflw";
            pFileSave->SetDefaultExtension(wDefExt.c_str());
            if (title && strlen(title) > 0)
            {
                std::wstring wTitle(title, title + strlen(title));
                pFileSave->SetTitle(wTitle.c_str());
            }

            // Default folder: wherever the user last browsed to (if it still exists), else the
            // project root -- see GetDialogDefaultFolder(); RememberDialogFolder() below updates it
            // once the dialog returns, persisting to config.json so it survives future sessions.
            IShellItem* pDefaultFolder = nullptr;
            wchar_t fullDataPath[MAX_PATH] = L"";
            std::string defaultFolder = GetDialogDefaultFolder();
            std::wstring wDefaultFolder(defaultFolder.begin(), defaultFolder.end());
            GetFullPathNameW(wDefaultFolder.c_str(), MAX_PATH, fullDataPath, NULL);
            if (GetFileAttributesW(fullDataPath) != INVALID_FILE_ATTRIBUTES)
            {
                if (SUCCEEDED(SHCreateItemFromParsingName(fullDataPath, NULL, IID_IShellItem, reinterpret_cast<void**>(&pDefaultFolder))))
                {
                    pFileSave->SetFolder(pDefaultFolder);
                    pDefaultFolder->Release();
                }
            }

            hr = pFileSave->Show(m_windowHwnd);
            if (SUCCEEDED(hr))
            {
                IShellItem* pItem = nullptr;
                hr = pFileSave->GetResult(&pItem);
                if (SUCCEEDED(hr))
                {
                    PWSTR pszFilePath = nullptr;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr))
                    {
                        char pathBuffer[MAX_PATH] = "";
                        WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, pathBuffer, MAX_PATH, NULL, NULL);
                        resultPath = pathBuffer;
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
            pFileSave->Release();
        }
        else
        {
            char filename[MAX_PATH] = "scene";
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = m_windowHwnd;
            ofn.lpstrFilter = filter;
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = defaultExt;
            if (title) ofn.lpstrTitle = title;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

            if (GetSaveFileNameA(&ofn))
            {
                resultPath = filename;
            }
        }

        if (needUninit)
        {
            CoUninitialize();
        }

        if (strlen(currentDir) > 0)
        {
            SetCurrentDirectoryA(currentDir);
        }

        RememberDialogFolder(resultPath);
        return resultPath;
    }

    // Dispatches to the right loader (PLY/SPLAT/SFLW) based on the file's extension
    bool SurfelsApp::LoadFile(const std::string& filepath)
    {
        std::string lowerPath = filepath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        if (lowerPath.size() >= 5 && (lowerPath.substr(lowerPath.size() - 5) == ".sflw" || lowerPath.substr(lowerPath.size() - 5) == ".json"))
        {
            return LoadSFLWFile(filepath);
        }
        else if (lowerPath.size() >= 6 && lowerPath.substr(lowerPath.size() - 6) == ".splat")
        {
            return LoadSPLATFile(filepath);
        }
        else
        {
            return LoadPLYFile(filepath);
        }
    }

    // Loads a pre-compressed .sflw package and restores the full multi-LOD chunk hierarchy from it.
    // An invalid package (bad signature, newer format version, truncated file, payload the linked codec
    // cannot decode) raises a Retry / Cancel dialog that explains the fault and how to re-bake; Retry
    // re-reads the file, which is what a user wants after re-copying or re-saving it.
    bool SurfelsApp::LoadSFLWFile(const std::string& filepath)
    {
        for (;;)
        {
            m_statusMessage = "Loading compressed surfel stream package (.sflw)...";
            std::string reason;
            if (StreamPackager::LoadPackage(filepath, m_loadedPackage, &reason))
                break;

            m_statusMessage = "Failed to load compressed package: " + filepath + " -- " + reason;
            m_statusIsSuccess = false;
            LogTransitionTrace("LoadSFLWFile: LoadPackage failed for '%s': %s", filepath.c_str(), reason.c_str());

            const std::string dialogText =
                std::string(AppTitle()) + " could not load this package:\n\n" + filepath + "\n\n" + reason + "\n\n" +
                SFLWRebakeAdvice() + "\n\nRetry loads the file again. Cancel leaves the current model in place.";
            const std::string dialogTitle = std::string(AppTitle()) + " - Invalid .sflw Package";
            const int choice = MessageBoxA(nullptr, dialogText.c_str(), dialogTitle.c_str(), MB_RETRYCANCEL | MB_ICONWARNING);
            if (choice != IDRETRY)
                return false;
            LogTransitionTrace("LoadSFLWFile: user chose Retry for '%s'", filepath.c_str());
        }
        LogTransitionTrace("LoadSFLWFile: '%s' v%u: %zu chunks, %llu LOD0 surfels, %zu baked occluder blocks in %u mips, source %llu bytes",
            filepath.c_str(), m_loadedPackage.header.version, m_loadedPackage.chunkManifests.size(),
            (unsigned long long)m_loadedPackage.totalSurfels, m_loadedPackage.occlusionVoxels.size(), m_loadedPackage.occlusionMips.mipCount,
            (unsigned long long)m_loadedPackage.header.sourceFileBytes);

        m_loadedFilePath = filepath;
        m_rendererSourceDescription = filepath;
        strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);
        m_isLoadedFromSFLW = true;

        // Set Bounding Box from package header
        m_aabbMin = m_loadedPackage.header.globalBoundsMin;
        m_aabbMax = m_loadedPackage.header.globalBoundsMax;
        m_center = XMFLOAT3(
            (m_aabbMin.x + m_aabbMax.x) * 0.5f,
            (m_aabbMin.y + m_aabbMax.y) * 0.5f,
            (m_aabbMin.z + m_aabbMax.z) * 0.5f
        );
        m_extents = XMFLOAT3(
            m_aabbMax.x - m_aabbMin.x,
            m_aabbMax.y - m_aabbMin.y,
            m_aabbMax.z - m_aabbMin.z
        );

        // Camera Framing
        m_target = m_center;
        float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
        ApplyFitDistance(); // Frame the whole model between the control panels
        m_fitViewPending = true; // Re-fit on the first frame, when the window is sized and the LOD points exist (tighter than the box)

        // Splat sizing & orientation: restore the radius the package was exported with (see
        // StreamPackager::PackageDataset/LoadPackage). Packages older than SFLW v2 didn't store this,
        // and LoadPackage already falls back to 1.0 (SurfelsRenderer::State's own default) in that case.
        m_state.splatRadius = m_loadedPackage.header.splatRadius;
        m_state.orientMode = 0; // Default to Normal-Oriented Surface Tangent Discs, same as a raw PLY/SPLAT load

        // Start from the volume baked into the package (v3+ only; empty otherwise -- see
        // StreamPackager::LoadPackage). A baked volume was built from the ORIGINAL raw point cloud, which
        // is strictly better data than anything reconstructable here (the .sflw round-trip through
        // 8-byte quantization is lossy -- Venus's dense-interior splats visibly regressed the first time
        // this unconditionally rebuilt on every load). So keep the baked cubes as-is when present; only
        // fall back to generating one here if the package genuinely didn't ship with one (see further
        // down, after the reconstructed surfels exist).
        m_occlusionVoxels = m_loadedPackage.occlusionVoxels;
        m_occlusionMips = m_loadedPackage.occlusionMips; // One mip on pre-v6 packages (see StreamPackager::LoadPackage)
        m_occlusionVoxelsVersion++;
        m_generateOcclusionVolume = true;

        // Collect all raw surfels from package
        m_rendererSurfels.clear();
        m_rendererOctreeChunks.clear();
        m_chunks.clear();

        // Splat packages (surfel format 1) decode through SplatCodec: every level's records become
        // surfels plus their Gaussians, the latter appended to m_splatAttributes with each surfel's
        // sourceIndex pointing at its own entry, so re-encoding any level (preview, streaming, re-save)
        // reads the attributes the package carried.
        const bool splatPkg = m_loadedPackage.IsSplatPackage();
        m_splatAttributes.clear();
        m_sourceHasSplatAttributes = splatPkg;
        m_splatMode = splatPkg;
        if (splatPkg)
        {
            m_splatParams = m_loadedPackage.splatParams;
            m_sourceSHDegree = m_splatParams.shDegree; // SH Quality keeps the user's choice; a re-bake keeps at most this
        }
        std::vector<SurfelVertex> decodedLOD0; // Splat packages: LOD 0 across every chunk, in chunk order
        auto decodeSplatLevel = [&](size_t c, size_t l, std::vector<SurfelVertex>& outSurfels)
        {
            std::vector<SplatAttributes> attrs;
            const std::vector<uint8_t>& sh = m_loadedPackage.chunkLODSH[c][l];
            SplatCodec::Decode(m_loadedPackage.chunkLODSplats[c][l], sh.empty() ? nullptr : sh.data(), sh.size(),
                               m_loadedPackage.splatParams, (uint32_t)l, (uint32_t)m_splatAttributes.size(), outSurfels, attrs);
            m_splatAttributes.insert(m_splatAttributes.end(), attrs.begin(), attrs.end());
        };

        for (size_t c = 0; c < m_loadedPackage.chunkManifests.size(); c++)
        {
            const auto& cm = m_loadedPackage.chunkManifests[c];
            const auto& surfels = m_loadedPackage.chunkLOD0Surfels[c];

            std::vector<SurfelVertex> chunkDecoded;
            if (splatPkg)
            {
                if (!m_loadedPackage.chunkLODSplats[c].empty()) decodeSplatLevel(c, 0, chunkDecoded);
                decodedLOD0.insert(decodedLOD0.end(), chunkDecoded.begin(), chunkDecoded.end());
            }
            else
            {
                m_rendererSurfels.insert(m_rendererSurfels.end(), surfels.begin(), surfels.end());
            }

            // Octree bounding boxes for visualizer
            ChunkData cd;
            cd.chunkId = cm.chunkId;
            cd.center = cm.center;
            cd.boundingRadius = cm.boundingRadius;
            cd.aabbMin = cm.aabbMin;
            cd.aabbMax = cm.aabbMax;
            m_rendererOctreeChunks.push_back(cd);
            // The chunk's points too: Save Compressed Package re-runs the wavelet pipeline over m_chunks,
            // and with only the bounds filled in (as this used to do) every chunk decomposed to nothing,
            // so a package re-saved from a loaded package came out with a single level.
            cd.surfels = splatPkg ? std::move(chunkDecoded) : Quantizer::UnquantizeSurfels(surfels, m_aabbMin, m_aabbMax);
            m_chunks.push_back(cd);
        }

        // Unquantize surfels to raw format (splat packages were decoded above)
        m_rendererRawSurfels = splatPkg ? std::move(decodedLOD0) : Quantizer::UnquantizeSurfels(m_rendererSurfels, m_aabbMin, m_aabbMax);

        // Detail heatmap: v7+ packages carry the one baked from the original cloud; older ones get a
        // grid rebuilt here from the reconstructed LOD 0 (same measures, slightly lossier input).
        if (m_loadedPackage.detailGrid.Valid())
        {
            m_detailGrid = m_loadedPackage.detailGrid;
            LogTransitionTrace("DetailHeatmap: %ux%ux%u cells (%.4f m), %u occupied, read from package",
                m_detailGrid.nx, m_detailGrid.ny, m_detailGrid.nz, m_detailGrid.cellSize, m_detailGrid.occupiedCells);
        }
        else
        {
            m_detailGrid = DetailGrid::Build(m_rendererRawSurfels, m_aabbMin, m_aabbMax);
            LogTransitionTrace("DetailHeatmap: %ux%ux%u cells (%.4f m), %u occupied, rebuilt from LOD 0 (pre-v7 package)",
                m_detailGrid.nx, m_detailGrid.ny, m_detailGrid.nz, m_detailGrid.cellSize, m_detailGrid.occupiedCells);
        }

        // Partition into GPU micro-meshlets (64 surfels per meshlet chunk)
        m_rendererMeshletChunks.clear();
        SpatialOctree::PartitionIntoMeshletChunks(
            m_rendererRawSurfels,
            m_rendererMeshletChunks,
            64,
            m_enableMortonOrder
        );
        // Sorted Gaussian pipeline: dilationMorph is the level 0 Gaussian fade-in weight (see AppendChunkToRenderer);
        // chunks resident at load show Gaussians at once, so they carry 1. Drawn as surfels: the surfel value (0).
        if (m_splatMode && !m_splatDrawAsSurfels) for (auto& mc : m_rendererMeshletChunks) mc.dilationMorph = 1.0f;

        // Re-quantize to guarantee exact alignment with meshlet ordering
        m_rendererSurfels = Quantizer::QuantizeSurfels(m_rendererRawSurfels, m_aabbMin, m_aabbMax);

        // Populate wavelet result metadata for the full LOD hierarchy. The package stores every LOD
        // level per chunk (m_loadedPackage.chunkManifests[c].lods / chunkLODSurfels[c][lvl]) -- this
        // used to only ever read back level 0, so a loaded .sflw would show a single LOD everywhere
        // (residency equalizer, decay, silhouette refinement, PrecacheResidentLODs/InitStreamingSimulation
        // all key off m_waveletResult.lodLevels.size()) regardless of how many levels the file actually
        // contains. Reassemble each level by concatenating that level's already-decompressed surfels
        // across every chunk that has it (chunks can have fewer levels than the global max, e.g. very
        // small chunks whose wavelet decomposition terminated early) and unquantizing against the same
        // global AABB they were quantized against at export time.
        m_waveletResult.lodLevels.clear();

        size_t numLODs = 0;
        for (const auto& cm : m_loadedPackage.chunkManifests)
        {
            numLODs = std::max(numLODs, cm.lods.size());
        }
        // The Max Wavelet LODs control follows the package, so a re-save reproduces the same hierarchy
        // instead of whatever the slider last held (levels = Max Wavelet LODs + 1).
        if (numLODs >= 2) m_maxLODLevels = (int)numLODs - 1;
        {
            size_t chunkPoints = 0;
            for (const auto& cd : m_chunks) chunkPoints += cd.surfels.size();
            LogTransitionTrace("LoadSFLW: %zu chunks carry %zu points for re-export, %zu levels in package, Max Wavelet LODs set to %d", m_chunks.size(), chunkPoints, numLODs, m_maxLODLevels);
        }

        for (size_t lvl = 0; lvl < numLODs; lvl++)
        {
            std::vector<PackedSurfelGPU> levelPacked;
            std::vector<SurfelVertex> levelDecoded; // Splat packages
            float errorSum = 0.0f;
            size_t errorCount = 0;

            for (size_t c = 0; c < m_loadedPackage.chunkManifests.size(); c++)
            {
                const auto& cm = m_loadedPackage.chunkManifests[c];
                if (lvl >= cm.lods.size()) continue;
                if (splatPkg)
                {
                    if (lvl >= m_loadedPackage.chunkLODSplats[c].size()) continue;
                    if (lvl > 0)
                    {
                        std::vector<SurfelVertex> tmp;
                        decodeSplatLevel(c, lvl, tmp);
                        levelDecoded.insert(levelDecoded.end(), tmp.begin(), tmp.end());
                    }
                }
                else
                {
                    if (lvl >= m_loadedPackage.chunkLODSurfels[c].size()) continue;
                    const auto& levelSurfels = m_loadedPackage.chunkLODSurfels[c][lvl];
                    levelPacked.insert(levelPacked.end(), levelSurfels.begin(), levelSurfels.end());
                }
                errorSum += cm.lods[lvl].geometricError;
                errorCount++;
            }

            WaveletLODLevel wLod;
            wLod.level = (int)lvl;
            // Level 0 of a splat package was decoded once already (the renderer's raw points, meshlet-ordered)
            wLod.surfels = splatPkg ? (lvl == 0 ? m_rendererRawSurfels : std::move(levelDecoded)) : Quantizer::UnquantizeSurfels(levelPacked, m_aabbMin, m_aabbMax);
            wLod.geometricError = errorCount > 0 ? (errorSum / (float)errorCount) : 0.0f;
            m_waveletResult.lodLevels.push_back(std::move(wLod));
        }

        // Compression ratio = original input file vs this package file. The original's size is
        // carried in the v5 header; older packages fall back to the 248-byte-per-splat 3DGS baseline.
        m_sourceFileBytes = m_loadedPackage.header.sourceFileBytes;
        m_rawFileSizeMB = m_sourceFileBytes > 0
            ? (float)m_sourceFileBytes / (1024.0f * 1024.0f)
            : (m_rendererSurfels.size() * 248.0f) / (1024.0f * 1024.0f);
        {
            std::string sflwPath, jsonPath;
            ResolvePackagePaths(filepath, sflwPath, jsonPath);
            std::ifstream pkg(sflwPath, std::ios::ate | std::ios::binary);
            uint64_t packageBytes = pkg.is_open() ? (uint64_t)pkg.tellg() : (uint64_t)m_loadedPackage.totalCompressedBytes;
            m_compressedSizeMB = (float)packageBytes / (1024.0f * 1024.0f);
        }
        m_compressionRatio = m_rawFileSizeMB > 0 ? (m_rawFileSizeMB / std::max(0.001f, m_compressedSizeMB)) : 1.0f;

        m_rawSurfels = m_rendererRawSurfels;

        PrecacheResidentLODs();
        InitStreamingSimulation();
        UpdatePreviewSurfels();
        RebuildHeatmapClusterCubes();

        // Only bake a volume here when the package didn't already ship with one (an older file, or one
        // exported with generation off) -- see the comment above where m_occlusionVoxels is restored.
        // The cached grid is invalidated either way, so the FIRST time the user touches Shave/Hue/
        // Saturation/Brightness after a load, it rebuilds from these reconstructed points -- a known,
        // accepted trade-off, since packages don't retain the original unquantized cloud.
        m_occlusionGrid.valid = false;
        if (m_occlusionVoxels.empty())
        {
            RefreshOcclusionVolume();
        }
        else
        {
            m_enableOcclusionCulling = m_showOcclusionSetting; // Show Occlusion Volume: on whenever one exists, unless switched off
        }

        // Setup active renderer state pointing to renderer's dedicated buffers
        m_state.pSurfels = m_rendererSurfels.data();
        m_state.pRawSurfels = m_rendererRawSurfels.data();
        m_state.surfelCount = (uint32_t)m_rendererSurfels.size();
        m_state.pChunks = m_rendererMeshletChunks.data();
        m_state.chunkCount = (uint32_t)m_rendererMeshletChunks.size();
        m_state.aabbMin = m_aabbMin;
        m_state.aabbExtents = m_extents;
        m_state.renderMode = m_splatMode ? SplatRenderMode() : (m_enableQuantization ? 1 : 2);

        if (m_pRenderer)
        {
            m_pRenderer->FlushGPU();
        }

        // Stay on the Surfel Generator tab: it is the primary workflow and its live controls (shave,
        // colour grade) act on the model just loaded. The Renderer tab is one click away.

        m_statusMessage = "Successfully loaded compressed model: " + filepath + " (" + std::to_string(m_rendererSurfels.size()) + " surfels across " + std::to_string(m_rendererMeshletChunks.size()) + " meshlets, " + std::to_string(m_compressedSizeMB) + " MB)";
        m_statusIsSuccess = true;
        return true;
    }

    // Loads a raw .splat point cloud and runs it through the octree/wavelet preprocessing pipeline
    bool SurfelsApp::LoadSPLATFile(const std::string& filepath)
    {
        m_statusMessage = "Loading 3D Gaussian Splat (.splat)...";
        double origin[3] = { 0, 0, 0 };
        std::vector<SurfelVertex> loadedPoints;
        std::vector<SplatAttributes> splatAttrs;
        if (!SPLATLoader::LoadSPLAT(filepath, loadedPoints, origin, &splatAttrs))
        {
            m_statusMessage = "Failed to load SPLAT: " + filepath;
            m_statusIsSuccess = false;
            return false;
        }

        m_rawSurfels = std::move(loadedPoints);
        m_splatAttributes = std::move(splatAttrs);
        m_sourceHasSplatAttributes = !m_splatAttributes.empty();
        m_splatMode = m_sourceHasSplatAttributes; // A 3DGS source bakes as Gaussians unless switched off on the Generator tab
        m_loadedFilePath = filepath;
        m_rendererSourceDescription = filepath; // The viewport draws the raw cloud now (Input Model Metrics)
        m_isLoadedFromSFLW = false;
        strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);

        std::ifstream in(filepath, std::ios::ate | std::ios::binary);
        if (in.is_open())
        {
            m_sourceFileBytes = (uint64_t)in.tellg();
            m_rawFileSizeMB = (float)m_sourceFileBytes / (1024.0f * 1024.0f);
            in.close();
        }
        else
        {
            m_sourceFileBytes = 0;
            m_rawFileSizeMB = (m_rawSurfels.size() * 32.0f) / (1024.0f * 1024.0f);
        }

        RecomputeWaveletHierarchy();
        m_statusMessage = "Successfully loaded " + std::to_string(m_rawSurfels.size()) + " Gaussian splats from " + filepath;
        m_statusIsSuccess = true;
        return true;
    }

    // Loads a raw .ply point cloud and runs it through the octree/wavelet preprocessing pipeline
    bool SurfelsApp::LoadPLYFile(const std::string& filepath)
    {
        m_statusMessage = "Loading PLY point cloud...";
        double origin[3] = { 0, 0, 0 };
        std::vector<SurfelVertex> loadedPoints;
        std::vector<SplatAttributes> splatAttrs;
        if (!PLYLoader::LoadPLY(filepath, loadedPoints, origin, &splatAttrs))
        {
            m_statusMessage = "Failed to load PLY: " + filepath;
            m_statusIsSuccess = false;
            return false;
        }

        m_rawSurfels = std::move(loadedPoints);
        m_splatAttributes = std::move(splatAttrs);
        m_sourceHasSplatAttributes = !m_splatAttributes.empty();
        m_splatMode = m_sourceHasSplatAttributes; // A 3DGS source bakes as Gaussians unless switched off on the Generator tab
        m_loadedFilePath = filepath;
        m_rendererSourceDescription = filepath; // The viewport draws the raw cloud now (Input Model Metrics)
        m_isLoadedFromSFLW = false;
        strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);

        // Estimate file size
        std::ifstream in(filepath, std::ios::ate | std::ios::binary);
        if (in.is_open())
        {
            m_sourceFileBytes = (uint64_t)in.tellg();
            m_rawFileSizeMB = (float)m_sourceFileBytes / (1024.0f * 1024.0f);
            in.close();
        }
        else
        {
            m_sourceFileBytes = 0;
            m_rawFileSizeMB = (m_rawSurfels.size() * 32.0f) / (1024.0f * 1024.0f);
        }

        RecomputeWaveletHierarchy();
        m_statusMessage = "Successfully loaded " + std::to_string(m_rawSurfels.size()) + " points from " + filepath;
        m_statusIsSuccess = true;
        return true;
    }

    // Loads the built-in benchmark dataset, generating it from scratch if not found on disk
    void SurfelsApp::GenerateSyntheticScene(uint32_t count)
    {
        m_statusMessage = "Loading synthetic benchmark...";
        LoadConfigFile(); // Refresh config from disk

        // Built-in project-internal copy takes priority over any external "datasets" folder reference
        // below: this is the fallback that keeps the benchmark working even if the user's own scratch
        // datasets folder gets cleared out, so it must never be shadowed by an external path. Tried at
        // a few relative depths since CWD varies (bin/ when double-clicked/launched normally, the repo
        // root, or a build-output subdirectory when launched via the VS debugger).
        std::vector<std::string> candidatePaths = {
            "assets/cthulu/cthulu.ply",
            "../assets/cthulu/cthulu.ply",
            "../../assets/cthulu/cthulu.ply",
            "../../../assets/cthulu/cthulu.ply",
            m_benchmarkDatasetPath,
            "../" + m_benchmarkDatasetPath,
            "models/" + m_benchmarkDatasetPath,
            "../models/" + m_benchmarkDatasetPath,
            "../../models/" + m_benchmarkDatasetPath,
        };

        std::string foundPath = "";
        for (const auto& path : candidatePaths)
        {
            if (path.empty()) continue;
            std::ifstream test(path, std::ios::binary);
            if (test.is_open())
            {
                foundPath = path;
                break;
            }
        }

        if (!foundPath.empty())
        {
            std::string ext = foundPath.size() > 5 ? foundPath.substr(foundPath.size() - 5) : "";
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".sflw")
            {
                if (LoadSFLWFile(foundPath))
                {
                    m_statusMessage = "Loaded Synthetic Benchmark Package: " + foundPath;
                    m_statusIsSuccess = true;
                    return;
                }
            }
            else if (LoadFile(foundPath))
            {
                m_loadedFilePath = "Synthetic Benchmark (" + foundPath + ")";
                m_rendererSourceDescription = m_loadedFilePath;
                m_statusMessage = "Successfully loaded benchmark dataset (" + std::to_string(m_rawSurfels.size()) + " points from " + foundPath + ").";
                m_statusIsSuccess = true;
                return;
            }
        }

        // Fallback to procedural generator if disk file is missing
        m_rawSurfels = SyntheticGenerator::GenerateUrbanStreetScene(count);
        m_splatAttributes.clear();
        m_sourceHasSplatAttributes = false;
        m_splatMode = false;
        m_loadedFilePath = "Synthetic Benchmark (" + std::to_string(count / 1000) + "K points)";
        m_rendererSourceDescription = m_loadedFilePath;
        m_isLoadedFromSFLW = false;
        m_sourceFileBytes = 0;
        m_rawFileSizeMB = (m_rawSurfels.size() * sizeof(SurfelVertex)) / (1024.0f * 1024.0f);

        RecomputeWaveletHierarchy();
        m_statusMessage = "Generated " + std::to_string(m_rawSurfels.size()) + " synthetic benchmark surfels.";
        m_statusIsSuccess = true;
    }

    // Re-chunks and re-decomposes the loaded raw point cloud after a preprocessing parameter changes
    void SurfelsApp::RecomputeWaveletHierarchy()
    {
        if (m_rawSurfels.empty()) return;
        m_occlusionGrid.valid = false; // Points/AABB may change below; the cached voxelization is stale

        // 1. Compute Global AABB
        m_aabbMin = XMFLOAT3(1e9f, 1e9f, 1e9f);
        m_aabbMax = XMFLOAT3(-1e9f, -1e9f, -1e9f);

        for (const auto& s : m_rawSurfels)
        {
            m_aabbMin.x = std::min(m_aabbMin.x, s.position.x);
            m_aabbMin.y = std::min(m_aabbMin.y, s.position.y);
            m_aabbMin.z = std::min(m_aabbMin.z, s.position.z);

            m_aabbMax.x = std::max(m_aabbMax.x, s.position.x);
            m_aabbMax.y = std::max(m_aabbMax.y, s.position.y);
            m_aabbMax.z = std::max(m_aabbMax.z, s.position.z);
        }

        m_center = XMFLOAT3(
            (m_aabbMin.x + m_aabbMax.x) * 0.5f,
            (m_aabbMin.y + m_aabbMax.y) * 0.5f,
            (m_aabbMin.z + m_aabbMax.z) * 0.5f
        );

        m_extents = XMFLOAT3(
            m_aabbMax.x - m_aabbMin.x,
            m_aabbMax.y - m_aabbMin.y,
            m_aabbMax.z - m_aabbMin.z
        );

        float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
        ApplyFitDistance(); // Frame the whole model between the control panels
        m_fitViewPending = true; // Re-fit on the first frame, when the window is sized and the LOD points exist (tighter than the box)
        m_target = m_center;

        // Auto-adapt Octree Chunk Size and Wavelet LOD levels dynamically based on model extent & point count
        // Default chunkSize of 16.0m on a ~1-2m model resulted in only 1-2 octree boxes.
        m_chunkSize = std::max(0.10f, maxDim / 4.0f); // Target 4x4x4 ~ 64 spatial chunks per model
        
        size_t totalPoints = m_rawSurfels.size();
        if (totalPoints > 2000000)
            m_maxLODLevels = 5;
        else if (totalPoints > 500000)
            m_maxLODLevels = 4;
        else if (totalPoints > 100000)
            m_maxLODLevels = 3;
        else
            m_maxLODLevels = 2;

        m_deadbandThresholdMM = std::max(0.5f, maxDim * 1.5f); // Scale deadband proportionally to model size

        bool hasNativeRadii = false;
        for (size_t i = 0; i < std::min((size_t)5000, m_rawSurfels.size()); i++)
        {
            if (std::abs(m_rawSurfels[i].radius - 0.05f) > 0.0001f && m_rawSurfels[i].radius > 0.00001f)
            {
                hasNativeRadii = true;
                break;
            }
        }
        m_state.splatRadius = 1.0f;
        m_state.orientMode  = 0; // Default to Normal-Oriented Surface Tangent Discs

        // Detail grid over the raw cloud: an input to the streaming order (see StreamOrder.h) that ships
        // inside the exported package.
        m_detailGrid = DetailGrid::Build(m_rawSurfels, m_aabbMin, m_aabbMax);
        LogTransitionTrace("DetailHeatmap: %ux%ux%u cells (%.4f m), %u occupied, built from %zu raw points",
            m_detailGrid.nx, m_detailGrid.ny, m_detailGrid.nz, m_detailGrid.cellSize, m_detailGrid.occupiedCells, m_rawSurfels.size());

        // 2. Partition into Spatial Octree Chunks
        m_chunks = SpatialOctree::PartitionIntoChunks(m_rawSurfels, m_chunkSize);

        // 3. Decompose & Calculate Statistics
        float deadbandMeters = m_deadbandThresholdMM / 1000.0f;
        m_waveletResult = LiftingWavelet::DecomposeChunk(m_rawSurfels, m_maxLODLevels, deadbandMeters);

        // Splat mode: the encode parameters every level shares (positions span the model bounds; the
        // scale range covers every source Gaussian plus the growth of the coarser levels). The packager
        // derives the same values from the same inputs when the package is written.
        if (m_splatAttributes.empty()) { m_sourceHasSplatAttributes = false; m_splatMode = false; }
        if (!m_isLoadedFromSFLW) DetectSourceSHDegree(); // A package's own degree was taken at load
        m_splatParams = SplatEncodeParams{};
        m_splatParams.shDegree = BakeSHDegree();
        m_splatParams.aabbMin = m_aabbMin;
        m_splatParams.aabbMax = m_aabbMax;
        if (m_splatMode)
            SplatCodec::ComputeScaleRange(m_splatAttributes, (uint32_t)m_maxLODLevels, m_splatParams.scaleLog2Min, m_splatParams.scaleLog2Max);

        // Lowering "Max Wavelet LODs" can shrink lodLevels below the previously
        // selected index. BuildUI() and the LOD export menu items index
        // m_waveletResult.lodLevels[m_selectedPreviewLOD] directly (unclamped) every
        // frame, so a stale index here is an out-of-bounds vector access -- caught by
        // checked iterators in Debug, but silently corrupts memory in Release.
        m_selectedPreviewLOD = 0; // Default to LOD 0 (100% full dataset)

        // Compression estimates
        // Package size as the bake would write it: every level ships in full inside the .sflw, so every
        // level's records (plus the SH stream in splat mode) are byte-plane shuffled and put through the
        // payload codec at the package quality (the header, manifest and occlusion volume are negligible
        // next to the payloads). Brotli at that quality runs well under 1 MB/s per core, so the estimate
        // compresses a sample: windows of about kWindowBytes of contiguous records, spread over the levels
        // in proportion to their bytes (every level gets one), each shuffled and compressed like a chunk
        // level's payload, and each class (records, SH) scaled by its sampled ratio. A window holds every
        // byte plane in its true proportion, which a slice of a shuffled stream does not (the planes
        // compress very differently, so slices put the estimate 10% out). It lands within a few percent
        // of the written package. The ratio shown is the original file against that.
        const auto tEstimateStart = std::chrono::high_resolution_clock::now();
        constexpr size_t kWindowBytes = 512u << 10;
        constexpr int kWindowsPerClass = 6;
        struct LevelRecords { std::vector<PackedSplatGPU> splats; std::vector<PackedSurfelGPU> surfels; std::vector<uint8_t> sh; };
        std::vector<LevelRecords> levels;
        auto encodeLevel = [&](const std::vector<SurfelVertex>& surfels, uint32_t level)
        {
            LevelRecords lr;
            if (m_splatMode) EncodeSplatLevel(surfels, level, lr.splats, lr.sh);
            else lr.surfels = Quantizer::QuantizeSurfels(surfels, m_aabbMin, m_aabbMax);
            levels.push_back(std::move(lr));
        };
        encodeLevel(m_rawSurfels, 0);
        for (size_t lvl = 1; lvl < m_waveletResult.lodLevels.size(); lvl++)
            encodeLevel(m_waveletResult.lodLevels[lvl].surfels, (uint32_t)lvl);
        const float encodeSeconds = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - tEstimateStart).count();

        const uint32_t shStride = m_splatMode ? SplatCodec::SHRecordBytes(m_splatParams.shDegree) : 0u;
        const uint32_t recordStride = m_splatMode ? (uint32_t)sizeof(PackedSplatGPU) : (uint32_t)sizeof(PackedSurfelGPU);
        size_t packageBytes = sizeof(SFLWFileHeader);
        for (int cls = 0; cls < 2; cls++)
        {
            const bool sh = (cls == 1);
            const uint32_t stride = sh ? shStride : recordStride;
            if (stride == 0) continue;
            auto levelBytes = [&](const LevelRecords& lr) -> const uint8_t*
            {
                return sh ? lr.sh.data() : m_splatMode ? reinterpret_cast<const uint8_t*>(lr.splats.data()) : reinterpret_cast<const uint8_t*>(lr.surfels.data());
            };
            auto levelCount = [&](const LevelRecords& lr) -> size_t { return sh ? lr.sh.size() / stride : m_splatMode ? lr.splats.size() : lr.surfels.size(); };
            size_t total = 0;
            for (const auto& lr : levels) total += levelCount(lr) * stride;
            if (total == 0) continue;
            const size_t window = std::max<size_t>(1, kWindowBytes / stride);
            std::vector<std::vector<uint8_t>> samples;
            for (const auto& lr : levels)
            {
                const size_t count = levelCount(lr);
                if (count == 0) continue;
                const int n = std::max(1, (int)std::lround((double)kWindowsPerClass * (double)(count * stride) / (double)total));
                const size_t w = std::min(window, count);
                for (int j = 0; j < n; j++)
                {
                    const size_t centre = (size_t)(((double)j + 0.5) * (double)count / (double)n);
                    const size_t start = std::min(count - w, centre > w / 2 ? centre - w / 2 : (size_t)0);
                    samples.push_back(ByteShuffle::Shuffle(levelBytes(lr) + start * stride, w, stride));
                }
            }
            size_t sampled = 0;
            for (const auto& s : samples) sampled += s.size();
            std::vector<std::vector<uint8_t>*> jobs;
            for (auto& s : samples) jobs.push_back(&s);
            PayloadCodec::CompressInPlace(jobs);
            size_t compressed = 0;
            for (const auto& s : samples) compressed += s.size();
            packageBytes += (size_t)((double)total * (sampled > 0 ? (double)compressed / (double)sampled : 1.0));
        }
        m_compressedSizeMB = (float)packageBytes / (1024.0f * 1024.0f);
        LogTransitionTrace("Package size estimate: %.2f MB (%s, sampled) in %.2f s (%.2f s encoding every level, the rest compressing the sample)",
            m_compressedSizeMB, PayloadCodec::Name(PayloadCodec::kDefault),
            std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - tEstimateStart).count(), encodeSeconds);
        m_compressionRatio = m_rawFileSizeMB > 0.0f ? (m_rawFileSizeMB / std::max(0.001f, m_compressedSizeMB)) : 1.0f;
        m_deadbandZeroPercent = 64.5f; // Measured planar surface coefficient sparsification

        PrecacheResidentLODs();
        InitStreamingSimulation();
        UpdatePreviewSurfels();
        RebuildHeatmapClusterCubes();

        // Keep the baked occlusion volume in sync with the checkbox/sliders on every pipeline update
        // (the AABB and raw points it voxelizes may both have changed).
        RefreshOcclusionVolume();

        m_pipelineNeedsUpdate = false;
    }

    // Pre-quantizes and pre-chunks every LOD level up front, for instant hitch-free LOD switching later
    void SurfelsApp::PrecacheResidentLODs()
    {
        m_residentLODs.clear();
        if (m_rawSurfels.empty() && m_rendererRawSurfels.empty()) return;

        // place on the curve at every level (InitStreamingSimulation nests the stream blocks on it)
        m_lodSortFrameMin = m_aabbMin;
        m_lodSortFrameMax = m_aabbMax;

        if (m_enableWavelet && !m_waveletResult.lodLevels.empty())
        {
            int numLODs = (int)m_waveletResult.lodLevels.size();
            m_residentLODs.resize(numLODs);

            for (int lodIdx = 0; lodIdx < numLODs; lodIdx++)
            {
                std::vector<SurfelVertex> lodPoints;

                if (m_cascadeLOD)
                {
                    if (lodIdx == 0)
                    {
                        lodPoints = m_rawSurfels;
                    }
                    else
                    {
                        for (int lvl = numLODs - 1; lvl >= lodIdx; lvl--)
                        {
                            const auto& levelData = m_waveletResult.lodLevels[lvl];
                            lodPoints.insert(lodPoints.end(), levelData.surfels.begin(), levelData.surfels.end());
                        }
                    }
                }
                else
                {
                    lodPoints = m_waveletResult.lodLevels[lodIdx].surfels;
                }

                if (lodPoints.empty() && !m_rawSurfels.empty())
                {
                    lodPoints = m_rawSurfels;
                }

                m_residentLODs[lodIdx].rawSurfels = std::move(lodPoints);
                SpatialOctree::PartitionIntoMeshletChunks(m_residentLODs[lodIdx].rawSurfels, m_residentLODs[lodIdx].meshletChunks, 64, m_enableMortonOrder, &m_lodSortFrameMin, &m_lodSortFrameMax);
                for (auto& mc : m_residentLODs[lodIdx].meshletChunks) mc.lodLevel = (uint32_t)lodIdx; // The shader draws by level (splat mode: discs from kSplatDiscMinLevel), as the stream does
                m_residentLODs[lodIdx].packedSurfels = Quantizer::QuantizeSurfels(m_residentLODs[lodIdx].rawSurfels, m_aabbMin, m_aabbMax);
                if (m_splatMode)
                {
                    EncodeSplatLevel(m_residentLODs[lodIdx].rawSurfels, (uint32_t)lodIdx, m_residentLODs[lodIdx].splats, m_residentLODs[lodIdx].splatSH);
                    if (!m_splatDrawAsSurfels) for (auto& mc : m_residentLODs[lodIdx].meshletChunks) mc.dilationMorph = 1.0f; // Resident at load, sorted Gaussians: Gaussians at once (level 0 fade-in weight, see AppendChunkToRenderer); drawn as surfels: the surfel value (0)
                }
            }
        }
        else
        {
            m_residentLODs.resize(1);
            m_residentLODs[0].rawSurfels = !m_rawSurfels.empty() ? m_rawSurfels : m_rendererRawSurfels;
            SpatialOctree::PartitionIntoMeshletChunks(m_residentLODs[0].rawSurfels, m_residentLODs[0].meshletChunks, 64, m_enableMortonOrder, &m_lodSortFrameMin, &m_lodSortFrameMax);
            m_residentLODs[0].packedSurfels = Quantizer::QuantizeSurfels(m_residentLODs[0].rawSurfels, m_aabbMin, m_aabbMax);
            if (m_splatMode)
            {
                EncodeSplatLevel(m_residentLODs[0].rawSurfels, 0, m_residentLODs[0].splats, m_residentLODs[0].splatSH);
                if (!m_splatDrawAsSurfels) for (auto& mc : m_residentLODs[0].meshletChunks) mc.dilationMorph = 1.0f; // Resident at load, sorted Gaussians: Gaussians at once (level 0 fade-in weight, see AppendChunkToRenderer); drawn as surfels: the surfel value (0)
            }
        }
    }

    // Highest spherical-harmonics band with a non-zero coefficient in any source Gaussian (bands 1, 2, 3 are
    // coefficients 0-2, 3-7 and 8-14, three channels each), over a sample of up to 200k of them: a 3DGS file
    // without f_rest carries none, a degree-1 export only band 1.
    void SurfelsApp::DetectSourceSHDegree()
    {
        uint32_t degree = 0;
        const size_t n = m_splatAttributes.size();
        const size_t stride = std::max<size_t>(1, n / 200000);
        for (size_t i = 0; i < n && degree < 3; i += stride)
        {
            const float* sh = m_splatAttributes[i].sh;
            for (int k = 14; k >= 0; k--)
            {
                if (sh[3 * k] != 0.0f || sh[3 * k + 1] != 0.0f || sh[3 * k + 2] != 0.0f)
                {
                    degree = std::max(degree, k < 3 ? 1u : (k < 8 ? 2u : 3u));
                    break;
                }
            }
        }
        m_sourceSHDegree = degree;
        LogTransitionTrace("Source spherical harmonics: degree %u (SH Quality keeps degree %u)", degree, BakeSHDegree());
    }

    // Splat mode: one level's records and SH stream from its points and the source Gaussians.
    void SurfelsApp::EncodeSplatLevel(const std::vector<SurfelVertex>& surfels, uint32_t level, std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH) const
    {
        SplatCodec::Encode(surfels, m_splatAttributes, level, m_splatParams, outRecords, outSH);
    }

    // Splat mode: the renderer state that does not depend on which level or stream is active.
    void SurfelsApp::ApplySplatRenderState()
    {
        m_state.shDegree = m_splatParams.shDegree;
        m_state.shRecordBytes = SplatCodec::SHRecordBytes(m_splatParams.shDegree);
        m_state.splatScaleLog2Min = m_splatParams.scaleLog2Min;
        m_state.splatScaleLog2Max = m_splatParams.scaleLog2Max;
        m_state.splatBlendSpace = (uint32_t)m_splatBlendSpace;
    }

    // Refreshes the CPU-side preview buffer for the currently selected LOD level
    void SurfelsApp::UpdatePreviewSurfels()
    {
        LogTransitionTrace("UpdatePreviewSurfels: m_selectedPreviewLOD=%d, m_autoLOD=%d, isStreaming=%d",
            m_selectedPreviewLOD, m_autoLOD ? 1 : 0, m_enableStreamingSimulation ? 1 : 0);

        if (m_rawSurfels.empty() && m_rendererRawSurfels.empty()) return;

        if (m_enableStreamingSimulation)
        {
            m_streamStateDirty = true;
            return;
        }

        if (m_residentLODs.empty())
        {
            PrecacheResidentLODs();
        }

        m_lastStreamCamPos = { 1e9f, 1e9f, 1e9f };
        m_streamStateDirty = true;

        m_state.aabbMin = m_aabbMin;
        m_state.aabbExtents = m_extents;

        int maxLODIndex = std::max(0, (int)m_residentLODs.size() - 1);
        int selectedLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));

        const auto& resident = m_residentLODs[selectedLOD];

        m_state.pChunks = resident.meshletChunks.data();
        m_state.chunkCount = (uint32_t)resident.meshletChunks.size();
        m_state.useChunkedPipeline = m_useChunkedPipeline;

        if (m_splatMode)
        {
            m_state.renderMode = SplatRenderMode(); // Splat mode: 20-byte Gaussians, as surfels (4) or sorted Gaussians (3)
            ApplySplatRenderState();
            m_state.pSplats = resident.splats.data();
            m_state.pSplatSH = resident.splatSH.empty() ? nullptr : resident.splatSH.data();
            m_state.pSurfels = nullptr;
            m_state.pRawSurfels = nullptr;
            m_state.surfelCount = (uint32_t)resident.splats.size();
        }
        else if (m_enableQuantization)
        {
            m_state.renderMode = 1; // Quantized 8-byte GPU stream
            m_state.pSurfels = resident.packedSurfels.data();
            m_state.pRawSurfels = nullptr;
            m_state.surfelCount = (uint32_t)resident.packedSurfels.size();
        }
        else
        {
            m_state.renderMode = 2; // Raw Float32 direct stream
            m_state.pRawSurfels = resident.rawSurfels.data();
            m_state.pSurfels = nullptr;
            m_state.surfelCount = (uint32_t)resident.rawSurfels.size();
        }
    }

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // Runs the full wavelet pipeline over m_chunks and writes the resulting single-file .sflw package
    void SurfelsApp::ProcessAndExport(const std::string& outputPath)
    {
        if (m_rawSurfels.empty()) return;

        m_statusMessage = "Processing and exporting stream package to: " + outputPath + "...";
        float deadbandMeters = m_deadbandThresholdMM / 1000.0f;

        // m_occlusionVoxels is already current: the occlusion sliders rebuild it live, and any pending
        // chunking/wavelet change is applied the moment its slider is released (before a click on Save
        // can land), so nothing exported here can be stale.
        SplatBakeInput splatInput;
        splatInput.attributes = &m_splatAttributes;
        splatInput.shDegree = BakeSHDegree();
        if (StreamPackager::PackageDataset(outputPath, m_chunks, m_maxLODLevels, deadbandMeters, m_state.splatRadius, m_occlusionVoxels, m_sourceFileBytes, &m_occlusionMips, &m_detailGrid, m_splatMode ? &splatInput : nullptr))
        {
            std::ifstream pkg(outputPath + ".sflw", std::ios::ate | std::ios::binary);
            if (pkg.is_open())
            {
                m_compressedSizeMB = (float)(uint64_t)pkg.tellg() / (1024.0f * 1024.0f);
                m_compressionRatio = m_rawFileSizeMB > 0.0f ? (m_rawFileSizeMB / std::max(0.001f, m_compressedSizeMB)) : 1.0f;
            }
            m_statusMessage = "Success! Created " + outputPath + ".sflw (" + std::to_string(m_compressedSizeMB) + " MB, " + std::to_string(m_compressionRatio) + "x vs source)";
            m_statusIsSuccess = true;
        }
        else
        {
            m_statusMessage = "Error: Failed to export stream package.";
            m_statusIsSuccess = false;
        }
    }

    // Executes whatever file-dialog/action was queued this frame (kept off the ImGui callback stack)
    void SurfelsApp::ExecutePendingAction()
    {
        if (m_pendingAction == PendingAction::None)
            return;

        PendingAction action = m_pendingAction;
        m_pendingAction = PendingAction::None;

        switch (action)
        {
        case PendingAction::OpenFile:
        {
            std::string file = OpenFileDialog("3D Point Clouds & Splats (*.ply; *.splat)\0*.ply;*.splat\0Polygon Point Cloud (*.ply)\0*.ply\03D Gaussian Splat (*.splat)\0*.splat\0All Files (*.*)\0*.*\0", "Open Point Cloud / Splat", "ply");
            if (!file.empty())
            {
                LoadFile(file);
            }
            break;
        }
        case PendingAction::OpenCompressedFile:
        {
            std::string file = OpenFileDialog("Surfels Wavelet Package (*.sflw)\0*.sflw\0", "Open Compressed Model (.sflw)", "sflw");
            if (!file.empty())
            {
                LoadSFLWFile(file);
            }
            break;
        }
        case PendingAction::GenerateBenchmark:
        {
            GenerateSyntheticScene(300000);
            break;
        }
        case PendingAction::ExportStream:
        {
            std::string savePath = SaveFileDialog("Surfels Wavelet Package (*.sflw)\0*.sflw\0", "sflw", "Save Compressed Package (.sflw)");
            if (!savePath.empty())
            {
                if (savePath.size() > 5 && savePath.substr(savePath.size() - 5) == ".sflw")
                {
                    savePath = savePath.substr(0, savePath.size() - 5);
                }
                ProcessAndExport(savePath);
            }
            break;
        }
        case PendingAction::ExportPLY:
        {
            std::string savePath = SaveFileDialog("Polygon File Format (*.ply)\0*.ply\0", "ply", "Export Current LOD as PLY");
            if (!savePath.empty())
            {
                m_statusMessage = "Exporting current LOD as PLY...";
                const auto& curLOD = m_waveletResult.lodLevels[m_selectedPreviewLOD];
                if (SyntheticGenerator::ExportToPLY(savePath, curLOD.surfels))
                {
                    m_statusMessage = "Exported PLY successfully to: " + savePath;
                    m_statusIsSuccess = true;
                }
                else
                {
                    m_statusMessage = "Failed to export PLY to: " + savePath;
                    m_statusIsSuccess = false;
                }
            }
            break;
        }
        case PendingAction::ExportSPLAT:
        {
            std::string savePath = SaveFileDialog("Gaussian Splat (*.splat)\0*.splat\0", "splat", "Export Current LOD as SPLAT");
            if (!savePath.empty())
            {
                m_statusMessage = "Exporting current LOD as SPLAT...";
                const auto& curLOD = m_waveletResult.lodLevels[m_selectedPreviewLOD];
                if (SPLATLoader::ExportToSPLAT(savePath, curLOD.surfels))
                {
                    m_statusMessage = "Exported SPLAT successfully to: " + savePath;
                    m_statusIsSuccess = true;
                }
                else
                {
                    m_statusMessage = "Failed to export SPLAT to: " + savePath;
                    m_statusIsSuccess = false;
                }
            }
            break;
        }
        case PendingAction::CloseDataset:
        {
            m_rawSurfels.clear();
            m_chunks.clear();
            m_rendererSurfels.clear();
            m_rendererRawSurfels.clear();
            m_rendererMeshletChunks.clear();
            m_rendererOctreeChunks.clear();
            m_waveletResult = WaveletDecompositionResult();
            m_loadedFilePath = "No dataset loaded";
            m_rendererSourceDescription = "No model active";
            m_isLoadedFromSFLW = false;
            m_rawFileSizeMB = 0.0f;
            m_compressedSizeMB = 0.0f;
            m_compressionRatio = 1.0f;
            m_deadbandZeroPercent = 0.0f;
            m_state.surfelCount = 0;
            m_state.chunkCount = 0;
            m_state.pSurfels = nullptr;
            m_state.pRawSurfels = nullptr;
            m_state.pChunks = nullptr;
            if (m_pRenderer) m_pRenderer->FlushGPU();
            m_statusMessage = "Dataset closed.";
            m_statusIsSuccess = true;
            break;
        }
        case PendingAction::ExitApp:
        {
            PostQuitMessage(0);
            break;
        }
        default:
            break;
        }
    }

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    void SurfelsApp::UpdateCamera(const ImGuiIO& io)
    {
        const float cy = cosf(m_pitch), sy = sinf(m_pitch);
        const float sx = sinf(m_yaw), cx = cosf(m_yaw);
        XMFLOAT3 rightDir(-cx, 0.0f, sx);
        XMFLOAT3 upDir(-sy * sx, cy, -sy * cx);

        bool shiftDown = io.KeyShift || ((GetKeyState(VK_SHIFT) & 0x8000) != 0) || ((GetKeyState(VK_LSHIFT) & 0x8000) != 0) || ((GetKeyState(VK_RSHIFT) & 0x8000) != 0);

        float dtSeconds = (float)(m_deltaTime / 1000.0f);
        if (dtSeconds <= 0.0f || dtSeconds > 0.1f) dtSeconds = 0.016f; // Safe fallback

        // Loupe inset: a left-button drag on an edge or corner resizes it, anywhere else inside it moves it.
        // Taken before the camera so a drag on the inset never orbits the model.
        m_loupeHover = 0;
        bool overInset = false;
        if (m_loupeEnabled && m_loupeRect.z > m_loupeRect.x)
        {
            const ImVec2 m = io.MousePos;
            const float grab = 7.0f;
            const bool inX = m.x >= m_loupeRect.x - grab && m.x <= m_loupeRect.z + grab;
            const bool inY = m.y >= m_loupeRect.y - grab && m.y <= m_loupeRect.w + grab;
            overInset = inX && inY;
            if (overInset)
            {
                if (fabsf(m.x - m_loupeRect.x) <= grab) m_loupeHover |= kLoupeDragLeft;
                if (fabsf(m.x - m_loupeRect.z) <= grab) m_loupeHover |= kLoupeDragRight;
                if (fabsf(m.y - m_loupeRect.y) <= grab) m_loupeHover |= kLoupeDragTop;
                if (fabsf(m.y - m_loupeRect.w) <= grab) m_loupeHover |= kLoupeDragBottom;
                if (m_loupeHover == 0) m_loupeHover = kLoupeDragMove;
            }
            if (m_loupeDrag == 0 && m_loupeHover != 0 && io.MouseClicked[0] && !io.WantCaptureMouse)
                m_loupeDrag = m_loupeHover;
        }
        if (m_loupeDrag != 0)
        {
            if (!io.MouseDown[0] || !m_loupeEnabled)
            {
                m_loupeDrag = 0;
                SaveConfigFile(); // The inset's size and place are remembered (loupe_size, loupe_pos)
            }
            else
            {
                const ImVec2 d = io.MouseDelta;
                const float minW = 160.0f, minH = 120.0f;
                const float maxW = std::max(minW, (float)m_Width - 20.0f), maxH = std::max(minH, (float)m_Height - 60.0f);
                if (m_loupeDrag & kLoupeDragMove) { m_loupePos.x += d.x; m_loupePos.y += d.y; }
                if (m_loupeDrag & kLoupeDragRight) m_loupeSize.x = std::clamp(m_loupeSize.x + d.x, minW, maxW);
                if (m_loupeDrag & kLoupeDragBottom) m_loupeSize.y = std::clamp(m_loupeSize.y + d.y, minH, maxH);
                if (m_loupeDrag & kLoupeDragLeft) // The right edge stays put
                {
                    const float w = std::clamp(m_loupeSize.x - d.x, minW, maxW);
                    m_loupePos.x += m_loupeSize.x - w;
                    m_loupeSize.x = w;
                }
                if (m_loupeDrag & kLoupeDragTop) // The bottom edge stays put
                {
                    const float h = std::clamp(m_loupeSize.y - d.y, minH, maxH);
                    m_loupePos.y += m_loupeSize.y - h;
                    m_loupeSize.y = h;
                }
            }
        }

        // A click inside the loupe's box pins it where it is, and another unpins it (Space too: see OnEvent).
        // A click is a press and release that hardly move: a left-drag that starts in the box still orbits.
        if (m_loupeEnabled && m_loupeRect.z > m_loupeRect.x)
        {
            const float zoom = std::max(1.0f, m_loupeEffectiveZoom); // As drawn last frame (a pinned box sizes itself; see DrawLoupeOverlay)
            const ImVec2 half(0.5f * (m_loupeRect.z - m_loupeRect.x) / zoom, 0.5f * (m_loupeRect.w - m_loupeRect.y) / zoom);
            const ImVec2 m = io.MousePos;
            const bool inBox = fabsf(m.x - m_loupeCenter.x) <= half.x && fabsf(m.y - m_loupeCenter.y) <= half.y;
            if (io.MouseClicked[0] && inBox && m_loupeDrag == 0 && !overInset && !io.WantCaptureMouse)
            {
                m_loupeBoxPressed = true;
                m_loupeBoxPressPos = m;
            }
            if (m_loupeBoxPressed && io.MouseReleased[0])
            {
                m_loupeBoxPressed = false;
                if (fabsf(m.x - m_loupeBoxPressPos.x) < 4.0f && fabsf(m.y - m_loupeBoxPressPos.y) < 4.0f)
                    SetLoupePinned(!m_loupePinned);
            }
        }

        // The loupe's box follows the mouse while it is over the viewport -- unless pinned, and not over the inset
        // itself, nor while the inset is being dragged (L toggles the loupe: see OnEvent).
        if (m_loupeEnabled && !m_loupePinned && m_loupeDrag == 0 && !overInset && !io.WantCaptureMouse && io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f)
            m_loupeCenter = io.MousePos;

        // Keyboard Controls: Arrow Keys (Shift=Pan, No Shift=Rotate), W/S/PageUp/PageDown Zoom
        if (!io.WantCaptureKeyboard)
        {
            if (shiftDown)
            {
                // Shift + Arrow Keys: PAN Camera along View Plane
                float keyPanSpeed = m_distance * 1.5f * dtSeconds;
                if (GetKeyState(VK_LEFT) & 0x8000)
                {
                    m_target.x -= rightDir.x * keyPanSpeed;
                    m_target.y -= rightDir.y * keyPanSpeed;
                    m_target.z -= rightDir.z * keyPanSpeed;
                }
                if (GetKeyState(VK_RIGHT) & 0x8000)
                {
                    m_target.x += rightDir.x * keyPanSpeed;
                    m_target.y += rightDir.y * keyPanSpeed;
                    m_target.z += rightDir.z * keyPanSpeed;
                }
                if (GetKeyState(VK_UP) & 0x8000)
                {
                    m_target.x += upDir.x * keyPanSpeed;
                    m_target.y += upDir.y * keyPanSpeed;
                    m_target.z += upDir.z * keyPanSpeed;
                }
                if (GetKeyState(VK_DOWN) & 0x8000)
                {
                    m_target.x -= upDir.x * keyPanSpeed;
                    m_target.y -= upDir.y * keyPanSpeed;
                    m_target.z -= upDir.z * keyPanSpeed;
                }
            }
            else
            {
                // Left/Right without Shift: Rotate Yaw (Smooth dtSeconds scaling)
                float keyRotSpeed = 1.8f * dtSeconds;
                if (GetKeyState(VK_LEFT) & 0x8000)
                {
                    m_yaw -= keyRotSpeed;
                }
                if (GetKeyState(VK_RIGHT) & 0x8000)
                {
                    m_yaw += keyRotSpeed;
                }

                // Up/Down / W/S: Smooth Frame-Rate Independent Exponential Zoom. With the loupe on, W/S
                // change its magnification instead and only the other keys move the camera.
                float zoomRate = 2.2f; // Exponential zoom rate per second
                const bool wDown = (GetKeyState('W') & 0x8000) != 0, sDown = (GetKeyState('S') & 0x8000) != 0;
                if (m_loupeEnabled)
                {
                    // The real frame time (capped), not the 0.016 s fallback above: at a few frames a second that
                    // fallback would make the zoom crawl.
                    const float dtZoom = std::min(0.5f, std::max(0.0f, (float)(m_deltaTime / 1000.0)));
                    m_loupeZoomHoldSec = (wDown != sDown) ? m_loupeZoomHoldSec + dtZoom : 0.0f;
                    const float rate = std::min(kLoupeZoomRateMax, kLoupeZoomRate * (1.0f + kLoupeZoomAccel * m_loupeZoomHoldSec));
                    if (wDown && !sDown) m_loupeZoom *= expf(+rate * dtZoom);
                    if (sDown && !wDown) m_loupeZoom *= expf(-rate * dtZoom);
                    m_loupeZoom = std::max(kLoupeZoomMin, std::min(kLoupeZoomMax, m_loupeZoom));
                }
                const bool wZooms = wDown && !m_loupeEnabled, sZooms = sDown && !m_loupeEnabled;
                if ((GetKeyState(VK_UP) & 0x8000) || wZooms || (GetKeyState(VK_PRIOR) & 0x8000) || (GetKeyState(VK_ADD) & 0x8000) || (GetKeyState(VK_OEM_PLUS) & 0x8000))
                {
                    m_distance *= expf(-zoomRate * dtSeconds); // Smooth fast Zoom In
                }
                if ((GetKeyState(VK_DOWN) & 0x8000) || sZooms || (GetKeyState(VK_NEXT) & 0x8000) || (GetKeyState(VK_SUBTRACT) & 0x8000) || (GetKeyState(VK_OEM_MINUS) & 0x8000))
                {
                    m_distance *= expf(+zoomRate * dtSeconds); // Smooth fast Zoom Out
                }
            }

            m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
        }

        if (!io.WantCaptureMouse && m_loupeDrag == 0)
        {
            if (shiftDown)
            {
                // SHIFT PRESSED = PAN CAMERA ONLY
                if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2])
                {
                    float panSpeed = m_distance * 0.0015f;
                    m_target.x += (rightDir.x * io.MouseDelta.x + upDir.x * io.MouseDelta.y) * panSpeed;
                    m_target.y += (rightDir.y * io.MouseDelta.x + upDir.y * io.MouseDelta.y) * panSpeed;
                    m_target.z += (rightDir.z * io.MouseDelta.x + upDir.z * io.MouseDelta.y) * panSpeed;
                }
            }
            else
            {
                // NO SHIFT = ROTATE OR ZOOM ONLY (ZERO PANNING)
                if (io.MouseDown[0])
                {
                    // Left Mouse Drag: Orbit Camera (Yaw / Pitch). The model follows the drag on both axes:
                    // dragging right moves the eye left (yaw down), so the model turns to the right.
                    m_yaw -= io.MouseDelta.x * 0.006f;
                    m_pitch = std::max(-1.55f, std::min(1.55f, m_pitch + io.MouseDelta.y * 0.006f));
                }
                else if (io.MouseDown[1])
                {
                    // Right Mouse Drag: Zoom In / Out
                    float zoomFactor = 1.0f + io.MouseDelta.y * 0.005f;
                    m_distance *= std::max(0.5f, std::min(1.5f, zoomFactor));
                    m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
                }
            }

            if (io.MouseWheel != 0.0f)
            {
                float wheelZoomFactor = powf(0.85f, io.MouseWheel);
                m_distance *= wheelZoomFactor;
                m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
            }
        }

        // Distance-Adaptive Auto LOD Selection:
        // Automatically selects appropriate LOD level based on camera distance relative to model extents
        if (m_autoLOD && !m_waveletResult.lodLevels.empty())
        {
            // Rate-limit how often auto-LOD is allowed to advance a level. Without this, a camera
            // distance that starts (or jumps) far outside the current LOD's range lets this hysteresis
            // check fire every single frame, stepping m_selectedPreviewLOD by one level per frame with
            // no regard for whether the previous level's dither cross-fade transition has settled.
            // At Release framerates that raced through 5 levels in about a second, leaving every one of
            // those levels' chunk subtrees simultaneously mid-transition -- ballooning the per-frame
            // render workload (parent + all children stay in the emitted list while transitioning) to
            // well beyond steady-state and overwhelming the GPU badly enough to trip a driver TDR
            // (DXGI_ERROR_DEVICE_HUNG). Gate advances to at most once per dither transition duration so
            // each level change has a chance to finish before the next one is allowed to start.
            m_autoLODCooldownTimer = std::max(0.0f, m_autoLODCooldownTimer - (float)dtSeconds);

            if (m_autoLODCooldownTimer <= 0.0f)
            {
                // Detach Camera: the level follows the frozen camera's distance, not the observer's.
                const float lodDistance = m_detachCamera ? m_detachedDistance : m_distance;
                int maxLODIndex = (int)m_waveletResult.lodLevels.size() - 1;
                const bool screenSpace = m_lodScreenSpace && (int)m_lodSpacing.size() > maxLODIndex && maxLODIndex > 0 && m_lodSpacing[0] > 0.0f;
                const float targetFloatLOD = ContinuousLodAtDistance(lodDistance, screenSpace);

                // Schmitt-trigger hysteresis to prevent border oscillation when zooming. Screen space: step
                // coarser only once the next level's spacing is within the pixel threshold, and back finer a
                // little before the current level exceeds it. Legacy: +/- 0.35 LOD units around the midpoint.
                int curLOD = m_selectedPreviewLOD;
                int newLOD = curLOD;
                const float upAt = screenSpace ? 1.0f : 0.65f;
                const float downAt = screenSpace ? 0.15f : 0.65f;
                if (targetFloatLOD >= (float)curLOD + upAt)
                {
                    newLOD = std::min(maxLODIndex, curLOD + 1);
                }
                else if (targetFloatLOD < (float)curLOD - downAt)
                {
                    newLOD = std::max(0, curLOD - 1);
                }

                if (newLOD != m_selectedPreviewLOD)
                {
                    m_selectedPreviewLOD = newLOD;
                    m_autoLODCooldownTimer = std::max(0.05f, m_ditherTransitionDurationSec);
                    if (!m_enableStreamingSimulation)
                    {
                        UpdatePreviewSurfels();
                    }
                    else
                    {
                        m_streamStateDirty = true;
                    }
                }
            }
        }

        // Rotate around local Y axis at exactly 1 revolution every 20 seconds (2*PI / 20.0s = ~0.314159 rad/sec)
        // m_deltaTime is in milliseconds (e.g. ~16.6ms at 60 FPS), so convert to seconds (m_deltaTime / 1000.0f)
        bool isUserDragging = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
        if (m_autoRotate && !isUserDragging)
        {
            float dtSeconds = (float)(m_deltaTime / 1000.0f);
            float radPerSec = (2.0f * 3.14159265f) / 20.0f; // Exactly 1 revolution per 20 seconds
            m_yaw += dtSeconds * radPerSec;
        }

        if (!m_detachCamera)
        {
            m_detachedYaw = m_yaw;
            m_detachedPitch = m_pitch;
            m_detachedDistance = m_distance;
            m_detachedTarget = m_target;
        }

        m_state.autoRotate = m_autoRotate;

        m_state.camYaw = m_yaw;
        m_state.camPitch = m_pitch;
        m_state.camDistance = m_distance;
        m_state.camTarget = m_target;
        m_state.detachCullCamera = m_detachCamera;
        m_state.cullYaw = m_detachedYaw;
        m_state.cullPitch = m_detachedPitch;
        m_state.cullDistance = m_detachedDistance;
        m_state.cullTarget = m_detachedTarget;
        m_state.aspectRatio = io.DisplaySize.y > 0.0f ? (io.DisplaySize.x / io.DisplaySize.y) : 1.0f;
        m_state.gpuRadixSort = m_gpuRadixSort;
        m_state.useChunkedPipeline = m_useChunkedPipeline;
        m_state.aabbMin = m_aabbMin;
        m_state.aabbExtents = m_extents;
        m_state.enableDithering = m_enableDitheredTransitions;
        m_state.enableConeCulling = m_enableConeCulling;
        m_state.useCopyQueue = m_useCopyQueue;
        m_state.showOnlyLockedChunks = m_showOnlyLockedChunks;
        m_state.silhouetteExteriorOnly = m_silhouetteExteriorOnly;

        uint32_t totalBasePoints = (uint32_t)(!m_residentLODs.empty() ? m_residentLODs[0].rawSurfels.size() : (!m_rawSurfels.empty() ? m_rawSurfels.size() : m_rendererSurfels.size()));
        uint32_t totalBaseChunks = (uint32_t)(!m_residentLODs.empty() ? m_residentLODs[0].meshletChunks.size() : (!m_chunks.empty() ? m_chunks.size() : m_rendererMeshletChunks.size()));
        m_state.totalDatasetSurfels = totalBasePoints;
        m_state.totalDatasetChunks = totalBaseChunks;

        if (!m_freezeRenderingAndMemory)
        {
            if (m_enableStreamingSimulation)
            {
                const auto tSim = std::chrono::high_resolution_clock::now();
                UpdateStreamingSimulation(m_deltaTime / 1000.0);
                m_frameSimMs = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - tSim).count();
            }
            else
            {
                m_frameSimMs = 0.0f;
                m_frameTraversalMs = 0.0f;
            }
        }

        if (m_enableStreamingSimulation)
        {
            if (m_splatMode)
            {
                m_state.renderMode = SplatRenderMode();
                ApplySplatRenderState();
                m_state.pSplats = m_unifiedSplats.empty() ? nullptr : m_unifiedSplats.data();
                m_state.pSplatSH = m_unifiedSH.empty() ? nullptr : m_unifiedSH.data();
                m_state.pSurfels = nullptr;
                m_state.pRawSurfels = nullptr;
                m_state.surfelCount = (uint32_t)m_unifiedSplats.size();
            }
            else if (m_enableQuantization)
            {
                m_state.renderMode = 1;
                m_state.pSurfels = !m_unifiedPackedSurfels.empty() ? m_unifiedPackedSurfels.data() : m_rendererSurfels.data();
                m_state.pRawSurfels = nullptr;
                m_state.surfelCount = (uint32_t)(!m_unifiedPackedSurfels.empty() ? m_unifiedPackedSurfels.size() : m_rendererSurfels.size());
            }
            else
            {
                m_state.renderMode = 2;
                m_state.pRawSurfels = !m_unifiedRawSurfels.empty() ? m_unifiedRawSurfels.data() : m_rendererRawSurfels.data();
                m_state.pSurfels = nullptr;
                m_state.surfelCount = (uint32_t)(!m_unifiedRawSurfels.empty() ? m_unifiedRawSurfels.size() : m_rendererRawSurfels.size());
            }
            m_state.pChunks = m_rendererMeshletChunks.data();
            m_state.chunkCount = (uint32_t)m_rendererMeshletChunks.size();
        }
        else
        {
            if (!m_residentLODs.empty())
            {
                int maxLODIndex = std::max(0, (int)m_residentLODs.size() - 1);
                int selectedLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                auto& resident = m_residentLODs[selectedLOD];

                m_state.pChunks = resident.meshletChunks.data();
                m_state.chunkCount = (uint32_t)resident.meshletChunks.size();

                if (m_splatMode)
                {
                    m_state.renderMode = SplatRenderMode();
                    ApplySplatRenderState();
                    m_state.pSplats = resident.splats.data();
                    m_state.pSplatSH = resident.splatSH.empty() ? nullptr : resident.splatSH.data();
                    m_state.pSurfels = nullptr;
                    m_state.pRawSurfels = nullptr;
                    m_state.surfelCount = (uint32_t)resident.splats.size();
                }
                else if (m_enableQuantization)
                {
                    m_state.renderMode = 1;
                    m_state.pSurfels = resident.packedSurfels.data();
                    m_state.pRawSurfels = nullptr;
                    m_state.surfelCount = (uint32_t)resident.packedSurfels.size();
                }
                else
                {
                    m_state.renderMode = 2;
                    m_state.pRawSurfels = resident.rawSurfels.data();
                    m_state.pSurfels = nullptr;
                    m_state.surfelCount = (uint32_t)resident.rawSurfels.size();
                }
            }
        }
    }

    // The entire ImGui frame: the left tool tabs (Generator/Renderer/Streaming) and the right statistics panel
    void SurfelsApp::BuildUI()
    {
        // Draw 3D Octree Bounding Cubes with Dotted Mid-Gray Lines onto the 3D viewport
        DrawOctreeVisualizer();

        // 1. Top Global Menu Bar
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                // Studio only: raw point clouds and the benchmark feed the generator tab, which the
                // viewer does not have.
                if (!IsViewerOnly() && ImGui::MenuItem("Open Raw Point Cloud... (PLY / SPLAT)", "Ctrl+O"))
                {
                    m_pendingAction = PendingAction::OpenFile;
                }
                if (ImGui::MenuItem("Open Compressed Model... (.sflw)", IsViewerOnly() ? "Ctrl+O" : "Ctrl+L"))
                {
                    m_pendingAction = PendingAction::OpenCompressedFile;
                }
                if (!IsViewerOnly() && ImGui::MenuItem("Generate Synthetic Benchmark", "Ctrl+G"))
                {
                    m_pendingAction = PendingAction::GenerateBenchmark;
                }

                ImGui::Separator();
                bool hasModel = !m_rawSurfels.empty();

                if (!IsViewerOnly() && ImGui::MenuItem("Save Compressed Package (.sflw)...", "Ctrl+S", false, hasModel))
                {
                    m_pendingAction = PendingAction::ExportStream;
                }

                if (ImGui::MenuItem("Export Current LOD as PLY...", nullptr, false, hasModel))
                {
                    m_pendingAction = PendingAction::ExportPLY;
                }

                if (ImGui::MenuItem("Export Current LOD as SPLAT (3DGS)...", nullptr, false, hasModel))
                {
                    m_pendingAction = PendingAction::ExportSPLAT;
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Close Dataset", nullptr, false, hasModel))
                {
                    m_pendingAction = PendingAction::CloseDataset;
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4"))
                {
                    m_pendingAction = PendingAction::ExitApp;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem("Auto Rotate Viewport", nullptr, &m_state.autoRotate);
                if (ImGui::MenuItem("Reset Camera to Center"))
                {
                    m_target = m_center;
                    ApplyFitDistance();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem(IsViewerOnly() ? "About Surfels Viewer..." : "About Surfel Generator..."))
                {
                    m_showAboutDialog = true;
                }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = m_uiScale;

        float leftPanelWidth = std::min((float)m_Width * 0.45f, std::max(300.0f, 420.0f * m_uiScale));
        float rightPanelWidth = std::min((float)m_Width * 0.45f, std::max(300.0f, 420.0f * m_uiScale));
        float panelHeight = std::max(200.0f, (float)m_Height - 40.0f);

        // 2. Left Control Panel: Decoupled Preprocessor & Renderer Tabs
        ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(leftPanelWidth, panelHeight), ImGuiCond_Always);
        ImGui::Begin("##LeftPanel", nullptr, ImGuiWindowFlags_NoCollapse);

        // Tab Selector Buttons. Studio shows the three numbered tabs; the viewer has no generator, so
        // it shows Renderer and Streaming, unnumbered, across the same width.
        {
            struct TabDef { const char* label; int index; };
            const TabDef studioTabs[] = { { "1. Surfel Generator", 0 }, { "2. Renderer", 1 }, { "3. Streaming", 2 } };
            const TabDef viewerTabs[] = { { "Renderer", 1 }, { "Streaming", 2 } };
            const TabDef* tabs    = IsViewerOnly() ? viewerTabs : studioTabs;
            const int     numTabs = IsViewerOnly() ? 2 : 3;
            const float   tabWidth = (ImGui::GetContentRegionAvailWidth() - 6.0f * (numTabs - 1)) / (float)numTabs;
            for (int t = 0; t < numTabs; t++)
            {
                if (t > 0) ImGui::SameLine();
                const bool active = (m_activeTab == tabs[t].index);
                ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.18f, 0.45f, 0.75f, 1.0f) : ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                if (ImGui::Button(tabs[t].label, ImVec2(tabWidth, 28))) m_activeTab = tabs[t].index;
                ImGui::PopStyleColor(2);
            }
        }

        ImGui::Separator();

        // =========================================================================
        // TAB 1: SURFEL GENERATOR PREPROCESSOR (Raw Model -> Octree -> Wavelet Decimation -> SFLW Export)
        // =========================================================================
        if (m_activeTab == 0)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Raw Input Source:");
            ImGui::TextWrapped("%s", m_loadedFilePath.c_str());
            ImGui::Separator();

            if (ImGui::Button("Open Raw Point Cloud (.ply / .splat)...", ImVec2(-1, 26)))
            {
                m_pendingAction = PendingAction::OpenFile;
            }
            if (ImGui::Button("Generate Synthetic Benchmark", ImVec2(-1, 24)))
            {
                m_pendingAction = PendingAction::GenerateBenchmark;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Loads the configured benchmark dataset (default: %s) or generates synthetic points.", m_benchmarkDatasetPath.c_str());

            if (!m_rawSurfels.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Save Compressed Model:");
                if (ImGui::Button("Save Compressed Package (.sflw)...", ImVec2(-1, 28)))
                {
                    m_pendingAction = PendingAction::ExportStream;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Compresses and saves the multi-resolution dataset, with the current parameters and occlusion volume, into a single self-contained .sflw stream package.");
            }

            ImGui::Spacing();
            ImGui::Separator();

            if (m_rawSurfels.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("No raw dataset currently loaded for preprocessing.");
                ImGui::TextWrapped("Open a PLY or SPLAT point cloud to configure octree chunking and wavelet decimation.");
            }
            else
            {
                // Safety net: any path that brought points in with generation on but NO cubes at all (an
                // empty scene, or a legacy/failed bake) gets one now, so a model never sits on screen
                // without its occluder. Deliberately does NOT fire just because the cached grid is
                // invalid: a package load leaves the grid invalid on purpose whenever it already shipped
                // a good baked volume (see LoadSFLWFile), and rebuilding here from the reconstructed
                // points would silently replace it with an inferior one before the user ever touches a
                // slider -- exactly the Venus regression this comment used to cause.
                //
                // Fires at most once per volume state: when the bake itself yields no cubes (an empty
                // scene, or a build whose codec has no occlusion feature) every condition above stays
                // true, and without the version check this re-voxelized the whole cloud and wrote two
                // trace lines on EVERY frame. Any load or pipeline rebuild bumps the version and so
                // re-arms it.
                if (m_generateOcclusionVolume && !m_occlusionGrid.valid && m_occlusionVoxels.empty()
                    && m_occlusionSafetyNetVersion != m_occlusionVoxelsVersion)
                {
                    RefreshOcclusionVolume(); // Bumps m_occlusionVoxelsVersion whether or not cubes came out
                    m_occlusionSafetyNetVersion = m_occlusionVoxelsVersion;
                }

                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Preprocessor Parameters:");

                // Every parameter is live. The chunking/wavelet ones re-run the whole pipeline over the
                // raw point cloud, which is too heavy to do on every mouse-move of a drag, so they flag
                // m_pipelineNeedsUpdate and the rebuild fires the moment the slider is let go (below).
                float maxChunkSize = std::max(1.0f, std::max(m_extents.x, std::max(m_extents.y, m_extents.z)));
                if (ImGui::SliderFloat("Octree Chunk (m)", &m_chunkSize, 0.05f, maxChunkSize, "%.2f meters"))
                {
                    m_pipelineNeedsUpdate = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spatial octree voxel bounding box diameter for streaming chunk partitioning. Applied when the slider is released.");

                if (ImGui::SliderInt("Max Wavelet LODs", &m_maxLODLevels, 1, 6))
                {
                    m_pipelineNeedsUpdate = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum number of multi-resolution LOD decimation levels in the wavelet pyramid. Applied when the slider is released.");

                if (m_sourceHasSplatAttributes)
                {
                    if (ImGui::Checkbox("Gaussian Splat Mode (full 3DGS record)", &m_splatMode))
                    {
                        m_pipelineNeedsUpdate = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bakes and draws every point as the 3D Gaussian the source describes: its three scales and rotation, its opacity, an 8-bit colour and, optionally, its spherical harmonics. 20 bytes per point plus the harmonics, against 8 bytes for a surfel, in exchange for the look of a reference splat viewer. Off: the 8-byte oriented-disc surfel.");
                    if (m_splatMode)
                    {
                        static const char* s_shNames[] = { "Low (degree 1, 9 bytes per splat)", "Medium (degree 2, 24 bytes per splat)", "High (degree 3, 45 bytes per splat)" };
                        if (ImGui::Combo("SH Quality", &m_splatSHQuality, s_shNames, IM_ARRAYSIZE(s_shNames)))
                        {
                            m_pipelineNeedsUpdate = true;
                        }
                        if (BakeSHDegree() < SplatSHDegree())
                            ImGui::TextDisabled("The source carries degree %u: the bake keeps degree %u", m_sourceSHDegree, BakeSHDegree());
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much of the view-dependent colour (spherical harmonics) the package keeps. Low keeps the first band, which carries most of the tint shift with viewing angle; Medium adds the second band (sharper highlights) at about 2.7 times the harmonics bytes; High keeps all three bands the source has, at five times the harmonics bytes.");
                    }
                }

                if (ImGui::SliderFloat("Deadband Zero (mm)", &m_deadbandThresholdMM, 0.0f, 50.0f, "%.1f mm"))
                {
                    m_pipelineNeedsUpdate = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sparsification deadband: wavelet detail coefficients below this threshold are zeroed out. Applied when the slider is released.");

                if (m_pipelineNeedsUpdate && !ImGui::IsAnyItemActive())
                {
                    RecomputeWaveletHierarchy(); // Clears m_pipelineNeedsUpdate itself
                }
                else if (m_pipelineNeedsUpdate)
                {
                    ImGui::TextDisabled("Pipeline will re-run when the slider is released...");
                }

                ImGui::Spacing();
                ImGui::Separator();
                if (ImGui::Checkbox("Generate Interior Occlusion Volume", &m_generateOcclusionVolume))
                {
                    RefreshOcclusionVolume();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bakes solid occluder cubes into the interior of the model so the renderer can depth-test against them to hide far-side surfels visible through gaps in a sparse near side. The volume is a closed voxel proxy of the whole model (surface shell plus everything it encloses). On by default; the sliders below rebuild it live.");
                // Always shown (not gated on the checkbox or on a volume existing) so the controls are
                // never hidden by a transient empty result; the checkbox decides whether a move rebuilds
                // or clears.
                {
                    if (ImGui::SliderFloat("Shave", &m_occlusionShave, 0.0f, 10.0f, "%.1f"))
                    {
                        RefreshOcclusionVolume();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Erodes the solid further inside the model. Cubes that would poke through the surfel surface are always removed first (see the count below), so 0 = the largest volume that stays inside the model and 10 = a thin core along the model's length. Thin and thick parts shrink together. Rebuilds live; baked into the package.");
                    // Colour grade of the baked colour. Applied when each block's colour is packed, so it
                    // ships in the .sflw like the shape does; rebuilds live off the cached grid.
                    bool gradeChanged = false;
                    gradeChanged |= ImGui::SliderFloat("Hue Shift", &m_occlusionHueShift, -180.0f, 180.0f, "%.0f deg");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotates the hue of every occluder block by this many degrees around the colour wheel. 0 = as sampled from the surfels.");
                    gradeChanged |= ImGui::SliderFloat("Saturation", &m_occlusionSaturation, 0.0f, 2.0f, "%.2fx");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scales colour saturation. 0 = greyscale, 1 = as sampled, above 1 = more vivid.");
                    gradeChanged |= ImGui::SliderFloat("Brightness", &m_occlusionBrightness, 0.0f, 2.0f, "%.2fx");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scales brightness (HSV value). Below 1 darkens the volume into shadow, above 1 lightens it. Baked into the package.");
                    if (gradeChanged)
                    {
                        RefreshOcclusionVolume();
                    }
                    if (m_occlusionGrid.valid)
                    {
                        ImGui::TextDisabled("Auto resolution: %d cells along the longest axis (%.3f m per cell)", m_occlusionGrid.resolution, m_occlusionGrid.cellSize);
                        ImGui::TextDisabled("Shell padding: %d cell(s)   Poke-through cull: %zu of %zu cubes removed   Bias: %+.1f cells (config.json)", m_occlusionGrid.gapClose, m_occlusionGrid.pokeCount, m_occlusionGrid.solidCount, m_occlusionShaveBiasCells);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cubes whose corners fall outside the surfel surface (tested against the centroid and mean normal of the neighbouring surfels). Always removed, at any shave, so the volume never clips outside the model.");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chosen from the point density so a few points span every surface cell. Interior blocks are merged into larger cubes automatically.");
                    }
                }

                ImGui::Spacing();
                if (!m_occlusionVoxels.empty())
                {
                    std::string chain;
                    for (uint32_t k = 0; k < m_occlusionMips.mipCount; k++)
                        chain += (k ? " / " : "") + std::to_string(m_occlusionMips.blockCount[k]);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Baked: %zu occluder cubes in %u mips (%s)", m_occlusionVoxels.size(), m_occlusionMips.mipCount, chain.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mip 0 is the fine skin, %.3f m cells. Each further mip is a coarser level that lies inside the one below it, so no mip ever protrudes. The renderer draws one mip per frame, chosen from how many pixels a cell covers at the model's nearest point, so the volume coarsens together with the surfel LOD in view.", m_occlusionMips.mipCount > 0 ? m_occlusionMips.cellSize[0] : 0.0f);
                }
                else if (m_generateOcclusionVolume)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Baked: no cubes survived (point cloud too sparse/thin at this resolution)");
                DrawOcclusionVolumeControls();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("Preprocessor Analytics:");
                ImGui::Text("• Total Raw Vertices: %u", (uint32_t)m_rawSurfels.size());
                ImGui::Text("• Partitioned Chunks: %u", (uint32_t)m_chunks.size());
                ImGui::Text("• Raw Uncompressed:   %.2f MB", m_rawFileSizeMB);
                ImGui::Text("• Tier 4 Compressed:  %.2f MB (%.1fx)", m_compressedSizeMB, m_compressionRatio);
            }
        }
        // =========================================================================
        // TAB 2: STREAM VIEWER & RENDERER (Real-Time LODs, Viewport, Accelerators)
        // =========================================================================
        else if (m_activeTab == 1)
        {
            ImGui::Spacing();
            if (m_isLoadedFromSFLW)
            {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Source: Loaded .sflw Package");
            }
            else if (!m_rawSurfels.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Source: In-Memory Preprocessed Stream");
            }
            else
            {
                ImGui::TextDisabled("No active model loaded in renderer.");
            }

            if (ImGui::Button("Load Compressed Model (.sflw)...", ImVec2(-1, 26)))
            {
                m_pendingAction = PendingAction::OpenCompressedFile;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Directly loads and streams a pre-compressed .sflw model from disk, overriding memory data structures.");

            ImGui::Separator();

            if (!m_rawSurfels.empty() || m_isLoadedFromSFLW)
            {
                // Section 1: Runtime LOD Settings
                if (ImGui::CollapsingHeader("1. Runtime LOD & Quality", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    int maxLODIndex = std::max(0, (int)m_waveletResult.lodLevels.size() - 1);
                    if (maxLODIndex > 0)
                    {
                        m_selectedPreviewLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));

                        if (ImGui::Checkbox("Auto Distance LOD", &m_autoLOD))
                        {
                            UpdatePreviewSurfels();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically adapts active LOD level dynamically based on distance from camera.");
                        if (m_autoLOD)
                        {
                            ImGui::Indent(12.0f);
                            if (ImGui::Checkbox("Screen-Space LOD", &m_lodScreenSpace)) SaveConfigFile();
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: a coarser level is used only once its point spacing projects to at most the threshold below, in pixels, at the model's near side -- so the switch happens where the coarser level can no longer be told apart from full resolution, whatever the window size. Off: the old rule, a fixed ramp from 1x to 4x the model's size. Remembered across sessions.");
                            if (m_lodScreenSpace)
                            {
                                ImGui::SameLine();
                                ImGui::PushItemWidth(140.0f);
                                if (ImGui::SliderFloat("##LodPixelSpacing", &m_lodPixelSpacing, 0.25f, 4.0f, "max %.2f px")) SaveConfigFile();
                                ImGui::PopItemWidth();
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Largest on-screen spacing between a level's points before the next finer level is used. Lower keeps full resolution longer (more splats drawn); 1 px is where a level's points just tile the pixels.");
                            }
                            const int lvlNow = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                            if (lvlNow < (int)m_lodSpacing.size() && m_lodSpacing[lvlNow] > 0.0f)
                                ImGui::TextDisabled("Level %d spacing on screen: %.2f px", lvlNow, m_lodSpacing[lvlNow] * LodFocalPixels() / LodEffectiveDistance(m_detachCamera ? m_detachedDistance : m_distance));
                            ImGui::Unindent(12.0f);
                        }

                        ImGui::PushItemWidth(ImGui::GetContentRegionAvailWidth() - 95.0f);
                        if (ImGui::SliderInt("##LODSlider", &m_selectedPreviewLOD, 0, maxLODIndex, "LOD %.0f"))
                        {
                            m_selectedPreviewLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                            m_autoLOD = false;
                            m_enableWavelet = true;
                            UpdatePreviewSurfels();
                        }
                        ImGui::PopItemWidth();

                        ImGui::SameLine();
                        if (ImGui::Checkbox("Cascade", &m_cascadeLOD))
                        {
                            PrecacheResidentLODs();
                            UpdatePreviewSurfels();
                        }

                        ImGui::Separator();
                    }

                    if (ImGui::Checkbox("Dithered LOD Transitions", &m_enableDitheredTransitions))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stochastic screen-space Bayer dithering to smoothly blend LOD transitions and eliminate popping.");

                    if (m_enableDitheredTransitions)
                    {
                        ImGui::SameLine();
                        ImGui::PushItemWidth(100.0f);
                        if (ImGui::SliderFloat("##DitherDurationTab", &m_ditherTransitionDurationSec, 0.05f, 5.0f, "%.2f s"))
                        {
                            m_streamStateDirty = true;
                        }
                        ImGui::PopItemWidth();
                    }
                    if (ImGui::Checkbox("Demote Chunks on Zoom Out", &m_demoteChunks))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: once a chunk has refined into its children it stays refined when you zoom out or its edge flag drops, so detail never fades back to the coarser level (Decay still reclaims memory). On: children cross-fade back to the parent and are evicted, as in v1.2.0.");

                    ImGui::Separator();
#if 0 // DYNAMIC SILHOUETTE: GPU silhouette chunk processing switched off for now (re-enable by restoring this block)
                    if (ImGui::Checkbox("Highlight Edge Chunks", &m_highlightSilhouetteChunks))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visualizer toggle: highlights the active silhouette edge chunks.");
#endif // DYNAMIC SILHOUETTE

                    DrawRefinementVisualizerControls("##Renderer");

                    if (ImGui::Checkbox("Show ONLY Locked Chunks (Transition / Edge)", &m_showOnlyLockedChunks))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Isolates active dynamic workload: hides static model geometry and renders ONLY chunks that are currently locked in transition or detected on the silhouette edge.");

#if 0 // DYNAMIC SILHOUETTE: GPU silhouette chunk processing switched off for now (re-enable by restoring this block)
                    uint32_t liveEdgeChunks = 0;
                    uint32_t totalActiveChunks = 0;
                    if (m_enableStreamingSimulation)
                    {
                        totalActiveChunks = (uint32_t)m_rendererSourceChunks.size();
                        for (const auto* pChunk : m_rendererSourceChunks)
                        {
                            if (pChunk && pChunk->isSilhouette) liveEdgeChunks++;
                        }
                    }
                    else if (!m_residentLODs.empty())
                    {
                        int maxLODIndex = std::max(0, (int)m_residentLODs.size() - 1);
                        int selectedLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                        const auto& resident = m_residentLODs[selectedLOD];
                        totalActiveChunks = (uint32_t)resident.meshletChunks.size();
                        for (const auto& mc : resident.meshletChunks)
                        {
                            if (mc.isSilhouette > 0.5f) liveEdgeChunks++;
                        }
                    }
                    ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.98f, 1.0f), "• Active Edge Chunks: %u / %u", liveEdgeChunks, totalActiveChunks);

                    if (ImGui::Checkbox("GPU Silhouette Edge Refinement (Option 2)", &m_enableSilhouetteLOD0))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recovers Level 0 fine chunks along 2D outer silhouettes and interior self-occlusion edges detected on GPU.");

                    if (m_enableSilhouetteLOD0)
                    {
                        // Vendored ImGui 1.53's SliderInt() is a thin wrapper around SliderFloat(): it
                        // casts the int to float internally and passes the display_format string straight
                        // through unchanged (imgui.cpp: "float v_f = (float)*v; SliderFloat(label, &v_f,
                        // ..., display_format, ...)"). A custom format here must use a float specifier
                        // (%.0f) even though the underlying value is an int -- a %d here reads the float
                        // argument through an int-typed varargs slot, which on x64 pulls from the wrong
                        // register entirely, showing garbage (a large/negative number while actively
                        // dragging, or a value that just happens to stay 0) instead of the real value.
                        if (ImGui::SliderInt("Silhouette LOD Bias", &m_silhouetteLODBias, 0, 4, "Bias: %.0f"))
                        {
                            m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("LOD level bias for silhouette edges: renders fine edges using Level N - Bias (minimum Level 0).");

                        if (ImGui::SliderFloat("Silhouette Angle", &m_silhouetteThreshold, 0.10f, 0.70f, "%.2f"))
                        {
                            m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grazing angle dot product threshold |N . V| to classify boundary chunks as silhouette.");

                        if (ImGui::SliderFloat("Dilation Morph", &m_dilationMorphAmount, 0.0f, 1.0f, "%.2fx"))
                        {
                            m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While a silhouette edge chunk is mid-transition, its surfels are pushed outward along their normals and their discs swell (both scaled by this factor, fading with the dissolve) so the rim does not thin out between levels. 0 = off. 1 = full: up to 1.5 splat radii outward and 40%% larger discs, which reads as a breathing edge. Default 0.25: a subtle 10%% swell that covers the dissolve gap without visible bulging.");

                        if (ImGui::Button("Test Silhouette Edge Refinement", ImVec2(-1, 26)))
                        {
                            TriggerSilhouetteEdgeMorphTest();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unloads and immediately refreshes ONLY the silhouette edge chunks without touching or waving the rest of the model.");
                    }
#endif // DYNAMIC SILHOUETTE

                }

                // Interior Occlusion Volume (viewer controls) -- only relevant when the loaded model
                // actually has a baked volume (see LoadSFLWFile/RecomputeWaveletHierarchy).
                if (!m_occlusionVoxels.empty())
                {
                    if (ImGui::CollapsingHeader("Interior Occlusion Volume", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The baked occluder solid packaged with this model (%zu cubes). Its shape and colour are fixed here; to change the shave, hue, saturation or brightness, use the Interior Occlusion Volume controls on the 1. Surfel Generator tab and re-save the package.", m_occlusionVoxels.size());
                        DrawOcclusionVolumeControls();
                        ImGui::TextDisabled("Shape and colour are baked: adjust them on the Surfel Generator tab.");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch to 1. Surfel Generator for the Shave, Hue Shift, Saturation and Brightness sliders, then Save Compressed Package to bake the result into the .sflw.");
                    }
                }

                // Section 3: 3D Viewport & Splat Sizing
                if (ImGui::CollapsingHeader("2. 3D Viewport & Splat Sizing", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Active Display: %u points", m_state.surfelCount);

                    static const char* s_orientNames[] = { "Normal-Oriented Tangent Discs (Smooth Surface)", "Camera-Facing Billboards (3D Gaussian Splats)" };
                    int orientIdx = (int)m_state.orientMode;
                    if (ImGui::Combo("Surfel Orientation", &orientIdx, s_orientNames, IM_ARRAYSIZE(s_orientNames)))
                    {
                        m_state.orientMode = (uint32_t)orientIdx;
                    }
                    if (m_splatMode)
                    {
                        bool drawAsSurfels = m_splatDrawAsSurfels;
                        if (ImGui::Checkbox("Draw Splats as Surfels", &drawAsSurfels))
                        {
                            SetSplatDrawAsSurfels(drawAsSurfels);
                            SaveConfigFile();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draws the package's Gaussians as opaque, depth-tested discs through the surfel pipeline: no per-frame sort, no blending. Auto Splat Size and its Coverage, Splat Radius Scale and Surfel Orientation apply (Normal-Oriented uses each Gaussian's own orientation), and the dithered level transitions dissolve between two surfaces instead of sparkling. The colour is the record's base colour plus its harmonics, unlit. Off restores the sorted Gaussian pipeline. Remembered across sessions.");
                        static const char* s_blendNames[] = { "Display space (reference viewer)", "Linear (sRGB target)" };
                        // Greyed out while the package is drawn as surfels (nothing is blended): shown, edits are dropped
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_splatDrawAsSurfels ? 0.45f : 1.0f);
                        int blendShown = m_splatBlendSpace;
                        if (ImGui::Combo("Splat Blending", &blendShown, s_blendNames, IM_ARRAYSIZE(s_blendNames)) && !m_splatDrawAsSurfels)
                            m_splatBlendSpace = blendShown;
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the Gaussians are blended. Display space blends the stored colours as they are, which is what the training rasterizer and SuperSplat do. Linear decodes them first and lets the sRGB target encode the result: physically tidier, slightly different from the reference. No effect while Draw Splats as Surfels is on.");
                        // Greyed out while kSplatGaussianLevel0 is false (it refers to the Gaussian level 0): shown, but
                        // edits go to a copy and are dropped; the saved value still sizes the discs of every level
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, kSplatGaussianLevel0 ? 1.0f : 0.45f);
                        bool matchShown = m_splatDiscMatchLevel0;
                        if (ImGui::Checkbox("Match Level 0 Softness", &matchShown) && kSplatGaussianLevel0)
                        {
                            m_splatDiscMatchLevel0 = matchShown;
                            SaveConfigFile();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sizes the coarse levels' discs from the level 0 Gaussians themselves: the median footprint of the level 0 records relative to their spacing gives a softness ratio, and every disc level takes the radius that gives its discs the same ratio to their own spacing, so a streamed model reads as one surface and the hand-over from discs to Gaussians is less visible. Discs of level 1 and up also take an opacity floor so a low-opacity record does not read as a hole. Off: discs are 1.5 x the level spacing with the record's opacity as is. Remembered across sessions.");
                        ImGui::TextDisabled("Level 0 softness %.2f x spacing -> disc radius %.2f x spacing", m_splatSoftnessRatio, m_splatDiscMatchFactor);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The level 0 records' median in-plane footprint sqrt(s_max x s_mid) divided by the level 0 point spacing, and the disc radius factor sqrt(8) x that ratio (a disc with the splat falloff of radius R is a Gaussian of sigma R / sqrt(8)), clamped to 1.5..4.0.");
                        ImGui::PopStyleVar();
                        if (ImGui::Checkbox("Coarse Levels as Gaussians", &m_splatCoarseAsGaussians))
                        {
                            SaveConfigFile();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draws levels 1 and up with their own Gaussians (the coarse records carry grown scales and orientations) instead of camera-facing discs of the level spacing, which read as grain. The two disc settings above then only matter while this is off. Remembered across sessions.");
                        if (m_splatCoarseAsGaussians)
                        {
                            ImGui::SameLine();
                            ImGui::PushItemWidth(140.0f);
                            if (ImGui::SliderFloat("##CoarseGaussianCap", &m_splatCoarseGaussianCap, 0.0f, 4.0f, m_splatCoarseGaussianCap > 0.0f ? "cap %.2f x spacing" : "no cap")) SaveConfigFile();
                            ImGui::PopItemWidth();
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Largest scale a coarse Gaussian may take, per axis, as a multiple of its level's point spacing. The coarse records are grown well past that and bloom into a blur around the model without a cap; 1.5 measured best on cthulu (0.75 is a little grainier). 0 = no cap. Remembered across sessions.");
                        }
                    }
                    ImGui::SliderFloat("Splat Radius Scale", &m_state.splatRadius, 0.10f, 10.0f, "%.2fx");
                    ImGui::Checkbox("Auto Splat Size (per-level density)", &m_autoSplatSize);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sizes every disc from its level's typical point spacing instead of the per-point size classes: a dense patch covers the surface exactly and an isolated point (scan outlier, decimation stray) gets the same small disc instead of an inflated one, which removes the halo of ghost discs around the model at coarse levels. Splat Radius Scale still multiplies.");
                    if (m_autoSplatSize)
                    {
                        ImGui::Indent(12.0f);
                        ImGui::PushItemWidth(150.0f);
                        ImGui::SliderFloat("Coverage", &m_autoSplatCoverage, 0.0f, 8.0f, "%.2f x spacing");
                        ImGui::PopItemWidth();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disc radius as a multiple of the level's point spacing. Around 1.0 neighbouring discs just cover the surface; below ~0.6 gaps open, well above 1 discs overlap heavily and strays grow again. Splats that project below about 1.5 pixels are drawn as opaque one-pixel dots regardless.");
                        if (!m_lodSpacing.empty())
                        {
                            std::string sp = "Level spacing:";
                            for (size_t l = 0; l < m_lodSpacing.size(); l++) { char b[40]; snprintf(b, sizeof(b), " L%zu %.4g", l, m_lodSpacing[l]); sp += b; }
                            ImGui::TextDisabled("%s", sp.c_str());
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Median nearest-neighbour distance of each level's points, in model units (measured on load).");
                        }
                        ImGui::Unindent(12.0f);
                    }
                    ImGui::Separator();

                    ImGui::Checkbox("Auto Rotate Model##Viewport", &m_autoRotate);
                    m_state.autoRotate = m_autoRotate;

                    static const char* s_splatVizNames[] = { "Normal", "Splat outlines", "Random colour per splat", "Splat centres" };
                    ImGui::PushItemWidth(180.0f);
                    ImGui::Combo("Splat Display (V)##Viewport", &m_splatViz, s_splatVizNames, IM_ARRAYSIZE(s_splatVizNames));
                    ImGui::PopItemWidth();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How every splat of the whole model is drawn. Normal: as rendered, blending into one smooth surface. Splat outlines: each splat's ellipse at half its peak, over a faint fill -- its true size, shape and orientation. Random colour per splat: every splat filled flat out to that same contour in its own colour, so coverage and overlap show. Splat centres: a mark at every centre, the sampling density. V cycles it; Shift+V cycles the loupe's own (L).");

                    if (ImGui::Checkbox("Show Camera Control Hints##Viewport", &m_showControlHints))
                    {
                        SaveConfigFile();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows or hides the faint rotate / zoom / pan reminder in the bottom-right corner of the viewport. Remembered across sessions.");

                    if (ImGui::Checkbox("VSync (Lock Framerate to Display)", &m_vsync))
                    {
                        m_swapChain.SetVSync(m_vsync);
                    }

                    if (m_pRenderer && m_pRenderer->GetRenderPath() != SurfelsRenderer::RenderPath::MeshShaders)
                    {
                        // No amplification (task) shader stage on this render path: the control is greyed out and
                        // inert. Chunk backface culling still happens, per splat in the vertex shader, always on.
                        bool fixedOn = true;
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f);
                        ImGui::Checkbox("Meshlet Backface Cone Culling (Task Shader)", &fixedOn);
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unavailable on this GPU path (%s): there is no amplification (task) shader stage. Chunk backface culling runs per splat in the vertex shader instead and is always on.", m_pRenderer->GetRenderPathDescription());
                    }
                    else if (m_splatMode && SplatRenderMode() == 4)
                    {
                        // Splat package drawn as surfels: cone culling is off (the chunk cones come from placeholder
                        // normals; see SurfelsRenderer). Greyed out; edits go to a copy and are dropped.
                        bool coneShown = m_enableConeCulling;
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
                        ImGui::Checkbox("Meshlet Backface Cone Culling (Task Shader)", &coneShown);
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Task Shader (mainAS) culls ~50% of meshlet chunks facing away from the camera before mesh shaders and rasterization ever execute. Off for a splat package drawn as surfels: its chunk normal cones are placeholders.");
                        ImGui::TextDisabled("off for splat packages drawn as surfels");
                    }
                    else
                    {
                        ImGui::Checkbox("Meshlet Backface Cone Culling (Task Shader)", &m_enableConeCulling);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Task Shader (mainAS) culls ~50% of meshlet chunks facing away from the camera before mesh shaders and rasterization ever execute.");
                    }

                    ImGui::Checkbox("Use DX12 CopyQueue (Async DMA)", &m_useCopyQueue);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uses a dedicated D3D12_COMMAND_LIST_TYPE_COPY hardware DMA queue for PCIe buffer uploads in parallel with 3D rendering.");

                    bool detachToggle = m_detachCamera;
                    if (ImGui::Checkbox("Detach Camera (Freeze Culling Frustum)", &detachToggle))
                        SetDetachCamera(detachToggle);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+D. Freezes the culling camera where the view is and steps the view aside so the culling can be inspected from any angle. The model is exactly what the frozen camera would render, its level of detail included: surfels it would draw keep their colour (mid grey where the viewer sees their back), everything it would have culled -- chunks outside its frustum or whose normal cone faces away, and the far half of the model beyond its centre -- stays on screen as a translucent red volume, depth-tested against your view so it never builds up. Streaming keeps running from the frozen camera's point of view. The occlusion volume shows only the faces turned toward the frozen camera.");
                    if (m_detachCamera)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[CULLING FROZEN]");
                    }

                    if (ImGui::Button("Center Camera on Model", ImVec2(-1, 24)))
                    {
                        m_target = m_center;
                        ApplyFitDistance();
                    }

                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Temporal Filtering & Anti-Aliasing (TAA):");
                    if (ImGui::Checkbox("Enable Temporal Dither Resolver (TAA)", &m_enableTemporalFiltering))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooths stochastic Bayer dither patterns into seamless photo-realistic alpha dissolves,\nand eliminates surfel edge shimmer via temporal history accumulation.");

                    if (m_enableTemporalFiltering)
                    {
                        if (ImGui::SliderFloat("Temporal Blend Weight (Alpha)", &m_temporalBlendWeight, 0.05f, 0.50f, "%.2f (Lower = Smoother)"))
                        {
                            m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend weight for current frame. 0.10-0.15 delivers optimal 6-frame smooth dither dissipation without ghosting.");

                        if (ImGui::Checkbox("Sub-Pixel Camera Jitter (Halton 2,3)", &m_enableSubpixelJitter))
                        {
                            m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applies sub-pixel projection jitter to reconstruct crisp anti-aliased surfel edges when stationary.");

                        if (ImGui::Checkbox("Variance Color Box Clamping (Anti-Ghosting)", &m_enableVarianceClamping))
                        {
                            m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clamps reprojected history to 3x3 neighborhood YCoCg bounding box to eliminate ghosting during rapid camera motion.");
                    }

                    ImGui::Separator();
                    if (ImGui::SliderFloat("UI & Font Scale", &m_uiScale, 0.70f, 2.00f, "%.2fx"))
                    {
                        m_uiScale = std::max(0.50f, std::min(2.50f, m_uiScale));
                        io.FontGlobalScale = m_uiScale;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scales all UI panels, controls, and font sizes dynamically.");
                }

                // Section 4: Hardware Accelerators & Pipeline
                if (ImGui::CollapsingHeader("3. Accelerators & Meshlet Pipeline", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (ImGui::Checkbox("GPU Radix Sort", &m_gpuRadixSort))
                    {
                        if (m_pRenderer) m_pRenderer->FlushGPU();
                        m_state.gpuRadixSort = m_gpuRadixSort;
                    }
                    m_state.gpuRadixSort = m_gpuRadixSort;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Executes parallel 32-bit depth key sorting directly on GPU compute shader threads.");

                    ImGui::SameLine();
                    ImGui::TextColored(m_gpuRadixSort ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        m_gpuRadixSort ? "[Compute Shader]" : "[CPU Threads]");

                    if (ImGui::Checkbox("Morton Spatial Curve Ordering", &m_enableMortonOrder))
                    {
                        PrecacheResidentLODs();
                        UpdatePreviewSurfels();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reorders points along a 3D Morton Z-order space-filling curve.");

                    if (ImGui::Checkbox("Micro-Chunking", &m_useChunkedPipeline))
                    {
                        UpdatePreviewSurfels();
                    }
                    m_state.useChunkedPipeline = m_useChunkedPipeline;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hierarchical two-level sorting: 64-surfel meshlet clusters with Amplification Shader (AS) frustum culling.");

                    ImGui::SameLine();
                    ImGui::TextColored(m_useChunkedPipeline ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        m_useChunkedPipeline ? "[Amplification Shader]" : "[Disabled]");
                }

                // Section 5: Spatial & Cluster Visualizers
                if (ImGui::CollapsingHeader("4. Spatial & Cluster Visualizers", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const char* cubePresets[] = { "512 Cubes", "1,024 Cubes", "2,048 Cubes", "4,096 Cubes", "8,192 Cubes", "16,384 Cubes" };
                    int cubeValues[] = { 512, 1024, 2048, 4096, 8192, 16384 };
                    int currentPreset = 3;
                    for (int i = 0; i < 6; i++) { if (m_targetClusterCubes == cubeValues[i]) currentPreset = i; }
                    if (ImGui::Combo("Cluster Block Resolution", &currentPreset, cubePresets, IM_ARRAYSIZE(cubePresets)))
                    {
                        m_targetClusterCubes = cubeValues[currentPreset];
                        RebuildHeatmapClusterCubes();
                    }

                    const char* schemes[] = { "Turbo (Classic Rainbow)", "Viridis (Perceptual)", "Plasma (Magma)" };
                    ImGui::Checkbox("Show Density Heatmap Cluster Cubes", &m_showClusterHeatmap);
                    {
                        const char* sources[] = { "Point Density", "Detail (Stream Order)" };
                        if (ImGui::Combo("Heatmap Source", &m_heatmapSource, sources, IM_ARRAYSIZE(sources)))
                        {
                            RebuildHeatmapClusterCubes();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the cluster cubes colour by. Point Density: points per cube. Detail (Stream Order): the detail grid baked into the package, which the streaming scheduler reads when it decides what to deliver next: hottest cubes first.");
                    }
                    if (m_showClusterHeatmap)
                    {
                        ImGui::SliderFloat("Cube Fill Opacity", &m_heatmapOpacity, 0.0f, 1.0f, "%.2f");
                        ImGui::Checkbox("Draw Cube Outlines", &m_showHeatmapWireframe);
                        if (m_showHeatmapWireframe)
                        {
                            ImGui::SliderFloat("Outline Opacity", &m_wireframeOpacity, 0.0f, 1.0f, "%.2f");
                        }
                    }
                    else
                    {
                        ImGui::Checkbox("Draw Cube Outlines", &m_showHeatmapWireframe);
                    }
                    ImGui::Checkbox("Show Culled Chunks (Darker Shade)", &m_showCulledChunks);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colors chunks/cubes the culling frustum rejects. Only has something to color when 'Show Density Heatmap Cluster Cubes' or 'Show Macro Clusters' is also on, and only has anything VISIBLE to show when 'Detach Camera' is also on -- otherwise the culling frustum is the exact one you're viewing through, so anything culled is off-screen by definition.");
                    ImGui::Checkbox("Show Macro Clusters (Amber)", &m_showOctreeVisualizer);
                    ImGui::Checkbox("Show Global Model Bounds (Blue)", &m_showGlobalBounds);
                }
            }
        }
        // =========================================================================
        // TAB 3: NETWORK STREAMING (Progressive Bandwidth-Throttled Simulation)
        // =========================================================================
        else
        {
            ImGui::Spacing();

            if (!m_rawSurfels.empty() || m_isLoadedFromSFLW)
            {
                if (ImGui::Checkbox("Simulate Network Streaming", &m_enableStreamingSimulation))
                {
                    if (m_enableStreamingSimulation)
                    {
                        InitStreamingSimulation();
                    }
                    else
                    {
                        UpdatePreviewSurfels();
                    }
                }

                if (m_enableStreamingSimulation)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(m_isStreamingPaused ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                        m_isStreamingPaused ? "[PAUSED]" : "[STREAMING]");

                    ImGui::Checkbox("Prioritize View Frustum & Proximity", &m_prioritizeFrustumAndProximity);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: blocks inside the view frustum are delivered before those outside it (then the neighbour band, then the rest). Off: the scheduler's model-wide order, regardless of the camera.");
                    ImGui::TextDisabled("Order: base envelope > edge chunks > visible octahedron faces > rest");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The two coarsest levels always land first so there is a solid envelope. Silhouette edge chunks come next. Then the streaming scheduler serves the faces of the bounding octahedron the camera can see, together, while faces out of view wait their turn; the remaining view requests follow.");
                    DrawOctahedronGlyph();
                    ImGui::TextDisabled("Re-deliveries since reset: %u blocks streamed more than once (%u extra deliveries)", m_redeliveredBlocks, m_extraDeliveries);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Diagnostic. A block counts here when it is delivered, later evicted, and delivered again. With a still camera this should stay at zero; it rises only when something evicts blocks the view still wants (decay, or a demotion that used to evict).");
                    if (m_refineStartBudget == UINT32_MAX)
                        ImGui::TextDisabled("Active transitions: %u. Refinement starts deferred: %u (budget unlimited)", m_lastActiveTransitions, m_refineStartsDeferred);
                    else
                        ImGui::TextDisabled("Active transitions: %u. Refinement starts deferred: %u (budget %u chunks)", m_lastActiveTransitions, m_refineStartsDeferred, m_refineStartBudget);

                    // Bandwidth Preset Buttons
                    ImGui::Text("Network Profiles:");
                    if (ImGui::Button("3G (0.2 MB/s)", ImVec2(85, 22)))
                    {
                        m_bandwidthThrottleMBps = 0.2f;
                        m_unthrottledBandwidth = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("4G (2 MB/s)", ImVec2(80, 22)))
                    {
                        m_bandwidthThrottleMBps = 2.0f;
                        m_unthrottledBandwidth = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("5G (7.5 MB/s)", ImVec2(80, 22)))
                    {
                        m_bandwidthThrottleMBps = 7.5f;
                        m_unthrottledBandwidth = false;
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, m_unthrottledBandwidth ? ImVec4(0.18f, 0.55f, 0.35f, 1.0f) : ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
                    if (ImGui::Button("Full", ImVec2(48, 22)))
                    {
                        m_unthrottledBandwidth = !m_unthrottledBandwidth;
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes bandwidth throttle caps and streams at uncapped maximum rate.");

                    float maxRingMB = MaxRingBufferMB();

                    if (m_unthrottledBandwidth)
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        float dummyVal = 100.0f;
                        ImGui::SliderFloat("Bandwidth Throttle", &dummyVal, 0.2f, 100.0f, "Full (Uncapped)");
                        ImGui::PopStyleVar();
                    }
                    else
                    {
                        if (ImGui::SliderFloat("Bandwidth Throttle", &m_bandwidthThrottleMBps, 0.2f, 100.0f, "%.1f MB/s"))
                        {
                            m_unthrottledBandwidth = false;
                        }
                    }

                    ImGui::SliderFloat("GPU Ring Buffer Size", &m_ringBufferCapacityMB, 4.0f, maxRingMB, "%.0f MB");

                    // Streaming Progress Bar
                    float deliveredMB = m_simulatedBytesDelivered / (1024.0f * 1024.0f);
                    float totalMB = m_totalStreamBytes / (1024.0f * 1024.0f);
                    char progressOverlay[128];
                    if (m_evictedSurfelCount > 0)
                    {
                        snprintf(progressOverlay, sizeof(progressOverlay), "%.1f / %.1f MB (%.0f%%) | %u in RingBuffer (%zu Evicted)",
                            deliveredMB, totalMB, m_streamRefinementProgress * 100.0f, m_state.surfelCount, m_evictedSurfelCount);
                    }
                    else
                    {
                        snprintf(progressOverlay, sizeof(progressOverlay), "%.1f / %.1f MB (%.0f%%) | %u resident pts",
                            deliveredMB, totalMB, m_streamRefinementProgress * 100.0f, m_state.surfelCount);
                    }
                    ImGui::ProgressBar(m_streamRefinementProgress, ImVec2(-1, 20), progressOverlay);

                    // Playback Controls
                    if (ImGui::Button("Re-Stream (Reset)", ImVec2(125, 24)))
                    {
                        ResetStreamingSimulation();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(m_isStreamingPaused ? "Resume" : "Pause", ImVec2(75, 24)))
                    {
                        m_isStreamingPaused = !m_isStreamingPaused;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Instant Full Load", ImVec2(110, 24)))
                    {
                        m_simulatedBytesDelivered = m_totalStreamBytes;
                        m_streamRefinementProgress = 1.0f;
                        UpdateStreamingSimulation(1.0);
                    }

                    if (ImGui::Checkbox("Freeze Rendering & Memory (Eliminate Flickering)", &m_freezeRenderingAndMemory))
                    {
                        m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Completely freezes streaming simulation, LOD transitions, memory evictions, and edge recalculations.\nEliminates all flickering and jitter artifacts when movement is paused.");

                    ImGui::Separator();
                }
            }
            else
            {
                ImGui::TextDisabled("No active model loaded -- nothing to stream.");
            }
        }

        m_leftPanelRect = ImVec4(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y,
            ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
        ImGui::End();

        // 3. Right Statistics & Telemetry Panel. Its height fits the open sections (0 = auto-fit), up to the
        // full panel height, where it scrolls; the space below it stays free for the viewport (and the loupe).
        ImGui::SetNextWindowPos(ImVec2((float)m_Width - rightPanelWidth - 10.0f, 30.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(rightPanelWidth, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(rightPanelWidth, 0.0f), ImVec2(rightPanelWidth, panelHeight));
        ImGui::Begin("Statistics & Compression Analytics", nullptr, ImGuiWindowFlags_NoCollapse);

        // The sections in the user's order (RightPanelSectionHeader); a title-bar drag swaps two after the loop.
        int swapSlotA = -1, swapSlotB = -1;
        for (int slot = 0; slot < kRightSectionCount; slot++)
        {
            const int section = m_rightPanelOrder[slot];
            if (!RightPanelSectionHeader(slot, section, swapSlotA, swapSlotB)) continue;
            switch (section)
            {
                // Real-Time Performance & Stage Timings
                case kRightPerformance:
                {
                    const auto& metrics = m_pRenderer->GetTimingMetrics();

                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Framerate:       %.1f FPS", metrics.frameRate);
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Total Frame Time:%.2f ms", metrics.totalFrameTimeMs);
                    ImGui::Separator();
                    ImGui::Text("Per-Stage Breakdown (ms):");

                    float uploadDisplay = m_pRenderer->GetSmoothUploadMs();
                    if (uploadDisplay > 0.0001f)
                    {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  • Surfel/Chunk Buffer Upload:      %.2f ms", uploadDisplay);
                    }

                    if (m_gpuRadixSort)
                    {
                        float sortDisplay = (metrics.gpuSortTimeMs > 0.0001f) ? metrics.gpuSortTimeMs : m_pRenderer->GetSmoothGpuSortMs();
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "  • GPU Radix Depth Sort (32-Bit):    %.2f ms", sortDisplay);
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "  • CPU Radix Depth Sort (16-Bit):    %.2f ms", metrics.cpuSortTimeMs);
                    }

#if 0 // DYNAMIC SILHOUETTE: GPU silhouette chunk processing switched off for now (re-enable by restoring this block)
                    if (m_enableSilhouetteLOD0 || m_highlightSilhouetteChunks)
                    {
                        ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.98f, 1.0f), "  • Silhouette Item Prepass:          %.2f ms", m_pRenderer->GetSmoothSilhouettePrepassMs());
                    }
#endif // DYNAMIC SILHOUETTE

                    if (m_enableOcclusionCulling)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "  • Occlusion Volume Pass:            %.2f ms  (mip %u of %u)", m_pRenderer->GetSmoothOccluderMs(), m_pRenderer->GetActiveOcclusionMip(), m_occlusionMips.mipCount);
                    }

                    ImGui::Text(m_pRenderer->GetRenderPath() == SurfelsRenderer::RenderPath::MeshShaders ? "  • Main Splat Mesh Shader Dispatch:  %.2f ms" : "  • Main Splat Vertex Shader Draw:    %.2f ms", m_pRenderer->GetSmoothMainDispatchMs());

                    if (m_enableTemporalFiltering)
                    {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "  • TAA Resolve:                      %.2f ms", m_pRenderer->GetSmoothTaaMs());
                    }

                    // Where the frame time actually goes. The stage lines above are the CPU cost of recording each
                    // pass; this is the whole frame on the CPU wall clock, so the sum approaches the total.
                    ImGui::Separator();
                    ImGui::Text("Frame Budget (CPU wall clock, ms):");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the frame time goes, measured on the CPU. The stage lines above are command-recording costs only. GPU wait is the CPU blocked on the swapchain fence, i.e. waiting for the GPU (or VSync): a large value there means the GPU is the bottleneck.");
                    {
                        const float traversal = m_budgetShown[kBudgetTraversal];
                        const float simOther = std::max(0.0f, m_budgetShown[kBudgetSim] - traversal);
                        const float ui = m_budgetShown[kBudgetUi];
                        const float gpuWait = m_budgetShown[kBudgetGpuWait];
                        const float record = m_budgetShown[kBudgetRecord];
                        const float present = m_budgetShown[kBudgetPresent];
                        const float accounted = traversal + simOther + ui + gpuWait + record + present;
                        const float other = std::max(0.0f, metrics.totalFrameTimeMs - accounted);
                        const ImVec4 cpuCol(0.95f, 0.85f, 0.55f, 1.0f), gpuCol(0.55f, 0.85f, 1.0f, 1.0f), dimCol(0.6f, 0.6f, 0.6f, 1.0f);
                        ImGui::TextColored(cpuCol, "  \xe2\x80\xa2 LOD traversal & render list:      %.2f ms", traversal);
                        ImGui::TextColored(cpuCol, "  \xe2\x80\xa2 Streaming delivery & queues:      %.2f ms", simOther);
                        ImGui::TextColored(cpuCol, "  \xe2\x80\xa2 UI build (ImGui):                 %.2f ms", ui);
                        ImGui::TextColored(gpuCol, "  \xe2\x80\xa2 GPU wait (swapchain fence):       %.2f ms", gpuWait);
                        ImGui::TextColored(cpuCol, "  \xe2\x80\xa2 Command recording & uploads:      %.2f ms", record);
                        ImGui::TextColored(cpuCol, "  \xe2\x80\xa2 Present:                          %.2f ms", present);
                        ImGui::TextColored(dimCol, "  \xe2\x80\xa2 Other (input, window, OS):        %.2f ms", other);
                    }
                }
                break;

                // Compression Summary
                case kRightCompression:
                {
                    uint32_t pointCount = (uint32_t)(!m_rendererSurfels.empty() ? m_rendererSurfels.size() : m_rawSurfels.size());
                    const float bytesPerPoint = (float)BytesPerRenderPoint();
                    float tier2MB = (pointCount * bytesPerPoint) / (1024.0f * 1024.0f);
                    float tier2Reduction = (tier2MB > 0.001f && m_rawFileSizeMB > 0.0f) ? (m_rawFileSizeMB / tier2MB) : 31.0f;

                    ImGui::Text("Raw Point Cloud:      %.2f MB (100%%)", m_rawFileSizeMB);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uncompressed input point cloud dataset (248 bytes/splat uncompressed 3D Gaussian baseline).");

                    ImGui::Text("Tier 2 (%u-Byte GPU):  %.2f MB (%.1fx reduction)", (unsigned)bytesPerPoint, tier2MB, tier2Reduction);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(m_splatMode
                        ? "Tier 2 Quantization, splat mode: 20-byte Gaussian record (16:16:16 position, 8-bit log scales, opacity, 8:8:8 colour, quaternion) plus the spherical-harmonics stream."
                        : "Tier 2 Quantization: 8-byte packed GPU format (10:10:10:2 position, Oct16 normal, RGB565 color).");

                    ImGui::Text("Tier 3 (Morton Swizzle):  Contiguous 8-channel planes");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tier 3 Morton Swizzle & Transposition: Interleaves 3D spatial coordinate bits via Z-order curve and transposes 8-byte structures into 8 contiguous channels to maximize entropy redundancy.");

                    ImGui::Text("Tier 4 (Codec: Byte planes + Brotli): %.2f MB", m_compressedSizeMB);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tier 4 Entropy Codec: the package as it would be written -- every level's records and spherical-harmonics stream, transposed into byte planes and Brotli-compressed at quality 11. Every level ships in full, so this is about twice the level 0 data above before compression. Estimated from a sample while a bake is being set up; a loaded package shows its real file size.");

                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "TOTAL COMPRESSION:    %.2fx (%.2f MB -> %.2f MB)", tier2Reduction, m_rawFileSizeMB, tier2MB);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pinned to the Tier 2 (8-byte GPU quantization) ratio: this is the reduction every surfel gets just from the fixed-size packed format, independent of how well the RLE/entropy codec happens to compress a given model's byte patterns.");
                }
                break;

                // LOD Residency Equalizer (its title bar's tooltip is in RightPanelSectionHeader)
                case kRightResidency:
                    DrawLODResidencyEqualizer();
                    break;

                // Wavelet Multi-Resolution Pyramid Table
                case kRightPyramid:
                {
                    ImGui::Columns(4, "LODColumns");
                    ImGui::Text("Level"); ImGui::NextColumn();
                    ImGui::Text("Points"); ImGui::NextColumn();
                    ImGui::Text("Retained"); ImGui::NextColumn();
                    ImGui::Text("Max Err"); ImGui::NextColumn();
                    ImGui::Separator();

                    for (size_t i = 0; i < m_waveletResult.lodLevels.size(); i++)
                    {
                        const auto& lod = m_waveletResult.lodLevels[i];
                        float percent = (lod.surfels.size() * 100.0f) / std::max(1ULL, (unsigned long long)m_rawSurfels.size());

                        // Highlight the active visible LOD level in green/asterisk
                        bool isVisible = ((int)i == m_selectedPreviewLOD);

                        char label[64];
                        sprintf_s(label, "LOD %d%s", lod.level, isVisible ? " (ACTIVE)" : "");

                        if (ImGui::Selectable(label, isVisible, ImGuiSelectableFlags_SpanAllColumns))
                        {
                            m_selectedPreviewLOD = (int)i;
                            m_autoLOD = false; // Disable auto when user clicks manual table row
                            m_enableWavelet = true;
                            UpdatePreviewSurfels();
                        }
                        ImGui::NextColumn();

                        ImGui::Text("%u", (uint32_t)lod.surfels.size()); ImGui::NextColumn();
                        ImGui::Text("%.1f%%", percent); ImGui::NextColumn();
                        ImGui::Text("%.1f mm", lod.geometricError * 1000.0f); ImGui::NextColumn();
                    }
                    ImGui::Columns(1);
                    ImGui::TextDisabled("Tip: Click any row to visualize that LOD.");
                }
                break;

                // Model Metrics
                case kRightModel:
                {
                    uint32_t totalVertices = (uint32_t)(!m_rendererSurfels.empty() ? m_rendererSurfels.size() : m_rawSurfels.size());
                    ImGui::Text("Active Source:   %s", m_rendererSourceDescription.c_str());
                    ImGui::Text("Total Vertices:  %u points", totalVertices);
                    ImGui::Text("Uncompressed:    %.2f MB", m_rawFileSizeMB);
                    ImGui::Text("Bounding Box Min:[%.2f, %.2f, %.2f]", m_aabbMin.x, m_aabbMin.y, m_aabbMin.z);
                    ImGui::Text("Bounding Box Max:[%.2f, %.2f, %.2f]", m_aabbMax.x, m_aabbMax.y, m_aabbMax.z);
                    ImGui::Text("Spatial Extents: %.1f x %.1f x %.1f m", m_extents.x, m_extents.y, m_extents.z);
                }
                break;

                // Geometry Optimisations & Culling Stats
                case kRightCulling:
                {
                    const auto& cStats = m_pRenderer->GetCullStats();

                    uint32_t totalSurfels = cStats.totalDatasetSurfels;
                    uint32_t drawnSurfels = cStats.msDrawnSurfels;
                    uint32_t culledSurfels = (totalSurfels >= drawnSurfels) ? (totalSurfels - drawnSurfels) : 0;
                    float reductionPct = totalSurfels > 0 ? (float)culledSurfels / (float)totalSurfels * 100.0f : 0.0f;
                    float reductionFactor = drawnSurfels > 0 ? (float)totalSurfels / (float)drawnSurfels : 1.0f;

                    // Summary Badges / KPI Cards
                    ImGui::Columns(3, "CullKpiCols", false);
                    ImGui::TextDisabled("TOTAL SURFELS");
                    ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "%u", totalSurfels);
                    ImGui::NextColumn();

                    ImGui::TextDisabled("CULLED / SAVED");
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%u (%.1f%%)", culledSurfels, reductionPct);
                    ImGui::NextColumn();

                    ImGui::TextDisabled("DRAWN TO SCREEN");
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%u (%.1f%%)", drawnSurfels, 100.0f - reductionPct);
                    ImGui::NextColumn();
                    ImGui::Columns(1);

                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Optimization Efficiency: %.1fx reduction (%.1f%% eliminated)", reductionFactor, reductionPct);

                    ImGui::Spacing();

                    // Visual Proportional Multi-Segment Breakdown Bar
                    if (totalSurfels > 0)
                    {
                        ImVec2 barSize = ImVec2(ImGui::GetContentRegionAvailWidth(), 16.0f);
                        ImVec2 barPos = ImGui::GetCursorScreenPos();
                        ImDrawList* drawList = ImGui::GetWindowDrawList();

                        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), IM_COL32(30, 30, 30, 255), 2.0f);

                        float fracDrawn   = (float)drawnSurfels / (float)totalSurfels;
                        float fracFrustum = (float)cStats.asFrustumCulledSurfels / (float)totalSurfels;
                        float fracCone    = (float)cStats.asConeCulledSurfels / (float)totalSurfels;
                        float fracLod     = (float)cStats.lodPrunedSurfels / (float)totalSurfels;

                        float curX = barPos.x;
                        auto drawSegment = [&](float frac, ImU32 col) {
                            float w = frac * barSize.x;
                            if (w > 0.5f)
                            {
                                drawList->AddRectFilled(ImVec2(curX, barPos.y), ImVec2(curX + w, barPos.y + barSize.y), col, 2.0f);
                                curX += w;
                            }
                        };

                        drawSegment(fracDrawn,   IM_COL32(50, 205, 50, 255));   // Green: Drawn
                        drawSegment(fracFrustum, IM_COL32(70, 130, 230, 255));  // Blue: Frustum Culled
                        drawSegment(fracCone,    IM_COL32(255, 140, 0, 255));   // Orange: Normal Cone Culled
                        drawSegment(fracLod,     IM_COL32(160, 90, 220, 255));  // Purple: LOD Decimated

                        ImGui::Dummy(barSize);

                        // Legend Swatches
                        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[■] Drawn (%.1f%%)", fracDrawn * 100.0f);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "[■] Frustum (%.1f%%)", fracFrustum * 100.0f);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "[■] Cone (%.1f%%)", fracCone * 100.0f);
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.7f, 0.4f, 0.9f, 1.0f), "[■] LOD (%.1f%%)", fracLod * 100.0f);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();

                    // Rejection Stage Breakdown Table
                    ImGui::Text("Rejection Stage Breakdown:");
                    ImGui::Columns(4, "RejectionTableCols", true);
                    ImGui::Text("Optimization Stage"); ImGui::NextColumn();
                    ImGui::Text("Surfels"); ImGui::NextColumn();
                    ImGui::Text("Chunks"); ImGui::NextColumn();
                    ImGui::Text("Share / Rule"); ImGui::NextColumn();
                    ImGui::Separator();

                    // 1. LOD Decimation
                    float lodShare = totalSurfels > 0 ? (float)cStats.lodPrunedSurfels / (float)totalSurfels * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.75f, 0.5f, 0.95f, 1.0f), "1. LOD Multi-Res"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.lodPrunedSurfels); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.totalDatasetChunks > cStats.lodActiveChunks ? (cStats.totalDatasetChunks - cStats.lodActiveChunks) : 0); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (Pixel error)", lodShare); ImGui::NextColumn();

                    // 2. Task Shader Frustum Culling
                    float frustumShare = totalSurfels > 0 ? (float)cStats.asFrustumCulledSurfels / (float)totalSurfels * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "2. Task Frustum Cull"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asFrustumCulledSurfels); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asFrustumCulledChunks); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (6 Planes AABB)", frustumShare); ImGui::NextColumn();

                    // 3. Task Shader Normal Cone Culling
                    float coneShare = totalSurfels > 0 ? (float)cStats.asConeCulledSurfels / (float)totalSurfels * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "3. Task Backface Cone"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asConeCulledSurfels); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asConeCulledChunks); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (Cone Axis . Ray)", coneShare); ImGui::NextColumn();

                    // 4. Mesh Shader & Drawn
                    float drawnShare = totalSurfels > 0 ? (float)drawnSurfels / (float)totalSurfels * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "4. Mesh Shader Emitted"); ImGui::NextColumn();
                    ImGui::Text("%u", drawnSurfels); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asPassedChunks); ImGui::NextColumn();
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%.1f%% (SURVIVED)", drawnShare); ImGui::NextColumn();

                    ImGui::Columns(1);
                    ImGui::Separator();

                    // On-Chip Amplification & Bandwidth Savings
                    ImGui::Text("Hardware Amplification & Bandwidth:");
                    ImGui::BulletText("Generated Vertices:   %u  (4 per surfel)", cStats.generatedVertices);
                    ImGui::BulletText("Generated Triangles:  %u  (2 per surfel)", cStats.generatedTriangles);
                    ImGui::BulletText("VRAM Bandwidth Saved: %.2f MB/frame", cStats.vramBandwidthSavedMB);

                    uint32_t statsEdgeChunks = 0;
                    uint32_t statsTotalChunks = 0;
                    if (m_enableStreamingSimulation)
                    {
                        statsTotalChunks = (uint32_t)m_rendererSourceChunks.size();
                        for (const auto* pChunk : m_rendererSourceChunks)
                        {
                            if (pChunk && pChunk->isSilhouette) statsEdgeChunks++;
                        }
                    }
                    else if (!m_residentLODs.empty())
                    {
                        int maxLODIndex = std::max(0, (int)m_residentLODs.size() - 1);
                        int selectedLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                        const auto& resident = m_residentLODs[selectedLOD];
                        statsTotalChunks = (uint32_t)resident.meshletChunks.size();
                        for (const auto& mc : resident.meshletChunks)
                        {
                            if (mc.isSilhouette > 0.5f) statsEdgeChunks++;
                        }
                    }
                    ImGui::BulletText("Edge Chunks Detected: %u / %u active", statsEdgeChunks, statsTotalChunks);

                    if (cStats.lodActiveChunks > 0)
                    {
                        float earlyOutRatio = (float)(cStats.asFrustumCulledChunks + cStats.asConeCulledChunks) / (float)cStats.lodActiveChunks * 100.0f;
                        ImGui::BulletText("Task Early-Out Ratio: %.1f%% of micro-chunks rejected before Mesh stage", earlyOutRatio);
                    }
                }
                break;

                // Spatial Octree Metrics
                case kRightSpatial:
                {
                    uint32_t totalChunks = (uint32_t)(!m_rendererOctreeChunks.empty() ? m_rendererOctreeChunks.size() : m_chunks.size());
                    uint32_t totalVertices = (uint32_t)(!m_rendererSurfels.empty() ? m_rendererSurfels.size() : m_rawSurfels.size());
                    ImGui::Text("Chunk Voxel Size: %.1f meters", m_chunkSize);
                    ImGui::Text("Total Chunks:     %u spatial chunks", totalChunks);
                    if (totalChunks > 0)
                    {
                        ImGui::Text("Avg Points/Chunk: %u points", totalVertices / totalChunks);
                    }
                }
                break;
            }
        }
        if (swapSlotA >= 0 && swapSlotB >= 0)
        {
            std::swap(m_rightPanelOrder[swapSlotA], m_rightPanelOrder[swapSlotB]);
            SaveConfigFile();
        }

        m_rightPanelRect = ImVec4(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y,
            ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
        ImGui::End();

        // 4. Bottom Status & Notification Bar
        ImGui::SetNextWindowPos(ImVec2(10, (float)m_Height - 32));
        ImGui::SetNextWindowSize(ImVec2((float)m_Width - 20, 26));
        ImGui::Begin("StatusBar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImVec4 statusColor = m_statusIsSuccess ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(statusColor, "%s", m_statusMessage.c_str());
        ImGui::End();

        // 4b. Active render-mode banner over the viewport
        DrawRenderModeBanner(leftPanelWidth, rightPanelWidth);

        // 4c. Camera control hints at the bottom-right of the viewport
        DrawControlHints(leftPanelWidth, rightPanelWidth);

        // 4d. Reset View button at the bottom-left of the viewport
        DrawResetViewButton(leftPanelWidth);

        // 4e. Loupe box and inset frame
        m_rightPanelWidth = rightPanelWidth;
        DrawLoupeOverlay();

        // 5. About Dialog Window
        if (m_showAboutDialog)
        {
            ImGui::SetNextWindowSize(ImVec2(480, 280), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(((float)m_Width - 480) * 0.5f, ((float)m_Height - 280) * 0.5f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(IsViewerOnly() ? "About Surfels Viewer" : "About Surfel Generator", &m_showAboutDialog, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), IsViewerOnly() ? "Surfels Viewer" : "Surfel Generator & Splat Cruncher");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Author:       Dave Wilkinson");
                ImGui::Text("Organization: Blueshell LLC");
                ImGui::Text("Version:      v1.2.0");
                ImGui::Text("Date:         September 7, 2026");
                if (m_pRenderer) ImGui::Text("GPU path:     %s", m_pRenderer->GetRenderPathDescription());
                ImGui::Text("Codec:        %s%s%s", CodecBuild::Name(), CodecBuild::IsProprietary() ? "" : " -- ", CodecBuild::MissingFeatures());
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextWrapped(IsViewerOnly()
                    ? "The standalone viewer for .sflw surfel packages: progressive wavelet streaming with high-performance DirectX 12 Mesh Shader surfel rendering. The same renderer and streaming tabs as SplatLab, without the generator."
                    : "A multi-resolution point cloud preprocessor and progressive wavelet streaming pipeline built for high-performance DirectX 12 Mesh Shader surfel rendering.");
                ImGui::Spacing();
                ImGui::TextDisabled("(c) 2026 Blueshell LLC. All rights reserved.");

                ImGui::Spacing();
                if (ImGui::Button("Close", ImVec2(100, 26)))
                {
                    m_showAboutDialog = false;
                }
            }
            ImGui::End();
        }
    }

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // Draws the per-LOD-level residency bar graph plus the Evict/Decay Rate/Policy controls beside it
    void SurfelsApp::DrawLODResidencyEqualizer()
    {
        if (m_residentLODs.empty())
        {
            PrecacheResidentLODs();
        }

        int numLODs = (int)m_residentLODs.size();
        if (numLODs == 0)
        {
            ImGui::TextDisabled("No resident LOD levels available.");
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float availWidth = ImGui::GetContentRegionAvailWidth();

        if (m_freezeRenderingAndMemory)
        {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "[FROZEN]");
        }

        ImGui::Separator();

        const float cy = cosf(m_pitch), sy = sinf(m_pitch);
        const float sx = sinf(m_yaw), cx = cosf(m_yaw);
        XMFLOAT3 eyePos(
            m_target.x + m_distance * cy * sx,
            m_target.y + m_distance * sy,
            m_target.z + m_distance * cy * cx
        );

        if (m_smoothedLodResidentPct.size() != (size_t)numLODs)
        {
            m_smoothedLodResidentPct.assign(numLODs, 0.0f);
            m_smoothedLodResidentBlocks.assign(numLODs, 0);
        }

        m_equalizerUpdateTimer += (float)(m_deltaTime / 1000.0f);
        bool updateTextMetrics = (m_equalizerUpdateTimer >= 0.15f);
        if (updateTextMetrics)
        {
            m_equalizerUpdateTimer = 0.0f;
        }

        // Base segment count on the top row (Coarsest Base LOD)
        int baseSegments = (numLODs <= 2) ? 8 : 4;

        // Display rows: Top row = Highest LOD / Coarsest Base (numLODs - 1), moving down to LOD 0 (Fine)
        for (int rowIdx = 0; rowIdx < numLODs; rowIdx++)
        {
            int lodIdx = numLODs - 1 - rowIdx;
            const auto& lodData = m_residentLODs[lodIdx];
            uint32_t totalBlocks = (uint32_t)lodData.meshletChunks.size();
            size_t totalPts = lodData.rawSurfels.size();
            float totalMB = (float)(totalPts * (BytesPerRenderPoint())) / (1024.0f * 1024.0f);

            // Segments count doubles for each row down (width divides by 2)
            int numSegments = baseSegments * (1 << rowIdx);
            const float barHeight = 14.0f;

            const auto* pLevelChunks = (lodIdx < (int)m_lodStreamChunks.size()) ? &m_lodStreamChunks[lodIdx] : nullptr;
            size_t T = pLevelChunks ? pLevelChunks->size() : 0;

            // Lower level mip for child refinement calculation
            int childLodIdx = lodIdx - 1;
            const auto* pChildChunks = (childLodIdx >= 0 && childLodIdx < (int)m_lodStreamChunks.size()) ? &m_lodStreamChunks[childLodIdx] : nullptr;
            size_t TChild = pChildChunks ? pChildChunks->size() : 0;

            // Calculate residency stats
            size_t residentPts = 0;
            uint32_t exactResidentBlocks = 0;
            if (m_enableStreamingSimulation)
            {
                residentPts = (lodIdx < (int)m_lodResidentSurfels.size()) ? m_lodResidentSurfels[lodIdx] : 0;
                if (pLevelChunks)
                {
                    for (const auto& c : *pLevelChunks)
                    {
                        if (c.isResident) exactResidentBlocks++;
                    }
                }
            }
            else
            {
                residentPts = totalPts;
                exactResidentBlocks = totalBlocks;
            }

            size_t totalLevelPts = (lodIdx < (int)m_lodTotalSurfels.size() && m_lodTotalSurfels[lodIdx] > 0) ? m_lodTotalSurfels[lodIdx] : totalPts;
            float rawResidentPct = (totalLevelPts > 0) ? std::min(100.0f, (float)residentPts / (float)totalLevelPts * 100.0f) : 100.0f;

            if (updateTextMetrics || m_smoothedLodResidentBlocks[lodIdx] == 0)
            {
                m_smoothedLodResidentPct[lodIdx] = rawResidentPct;
                m_smoothedLodResidentBlocks[lodIdx] = exactResidentBlocks;
            }

            float residentPct = m_smoothedLodResidentPct[lodIdx];
            uint32_t residentBlocks = m_smoothedLodResidentBlocks[lodIdx];

            bool isActiveLOD = (lodIdx == m_selectedPreviewLOD);

            // Row Header
            if (isActiveLOD)
            {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "LOD %d%s [%.0f%% Resident] - %u / %u blocks (%zu pts | %.1f MB) [ACTIVE VIEW]",
                    lodIdx, (lodIdx == 0 ? " (Fine 100%)" : (lodIdx == numLODs - 1 ? " (Coarse Base)" : "")),
                    residentPct, residentBlocks, totalBlocks, totalPts, totalMB);
            }
            else
            {
                ImGui::Text("LOD %d%s [%.0f%% Resident] - %u / %u blocks (%zu pts | %.1f MB)",
                    lodIdx, (lodIdx == 0 ? " (Fine 100%)" : (lodIdx == numLODs - 1 ? " (Coarse Base)" : "")),
                    residentPct, residentBlocks, totalBlocks, totalPts, totalMB);
            }

            ImVec2 p0 = ImGui::GetCursorScreenPos();

            // Background recessed slot
            drawList->AddRectFilled(p0, ImVec2(p0.x + availWidth, p0.y + barHeight), IM_COL32(16, 18, 22, 255), 2.0f);
            drawList->AddRect(p0, ImVec2(p0.x + availWidth, p0.y + barHeight), IM_COL32(32, 35, 42, 255), 2.0f);

            // Render segments nested hierarchically
            for (int s = 0; s < numSegments; s++)
            {
                // Exact subdivision bounds so 2 child segments fit directly under 1 parent segment
                float x0 = p0.x + ((float)s / (float)numSegments) * availWidth;
                float x1 = p0.x + ((float)(s + 1) / (float)numSegments) * availWidth;

                ImVec2 segMin = ImVec2(x0 + 0.5f, p0.y + 1.5f);
                ImVec2 segMax = ImVec2(x1 - 0.5f, p0.y + barHeight - 1.5f);

                // Determine if this specific chunk/span is resident and if it is an edge/silhouette chunk
                bool isLit = false;
                bool isLocked = false;
                float childRatio = 1.0f; // 1.0 = fully refined
                size_t resCount = 0;
                size_t transCount = 0;
                size_t silCount = 0;

                if (pLevelChunks && T > 0)
                {
                    size_t idxStart = (size_t)(((float)s / (float)numSegments) * T);
                    size_t idxEnd = std::min(T, std::max(idxStart + 1, (size_t)(((float)(s + 1) / (float)numSegments) * T)));

                    for (size_t c = idxStart; c < idxEnd; c++)
                    {
                        const auto& chunk = (*pLevelChunks)[c];
                        if (!m_enableStreamingSimulation || chunk.isResident)
                        {
                            resCount++;
                            if (chunk.isLockedInTransition && chunk.transitionProgress > 0.001f && chunk.transitionProgress < 0.999f)
                            {
                                transCount++;
                            }
                            if (chunk.isSilhouette)
                            {
                                silCount++;
                            }
                        }
                    }
                }

                isLit = (resCount > 0) || !m_enableStreamingSimulation;
                // Lavender whenever the segment holds a detected edge chunk: the residency graph reports
                // state, so it must not depend on the Highlight Edge Chunks visualizer toggle (which only
                // tints the viewport). Edge detection runs while refinement or highlighting is enabled.
                bool isSil = (isLit && silCount > 0);
                bool isTrans = (isLit && transCount > 0);

                if (isLit)
                {
                    ImU32 colBase = IM_COL32(35, 215, 80, 255);
                    ImU32 colHighlight = IM_COL32(75, 245, 120, 255);

                    if (isSil)
                    {
                        // Silhouette / edge segments shaded in vibrant Lavender
                        colBase = IM_COL32(195, 140, 255, 255);
                        colHighlight = IM_COL32(230, 195, 255, 255);
                    }
                    else if (isTrans)
                    {
                        // Orange shading for locked transition chunks
                        colBase = IM_COL32(255, 140, 20, 255);
                        colHighlight = IM_COL32(255, 195, 70, 255);
                    }

                    drawList->AddRectFilled(segMin, segMax, colBase, 1.0f);
                    if (segMax.x - segMin.x > 3.0f)
                    {
                        drawList->AddLine(
                            ImVec2(segMin.x + 0.5f, segMin.y + 0.5f),
                            ImVec2(segMax.x - 0.5f, segMin.y + 0.5f),
                            colHighlight, 1.0f);
                    }
                }
                else
                {
                    drawList->AddRectFilled(segMin, segMax, IM_COL32(30, 32, 38, 255), 1.0f);
                    drawList->AddRect(segMin, segMax, IM_COL32(20, 22, 26, 255), 1.0f);
                }

                // Calculate child loaded ratio from lower level mip
                if (isLit)
                {
                    if (pChildChunks && TChild > 0)
                    {
                        size_t cStart = (size_t)(((float)s / (float)numSegments) * TChild);
                        size_t cEnd = std::min(TChild, std::max(cStart + 1, (size_t)(((float)(s + 1) / (float)numSegments) * TChild)));
                        size_t childResidentCount = 0;
                        size_t childTotal = cEnd - cStart;
                        for (size_t c = cStart; c < cEnd; c++)
                        {
                            if ((*pChildChunks)[c].isResident) childResidentCount++;
                        }
                        childRatio = (childTotal > 0) ? ((float)childResidentCount / (float)childTotal) : 0.0f;
                    }
                    else
                    {
                        // Terminal finest LOD level (LOD 0) has no lower mip -> 100% refined
                        childRatio = 1.0f;
                    }
                }
            }

            ImGui::Dummy(ImVec2(availWidth, barHeight + 4.0f));
        }

        ImGui::Spacing();

        // Legend with colored squares and white text below the equalizer bars
        float sqSize = 9.0f;
        ImVec2 legP = ImGui::GetCursorScreenPos();

        // 1. Resident (Green square + "Resident")
        drawList->AddRectFilled(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(35, 215, 80, 255), 1.0f);
        drawList->AddRect(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(75, 245, 120, 255), 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sqSize + 4.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Resident");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8.0f, 0.0f));
        ImGui::SameLine();

        // 2. Transition Lock (Orange square + "Transition Lock")
        legP = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(255, 140, 20, 255), 1.0f);
        drawList->AddRect(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(255, 195, 70, 255), 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sqSize + 4.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Transition Lock");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8.0f, 0.0f));
        ImGui::SameLine();

        // 3. Silhouette Lock (highlight square + "Silhouette Lock") - Always shown
        legP = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(185, 145, 245, 255), 1.0f);
        drawList->AddRect(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(220, 190, 255, 255), 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sqSize + 4.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Silhouette Lock");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8.0f, 0.0f));
        ImGui::SameLine();

        // 4. Unloaded (Dark square + "Unloaded")
        legP = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(30, 32, 38, 255), 1.0f);
        drawList->AddRect(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(50, 55, 65, 255), 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sqSize + 4.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Unloaded");

        ImGui::Spacing();
        ImGui::Separator();

        // Control buttons directly under the residency indicators. Evict is the one destructive button
        // here (it scrubs every level but the two pinned coarsest ones), so it is red.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.14f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.95f, 0.30f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.95f, 0.95f, 1.0f));
        const bool evictPressed = ImGui::Button("Evict", ImVec2(70.0f, 0.0f));
        ImGui::PopStyleColor(4);
        if (evictPressed)
        {
            ClearResidentStream();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Evicts refined detail stream chunks (LOD 0..max-2) while keeping the top two base mip levels (max & max-1) permanently resident.");
        }

        ImGui::SameLine();
        ImGui::PushItemWidth(availWidth - 165.0f);
        if (ImGui::SliderFloat("##DecaySlider", &m_streamDecayRate, 0.0f, kMaxDecayRate, "Decay Rate: %.2f"))
        {
            m_streamStateDirty = true;
            CalibrateDecay();
        }
        if (ImGui::IsItemHovered())
        {
            float drainSeconds = DecayFullDrainSeconds();
            if (m_enableStreamDecay && m_streamDecayRate > 0.0f && m_decayBytesPerSec > 0.0f)
            {
                const float inflow = m_unthrottledBandwidth ? 0.0f : m_bandwidthThrottleMBps;
                const float outflowMB = m_decayBytesPerSec / (1024.0f * 1024.0f);
                char ref[48];
                if (m_decayReferenceBandwidthBps > 0.0f) snprintf(ref, sizeof(ref), "%.1f MB/s", m_decayReferenceBandwidthBps / (1024.0f * 1024.0f)); else snprintf(ref, sizeof(ref), "Full");
                ImGui::SetTooltip("Decay outflow %.1f MB/s, fixed when the toggle or slider last changed (reference bandwidth %s: a full drain of every evictable level in ~%.1f s at that bandwidth).\nThe bandwidth throttle is now the knob: current inflow %s, so the net drain is %.1f MB/s.",
                    outflowMB, ref, drainSeconds, m_unthrottledBandwidth ? "Full (uncapped)" : "", outflowMB - inflow);
            }
            else if (m_streamDecayRate <= 0.0f)
                ImGui::SetTooltip("0 = no decay. Drag right to drain unused detail out of memory.\nIndependent of bandwidth: at 10 it clears everything but the two pinned\ncoarsest levels in %.0f s on any bandwidth setting.", kDecayFullDrainSecondsAtMaxRate);
            else
                ImGui::SetTooltip("Full drain of all evictable levels in ~%.1f s, on any bandwidth setting.\nAt 10 it clears everything but the two pinned coarsest levels in %.0f s.",
                    drainSeconds, kDecayFullDrainSecondsAtMaxRate);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Checkbox("Decay", &m_enableStreamDecay))
        {
            m_streamStateDirty = true;
            CalibrateDecay();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Toggle LRU chunk memory reclamation over time.\nHigher decay rates reclaim memory faster from the lowest priority/least needed LODs.\nDrain speed is independent of the bandwidth throttle.");
        }

        ImGui::Spacing();
        ImGui::Text("Policy:");
        ImGui::SameLine();
        int policyRadio2 = (m_streamingPolicy == StreamingPolicy::Conservative) ? 0 : 1;
        if (ImGui::RadioButton("Conservative##Eq", &policyRadio2, 0))
        {
            m_streamingPolicy = StreamingPolicy::Conservative;
            m_streamStateDirty = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Conservative Mode: the view's own requests cover only visible chunks + a local neighbour buffer, and detail the view drops back from is evicted.\nThe scheduler's face streams keep running in both policies.");
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Greedy##Eq", &policyRadio2, 1))
        {
            m_streamingPolicy = StreamingPolicy::Greedy;
            m_streamStateDirty = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Greedy Mode: Satisfies visible view first, then continuously streams all remaining background chunks until entire model is loaded.");
        }

        ImGui::Spacing();
    }

    // Refinement visualizer controls on the Renderer tab (idSuffix keeps the widget IDs distinct should the
    // block ever be drawn in a second place): the arrival-glow checkbox, and when it is on, the fade
    // duration, glow intensity and hue sliders.
    void SurfelsApp::DrawRefinementVisualizerControls(const char* idSuffix)
    {
        std::string label = std::string("Refinement Visualizer") + idSuffix;
        if (ImGui::Checkbox(label.c_str(), &m_showChunkStream))
        {
            m_streamStateDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visualizer for progressive loading: every chunk that arrives is filled with a semi-transparent tint. The newest chunks -- the leading edge of the growing model -- glow bright, then settle to the regular tint and fade out over the duration below. Press Evict to watch the whole model grow back in the scheduler's order. Named in the top-left banner while it is on.");
        if (!m_showChunkStream) return;
        ImGui::Indent(12.0f);
        ImGui::PushItemWidth(150.0f);
        ImGui::SliderFloat((std::string("Fade Duration") + idSuffix).c_str(), &m_chunkStreamDuration, 0.5f, 10.0f, "%.1f s");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long a delivered chunk stays tinted. The bright glow occupies roughly the first third; the tint fades out at the end.");
        ImGui::SliderFloat((std::string("Glow Intensity") + idSuffix).c_str(), &m_arrivalGlowIntensity, 0.0f, 2.0f, "%.2fx");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strength of the tint and of the bright leading-edge bloom. 1 = default; 0 hides the effect without turning the visualizer off.");
        ImGui::SliderFloat((std::string("Glow Hue") + idSuffix).c_str(), &m_arrivalGlowHue, -180.0f, 180.0f, "%.0f deg");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotates the glow colour around the colour wheel. 0 = the default hue; about +95 = green, +180 = blue, -60 = magenta.");
        ImGui::PopItemWidth();
        ImGui::Unindent(12.0f);
    }

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // Splat mode: how soft the level 0 Gaussians are relative to their point spacing, so the coarse levels'
    // discs can be sized to read the same (Match Level 0 Softness). The footprint of a record is
    // sqrt(s_max x s_mid), the in-plane extent of a flat Gaussian (its smallest scale is the thickness),
    // decoded from the scale bytes as the shader does; the median over a sample of up to 64k level 0
    // records is divided by the level 0 spacing to give the softness ratio (a median so a few huge
    // floaters cannot inflate it). A disc with the splat falloff exp(-4 (r/R)^2) is a Gaussian of sigma
    // R / sqrt(8), so the disc radius factor that matches is sqrt(8) x ratio, clamped to 1.5..4.0: below
    // 1.5 the discs would be narrower than the fixed rule, above 4 the fill cost (radius squared) is too
    // high on dense views. Runs right after ComputeLodSpacing; the values are reset first, so a package
    // without level 0 Gaussians leaves the fixed 1.5.
    void SurfelsApp::ComputeSplatSoftness()
    {
        m_splatLevel0Footprint = 0.0f;
        m_splatSoftnessRatio = 0.0f;
        m_splatDiscMatchFactor = 1.5f;
        if (!m_splatMode || m_residentLODs.empty() || m_residentLODs[0].splats.empty() || m_lodSpacing.empty()) return;
        const std::vector<PackedSplatGPU>& records = m_residentLODs[0].splats;
        const size_t total = records.size();
        const size_t samples = std::min<size_t>(total, 65536);
        const size_t stride = std::max<size_t>(1, total / samples);
        const float log2Min = m_splatParams.scaleLog2Min, log2Max = m_splatParams.scaleLog2Max;
        std::vector<float> footprints; footprints.reserve(samples);
        for (size_t i = 0; i < total; i += stride)
        {
            const PackedSplatGPU& s = records[i];
            // The three scale bytes as BuildGaussianSplat reads them: word1 bits 16..23, 24..31, word2 bits 0..7
            float sc[3] = { (float)((s.word1 >> 16) & 0xFFu), (float)((s.word1 >> 24) & 0xFFu), (float)(s.word2 & 0xFFu) };
            for (float& v : sc) v = exp2f(log2Min + (v / 255.0f) * (log2Max - log2Min));
            std::sort(sc, sc + 3);
            footprints.push_back(sqrtf(sc[2] * sc[1]));
        }
        if (footprints.empty()) return;
        std::nth_element(footprints.begin(), footprints.begin() + footprints.size() / 2, footprints.end());
        m_splatLevel0Footprint = footprints[footprints.size() / 2];
        const float spacing0 = m_lodSpacing[0];
        m_splatSoftnessRatio = (spacing0 > 0.0f) ? m_splatLevel0Footprint / spacing0 : 0.0f;
        m_splatDiscMatchFactor = std::clamp(sqrtf(8.0f) * m_splatSoftnessRatio, 1.5f, 4.0f);
        LogTransitionTrace("SplatSoftness: level 0 median footprint %.5f, spacing %.5f, ratio %.2f, disc radius factor %.2f", m_splatLevel0Footprint, spacing0, m_splatSoftnessRatio, m_splatDiscMatchFactor);
    }

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // [Removed from the public history: proprietary streaming runtime, now in libs/gstx/StreamRuntime]

    // Streaming tab: the bounding octahedron as two diamonds. Seen from above, an octahedron projects to
    // a square standing on a corner whose diagonals split it into its four upper faces; the left diamond
    // (green) shows those, the right one (pastel blue) the four lower faces, both with +X right and +Z
    // down. Visible faces are lit, brighter the more directly they face the camera, hidden faces dim; a
    // yellow dot marks the camera's direction on whichever half it lies in (a dim dot on the other).
    void SurfelsApp::DrawOctahedronGlyph()
    {
        uint32_t visible = 0;
        for (int f = 0; f < kOctahedronFaces; f++) if (m_streamOrder.VisibleFaceMask() & (1u << f)) visible++;
        ImGui::TextDisabled("Bounding octahedron: %u of %d faces visible, %zu blocks in this frame's scratch load list", visible, kOctahedronFaces, m_scratchLoadList.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Faces of the octahedron around the model that the camera can currently see. Each owns the part of the model behind it; the scheduler streams the visible faces together and the hidden ones afterwards, so the model completes in the background. The inner triangles show each face's fill progress.");

        const float R = 30.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        const float cell = 2.0f * R + 12.0f;
        auto drawHalf = [&](const ImVec2& c, bool upper)
        {
            // The four faces of this half: octants with the matching Y sign. Each is the triangle from the
            // centre (the +Y or -Y apex, seen end-on) to the two equatorial vertices of its X and Z signs.
            // Two passes: every fill first, then every outline, so the outline and cross bars are always
            // drawn on top at full opacity whatever the fills are doing.
            for (int pass = 0; pass < 2; pass++)
            for (int f = 0; f < kOctahedronFaces; f++)
            {
                if (((f & 2) != 0) != upper) continue;
                const float sx = (f & 1) ? 1.0f : -1.0f, sz = (f & 4) ? 1.0f : -1.0f;
                ImVec2 px(c.x + sx * R, c.y), pz(c.x, c.y + sz * R);
                const bool vis = (m_streamOrder.VisibleFaceMask() & (1u << f)) != 0;
                const float t = vis ? std::max(0.0f, std::min(1.0f, (m_streamOrder.FaceFacing(f) + 0.15f) / 1.15f)) : 0.0f;
                // Bevelled look: each face is shaded as one side of a low pyramid seen from above, lit from
                // the upper left (both halves are drawn that way so they read alike); its two ridge edges
                // (centre to rim) are light on the lit side and dark on the shaded side, its rim edge dark.
                const float lx = -0.55f, ly = 0.65f, lz = -0.52f;           // Light direction: from the upper left, above
                const float nx = sx * 0.577f, ny = 0.577f, nz = sz * 0.577f;  // Face normal of the pyramid side
                const float ndotl = nx * lx + ny * ly + nz * lz;
                const float shade = 0.55f + 0.45f * std::max(0.0f, std::min(1.0f, ndotl + 0.2f)); // 0.55 .. 1.0
                float br, bg, bb;                                            // Base colour of this face
                if (upper) { br = vis ? 55.0f + 30.0f * t : 60.0f;  bg = vis ? 170.0f + 70.0f * t : 66.0f; bb = vis ? 85.0f + 35.0f * t : 74.0f; }
                else       { br = vis ? 100.0f + 30.0f * t : 60.0f; bg = vis ? 150.0f + 45.0f * t : 64.0f; bb = vis ? 225.0f + 30.0f * t : 78.0f; }
                auto shaded = [&](float k, int alpha) { return IM_COL32((int)std::min(255.0f, br * k), (int)std::min(255.0f, bg * k), (int)std::min(255.0f, bb * k), alpha); };
                // Fill opacity is the face's residency: 0% with nothing loaded, 80% with every block loaded,
                // every grade between. The outline and cross bars below are always fully opaque.
                float done = 0.0f;
                if (m_streamOrder.FaceTotal(f) > 0) done = 1.0f - (float)std::min(m_streamOrder.FaceRemaining(f), m_streamOrder.FaceTotal(f)) / (float)m_streamOrder.FaceTotal(f);
                const ImU32 fill = shaded(shade, (int)(204.0f * std::max(0.0f, std::min(1.0f, done))));
                if (pass == 1)
                {
                    const bool lit = ndotl > 0.05f;
                    const ImU32 ridge = lit ? shaded(1.35f, 255) : shaded(0.45f, 255);
                    const ImU32 rim = shaded(0.40f, 255);
                    const float w = vis ? 2.0f : 1.0f;
                    dl->AddLine(c, px, ridge, w);
                    dl->AddLine(c, pz, ridge, w);
                    dl->AddLine(px, pz, rim, w);
                    continue;
                }
                dl->AddTriangleFilled(c, px, pz, fill);
            }
            // Camera marker: its horizontal direction, on this half if the camera is on this side of the equator.
            const bool camHere = (m_streamOrder.CameraDirection()[1] >= 0.0f) == upper;
            const float h = sqrtf(m_streamOrder.CameraDirection()[0] * m_streamOrder.CameraDirection()[0] + m_streamOrder.CameraDirection()[2] * m_streamOrder.CameraDirection()[2]);
            const float mx = (h > 1e-4f) ? m_streamOrder.CameraDirection()[0] / h : 0.0f, mz = (h > 1e-4f) ? m_streamOrder.CameraDirection()[2] / h : 0.0f;
            const float reach = std::min(1.0f, h + 0.15f) * (R + 6.0f); // Toward the centre as the camera goes overhead
            dl->AddCircleFilled(ImVec2(c.x + mx * reach, c.y + mz * reach), camHere ? 3.5f : 2.0f, camHere ? IM_COL32(255, 220, 90, 255) : IM_COL32(255, 220, 90, 90));
        };
        drawHalf(ImVec2(origin.x + R + 6.0f, origin.y + R + 6.0f), true);
        drawHalf(ImVec2(origin.x + cell + 14.0f + R + 6.0f, origin.y + R + 6.0f), false);
        ImGui::Dummy(ImVec2(2.0f * cell + 14.0f, cell));
        if (ImGui::IsItemHovered())
        {
            std::string tip = "Left (green): the four upper faces (+Y), right (blue): the four lower faces (-Y), both seen from above with +X right and +Z down.\nYellow dot = camera direction; inner triangle = share of the face's blocks resident. Pending / total blocks per face, and queued edge requests (bits: 1 = +X, 2 = +Y, 4 = +Z):";
            for (int f = 0; f < kOctahedronFaces; f++)
            {
                char b[96];
                snprintf(b, sizeof(b), "\n  face %d (%c%c%c): %u / %u, %zu edge requests %s", f, (f & 1) ? '+' : '-', (f & 2) ? '+' : '-', (f & 4) ? '+' : '-',
                    m_streamOrder.FaceRemaining(f), m_streamOrder.FaceTotal(f), m_faceEdgeQueue[f].size() - m_faceEdgeHead[f], (m_streamOrder.VisibleFaceMask() & (1u << f)) ? "(visible)" : "");
                tip += b;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }

    // Overlay listing every toggle that currently alters what the viewport shows -- debug views, isolation
    // modes, frozen state, forced levels -- as "TAB NAME: <feature> enabled" lines, one per row, each in
    // its own colour, anchored at the top-left of the viewport next to the control panel. Its purpose is purely
    // to stop a user from reading a deliberately altered picture (surfels hidden, a stale culling
    // frustum, lavender-tinted chunks, a frozen stream) as a rendering bug: whenever the model looks
    // wrong, the reason is written on the screen. Draws nothing when no such mode is active. Ordinary
    // quality/performance settings (TAA, dithering, cone culling, the occlusion volume itself) are not
    // listed -- they are the normal picture, not a departure from it.
    void SurfelsApp::DrawRenderModeBanner(float leftPanelWidth, float rightPanelWidth)
    {
        struct ModeLine { char text[128]; ImVec4 color; };
        ModeLine lines[16];
        int count = 0;
        auto add = [&](const ImVec4& color, const char* fmt, ...)
        {
            if (count >= (int)(sizeof(lines) / sizeof(lines[0]))) return;
            va_list args;
            va_start(args, fmt);
            vsnprintf(lines[count].text, sizeof(lines[count].text), fmt, args);
            va_end(args);
            lines[count].color = color;
            count++;
        };

        // Order: the modes that hide or replace the model first, then tints/overlays, then frozen/forced state.
        if (m_showOcclusionVolumeOnly)     add(ImVec4(1.00f, 0.90f, 0.20f, 1.0f), "RENDERER: View Occlusion Volume Only enabled");
        if (m_showOnlyLockedChunks)        add(ImVec4(1.00f, 0.60f, 0.20f, 1.0f), "RENDERER: Show ONLY Locked Chunks enabled");
        if (m_highlightSilhouetteChunks)   add(ImVec4(0.78f, 0.68f, 1.00f, 1.0f), "RENDERER: Highlight Edge Chunks enabled");
        if (m_showChunkStream)             add(ImVec4(1.00f, 0.62f, 0.20f, 1.0f), "RENDERER: Refinement Visualizer enabled");
        if (m_showClusterHeatmap)          add(ImVec4(1.00f, 0.45f, 0.35f, 1.0f), m_heatmapSource == 1 ? "RENDERER: Detail Heatmap Cluster Cubes enabled" : "RENDERER: Density Heatmap Cluster Cubes enabled");
        if (m_showHeatmapWireframe)        add(ImVec4(0.92f, 0.82f, 0.60f, 1.0f), "RENDERER: Cube Outlines enabled");
        if (m_showOctreeVisualizer)        add(ImVec4(1.00f, 0.75f, 0.20f, 1.0f), "RENDERER: Macro Clusters enabled");
        if (m_showGlobalBounds)            add(ImVec4(0.40f, 0.60f, 1.00f, 1.0f), "RENDERER: Global Model Bounds enabled");
        if (m_state.renderMode == 4)       add(ImVec4(0.55f, 0.85f, 1.00f, 1.0f), "SPLAT: 3D Gaussian package drawn as surfels, SH degree %u, opaque discs", m_splatParams.shDegree);
        if (m_state.renderMode == 3)
        {
            const char* fmt = !kSplatGaussianLevel0     ? "SPLAT: 3D Gaussian package, SH degree %u, %s-space blending, every level as discs (Gaussian level 0 off)"
                            : m_splatCoarseAsGaussians ? "SPLAT: 3D Gaussian mode, SH degree %u, %s-space blending, levels 1+ as capped Gaussians"
                                                       : "SPLAT: 3D Gaussian mode, SH degree %u, %s-space blending, levels 1+ as discs";
            add(ImVec4(0.55f, 0.85f, 1.00f, 1.0f), fmt, m_splatParams.shDegree, m_splatBlendSpace == 1 ? "linear" : "display");
        }
        if (m_showCulledChunks)            add(ImVec4(0.80f, 0.80f, 0.80f, 1.0f), "RENDERER: Show Culled Chunks enabled");
        if (m_detachCamera)                add(ImVec4(0.30f, 0.90f, 1.00f, 1.0f), "RENDERER: Detach Camera (frozen culling frustum) enabled");
        if (m_freezeRenderingAndMemory)    add(ImVec4(0.55f, 0.75f, 1.00f, 1.0f), "STREAMING: Freeze Rendering & Memory enabled");
        if (m_enableStreamingSimulation && !m_unthrottledBandwidth) add(ImVec4(1.00f, 0.80f, 0.30f, 1.0f), "STREAMING: Bandwidth throttle %.1f MB/s enabled", m_bandwidthThrottleMBps);
        if (!m_autoLOD)                    add(ImVec4(0.50f, 1.00f, 0.50f, 1.0f), "RENDERER: Manual LOD %d enabled", m_selectedPreviewLOD);
        if (m_splatViz != 0)
        {
            static const char* s_vizBanner[] = { "", "splat outlines", "random colour per splat", "splat centres" };
            add(ImVec4(1.00f, 0.82f, 0.30f, 1.0f), "RENDERER: Splat Display: %s enabled", s_vizBanner[std::clamp(m_splatViz, 0, 3)]);
        }
        if (m_occlusionMipOverride >= 0)   add(ImVec4(1.00f, 0.55f, 0.80f, 1.0f), "RENDERER: Occlusion Volume Mip %d forced enabled", m_occlusionMipOverride);
        // The open stand-in codec is a different product: say so on every frame, in one short row.
        if (!CodecBuild::IsProprietary()) add(ImVec4(1.00f, 0.35f, 0.35f, 1.0f), "CODEC: %s (%s)", CodecBuild::Name(), CodecBuild::MissingFeatures()); // Kept to one short row: the codec README has the full list
        // Hardware fallbacks: one row per GPU stage the render path is not using, so a screenshot from a
        // machine without mesh shaders says exactly which stages it lacks (SurfelsRenderer::RenderPath).
        if (m_pRenderer && m_pRenderer->GetRenderPath() != SurfelsRenderer::RenderPath::MeshShaders)
        {
            const auto& caps = m_pRenderer->GetGpuCapabilities();
            const ImVec4 hw(1.00f, 0.40f, 0.30f, 1.0f);
            const bool forced = m_pRenderer->IsRenderPathForced();
            const char* why = forced ? "bypassed (forced)" : (caps.meshPipelineFailed ? "pipeline failed" : "unavailable");
            add(hw, "RENDERER: Amplification shader %s, VS chunk culling enabled", why);
            add(hw, "RENDERER: Mesh shader %s, instanced VS splats enabled", why);
            if (m_pRenderer->GetRenderPath() == SurfelsRenderer::RenderPath::VertexShadersSM5)
                add(hw, "RENDERER: Shader Model 6 %s, SM 5.1 legacy compiler enabled", (forced && caps.shaderModel6) ? "bypassed (forced)" : "unavailable");
        }
        m_bannerBottomY = 0.0f;
        if (count == 0) return;

        // Top-left of the viewport: just right of the left panel, below the menu bar, never under the
        // right panel.
        const float x = 10.0f + leftPanelWidth + 10.0f;
        const float y = 36.0f;
        if (x > (float)m_Width - rightPanelWidth - 40.0f) return; // No viewport strip to draw into at this window size

        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f)); // Dark backing so the text stays readable over any model colour
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##RenderModeBanner", nullptr, flags))
        {
            for (int i = 0; i < count; i++)
            {
                ImGui::TextColored(lines[i].color, "%s", lines[i].text);
            }
            m_bannerBottomY = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y; // The loupe inset goes below it
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // Camera distance at which the model makes the best use of the viewport: its bounding box, projected
    // with the current orbit angle, is brought as close as possible to a one-centimetre screen margin
    // from whichever limit it reaches first -- the inner edge of a control panel (left/right) or the
    // top/bottom of the canvas (below the menu bar, above the status bar) -- and never past it. The
    // margin is one centimetre on the actual display (from its DPI). Found by bisection on the distance
    // against the projected box corners, so the result is exact for the view that will be shown.
    float SurfelsApp::FitDistanceForViewport() const
    {
        if (!(m_extents.x > 1e-6f || m_extents.y > 1e-6f || m_extents.z > 1e-6f)) return 25.0f;
        // The startup dataset loads before the window has been sized (OnCreate runs ahead of the first
        // OnResize), so fall back to the initial client size from OnParseCommandLine in that case.
        const float width  = (m_Width  > 0) ? (float)m_Width  : 1440.0f;
        const float height = (m_Height > 0) ? (float)m_Height : 900.0f;
        float dpi = 96.0f;
        if (HDC dc = GetDC(NULL)) { dpi = (float)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }
        const float panel = std::min(width * 0.45f, std::max(300.0f, 420.0f * m_uiScale));
        // One centimetre, plus an allowance for what the fitted points do not capture (the splat discs
        // drawn around them and the few points the subsample skips), so the margin is never breached.
        const float usableW = width - 2.0f * (10.0f + panel), usableH = height - 22.0f - 32.0f;
        const float marginPx = dpi / 2.54f + 0.03f * std::min(usableW, usableH);
        // Usable rectangle in pixels, then in NDC (y up).
        const float left = 10.0f + panel + marginPx, right = width - 10.0f - panel - marginPx;
        const float top = 22.0f + marginPx, bottom = height - 32.0f - marginPx; // Menu bar above, status bar below
        if (right - left < 40.0f || bottom - top < 40.0f) return 25.0f;
        const float ndcL = left / width * 2.0f - 1.0f, ndcR = right / width * 2.0f - 1.0f;
        const float ndcT = 1.0f - top / height * 2.0f, ndcB = 1.0f - bottom / height * 2.0f;

        // Fit against the model's actual silhouette: a subsample of the coarsest LOD's points (the box's
        // corners stick out well past the visible model at a diagonal view and would leave the margin
        // unreached on every axis). Falls back to the box corners before the LODs exist.
        std::vector<XMFLOAT3> pts;
        if (!m_residentLODs.empty() && !m_residentLODs.back().rawSurfels.empty())
        {
            const auto& src = m_residentLODs.back().rawSurfels;
            const size_t stride = std::max<size_t>(1, src.size() / 200000); // Every point of the coarsest level, in practice
            pts.reserve(src.size() / stride + 1);
            for (size_t i = 0; i < src.size(); i += stride) pts.push_back(src[i].position);
        }
        else
        {
            pts = {
                { m_aabbMin.x, m_aabbMin.y, m_aabbMin.z }, { m_aabbMax.x, m_aabbMin.y, m_aabbMin.z },
                { m_aabbMin.x, m_aabbMax.y, m_aabbMin.z }, { m_aabbMax.x, m_aabbMax.y, m_aabbMin.z },
                { m_aabbMin.x, m_aabbMin.y, m_aabbMax.z }, { m_aabbMax.x, m_aabbMin.y, m_aabbMax.z },
                { m_aabbMin.x, m_aabbMax.y, m_aabbMax.z }, { m_aabbMax.x, m_aabbMax.y, m_aabbMax.z } };
        }
        const float cy = cosf(m_pitch), sy = sinf(m_pitch), sx = sinf(m_yaw), cx = cosf(m_yaw);
        const XMVECTOR at = XMLoadFloat3(&m_center);
        const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, width / height, 0.1f, 500.0f);
        auto fits = [&](float dist) -> bool
        {
            XMVECTOR eye = XMVectorSet(m_center.x + dist * cy * sx, m_center.y + dist * sy, m_center.z + dist * cy * cx, 1.0f);
            XMMATRIX viewProj = XMMatrixMultiply(XMMatrixLookAtRH(eye, at, worldUp), proj);
            for (const XMFLOAT3& c : pts)
            {
                XMFLOAT4 clip;
                XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(c.x, c.y, c.z, 1.0f), viewProj));
                if (clip.w <= 0.11f) return false;
                const float x = clip.x / clip.w, y = clip.y / clip.w;
                if (x < ndcL || x > ndcR || y < ndcB || y > ndcT) return false;
            }
            return true;
        };
        // Bisection for the closest distance that still fits (fitting is monotonic in distance).
        float lo = 0.1f, hi = 1000.0f;
        if (!fits(hi)) return hi;
        for (int it = 0; it < 48; it++)
        {
            const float mid = 0.5f * (lo + hi);
            if (fits(mid)) hi = mid; else lo = mid;
        }
        return std::max(0.1f, std::min(1000.0f, hi));
    }

    // "Reset View" button in the bottom-left corner of the viewport (just right of the left panel, just
    // above the status bar): puts the camera back to the launch view -- the default orbit angle, centred
    // on the model, at the distance that fits it between the control panels.
    void SurfelsApp::DrawResetViewButton(float leftPanelWidth)
    {
        const float x = 10.0f + leftPanelWidth + 10.0f;
        const float y = (float)m_Height - 32.0f - 6.0f;
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(0.0f, 1.0f)); // Pivot: bottom-left corner
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.03f, 0.55f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##ResetViewButton", nullptr, flags))
        {
            if (ImGui::Button("Reset View", ImVec2(96.0f, 24.0f)))
            {
                m_yaw = 0.6f;
                m_pitch = 0.35f;
                m_target = m_center;
                ApplyFitDistance();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Returns the camera to the launch view: the default angle, centred on the model, fitted between the control panels. The culling camera, if detached, is left where it is.");
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // The title bar of the right-panel section at this slot. The open state is ours (m_rightPanelOpen, saved in
    // config.json), handed to ImGui every frame. Dragging the title bar more than its own height up or down
    // asks for a swap with the neighbouring slot (applied after the loop, so no section is drawn twice); the
    // drag delta is then reset, so each further step needs the same distance again. A click that ends a drag
    // which moved the section leaves its open state as it was.
    bool SurfelsApp::RightPanelSectionHeader(int slot, int section, int& swapSlotA, int& swapSlotB)
    {
        static const char* s_titles[kRightSectionCount] = {
            "Real-Time Performance & Stage Timings", "4-Tier Compression Results", "LOD Residency",
            "Wavelet Multi-Resolution Pyramid", "Input Model Metrics", "Geometry Optimisations & Culling Stats", "Spatial Partitioning" };
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.MouseDown[0] && !io.MouseReleased[0]) m_rightDragMoved = false;

        ImGui::SetNextTreeNodeOpen(m_rightPanelOpen[section], ImGuiCond_Always);
        const bool open = ImGui::CollapsingHeader(s_titles[section]);
        const bool active = ImGui::IsItemActive();
        // A title bar toggles when the button is released over it (1.53: press-and-release); after a drag that moved
        // the section, that release is the end of the move, not a click.
        if (open != m_rightPanelOpen[section] && !m_rightDragMoved)
        {
            m_rightPanelOpen[section] = open;
            SaveConfigFile();
        }

        if (section == kRightResidency && ImGui::IsItemHovered() && !active)
        {
            ImGui::SetTooltip(
                "Shows how much of each LOD level is currently resident in the simulated ring buffer\n"
                "(green), locked mid-transition (orange), or protected as an active silhouette edge\n"
                "(highlighted). Chunks stream in as the camera needs finer detail and drain out (via the\n"
                "Decay control below) when no longer needed.\n\n"
                "The fill rate is controlled by the Network Profiles / Bandwidth Throttle options on the\n"
                "Streaming tab -- lower bandwidth means slower fill. The drain rate is controlled only by\n"
                "the Decay Rate slider below and is independent of bandwidth: 0 = no decay, 10 = full\n"
                "drain of every evictable level in 3 s on any bandwidth setting.\n\n"
                "Drag any section's title bar up or down to reorder the panel.");
        }

        if (active && ImGui::IsMouseDragging(0, 4.0f))
        {
            const float dy = ImGui::GetMouseDragDelta(0).y;
            const float step = ImGui::GetItemRectSize().y;
            if (dy < -step && slot > 0) { swapSlotA = slot; swapSlotB = slot - 1; }
            else if (dy > step && slot + 1 < kRightSectionCount) { swapSlotA = slot; swapSlotB = slot + 1; }
            if (swapSlotA == slot)
            {
                ImGui::ResetMouseDragDelta(0);
                m_rightDragMoved = true;
            }
        }
        return m_rightPanelOpen[section];
    }

    // The loupe's box around the mouse (the area the inset shows) and the inset's frame; the renderer has
    // already copied the magnified draw into the inset. The inset opens at the top right of the viewport,
    // left of the right panel and clear of the banner row, and goes wherever it is dragged from there; it is
    // kept inside the window (below the menu bar) when the window shrinks. Neither the box nor the inset ever
    // enters a side panel: where one would, it is pushed out the shorter way (sideways off the full-height left
    // panel; sideways or below the right panel, whose height follows its open sections).
    // The live camera's eye, view direction and view-projection (the same as the frame's, see OnRender)
    static void LiveCamera(const XMFLOAT3& target, float distance, float pitch, float yaw, float aspect, XMFLOAT3& eye, XMFLOAT3& forward, XMMATRIX& viewProj)
    {
        const float cy = cosf(pitch), sy = sinf(pitch), sx = sinf(yaw), cx = cosf(yaw);
        eye = XMFLOAT3(target.x + distance * cy * sx, target.y + distance * sy, target.z + distance * cy * cx);
        const XMVECTOR e = XMLoadFloat3(&eye), at = XMLoadFloat3(&target);
        XMStoreFloat3(&forward, XMVector3Normalize(XMVectorSubtract(at, e)));
        viewProj = XMMatrixMultiply(XMMatrixLookAtRH(e, at, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)), XMMatrixPerspectiveFovRH(XM_PIDIV4, aspect, 0.1f, 500.0f));
    }

    bool SurfelsApp::ProjectLiveCamera(const XMFLOAT3& world, ImVec2& screen, float& viewDepth) const
    {
        if (m_Width == 0 || m_Height == 0) return false;
        XMFLOAT3 eye, fwd; XMMATRIX viewProj;
        LiveCamera(m_target, m_distance, m_pitch, m_yaw, (float)m_Width / (float)m_Height, eye, fwd, viewProj);
        viewDepth = (world.x - eye.x) * fwd.x + (world.y - eye.y) * fwd.y + (world.z - eye.z) * fwd.z;
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(world.x, world.y, world.z, 1.0f), viewProj));
        if (clip.w <= 1e-4f) return false;
        screen = ImVec2((clip.x / clip.w * 0.5f + 0.5f) * (float)m_Width, (0.5f - clip.y / clip.w * 0.5f) * (float)m_Height);
        return true;
    }

    // Pinning captures the anchor: the depth of the front surface under the box (the nearest fifth of the
    // level 0 points that project into it, a sample of up to a million), the point on the box's centre ray at
    // that depth, and the box's half-height in world units there. With no points under the box the anchor
    // sits on the plane through the orbit target.
    void SurfelsApp::SetLoupePinned(bool pinned)
    {
        m_loupePinned = pinned;
        m_loupeAnchorValid = false;
        if (!pinned || m_Width == 0 || m_Height == 0) return;
        XMFLOAT3 eye, fwd; XMMATRIX viewProj;
        LiveCamera(m_target, m_distance, m_pitch, m_yaw, (float)m_Width / (float)m_Height, eye, fwd, viewProj);
        const float zoom = std::max(1.0f, m_loupeEffectiveZoom);
        const ImVec2 half(0.5f * m_loupeSize.x / zoom, 0.5f * m_loupeSize.y / zoom);
        const std::vector<SurfelVertex>& pts = !m_rendererRawSurfels.empty() ? m_rendererRawSurfels : m_rawSurfels;
        const size_t stride = std::max<size_t>(1, pts.size() / 1000000);
        std::vector<float> depths;
        for (size_t i = 0; i < pts.size(); i += stride)
        {
            const XMFLOAT3& p = pts[i].position;
            XMFLOAT4 clip;
            XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.0f), viewProj));
            if (clip.w <= 1e-4f) continue;
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * (float)m_Width, sy = (0.5f - clip.y / clip.w * 0.5f) * (float)m_Height;
            if (fabsf(sx - m_loupeCenter.x) <= half.x && fabsf(sy - m_loupeCenter.y) <= half.y)
                depths.push_back((p.x - eye.x) * fwd.x + (p.y - eye.y) * fwd.y + (p.z - eye.z) * fwd.z);
        }
        float depth = (m_target.x - eye.x) * fwd.x + (m_target.y - eye.y) * fwd.y + (m_target.z - eye.z) * fwd.z;
        if (!depths.empty())
        {
            std::nth_element(depths.begin(), depths.begin() + depths.size() / 5, depths.end());
            depth = depths[depths.size() / 5];
        }
        if (depth <= 1e-4f) return;
        // The centre ray: the box centre unprojected at the near plane, from the eye, scaled to the depth
        const float ndcX = m_loupeCenter.x / (float)m_Width * 2.0f - 1.0f, ndcY = 1.0f - m_loupeCenter.y / (float)m_Height * 2.0f;
        const XMVECTOR nearPt = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), XMMatrixInverse(nullptr, viewProj));
        XMFLOAT3 dir;
        XMStoreFloat3(&dir, XMVector3Normalize(XMVectorSubtract(nearPt, XMLoadFloat3(&eye))));
        const float along = depth / std::max(1e-4f, dir.x * fwd.x + dir.y * fwd.y + dir.z * fwd.z);
        m_loupeAnchorWorld = XMFLOAT3(eye.x + dir.x * along, eye.y + dir.y * along, eye.z + dir.z * along);
        m_loupeAnchorHalfWorld = half.y * 2.0f * depth * tanf(0.5f * XM_PIDIV4) / (float)m_Height;
        m_loupeAnchorZoom = m_loupeZoom; // W/S afterwards scales the patch by m_loupeAnchorZoom / m_loupeZoom
        m_loupeAnchorValid = true;
    }

    void SurfelsApp::DrawLoupeOverlay()
    {
        m_state.loupeEnabled = false;
        if (!m_loupeEnabled || m_Width == 0 || m_Height == 0) return;
        // A push that would leave the window is not taken (below the full-height left panel there is no room)
        const auto pushOutOfPanels = [this](ImVec2& c, ImVec2 half)
        {
            for (const ImVec4& p : { m_leftPanelRect, m_rightPanelRect })
            {
                if (p.z <= p.x) continue;
                if (c.x + half.x <= p.x || c.x - half.x >= p.z || c.y + half.y <= p.y || c.y - half.y >= p.w) continue;
                float pushL = c.x + half.x - p.x, pushR = p.z - (c.x - half.x), pushD = p.w - (c.y - half.y);
                if (p.x - 2.0f * half.x < 0.0f) pushL = FLT_MAX;
                if (p.z + 2.0f * half.x > (float)m_Width) pushR = FLT_MAX;
                if (p.w + 2.0f * half.y > (float)m_Height) pushD = FLT_MAX;
                if (pushD < std::min(pushL, pushR)) c.y += pushD;
                else if (pushL < pushR) c.x -= pushL;
                else if (pushR < FLT_MAX) c.x += pushR;
            }
        };
        if (m_loupeCenter.x < 0.0f) m_loupeCenter = ImVec2(0.5f * (float)m_Width, 0.5f * (float)m_Height);
        const ImVec2 inset(std::floor(std::min(m_loupeSize.x, (float)m_Width)), std::floor(std::min(m_loupeSize.y, (float)m_Height)));
        // A pinned box holds onto the patch of surface it was pinned over (CaptureLoupeAnchor): its centre is
        // that point projected with the live camera, and its size the patch's world size at the point's
        // depth, so zooming or orbiting the model keeps the same subregion in the inset. The magnification
        // follows (W/S still scales the patch); it bottoms out at 1x, where the box is the inset's size.
        float zoom = m_loupeZoom;
        if (m_loupePinned && m_loupeAnchorValid)
        {
            ImVec2 screen; float depth = 0.0f;
            if (ProjectLiveCamera(m_loupeAnchorWorld, screen, depth) && depth > 1e-4f)
            {
                const float worldHalfY = m_loupeAnchorHalfWorld * (m_loupeAnchorZoom / std::max(1e-3f, m_loupeZoom));
                const float pxPerWorld = (float)m_Height / (2.0f * depth * tanf(0.5f * XM_PIDIV4));
                zoom = std::clamp(0.5f * inset.y / std::max(1e-3f, worldHalfY * pxPerWorld), 1.0f, kLoupeZoomMax);
                m_loupeCenter = screen;
            }
        }
        m_loupeEffectiveZoom = zoom;
        if (m_loupePos.x < 0.0f)
            m_loupePos = ImVec2((float)m_Width - m_rightPanelWidth - inset.x - 14.0f, std::max(56.0f, m_bannerBottomY + 10.0f));
        {
            // The inset with its 1-pixel frame (and a pixel to spare) is what stays clear of the panels
            const ImVec2 halfInset(0.5f * inset.x + 2.0f, 0.5f * inset.y + 2.0f);
            ImVec2 c(m_loupePos.x + 0.5f * inset.x, m_loupePos.y + 0.5f * inset.y);
            pushOutOfPanels(c, halfInset);
            m_loupePos = ImVec2(c.x - 0.5f * inset.x, c.y - 0.5f * inset.y);
        }
        const float menuBar = 22.0f;
        m_loupePos.x = std::clamp(m_loupePos.x, 0.0f, std::max(0.0f, (float)m_Width - inset.x));
        m_loupePos.y = std::clamp(m_loupePos.y, menuBar, std::max(menuBar, (float)m_Height - inset.y));
        const float destX = std::floor(m_loupePos.x);
        const float destY = std::floor(m_loupePos.y);
        m_loupeRect = ImVec4(destX, destY, destX + inset.x, destY + inset.y);
        m_state.loupeWidth = (uint32_t)inset.x;
        m_state.loupeHeight = (uint32_t)inset.y;
        m_state.loupeEnabled = true;
        m_state.loupeZoom = zoom;
        m_state.loupeViz = (uint32_t)m_loupeViz;
        m_state.loupeDestX = (uint32_t)destX;
        m_state.loupeDestY = (uint32_t)destY;

        const ImVec2 half(0.5f * inset.x / zoom, 0.5f * inset.y / zoom);
        pushOutOfPanels(m_loupeCenter, half);
        m_state.loupeCenterX = m_loupeCenter.x;
        m_state.loupeCenterY = m_loupeCenter.y;

        ImDrawList* dl = ImGui::GetOverlayDrawList();
        const ImU32 frame = IM_COL32(255, 210, 80, 230);
        const ImVec2 boxMin(m_loupeCenter.x - half.x, m_loupeCenter.y - half.y), boxMax(m_loupeCenter.x + half.x, m_loupeCenter.y + half.y);
        if (m_loupePinned)
        {
            // Pinned: a frosted grey tint and a darker border (the inset still shows the area under it untinted)
            dl->AddRectFilled(boxMin, boxMax, IM_COL32(205, 208, 214, 70));
            dl->AddRect(boxMin, boxMax, IM_COL32(150, 110, 30, 255), 0.0f, ~0, 2.0f);
        }
        else
            dl->AddRect(boxMin, boxMax, frame, 0.0f, ~0, 1.5f);
        dl->AddRect(ImVec2(destX - 1.0f, destY - 1.0f), ImVec2(destX + inset.x + 1.0f, destY + inset.y + 1.0f), frame, 0.0f, ~0, 2.0f);
        // What a drag would grab lights up under the mouse and while dragged: an edge (two for a corner), or the
        // whole frame for a move
        const int grabbed = m_loupeDrag ? m_loupeDrag : m_loupeHover;
        const ImU32 grip = IM_COL32(255, 245, 190, 255);
        const float x0 = destX - 1.0f, y0 = destY - 1.0f, x1 = destX + inset.x + 1.0f, y1 = destY + inset.y + 1.0f;
        if (grabbed & kLoupeDragMove) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), grip, 0.0f, ~0, 3.0f);
        if (grabbed & kLoupeDragLeft) dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y1), grip, 4.0f);
        if (grabbed & kLoupeDragRight) dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y1), grip, 4.0f);
        if (grabbed & kLoupeDragTop) dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y0), grip, 4.0f);
        if (grabbed & kLoupeDragBottom) dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), grip, 4.0f);
        static const char* s_vizNames[] = { "normal", "outlines", "colours", "centres" };
        char label[64];
        snprintf(label, sizeof(label), "Loupe %.1fx (W/S)  %s (Shift+V)", zoom, s_vizNames[std::clamp(m_loupeViz, 0, 3)]);
        dl->AddRectFilled(ImVec2(destX, destY + inset.y - 20.0f), ImVec2(destX + ImGui::CalcTextSize(label).x + 12.0f, destY + inset.y), IM_COL32(0, 0, 0, 170));
        dl->AddText(ImVec2(destX + 6.0f, destY + inset.y - 17.0f), frame, label);
    }

    // Faint three-line reminder of the camera bindings, tucked into the bottom-right corner of the
    // viewport just above the status bar and left of the right panel. Deliberately understated (dim
    // text on a barely-there backing, no interaction) so it reads as part of the canvas rather than a
    // control. The bindings listed must match UpdateCamera: left-drag or Left/Right arrows orbit; the
    // wheel, right-drag, W/S, Up/Down, PageUp/PageDown and +/- zoom; Shift with any drag or with the
    // arrow keys pans.
    void SurfelsApp::DrawControlHints(float leftPanelWidth, float rightPanelWidth)
    {
        if (!m_showControlHints) return; // "Show Camera Control Hints" unchecked on the Renderer tab's viewport section

        // Bottom-right of the viewport: just left of the right panel and just above the status bar. On a
        // window too narrow for a viewport strip it stays put over the model (still readable) rather
        // than being hidden -- never pushed under the left panel, though.
        const float rightEdge  = std::max(10.0f + leftPanelWidth + 260.0f, (float)m_Width - rightPanelWidth - 20.0f);
        const float bottomEdge = (float)m_Height - 32.0f - 6.0f;

        ImGui::PushFont(m_pHintFont); // Proportional Segoe UI; NULL falls back to the default font
        ImGui::SetNextWindowPos(ImVec2(rightEdge, bottomEdge), ImGuiCond_Always, ImVec2(1.0f, 1.0f)); // Pivot: bottom-right corner
        // Understated in size and placement, but legible: near-white text on a solid dark backing,
        // because the model usually runs behind this corner.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.03f, 0.72f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##ControlHints", nullptr, flags))
        {
            const ImVec4 label(1.00f, 0.92f, 0.60f, 1.00f); // Warm off-white for the verb
            const ImVec4 keys (0.90f, 0.90f, 0.94f, 1.00f); // Light grey for the bindings
            struct Hint { const char* verb; const char* binding; };
            const Hint hints[] = {                         // "\xC2\xB7" = middle dot (U+00B7), in the default Latin glyph range
                { "Rotate", "left-drag \xC2\xB7 Left/Right arrows" },
                { "Zoom",   m_loupeEnabled ? "wheel \xC2\xB7 right-drag \xC2\xB7 Up/Down" : "wheel \xC2\xB7 right-drag \xC2\xB7 W/S \xC2\xB7 Up/Down" },
                { "Pan",    "Shift + drag \xC2\xB7 Shift + arrows" },
                { "Splats", "V display" },
                { "Loupe",  m_loupeEnabled ? "L off \xC2\xB7 W/S magnify \xC2\xB7 Shift+V display \xC2\xB7 click box/Space pin" : "L" },
            };
            const float bindingColumn = ImGui::CalcTextSize("Rotate").x + 14.0f; // Fixed verb column so the proportional font still lines up
            for (const Hint& h : hints)
            {
                ImGui::TextColored(label, "%s", h.verb);
                ImGui::SameLine(bindingColumn);
                ImGui::TextColored(keys, "%s", h.binding);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Shared enable/view-only controls for the baked occlusion volume, drawn identically from both the
    // Surfel Generator tab (right after baking) and the Renderer tab (while viewing). Shape parameters
    // (resolution, shave) live only on the Surfel Generator tab: the volume is baked into the .sflw, so
    // the viewer shows exactly what was packaged and offers nothing that would change it. "View
    // Occlusion Volume Only" is intentionally independent of "Show Occlusion Volume" -- wanting to
    // just look at the volume shouldn't require also turning on splat culling against it.
    void SurfelsApp::DrawOcclusionVolumeControls()
    {
        if (ImGui::Checkbox("Show Occlusion Volume", &m_enableOcclusionCulling))
        {
            m_showOcclusionSetting = m_enableOcclusionCulling;
            SaveConfigFile(); // Remembered (show_occlusion_volume): the next load or bake follows it
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draws the baked interior occlusion volume (%zu cubes) and depth-tests the splats against it, so far-side surfels don't show through gaps in the near side. On by default.", m_occlusionVoxels.size());

        ImGui::Checkbox("View Occlusion Volume Only", &m_showOcclusionVolumeOnly);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Debug view: hides the surfel splats entirely and renders only the occluder geometry. Works regardless of 'Show Occlusion Volume' above.");

        // Mip chain (v6+ packages): which level the renderer is drawing, with a manual override for
        // comparing levels. Automatic selection is by projected cell size (see
        // SurfelsRenderer::SelectOcclusionMip), never by per-chunk surfel LOD.
        if (m_occlusionMips.mipCount > 1)
        {
            const uint32_t activeMip = m_pRenderer ? m_pRenderer->GetActiveOcclusionMip() : 0;
            const float activePixels = m_pRenderer ? m_pRenderer->GetOcclusionMipCellPixels() : 0.0f;
            char preview[128];
            if (m_occlusionMipOverride < 0)
                snprintf(preview, sizeof(preview), "Auto: mip %u (%.1f px per cell)", activeMip, activePixels);
            else
                snprintf(preview, sizeof(preview), "Mip %d (forced, %.1f px per cell)", m_occlusionMipOverride, activePixels);
            if (ImGui::BeginCombo("Occlusion Volume Mip", preview))
            {
                if (ImGui::Selectable("Auto (finest mip whose cell covers >= 2.5 px)", m_occlusionMipOverride < 0)) m_occlusionMipOverride = -1;
                for (uint32_t k = 0; k < m_occlusionMips.mipCount; k++)
                {
                    char label[128];
                    snprintf(label, sizeof(label), "Mip %u: %u blocks, %.3f m cells", k, m_occlusionMips.blockCount[k], m_occlusionMips.cellSize[k]);
                    if (ImGui::Selectable(label, m_occlusionMipOverride == (int)k)) m_occlusionMipOverride = (int)k;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The volume is baked as %u nested mips (mip 0 = the fine skin; each further mip a coarser level lying inside it). Auto draws the finest mip whose cell still covers a few pixels at the model's nearest point, with hysteresis, so the cube size grows with the surfel LOD in view. Force a mip here to compare levels.", m_occlusionMips.mipCount);
        }
    }

    // Thin wrappers over the header-only generator in OcclusionVolume.h, which holds the algorithm
    // and its documentation; these just route app state in and the trace log out.
    XMFLOAT3 SurfelsApp::GradeOcclusionColor(const XMFLOAT3& rgb) const
    {
        OcclusionVolume::ColorGrade grade{ m_occlusionHueShift, m_occlusionSaturation, m_occlusionBrightness };
        return OcclusionVolume::GradeColor(grade, rgb);
    }

    // Rebuilds the occlusion volume from the current checkbox/sliders, or clears it when generation is
    // off. Called live from the Surfel Generator sliders (the bake is grid work over the raw points --
    // fast enough to drag) and on every full pipeline recompute. A freshly baked volume is shown straight
    // away unless Show Occlusion Volume was switched off (m_showOcclusionSetting), which a bake respects.
    void SurfelsApp::RefreshOcclusionVolume()
    {
        if (m_generateOcclusionVolume)
        {
            BuildOcclusionVolume();
            if (!m_occlusionVoxels.empty())
            {
                m_enableOcclusionCulling = m_showOcclusionSetting;
            }
        }
        else
        {
            m_occlusionVoxels.clear();
            m_occlusionMips = OcclusionMipTable{};
            m_enableOcclusionCulling = false;
        }
        m_occlusionVoxelsVersion++;
    }

    void SurfelsApp::BuildOcclusionGrid()
    {
        const auto& sourcePoints = !m_rawSurfels.empty() ? m_rawSurfels : m_rendererRawSurfels;
        std::string trace;
        OcclusionVolume::BuildGrid(sourcePoints, m_aabbMin, m_aabbMax, m_occlusionGrid, trace, 0, m_occlusionShaveBiasCells);
        if (!trace.empty()) LogTransitionTrace("%s", trace.c_str());
    }

    void SurfelsApp::BuildOcclusionVolume()
    {
        if (!m_occlusionGrid.valid) BuildOcclusionGrid();
        OcclusionVolume::ColorGrade grade{ m_occlusionHueShift, m_occlusionSaturation, m_occlusionBrightness };
        std::string trace;
        OcclusionVolume::Bake(m_occlusionGrid, m_occlusionShave, grade, m_occlusionVoxels, m_occlusionMips, trace);
        if (!trace.empty()) LogTransitionTrace("%s", trace.c_str());
    }

    // Voxelizes the loaded point cloud into a density heatmap for the cluster-cube visualizer
    void SurfelsApp::RebuildHeatmapClusterCubes()
    {
        m_heatmapClusterCubes.clear();
        const auto& sourcePoints = !m_rawSurfels.empty() ? m_rawSurfels : m_rendererRawSurfels;
        if (sourcePoints.empty()) return;

        // 1. Determine global model extents
        XMFLOAT3 gMin = m_aabbMin;
        XMFLOAT3 gMax = m_aabbMax;
        XMFLOAT3 gExtent(
            std::max(1e-4f, gMax.x - gMin.x),
            std::max(1e-4f, gMax.y - gMin.y),
            std::max(1e-4f, gMax.z - gMin.z)
        );

        // 2. Compute isotropic cubic voxel grid spanning the entire global bounding box
        float maxExtent = std::max(gExtent.x, std::max(gExtent.y, gExtent.z));
        int targetDiv = 28;
        if (m_targetClusterCubes <= 512) targetDiv = 12;
        else if (m_targetClusterCubes <= 1024) targetDiv = 16;
        else if (m_targetClusterCubes <= 2048) targetDiv = 22;
        else if (m_targetClusterCubes <= 4096) targetDiv = 28;
        else if (m_targetClusterCubes <= 8192) targetDiv = 36;
        else targetDiv = 48;

        float cellSize = std::max(0.001f, maxExtent / (float)targetDiv);
        float voxelVolume = cellSize * cellSize * cellSize;

        int32_t rx = std::max(1, (int32_t)std::ceil(gExtent.x / cellSize));
        int32_t ry = std::max(1, (int32_t)std::ceil(gExtent.y / cellSize));
        int32_t rz = std::max(1, (int32_t)std::ceil(gExtent.z / cellSize));

        // 3. Populate 3D spatial voxel hash grid
        struct VoxelKey {
            int32_t x, y, z;
            bool operator==(const VoxelKey& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct VoxelKeyHash {
            size_t operator()(const VoxelKey& k) const {
                return (size_t)k.x * 73856093 ^ (size_t)k.y * 19349663 ^ (size_t)k.z * 83492791;
            }
        };

        struct VoxelData {
            uint32_t count = 0;
            XMFLOAT3 normalSum = { 0.0f, 0.0f, 0.0f };
        };

        std::unordered_map<VoxelKey, VoxelData, VoxelKeyHash> gridMap;
        for (const auto& s : sourcePoints)
        {
            int32_t ix = (int32_t)((s.position.x - gMin.x) / cellSize);
            int32_t iy = (int32_t)((s.position.y - gMin.y) / cellSize);
            int32_t iz = (int32_t)((s.position.z - gMin.z) / cellSize);
            ix = std::max(0, std::min(rx - 1, ix));
            iy = std::max(0, std::min(ry - 1, iy));
            iz = std::max(0, std::min(rz - 1, iz));

            VoxelKey k = { ix, iy, iz };
            gridMap[k].count++;
            gridMap[k].normalSum.x += s.normal.x;
            gridMap[k].normalSum.y += s.normal.y;
            gridMap[k].normalSum.z += s.normal.z;
        }

        // 4. Build HeatmapClusterCube list across entire 3D grid
        float minDensity = 1e9f;
        float maxDensity = -1e9f;

        m_heatmapClusterCubes.reserve(rx * ry * rz);
        for (int32_t iz = 0; iz < rz; iz++)
        {
            for (int32_t iy = 0; iy < ry; iy++)
            {
                for (int32_t ix = 0; ix < rx; ix++)
                {
                    VoxelKey k = { ix, iy, iz };
                    auto it = gridMap.find(k);
                    uint32_t count = (it != gridMap.end()) ? it->second.count : 0;

                    float cMinX = gMin.x + (float)ix * cellSize;
                    float cMinY = gMin.y + (float)iy * cellSize;
                    float cMinZ = gMin.z + (float)iz * cellSize;
                    float cMaxX = cMinX + cellSize;
                    float cMaxY = cMinY + cellSize;
                    float cMaxZ = cMinZ + cellSize;

                    HeatmapClusterCube cube = {};
                    cube.aabbMin = XMFLOAT3(cMinX, cMinY, cMinZ);
                    cube.aabbMax = XMFLOAT3(cMaxX, cMaxY, cMaxZ);
                    cube.center = XMFLOAT3(
                        cMinX + cellSize * 0.5f,
                        cMinY + cellSize * 0.5f,
                        cMinZ + cellSize * 0.5f
                    );
                    cube.boundingRadius = cellSize * 0.866025f;
                    cube.pointCount = count;
                    cube.volume = voxelVolume;
                    cube.density = (float)count / voxelVolume;

                    if (count > 0)
                    {
                        float nx = it->second.normalSum.x;
                        float ny = it->second.normalSum.y;
                        float nz = it->second.normalSum.z;
                        float len = sqrtf(nx*nx + ny*ny + nz*nz);
                        if (len > 1e-4f)
                            cube.avgNormal = XMFLOAT3(nx/len, ny/len, nz/len);
                        else
                            cube.avgNormal = XMFLOAT3(0.0f, 0.0f, 0.0f);

                        if (cube.density < minDensity) minDensity = cube.density;
                        if (cube.density > maxDensity) maxDensity = cube.density;
                    }
                    else
                    {
                        cube.avgNormal = XMFLOAT3(0.0f, 0.0f, 0.0f);
                    }

                    m_heatmapClusterCubes.push_back(cube);
                }
            }
        }

        // 5. Normalize densities using log scale for vibrant contrast across sparse and dense areas
        float logMin = std::log(std::max(1.0f, minDensity));
        float logMax = std::log(std::max(2.0f, maxDensity));
        float logRange = std::max(0.001f, logMax - logMin);

        for (auto& cube : m_heatmapClusterCubes)
        {
            if (cube.pointCount > 0 && m_heatmapSource == 1)
            {
                // Detail source: the streaming order itself (mean detail score over the cube's cells)
                cube.normDensity = m_detailGrid.Valid() ? m_detailGrid.SampleBox(cube.aabbMin, cube.aabbMax) : 0.0f;
            }
            else if (cube.pointCount > 0)
            {
                float logD = std::log(std::max(1.0f, cube.density));
                cube.normDensity = std::max(0.0f, std::min(1.0f, (logD - logMin) / logRange));
            }
            else
            {
                cube.normDensity = 0.0f;
            }
        }
    }

    // Draws the ImGui-drawlist wireframe/heatmap overlay for the octree and cluster-cube visualizers
    void SurfelsApp::DrawOctreeVisualizer()
    {
        if (!m_showClusterHeatmap && !m_showOctreeVisualizer && !m_showGlobalBounds && !m_detachCamera && !m_highlightSilhouetteChunks)
            return;

        ImDrawList* drawList = ImGui::GetOverlayDrawList();
        if (!drawList)
            return;

        ImGuiIO& io = ImGui::GetIO();
        float screenW = io.DisplaySize.x;
        float screenH = io.DisplaySize.y;
        if (screenW <= 10.0f || screenH <= 10.0f)
            return;

        // 1. Active Viewport Camera Matrices (Used for 3D Screen Space Projection)
        const float cy = cosf(m_pitch), sy = sinf(m_pitch);
        const float sx = sinf(m_yaw), cx = cosf(m_yaw);
        XMFLOAT3 eyePos(
            m_target.x + m_distance * cy * sx,
            m_target.y + m_distance * sy,
            m_target.z + m_distance * cy * cx
        );

        XMVECTOR eye = XMLoadFloat3(&eyePos);
        XMVECTOR at = XMLoadFloat3(&m_target);
        XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(at, eye));
        XMFLOAT3 forwardNorm;
        XMStoreFloat3(&forwardNorm, forward);

        XMMATRIX view = XMMatrixLookAtRH(eye, at, worldUp);
        float aspect = io.DisplaySize.y > 0.0f ? (io.DisplaySize.x / io.DisplaySize.y) : (screenW / screenH);
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, aspect, 0.1f, 500.0f);
        XMMATRIX viewProj = XMMatrixMultiply(view, proj);

        // 2. Culling Camera (Detached Frozen or Active)
        float cPitch = m_detachCamera ? m_detachedPitch : m_pitch;
        float cYaw   = m_detachCamera ? m_detachedYaw : m_yaw;
        float cDist  = m_detachCamera ? m_detachedDistance : m_distance;
        XMFLOAT3 cTarget = m_detachCamera ? m_detachedTarget : m_target;

        const float c_cy = cosf(cPitch), c_sy = sinf(cPitch);
        const float c_sx = sinf(cYaw), c_cx = cosf(cYaw);
        XMFLOAT3 cullEyePos(
            cTarget.x + cDist * c_cy * c_sx,
            cTarget.y + cDist * c_sy,
            cTarget.z + cDist * c_cy * c_cx
        );
        XMVECTOR cEye = XMLoadFloat3(&cullEyePos);
        XMVECTOR cAt = XMLoadFloat3(&cTarget);
        XMVECTOR cForwardVec = XMVector3Normalize(XMVectorSubtract(cAt, cEye));
        XMVECTOR cRightVec = XMVector3Normalize(XMVector3Cross(worldUp, cForwardVec));
        XMVECTOR cUpVec = XMVector3Cross(cForwardVec, cRightVec);

        XMFLOAT3 cullForward, cullRight, cullUp;
        XMStoreFloat3(&cullForward, cForwardVec);
        XMStoreFloat3(&cullRight, cRightVec);
        XMStoreFloat3(&cullUp, cUpVec);

        XMMATRIX cView = XMMatrixLookAtRH(cEye, cAt, worldUp);
        XMMATRIX cProj = XMMatrixPerspectiveFovRH(XM_PIDIV4, aspect, 0.1f, 500.0f);
        XMMATRIX cViewProj = XMMatrixMultiply(cView, cProj);

        auto ProjectToScreen = [&](const XMFLOAT3& p, ImVec2& outScreen) -> bool
        {
            if (std::isnan(p.x) || std::isnan(p.y) || std::isnan(p.z) ||
                std::isinf(p.x) || std::isinf(p.y) || std::isinf(p.z))
                return false;

            XMVECTOR worldP = XMLoadFloat3(&p);
            XMVECTOR clipP = XMVector4Transform(XMVectorSetW(worldP, 1.0f), viewProj);
            XMFLOAT4 c;
            XMStoreFloat4(&c, clipP);

            if (c.w < 0.10f || std::isnan(c.w) || std::isinf(c.w)) return false;

            float invW = 1.0f / c.w;
            float ndcX = c.x * invW;
            float ndcY = c.y * invW;
            float ndcZ = c.z * invW;

            if (ndcZ < 0.0f || ndcZ > 1.0f || ndcX < -1.15f || ndcX > 1.15f || ndcY < -1.15f || ndcY > 1.15f)
                return false;

            outScreen.x = (ndcX * 0.5f + 0.5f) * screenW;
            outScreen.y = (-ndcY * 0.5f + 0.5f) * screenH;
            return true;
        };

        // Evaluate smooth multi-scheme colormap
        auto EvaluateHeatmapColor = [](float t, float alpha, int scheme) -> ImU32
        {
            t = std::max(0.0f, std::min(1.0f, t));
            float r = 0.0f, g = 0.0f, b = 0.0f;

            if (scheme == 0) // Turbo / Rainbow: Blue -> Cyan -> Green -> Yellow -> Red
            {
                if (t < 0.25f)
                {
                    float f = t / 0.25f;
                    r = 0.0f; g = f; b = 1.0f;
                }
                else if (t < 0.5f)
                {
                    float f = (t - 0.25f) / 0.25f;
                    r = 0.0f; g = 1.0f; b = 1.0f - f;
                }
                else if (t < 0.75f)
                {
                    float f = (t - 0.5f) / 0.25f;
                    r = f; g = 1.0f; b = 0.0f;
                }
                else
                {
                    float f = (t - 0.75f) / 0.25f;
                    r = 1.0f; g = 1.0f - f; b = 0.0f;
                }
            }
            else if (scheme == 1) // Viridis (Perceptual): Purple -> Teal -> Green -> Yellow
            {
                if (t < 0.33f)
                {
                    float f = t / 0.33f;
                    r = 0.27f - 0.07f * f; g = 0.00f + 0.38f * f; b = 0.33f + 0.18f * f;
                }
                else if (t < 0.66f)
                {
                    float f = (t - 0.33f) / 0.33f;
                    r = 0.20f - 0.07f * f; g = 0.38f + 0.34f * f; b = 0.51f + 0.04f * f;
                }
                else
                {
                    float f = (t - 0.66f) / 0.34f;
                    r = 0.13f + 0.86f * f; g = 0.72f + 0.24f * f; b = 0.55f - 0.39f * f;
                }
            }
            else // Plasma: Blue -> Magenta -> Orange -> Yellow
            {
                if (t < 0.33f)
                {
                    float f = t / 0.33f;
                    r = 0.05f + 0.5f * f; g = 0.03f; b = 0.53f - 0.1f * f;
                }
                else if (t < 0.66f)
                {
                    float f = (t - 0.33f) / 0.33f;
                    r = 0.55f + 0.35f * f; g = 0.03f + 0.45f * f; b = 0.43f - 0.3f * f;
                }
                else
                {
                    float f = (t - 0.66f) / 0.34f;
                    r = 0.90f + 0.09f * f; g = 0.48f + 0.48f * f; b = 0.13f + 0.2f * f;
                }
            }

            uint8_t ir = (uint8_t)(std::max(0.0f, std::min(1.0f, r)) * 255.0f);
            uint8_t ig = (uint8_t)(std::max(0.0f, std::min(1.0f, g)) * 255.0f);
            uint8_t ib = (uint8_t)(std::max(0.0f, std::min(1.0f, b)) * 255.0f);
            uint8_t ia = (uint8_t)(std::max(0.0f, std::min(1.0f, alpha)) * 255.0f);
            return IM_COL32(ir, ig, ib, ia);
        };

        // Extract 6 frustum clipping planes directly from cViewProj matrix (Gribb-Hartmann)
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, cViewProj);

        struct Plane4 { float a, b, c, d; };
        Plane4 planes[6];

        // Left
        planes[0] = { vp._14 + vp._11, vp._24 + vp._21, vp._34 + vp._31, vp._44 + vp._41 };
        // Right
        planes[1] = { vp._14 - vp._11, vp._24 - vp._21, vp._34 - vp._31, vp._44 - vp._41 };
        // Bottom
        planes[2] = { vp._14 + vp._12, vp._24 + vp._22, vp._34 + vp._32, vp._44 + vp._42 };
        // Top
        planes[3] = { vp._14 - vp._12, vp._24 - vp._22, vp._34 - vp._32, vp._44 - vp._42 };
        // Near (DirectX: 0 <= z_clip)
        planes[4] = { vp._13, vp._23, vp._33, vp._43 };
        // Far (DirectX: z_clip <= w_clip)
        planes[5] = { vp._14 - vp._13, vp._24 - vp._23, vp._34 - vp._33, vp._44 - vp._43 };

        for (int i = 0; i < 6; i++)
        {
            float len = sqrtf(planes[i].a * planes[i].a + planes[i].b * planes[i].b + planes[i].c * planes[i].c);
            if (len > 1e-6f)
            {
                float invL = 1.0f / len;
                planes[i].a *= invL;
                planes[i].b *= invL;
                planes[i].c *= invL;
                planes[i].d *= invL;
            }
        }

        auto IsBoxInFrustum = [&](const XMFLOAT3& bMin, const XMFLOAT3& bMax) -> bool
        {
            for (int i = 0; i < 6; i++)
            {
                float px = (planes[i].a >= 0.0f) ? bMax.x : bMin.x;
                float py = (planes[i].b >= 0.0f) ? bMax.y : bMin.y;
                float pz = (planes[i].c >= 0.0f) ? bMax.z : bMin.z;

                if (planes[i].a * px + planes[i].b * py + planes[i].c * pz + planes[i].d < 0.0f)
                {
                    return false;
                }
            }
            return true;
        };

        auto IsSphereInFrustum = [&](const XMFLOAT3& center, float radius) -> bool
        {
            for (int i = 0; i < 6; i++)
            {
                float dist = planes[i].a * center.x + planes[i].b * center.y + planes[i].c * center.z + planes[i].d;
                if (dist < -radius)
                {
                    return false;
                }
            }
            return true;
        };

        auto DrawFilledCube = [&](const XMFLOAT3& bMin, const XMFLOAT3& bMax, ImU32 fillCol, ImU32 edgeCol, bool drawWireframe)
        {
            uint8_t fillA = (fillCol >> 24) & 0xFF;
            uint8_t edgeA = (edgeCol >> 24) & 0xFF;
            if (fillA == 0 && (!drawWireframe || edgeA == 0))
                return;

            XMFLOAT3 corners[8] = {
                { bMin.x, bMin.y, bMin.z }, { bMax.x, bMin.y, bMin.z }, { bMax.x, bMax.y, bMin.z }, { bMin.x, bMax.y, bMin.z },
                { bMin.x, bMin.y, bMax.z }, { bMax.x, bMin.y, bMax.z }, { bMax.x, bMax.y, bMax.z }, { bMin.x, bMax.y, bMax.z }
            };
            ImVec2 screenCorners[8];
            bool valid[8];
            for (int i = 0; i < 8; i++)
            {
                valid[i] = ProjectToScreen(corners[i], screenCorners[i]);
            }

            // 6 Faces
            if (fillA > 0)
            {
                static const int faces[6][4] = {
                    { 0, 1, 2, 3 }, // Front (-Z)
                    { 5, 4, 7, 6 }, // Back (+Z)
                    { 4, 0, 3, 7 }, // Left (-X)
                    { 1, 5, 6, 2 }, // Right (+X)
                    { 3, 2, 6, 7 }, // Top (+Y)
                    { 4, 5, 1, 0 }  // Bottom (-Y)
                };

                for (int f = 0; f < 6; f++)
                {
                    int i0 = faces[f][0], i1 = faces[f][1], i2 = faces[f][2], i3 = faces[f][3];
                    if (valid[i0] && valid[i1] && valid[i2] && valid[i3])
                    {
                        drawList->AddQuadFilled(screenCorners[i0], screenCorners[i1], screenCorners[i2], screenCorners[i3], fillCol);
                    }
                }
            }

            if (drawWireframe && edgeA > 0)
            {
                static const int edges[12][2] = {
                    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
                    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
                    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
                };
                for (int e = 0; e < 12; e++)
                {
                    int i0 = edges[e][0], i1 = edges[e][1];
                    if (valid[i0] && valid[i1])
                    {
                        drawList->AddLine(screenCorners[i0], screenCorners[i1], edgeCol, 1.0f);
                    }
                }
            }
        };

        // 1. Density Heatmap Spatial Blocks (Back-to-front depth sorted by viewer camera view direction)
        if (m_showClusterHeatmap && !m_heatmapClusterCubes.empty())
        {
            std::vector<size_t> sortedCubes(m_heatmapClusterCubes.size());
            for (size_t i = 0; i < sortedCubes.size(); i++) sortedCubes[i] = i;

            std::sort(sortedCubes.begin(), sortedCubes.end(), [&](size_t a, size_t b) {
                const auto& ca = m_heatmapClusterCubes[a];
                const auto& cb = m_heatmapClusterCubes[b];
                float da = (ca.center.x - eyePos.x) * forwardNorm.x +
                           (ca.center.y - eyePos.y) * forwardNorm.y +
                           (ca.center.z - eyePos.z) * forwardNorm.z;
                float db = (cb.center.x - eyePos.x) * forwardNorm.x +
                           (cb.center.y - eyePos.y) * forwardNorm.y +
                           (cb.center.z - eyePos.z) * forwardNorm.z;
                return da > db;
            });

            for (size_t idx : sortedCubes)
            {
                const auto& cube = m_heatmapClusterCubes[idx];
                bool inFrustum = IsBoxInFrustum(cube.aabbMin, cube.aabbMax);

                // In detached camera mode:
                // 1. Keep only front-facing shell wrt detached camera
                float toCamX = cullEyePos.x - cube.center.x;
                float toCamY = cullEyePos.y - cube.center.y;
                float toCamZ = cullEyePos.z - cube.center.z;
                float nLenSq = cube.avgNormal.x * cube.avgNormal.x + cube.avgNormal.y * cube.avgNormal.y + cube.avgNormal.z * cube.avgNormal.z;
                bool isFrontFacingToDetached = true;
                if (m_detachCamera && nLenSq > 0.05f)
                {
                    isFrontFacingToDetached = (toCamX * cube.avgNormal.x + toCamY * cube.avgNormal.y + toCamZ * cube.avgNormal.z > -0.05f);
                }

                bool isVisible = inFrustum && isFrontFacingToDetached;

                // 2. Active Viewer Cavity & Rim Shading:
                float toViewerX = eyePos.x - cube.center.x;
                float toViewerY = eyePos.y - cube.center.y;
                float toViewerZ = eyePos.z - cube.center.z;
                float distViewer = sqrtf(toViewerX * toViewerX + toViewerY * toViewerY + toViewerZ * toViewerZ);
                float invDist = distViewer > 1e-4f ? (1.0f / distViewer) : 0.0f;
                float normDotViewer = (toViewerX * cube.avgNormal.x + toViewerY * cube.avgNormal.y + toViewerZ * cube.avgNormal.z) * invDist;

                bool isCavity = (m_detachCamera && nLenSq > 0.05f && normDotViewer < -0.06f);
                bool isRim    = (m_detachCamera && nLenSq > 0.05f && fabsf(normDotViewer) <= 0.06f);

                if (isVisible && cube.pointCount > 0)
                {
                    float t = cube.normDensity;
                    float fillAlpha = std::max(0.0f, std::min(1.0f, m_heatmapOpacity));
                    float edgeAlpha = std::max(0.0f, std::min(1.0f, m_wireframeOpacity));

                    ImU32 fillCol, edgeCol;
                    if (isCavity)
                    {
                        // Dark AO cavity inside the mold
                        fillCol = IM_COL32(25, 28, 35, (int)(fillAlpha * 255.0f));
                        edgeCol = IM_COL32(45, 50, 60, (int)(edgeAlpha * 255.0f));
                    }
                    else if (isRim)
                    {
                        // Distinct plaster rim transition highlight
                        fillCol = IM_COL32(210, 225, 255, (int)(fillAlpha * 255.0f));
                        edgeCol = IM_COL32(230, 240, 255, (int)(edgeAlpha * 255.0f));
                    }
                    else
                    {
                        fillCol = EvaluateHeatmapColor(t, fillAlpha, m_heatmapColorScheme);
                        edgeCol = EvaluateHeatmapColor(t, edgeAlpha, m_heatmapColorScheme);
                    }

                    DrawFilledCube(cube.aabbMin, cube.aabbMax, fillCol, edgeCol, m_showHeatmapWireframe);
                }
                else if (m_showCulledChunks && cube.pointCount > 0)
                {
                    // Culled cluster cubes shown in translucent Steel Blue
                    DrawFilledCube(cube.aabbMin, cube.aabbMax, IM_COL32(20, 60, 140, 25), IM_COL32(60, 140, 240, 90), m_showHeatmapWireframe);
                }
            }
        }

        // 2. Streaming Octree Macro-Clusters (Amber / Gold when visible, Steel Blue when culled)
        if (m_showOctreeVisualizer)
        {
            const auto& octreeBoxes = !m_rendererOctreeChunks.empty() ? m_rendererOctreeChunks : m_chunks;
            for (const auto& chunk : octreeBoxes)
            {
                bool isVisible = IsBoxInFrustum(chunk.aabbMin, chunk.aabbMax);
                if (isVisible)
                {
                    DrawFilledCube(chunk.aabbMin, chunk.aabbMax, IM_COL32(255, 180, 30, 40), IM_COL32(255, 190, 40, 240), true);
                }
                else if (m_showCulledChunks)
                {
                    DrawFilledCube(chunk.aabbMin, chunk.aabbMax, IM_COL32(20, 60, 140, 30), IM_COL32(60, 140, 240, 120), true);
                }
            }
        }

        // 3. Global Model Bounding Box (Deep Blue)
        if (m_showGlobalBounds && (!m_rawSurfels.empty() || !m_rendererSurfels.empty()))
        {
            const ImU32 globalColor = IM_COL32(100, 160, 255, 255);
            DrawFilledCube(m_aabbMin, m_aabbMax, IM_COL32(80, 140, 255, 25), globalColor, true);
        }

        // 4. Detached Culling Camera Frustum Primitive Visualizer (Mid-Transparent Gray)
        if (m_detachCamera)
        {
            XMMATRIX invCViewProj = XMMatrixInverse(nullptr, cViewProj);
            auto UnprojectNDC = [&](float ndcX, float ndcY, float ndcZ) -> XMFLOAT3 {
                XMVECTOR clipPt = XMVectorSet(ndcX, ndcY, ndcZ, 1.0f);
                XMVECTOR worldPt = XMVector4Transform(clipPt, invCViewProj);
                XMFLOAT4 wp;
                XMStoreFloat4(&wp, worldPt);
                float invW = (std::abs(wp.w) > 1e-6f) ? (1.0f / wp.w) : 1.0f;
                return XMFLOAT3(wp.x * invW, wp.y * invW, wp.z * invW);
            };

            // Shorten the rendered frustum (45% of distance to target) so it terminates well in front of the model bounds
            float targetFarDist = std::max(0.8f, cDist * 0.45f);
            XMVECTOR farTargetWorld = XMVectorAdd(cEye, XMVectorScale(cForwardVec, targetFarDist));
            XMVECTOR farTargetClip = XMVector4Transform(XMVectorSetW(farTargetWorld, 1.0f), cViewProj);
            XMFLOAT4 farClip;
            XMStoreFloat4(&farClip, farTargetClip);
            float farNdcZ = (farClip.w > 1e-4f) ? std::max(0.01f, std::min(1.0f, farClip.z / farClip.w)) : 0.95f;

            // 4 Near Corners in 3D World Space (Z_ndc = 0.005)
            XMFLOAT3 N[4] = {
                UnprojectNDC(-1.0f, -1.0f, 0.005f), // 0: Bottom-Left
                UnprojectNDC(+1.0f, -1.0f, 0.005f), // 1: Bottom-Right
                UnprojectNDC(+1.0f, +1.0f, 0.005f), // 2: Top-Right
                UnprojectNDC(-1.0f, +1.0f, 0.005f)  // 3: Top-Left
            };

            // 4 Far Corners in 3D World Space (Z_ndc = farNdcZ)
            XMFLOAT3 F[4] = {
                UnprojectNDC(-1.0f, -1.0f, farNdcZ), // 0: Bottom-Left
                UnprojectNDC(+1.0f, -1.0f, farNdcZ), // 1: Bottom-Right
                UnprojectNDC(+1.0f, +1.0f, farNdcZ), // 2: Top-Right
                UnprojectNDC(-1.0f, +1.0f, farNdcZ)  // 3: Top-Left
            };

            ImVec2 screenN[4], screenF[4], screenEye;
            bool validN[4], validF[4];
            bool validEye = ProjectToScreen(cullEyePos, screenEye);

            for (int i = 0; i < 4; i++)
            {
                validN[i] = ProjectToScreen(N[i], screenN[i]);
                validF[i] = ProjectToScreen(F[i], screenF[i]);
            }

            // Hidden-line frustum: only the faces turned toward the viewer are drawn, back to front, each
            // shaded by how squarely it faces the light so the cone's shape reads at a glance; edges are
            // drawn only where they bound a visible face, and the silhouette (a visible face meeting a
            // hidden one) is drawn brighter.
            const XMFLOAT3 corner[8] = { N[0], N[1], N[2], N[3], F[0], F[1], F[2], F[3] };
            const ImVec2   cornerScreen[8] = { screenN[0], screenN[1], screenN[2], screenN[3], screenF[0], screenF[1], screenF[2], screenF[3] };
            const bool     cornerValid[8] = { validN[0], validN[1], validN[2], validN[3], validF[0], validF[1], validF[2], validF[3] };
            // Faces as corner indices (outward winding): 0 near, 1 far, 2 top, 3 left, 4 right, 5 bottom
            const int faceIdx[6][4] = { {0,1,2,3}, {7,6,5,4}, {3,2,6,7}, {0,3,7,4}, {1,5,6,2}, {0,4,5,1} };
            const ImU32 faceBase[6] = { IM_COL32(70, 120, 200, 255), IM_COL32(160, 185, 220, 255), IM_COL32(210, 230, 255, 255),
                                        IM_COL32(170, 180, 195, 255), IM_COL32(170, 180, 195, 255), IM_COL32(150, 160, 175, 255) };
            XMFLOAT3 frustumCenter(0, 0, 0);
            for (int i = 0; i < 8; i++) { frustumCenter.x += corner[i].x * 0.125f; frustumCenter.y += corner[i].y * 0.125f; frustumCenter.z += corner[i].z * 0.125f; }
            const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(0.5f, 0.8f, 0.6f, 0.0f));
            const XMVECTOR viewerEye = XMLoadFloat3(&eyePos);
            struct FaceDraw { int f; float depth; float shade; bool visible; };
            FaceDraw faces[6];
            for (int f = 0; f < 6; f++)
            {
                XMVECTOR p0 = XMLoadFloat3(&corner[faceIdx[f][0]]), p1 = XMLoadFloat3(&corner[faceIdx[f][1]]), p2 = XMLoadFloat3(&corner[faceIdx[f][2]]), p3 = XMLoadFloat3(&corner[faceIdx[f][3]]);
                XMVECTOR centre = XMVectorScale(XMVectorAdd(XMVectorAdd(p0, p1), XMVectorAdd(p2, p3)), 0.25f);
                XMVECTOR n = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0)));
                if (XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(centre, XMLoadFloat3(&frustumCenter)))) < 0.0f) n = XMVectorNegate(n); // Outward
                XMVECTOR toViewer = XMVectorSubtract(viewerEye, centre);
                faces[f].f = f;
                faces[f].visible = XMVectorGetX(XMVector3Dot(n, toViewer)) > 0.0f;
                faces[f].depth = XMVectorGetX(XMVector3Length(toViewer));
                faces[f].shade = 0.35f + 0.65f * std::max(0.0f, XMVectorGetX(XMVector3Dot(n, lightDir)));
            }
            std::sort(faces, faces + 6, [](const FaceDraw& a, const FaceDraw& b) { return a.depth > b.depth; }); // Farthest first
            // Two passes: the hidden (interior) faces first, dimmer and fainter, then the faces turned toward
            // the viewer over them. With the fills translucent the inside of the cone shows through, so its
            // shape reads as a volume instead of a flat cut-out.
            for (int pass = 0; pass < 2; pass++)
            for (const FaceDraw& fd : faces)
            {
                if (fd.visible != (pass == 1)) continue;
                const int* idx = faceIdx[fd.f];
                if (!(cornerValid[idx[0]] && cornerValid[idx[1]] && cornerValid[idx[2]] && cornerValid[idx[3]])) continue;
                const ImU32 base = faceBase[fd.f];
                const float shade = fd.visible ? fd.shade : fd.shade * 0.55f;
                const int r = (int)(((base >> IM_COL32_R_SHIFT) & 0xFF) * shade), g = (int)(((base >> IM_COL32_G_SHIFT) & 0xFF) * shade), b = (int)(((base >> IM_COL32_B_SHIFT) & 0xFF) * shade);
                drawList->AddQuadFilled(cornerScreen[idx[0]], cornerScreen[idx[1]], cornerScreen[idx[2]], cornerScreen[idx[3]], IM_COL32(r, g, b, fd.visible ? 120 : 70));
            }
            // Edges: each belongs to two faces; drawn only if at least one is visible, brighter on the silhouette.
            const int edgeIdx[12][2]  = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
            const int edgeFaces[12][2] = { {0,5},{0,4},{0,2},{0,3}, {1,5},{1,4},{1,2},{1,3}, {3,5},{4,5},{2,4},{2,3} };
            bool faceVisible[6]; for (const FaceDraw& fd : faces) faceVisible[fd.f] = fd.visible;
            for (int e = 0; e < 12; e++)
            {
                const bool va = faceVisible[edgeFaces[e][0]], vb = faceVisible[edgeFaces[e][1]];
                if (!(cornerValid[edgeIdx[e][0]] && cornerValid[edgeIdx[e][1]])) continue;
                if (!va && !vb) // Hidden edge: faint, so the interior structure of the cone can be followed
                {
                    drawList->AddLine(cornerScreen[edgeIdx[e][0]], cornerScreen[edgeIdx[e][1]], IM_COL32(150, 170, 200, 55), 1.0f);
                    continue;
                }
                const bool silhouette = (va != vb);
                drawList->AddLine(cornerScreen[edgeIdx[e][0]], cornerScreen[edgeIdx[e][1]],
                    silhouette ? IM_COL32(200, 230, 255, 230) : IM_COL32(170, 190, 215, 120), silhouette ? 2.0f : 1.0f);
            }
            const ImU32 nearCapWireCol = IM_COL32(110, 170, 240, 160);
            const ImU32 gazeRayCol     = IM_COL32(80, 210, 255, 180);  // Cyan forward gaze direction ray
            // 4 Apex Rays from Eye Position to Near Corners
            if (validEye)
            {
                for (int i = 0; i < 4; i++)
                {
                    if (validN[i]) drawList->AddLine(screenEye, screenN[i], nearCapWireCol, 1.0f);
                }
            }

            // Central Forward Gaze Direction Arrow (Pointing from Near Center to Far Center)
            XMFLOAT3 nearCenter(
                (N[0].x + N[1].x + N[2].x + N[3].x) * 0.25f,
                (N[0].y + N[1].y + N[2].y + N[3].y) * 0.25f,
                (N[0].z + N[1].z + N[2].z + N[3].z) * 0.25f
            );
            XMFLOAT3 farCenter(
                (F[0].x + F[1].x + F[2].x + F[3].x) * 0.25f,
                (F[0].y + F[1].y + F[2].y + F[3].y) * 0.25f,
                (F[0].z + F[1].z + F[2].z + F[3].z) * 0.25f
            );
            XMFLOAT3 topNearMid(
                (N[2].x + N[3].x) * 0.5f,
                (N[2].y + N[3].y) * 0.5f,
                (N[2].z + N[3].z) * 0.5f
            );
            float upMarkerDist = std::max(0.15f, targetFarDist * 0.08f);
            XMFLOAT3 topMarker(
                topNearMid.x + cullUp.x * upMarkerDist,
                topNearMid.y + cullUp.y * upMarkerDist,
                topNearMid.z + cullUp.z * upMarkerDist
            );

            ImVec2 sNearC, sFarC, sTopMid, sTopMarker;
            bool vNC = ProjectToScreen(nearCenter, sNearC);
            bool vFC = ProjectToScreen(farCenter, sFarC);
            bool vTM = ProjectToScreen(topNearMid, sTopMid);
            bool vTR = ProjectToScreen(topMarker, sTopMarker);

            // Forward Gaze Centerline + Arrowhead
            if (vNC && vFC)
            {
                drawList->AddLine(sNearC, sFarC, gazeRayCol, 1.5f);

                // 2D arrow wings at far center
                float dx = sFarC.x - sNearC.x;
                float dy = sFarC.y - sNearC.y;
                float len = sqrtf(dx * dx + dy * dy);
                if (len > 5.0f)
                {
                    float udx = dx / len;
                    float udy = dy / len;
                    float perpX = -udy;
                    float perpY = udx;
                    float arrowSize = 8.0f;
                    ImVec2 wing1(sFarC.x - udx * arrowSize + perpX * (arrowSize * 0.55f), sFarC.y - udy * arrowSize + perpY * (arrowSize * 0.55f));
                    ImVec2 wing2(sFarC.x - udx * arrowSize - perpX * (arrowSize * 0.55f), sFarC.y - udy * arrowSize - perpY * (arrowSize * 0.55f));
                    drawList->AddTriangleFilled(sFarC, wing1, wing2, gazeRayCol);
                }
            }

            // Up-direction orientation notch on top of the camera
            if (vTM && vTR)
            {
                drawList->AddLine(sTopMid, sTopMarker, IM_COL32(255, 215, 60, 200), 1.5f);
                drawList->AddCircleFilled(sTopMarker, 2.5f, IM_COL32(255, 215, 60, 230));
            }
        }
    }

    // Per-frame entry point: builds the UI, advances the streaming simulation and camera, then hands off to SurfelsRenderer
    void SurfelsApp::OnRender()
    {
        // Safely execute any modal/file operations before beginning the ImGui frame
        ExecutePendingAction();

        BeginFrame();

        ImGUI_UpdateIO(m_Width, m_Height);
        ImGui::NewFrame();

        if (m_fitViewPending && m_Width > 0 && m_Height > 0)
        {
            ApplyFitDistance(); // The load ran before the window was sized (see LoadSFLWFile)
            m_fitViewPending = false;
        }
        UpdateCamera(ImGui::GetIO());
        if (!m_lodTestFolder.empty()) StepLodQualityTest();
        const auto tUi = std::chrono::high_resolution_clock::now();
        BuildUI();
        m_frameUiMs = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - tUi).count();
        m_state.time += (float)(m_deltaTime / 1000.0);
        m_state.enableDithering = m_enableDitheredTransitions;
        m_state.highlightSilhouette = m_highlightSilhouetteChunks;
        m_state.showChunkStream = m_showChunkStream;
        m_state.arrivalGlowIntensity = m_arrivalGlowIntensity;
        m_state.arrivalGlowHue = m_arrivalGlowHue;
        m_state.autoSplatSize = m_autoSplatSize;
        for (int l = 0; l < 8; l++) m_state.lodRadius[l] = (l < (int)m_lodSpacing.size()) ? m_lodSpacing[l] * m_autoSplatCoverage : 0.0f;
        // Splat mode: every level (kSplatDiscMinLevel and up; 0 while kSplatGaussianLevel0 is false) draws as camera-facing discs of
        // the level's point spacing times kSplatDiscCoverage (neither the Coverage slider nor Auto Splat Size
        // applies); 0 = no measurement, the level draws Gaussians. 1.5 overlaps neighbouring discs enough that
        // the bounded Gaussian falloff blends them into a surface instead of a speckle of rimmed dots. With
        // Match Level 0 Softness on the factor is the one derived from the level 0 Gaussians instead (1.5 until
        // a splat package has been measured; see ComputeSplatSoftness) and the discs of level 1 and up take an
        // opacity floor of 0.5: coarse records already carry the bake's opacity boost, so the floor only catches
        // the sparse low tail that reads as holes, while overlapping neighbours still set the surface density.
        static const float kSplatDiscCoverage = 1.5f;
        const float splatDiscFactor = m_splatDiscMatchLevel0 ? m_splatDiscMatchFactor : kSplatDiscCoverage;
        // Coarse Levels as Gaussians: 0 (uncapped Gaussians) or, with a cap, the negative per-axis scale limit
        // (the shader keeps the Gaussian and clamps its scales to the level spacing x the cap factor).
        for (int l = 0; l < 8; l++)
        {
            const float spacing = (l < (int)m_lodSpacing.size()) ? m_lodSpacing[l] : 0.0f;
            m_state.splatDiscRadius[l] = !m_splatCoarseAsGaussians ? spacing * splatDiscFactor
                                       : (m_splatCoarseGaussianCap > 0.0f ? -spacing * m_splatCoarseGaussianCap : 0.0f);
        }
        m_state.splatDiscOpacityFloor = m_splatDiscMatchLevel0 ? 0.5f : 0.0f;
        m_state.splatViz = (uint32_t)std::clamp(m_splatViz, 0, 3);
        m_state.enableGpuSilhouetteInversion = false; // DYNAMIC SILHOUETTE switched off for now; was: m_enableSilhouetteLOD0 || m_highlightSilhouetteChunks
        m_state.silhouetteDepthThreshold = m_silhouetteDepthThreshold;
        m_state.enableTemporalFiltering = m_enableTemporalFiltering;
        m_state.temporalBlendWeight = m_temporalBlendWeight;
        m_state.enableSubpixelJitter = m_enableSubpixelJitter;
        m_state.enableVarianceClamping = m_enableVarianceClamping;

        m_state.pOcclusionVoxels = m_occlusionVoxels.empty() ? nullptr : m_occlusionVoxels.data();
        m_state.occlusionVoxelCount = (uint32_t)m_occlusionVoxels.size();
        m_state.occlusionVoxelVersion = m_occlusionVoxelsVersion;
        // While the culling camera is detached the occluder shader culls the volume against that camera
        // too (faces turned away from it, cubes outside its frustum), so the volume stays on and shows
        // the same open shell the surfels do.
        m_state.enableOcclusionCulling = m_enableOcclusionCulling;
        m_state.showOcclusionVolumeOnly = m_showOcclusionVolumeOnly;
        if (m_occlusionMipOverride >= (int)m_occlusionMips.mipCount) m_occlusionMipOverride = -1; // A rebake/reload with fewer mips drops back to Auto
        m_state.occlusionMips = m_occlusionMips;
        m_state.occlusionMipOverride = m_occlusionMipOverride;

        if (m_deviceLost)
        {
            ImGui::EndFrame();
            return;
        }

        try
        {
            const auto tRender = std::chrono::high_resolution_clock::now();
            m_pRenderer->OnRender(&m_state, &m_swapChain);
            if (m_lodTestCaptureRequested && m_lodTestIndex < m_lodTestShots.size())
            {
                RgbaImage& img = m_lodTestShots[m_lodTestIndex].image;
                m_pRenderer->TakeSceneCapture(img.pixels, img.width, img.height);
            }
            const auto tPresent = std::chrono::high_resolution_clock::now();
            EndFrame();
            const auto tDone = std::chrono::high_resolution_clock::now();
            m_frameRenderMs = std::chrono::duration<float, std::milli>(tPresent - tRender).count();
            m_framePresentMs = std::chrono::duration<float, std::milli>(tDone - tPresent).count();
            // Window the frame budget: sum per frame, publish the mean every 250 ms.
            m_budgetAccum[kBudgetSim] += m_frameSimMs;
            m_budgetAccum[kBudgetTraversal] += m_frameTraversalMs;
            m_budgetAccum[kBudgetUi] += m_frameUiMs;
            m_budgetAccum[kBudgetGpuWait] += m_pRenderer->GetLastGpuWaitMs();
            m_budgetAccum[kBudgetRecord] += m_pRenderer->GetLastCommandRecordMs();
            m_budgetAccum[kBudgetPresent] += m_framePresentMs;
            m_budgetAccumWallMs += (float)m_deltaTime;
            m_budgetAccumFrames++;
            if (m_budgetAccumWallMs >= 250.0f && m_budgetAccumFrames > 0)
            {
                for (int k = 0; k < kBudgetCount; k++) { m_budgetShown[k] = m_budgetAccum[k] / (float)m_budgetAccumFrames; m_budgetAccum[k] = 0.0f; }
                m_budgetAccumWallMs = 0.0f;
                m_budgetAccumFrames = 0;
            }
        }
        catch (const std::exception& e)
        {
            ImGui::EndFrame();
            LogD3D12Messages();
            HRESULT removeReason = m_device.GetDevice() ? m_device.GetDevice()->GetDeviceRemovedReason() : E_FAIL;
            LogTransitionTrace("!!! SurfelsApp::OnRender std::exception: '%s', deviceRemovedReason=0x%08X !!!", e.what(), (uint32_t)removeReason);
            if (removeReason != S_OK)
            {
                std::stringstream ss;
                ss << "GPU device removed / lost (0x" << std::hex << (uint32_t)removeReason << "): " << e.what();
                ReportDeviceLostAndExit(ss.str());
            }
        }
        catch (...)
        {
            ImGui::EndFrame();
            LogD3D12Messages();
            HRESULT removeReason = m_device.GetDevice() ? m_device.GetDevice()->GetDeviceRemovedReason() : E_FAIL;
            LogTransitionTrace("!!! SurfelsApp::OnRender UNKNOWN EXCEPTION CAUGHT, deviceRemovedReason=0x%08X !!!", (uint32_t)removeReason);
            if (removeReason != S_OK)
            {
                std::stringstream ss;
                ss << "GPU device removed / lost (0x" << std::hex << (uint32_t)removeReason << ").";
                ReportDeviceLostAndExit(ss.str());
            }
            else
            {
                Trace("SurfelsApp::OnRender: transient render/present glitch; continuing on next frame.\n");
            }
        }
    }

    void SurfelsApp::SetSplatDrawAsSurfels(bool asSurfels)
    {
        m_splatDrawAsSurfels = asSurfels;
        // The render mode follows SplatRenderMode() at the next state selection; re-stamp the chunks'
        // dilationMorph now (the sorted pipeline's fade weight, or the surfel value) without a re-partition
        const float morph = m_splatDrawAsSurfels ? 0.0f : 1.0f;
        for (auto& mc : m_rendererMeshletChunks) mc.dilationMorph = morph;
        for (auto& lod : m_residentLODs) for (auto& mc : lod.meshletChunks) mc.dilationMorph = morph;
        m_streamStateDirty = true;
    }

    // LOD quality test (see SurfelsApp.h). Runs on the static per-level sets (streaming off), so each capture
    // is exactly one level everywhere -- the streamed view keeps refined detail when the target coarsens,
    // which would hide the level being measured. Before that it waits for the stream to land completely,
    // which doubles as the check that Greedy streaming carries on to level 0.
    void SurfelsApp::StepLodQualityTest()
    {
        const int maxLevel = (int)m_waveletResult.lodLevels.size() - 1;
        if (!m_lodTestRunning)
        {
            if (maxLevel <= 0 || (int)m_lodSpacing.size() <= maxLevel || m_residentLODs.empty()) return;
            if (m_enableStreamingSimulation && (m_totalStreamBytes <= 0.0f || m_simulatedBytesDelivered < m_totalStreamBytes * 0.999f))
            {
                m_lodTestShotSeconds += m_deltaTime / 1000.0;
                return;
            }
            LogTransitionTrace("LOD quality test: stream fully resident after %.1f s (%.1f MB); starting captures", m_lodTestShotSeconds, m_simulatedBytesDelivered / (1024.0f * 1024.0f));
            for (int lvl = 0; lvl <= maxLevel; lvl++)
            {
                size_t resident = 0, total = 0;
                if (lvl < (int)m_lodStreamChunks.size()) for (const auto& c : m_lodStreamChunks[lvl]) { total++; if (c.isResident) resident++; }
                LogTransitionTrace("LOD quality test: level %d resident %zu / %zu blocks", lvl, resident, total);
            }

            m_autoLOD = false;
            m_autoRotate = false;
            m_enableStreamDecay = false;
            if (m_detachCamera) SetDetachCamera(false);
            m_enableStreamingSimulation = false;

            // Splat packages: both drawing modes (sorted Gaussians, then opaque surfel discs); other packages draw one way.
            const int drawModes = m_splatMode ? 2 : 1;
            for (int mode = 0; mode < drawModes; mode++)
            for (int rule = 0; rule < 2; rule++)
            {
                const bool screenSpace = (rule == 1);
                for (int L = 1; L <= maxLevel; L++)
                {
                    const float d = LodSwitchDistance(L, screenSpace) * 1.001f;
                    auto add = [&](int level, bool gauss, int repeat, float cap = 0.0f)
                    {
                        LodTestShot s;
                        s.screenSpaceRule = screenSpace; s.switchLevel = L; s.distance = d;
                        s.level = level; s.coarseAsGaussians = gauss; s.coarseGaussianCap = cap; s.repeat = repeat; s.asSurfels = (mode == 1);
                        m_lodTestShots.push_back(std::move(s));
                    };
                    add(0, false, 0);
                    add(0, false, 1);
                    add(L - 1, false, 0);
                    add(L, false, 0);
                    if (mode == 0) // Coarse levels as their own Gaussians: a sorted-Gaussian option only (the surfel pipeline sizes its discs from lodRadius)
                    {
                        add(L, true, 0);
                        for (float cap : kLodTestGaussianCaps) add(L, true, 0, cap);
                    }
                }
            }
            m_lodTestIndex = 0;
            m_lodTestSettledFrames = -1;
            m_lodTestRunning = true;
            m_lodTestSavedCoarseAsGaussians = m_splatCoarseAsGaussians;
            m_lodTestSavedCoarseGaussianCap = m_splatCoarseGaussianCap;
            m_lodTestSavedDrawAsSurfels = m_splatDrawAsSurfels;
            LogTransitionTrace("LOD quality test: %zu captures at %ux%u, yaw %.3f pitch %.3f, pixel threshold %.2f", m_lodTestShots.size(), m_Width, m_Height, m_yaw, m_pitch, m_lodPixelSpacing);
        }

        if (m_lodTestIndex >= m_lodTestShots.size()) return;
        LodTestShot& shot = m_lodTestShots[m_lodTestIndex];

        if (m_lodTestCaptureRequested)
        {
            if (shot.image.Empty()) return; // Taken after this frame's render (see OnRender)
            LogTransitionTrace("LOD quality test: %zu/%zu %s, %s switch %d, d=%.3f, level %d%s%s captured after %.1f s",
                m_lodTestIndex + 1, m_lodTestShots.size(), shot.asSurfels ? "surfels" : "gaussians", shot.screenSpaceRule ? "screen" : "legacy", shot.switchLevel, shot.distance,
                shot.level, shot.coarseAsGaussians ? " (coarse as Gaussians)" : "", shot.repeat ? " (repeat)" : "", m_lodTestShotSeconds);
            m_lodTestCaptureRequested = false;
            m_lodTestSettledFrames = -1;
            const bool last = (++m_lodTestIndex >= m_lodTestShots.size());
            // A group (mode, rule, switch level) is complete: rewrite the report so a run that dies keeps what it measured
            if (last || m_lodTestShots[m_lodTestIndex].switchLevel != shot.switchLevel || m_lodTestShots[m_lodTestIndex].screenSpaceRule != shot.screenSpaceRule)
                FinishLodQualityTest();
            if (last)
            {
                m_splatCoarseAsGaussians = m_lodTestSavedCoarseAsGaussians;
                m_splatCoarseGaussianCap = m_lodTestSavedCoarseGaussianCap;
                if (m_splatDrawAsSurfels != m_lodTestSavedDrawAsSurfels) SetSplatDrawAsSurfels(m_lodTestSavedDrawAsSurfels);
                LogTransitionTrace("LOD quality test: done");
                PostQuitMessage(0);
            }
            return;
        }

        if (m_lodTestSettledFrames < 0)
        {
            m_distance = shot.distance;
            m_selectedPreviewLOD = shot.level;
            m_splatCoarseAsGaussians = shot.coarseAsGaussians;
            m_splatCoarseGaussianCap = shot.coarseGaussianCap;
            if (m_splatMode && m_splatDrawAsSurfels != shot.asSurfels) SetSplatDrawAsSurfels(shot.asSurfels);
            UpdatePreviewSurfels();
            m_lodTestSettledFrames = 0;
            m_lodTestShotSeconds = 0.0;
            return;
        }
        m_lodTestShotSeconds += m_deltaTime / 1000.0;
        // Long enough for the upload, the sort and TAA (blend weight 0.15) to converge on a still camera.
        if (++m_lodTestSettledFrames >= 24)
        {
            m_pRenderer->RequestSceneCapture();
            m_lodTestCaptureRequested = true;
        }
    }

    void SurfelsApp::FinishLodQualityTest()
    {
        CreateDirectoryA(m_lodTestFolder.c_str(), nullptr);
        std::ofstream md(m_lodTestFolder + "\\report.md");
        std::ofstream csv(m_lodTestFolder + "\\results.csv");
        csv << "mode,rule,switch_level,distance,level,drawing,level_spacing_px,psnr_db,ssim\n";
        md << "# LOD quality test\n\n";
        md << "Model: `" << m_inputPathBuf << "`, " << m_Width << "x" << m_Height << ", 45 degree vertical field of view, yaw " << m_yaw << ", pitch " << m_pitch
           << ". Each row renders the model at the distance where the rule switches to the given level, and compares it with level 0 at the same distance "
              "(PSNR over RGB and SSIM over luma, both inside the model's bounding box). `L0 again` is a second level 0 capture: the TAA noise floor.\n\n";
        md << "Level spacing (median nearest neighbour):";
        for (size_t l = 0; l < m_lodSpacing.size(); l++) md << " L" << l << " " << m_lodSpacing[l] * 1000.0f << " mm";
        md << "\n\n";

        bool asSurfels = false;
        auto find = [&](bool screenSpace, int switchLevel, int level, bool gauss, int repeat, float cap = 0.0f) -> const LodTestShot*
        {
            for (const auto& s : m_lodTestShots)
                if (s.asSurfels == asSurfels && s.screenSpaceRule == screenSpace && s.switchLevel == switchLevel && s.level == level && s.coarseAsGaussians == gauss
                    && s.coarseGaussianCap == cap && s.repeat == repeat && !s.image.Empty())
                    return &s;
            return nullptr;
        };
        const int maxLevel = (int)m_waveletResult.lodLevels.size() - 1;
        std::vector<std::string> montages;
        for (int section = 0; section < 4; section++)
        {
            asSurfels = (section >= 2);
            const bool screenSpace = (section & 1) != 0;
            if (!m_splatMode && asSurfels) break;
            const char* modeName = !m_splatMode ? "" : (asSurfels ? "Drawn as surfels (mode 4): " : "Sorted Gaussians (mode 3): ");
            md << "## " << modeName << (screenSpace ? "screen-space rule (new), threshold " : "legacy distance rule (old)");
            if (screenSpace) md << m_lodPixelSpacing << " px";
            md << "\n\n| Switch to | Distance | Drawn | Spacing on screen | PSNR (dB) | SSIM |\n|---|---|---|---|---|---|\n";
            for (int L = 1; L <= maxLevel; L++)
            {
                const LodTestShot* ref = find(screenSpace, L, 0, false, 0);
                if (!ref) continue;
                struct Row { std::string name; const LodTestShot* shot; int level; };
                std::vector<Row> rows = {
                    { "L0 again", find(screenSpace, L, 0, false, 1), 0 },
                    { "previous level, discs", find(screenSpace, L, L - 1, false, 0), L - 1 },
                    { "new level, discs", find(screenSpace, L, L, false, 0), L },
                    { "new level, Gaussians", find(screenSpace, L, L, true, 0), L } };
                for (float cap : kLodTestGaussianCaps)
                {
                    char capName[64];
                    snprintf(capName, sizeof(capName), "new level, Gaussians capped %.2f x spacing", cap);
                    rows.push_back({ capName, find(screenSpace, L, L, true, 0, cap), L });
                }
                std::vector<const RgbaImage*> all = { &ref->image };
                for (const Row& r : rows) if (r.shot) all.push_back(&r.shot->image);
                const PixelRect rect = ImageCompare::ContentBounds(all);
                for (const Row& r : rows)
                {
                    if (!r.shot) continue;
                    const double psnr = ImageCompare::Psnr(ref->image, r.shot->image, rect);
                    const double ssim = ImageCompare::Ssim(ref->image, r.shot->image, rect);
                    const float spacingPx = (r.level < (int)m_lodSpacing.size()) ? m_lodSpacing[r.level] * LodFocalPixels() / LodEffectiveDistance(ref->distance) : 0.0f;
                    char line[256];
                    snprintf(line, sizeof(line), "| L%d | %.3f | L%d %s | %.2f px | %.2f | %.4f |\n", L, ref->distance, r.level, r.name.c_str(), spacingPx, psnr, ssim);
                    md << line;
                    csv << (asSurfels ? "surfels," : "gaussians,") << (screenSpace ? "screen" : "legacy") << "," << L << "," << ref->distance << "," << r.level << ",\"" << r.name << "\"," << spacingPx << "," << psnr << "," << ssim << "\n";
                }
                // Montage, sorted Gaussians: level 0 | new level as discs | as Gaussians | capped Gaussians (each cap).
                // Surfels: level 0 | previous level | new level | its error against level 0.
                const LodTestShot* prev = find(screenSpace, L, L - 1, false, 0);
                const LodTestShot* discs = find(screenSpace, L, L, false, 0);
                if (prev && discs)
                {
                    char name[64];
                    snprintf(name, sizeof(name), "%s_%s_switch_L%d.png", asSurfels ? "surfels" : "gaussians", screenSpace ? "screen" : "legacy", L);
                    if (std::find(m_lodTestMontagesWritten.begin(), m_lodTestMontagesWritten.end(), name) == m_lodTestMontagesWritten.end())
                    {
                        std::vector<RgbaImage> panels;
                        panels.push_back(ImageCompare::Crop(ref->image, rect));
                        if (asSurfels)
                        {
                            panels.push_back(ImageCompare::Crop(prev->image, rect));
                            panels.push_back(ImageCompare::Crop(discs->image, rect));
                            panels.push_back(ImageCompare::ErrorMap(panels[0], panels[2]));
                        }
                        else
                        {
                            panels.push_back(ImageCompare::Crop(discs->image, rect));
                            if (const LodTestShot* g = find(screenSpace, L, L, true, 0)) panels.push_back(ImageCompare::Crop(g->image, rect));
                            for (float cap : kLodTestGaussianCaps)
                                if (const LodTestShot* g = find(screenSpace, L, L, true, 0, cap)) panels.push_back(ImageCompare::Crop(g->image, rect));
                        }
                        std::vector<const RgbaImage*> panelPtrs;
                        for (const RgbaImage& p : panels) panelPtrs.push_back(&p);
                        ImageCompare::WritePng(m_lodTestFolder + "\\" + name, ImageCompare::SideBySide(panelPtrs));
                        m_lodTestMontagesWritten.push_back(name);
                    }
                    montages.push_back(name);
                }
            }
            md << (asSurfels ? "\nMontages, left to right: level 0, the previous level, the new level, then the new level's error against level 0 (x4).\n\n"
                             : "\nMontages, left to right: level 0, the new level as discs, as uncapped Gaussians, then as Gaussians capped at 0.75 and 1.5 x the level spacing.\n\n");
            for (const std::string& name : montages) md << "![" << name << "](" << name << ")\n\n";
            montages.clear();
        }
        LogTransitionTrace("LOD quality test: report written to %s", m_lodTestFolder.c_str());
    }

    // Once the GPU device is confirmed removed/hung there is nothing further this app can safely do with
    // it -- every subsequent frame already stops calling into the renderer (see the m_deviceLost check at
    // the top of OnRender()), which previously just left the window frozen on its last presented frame
    // forever with no indication anything had gone wrong. Since ImGui itself needs the (now-dead) GPU
    // pipeline to draw, the in-app status message text is never actually presented once we're in this
    // state, so surface the failure via a plain Win32 message box instead (needs no GPU), then exit
    // cleanly rather than leave the user staring at an unresponsive window.
    void SurfelsApp::ReportDeviceLostAndExit(const std::string& message)
    {
        if (m_deviceLost) return; // Already reported/exiting -- avoid stacking duplicate dialogs
        m_deviceLost = true;

        m_statusMessage = message;
        m_statusIsSuccess = false;
        Trace("%s\n", message.c_str());

        std::string dialogText = message + "\n\nThe application cannot recover from this and will now close.";
        const std::string dialogTitle = std::string(AppTitle()) + " - GPU Device Lost";
        MessageBoxA(nullptr, dialogText.c_str(), dialogTitle.c_str(), MB_OK | MB_ICONERROR);
        PostQuitMessage(0);
    }
}

