// SplatPakApp.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Implements the SplatPak app shell: file I/O, the wavelet preprocessing
// pipeline, the streaming/decay/silhouette simulation, and every ImGui panel. See
// SplatPakApp.h for the class overview and SplatRenderer.cpp for the GPU side.

#include "SplatPakApp.h"
#include <chrono>
#include <cfloat>
#include "../../libs/gstx/OcclusionVolume.h"
#include <DirectXCollision.h>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <shobjidl.h>
#include <shlobj.h>
#include "BakeTuningCache.h"
#include "RdoDispatcher.h"

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 614; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

namespace Splat
{
    static constexpr const char* kTraceLogFilename = "lod_transition_trace.log";

    // Every tooltip in the app: a smaller proportional face (s_tooltipFont, Segoe UI, registered in OnCreate), wrapped to a
    // block about 3 wide by 4 tall -- taller when the text has short lines of its own, never wider; a short tip stays on one
    // line. Laid out unwrapped the text covers an area A (each paragraph's width times the line height); wrapped at width w
    // it is about A / w tall, so w = sqrt(0.75 A).
    static ImFont* s_tooltipFont = nullptr;
    static void ShowTooltip(const char* fmt, ...)
    {
        char text[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        ImGui::BeginTooltip();
        if (s_tooltipFont) ImGui::PushFont(s_tooltipFont);
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float area = 0.0f, widest = 0.0f;
        for (const char* p = text;;)
        {
            const char* end = strchr(p, '\n');
            if (!end) end = p + strlen(p);
            const float w = ImGui::CalcTextSize(p, end).x;
            area += w * lineHeight;
            widest = std::max(widest, w);
            if (!*end) break;
            p = end + 1;
        }
        const float minWidth = 16.0f * ImGui::GetFontSize();
        const float wrap = std::min(widest + 2.0f, std::max(minWidth, sqrtf(0.75f * area))); // A little slack: wrapping at exactly the width breaks the line
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        if (s_tooltipFont) ImGui::PopFont();
        ImGui::EndTooltip();
    }

    // Every progress bar in the app: the label (a percentage, or more) centred in the bar, which ImGui 1.53 puts at the end
    // of the fill instead. Labels are kept compact; one that would not fit with a comfortable margin falls back to the percentage.
    static void ProgressBarCentred(float fraction, const ImVec2& size, const char* label)
    {
        ImGui::ProgressBar(fraction, size, "");
        if (!label || !*label) return;
        const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
        char pct[16];
        if (ImGui::CalcTextSize(label).x > (bmax.x - bmin.x) - 2.0f * ImGui::GetFontSize())
        {
            snprintf(pct, sizeof(pct), "%.0f%%", std::clamp(fraction, 0.0f, 1.0f) * 100.0f);
            label = pct;
        }
        const ImVec2 ts = ImGui::CalcTextSize(label);
        ImGui::GetWindowDrawList()->AddText(ImVec2((bmin.x + bmax.x - ts.x) * 0.5f, (bmin.y + bmax.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
    }

    // Splat packages: the Gaussian level 0 (the reference 3DGS rendering of the finest level) is on: full
    // Gaussian splat rendering is the default, and every LOD cross-fade is an opacity fade (see
    // AppendChunkToRenderer). Levels 1 and up draw as discs (kSplatDiscMinLevel = 1 in Splat.hlsl, which must agree),
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

    SplatPakApp::SplatPakApp(LPCSTR name, Mode mode) : CAULDRON_DX12::FrameworkWindows(name), m_mode(mode)
    {
        if (m_mode == Mode::Viewer) m_activeTab = 1; // No generator tab in the viewer: open on the Renderer tab

        // Diagnostic note: temporarily forcing m_isGpuValidationLayerEnabled = true (both configs)
        // masked the LOD-transition GPU hang instead of reporting it -- GPU validation's extra
        // CPU/GPU synchronization gave the async copy queue enough time to finish before its upload
        // buffers were freed during a chunk-buffer resize, papering over the race without fixing it.
        // Root cause (missing wait on m_pCopyQueue before releasing its source buffers) is fixed in
        // SplatRenderer::FlushCopyQueue() and its call sites; restored to normal here since GPU
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
    std::vector<std::string> SplatPakApp::SplitCommandLine(const char* cmdLine)
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

    const char* SplatPakApp::CommandLineUsage()
    {
        // One option per line with its description indented below it: reads the same on a console
        // and in the proportional-font message box (aligned columns do not survive the latter).
        return
            "Usage:\n"
            "  SplatPak.exe [options] [file]\n"
            "  SplatPakViewer.exe [options] [file]\n"
            "\n"
            "file\n"
            "  A .gstx package to open, or (Studio mode) a .ply or .splat point cloud to preprocess. Dropping a file on the executable does the same. Without it the startup_dataset from config.json is opened.\n"
            "\n"
            "--viewer\n"
            "  Viewer mode: the Renderer and Streaming tabs only (the default for SplatPakViewer.exe).\n"
            "\n"
            "--studio\n"
            "  Studio mode: the Package Bake, Renderer and Streaming tabs (the default for SplatPak.exe).\n"
            "\n"
            "--render-path <p>\n"
            "  GPU path for this run: auto, mesh, vs6 or vs5. Overrides render_path in config.json without changing it.\n"
            "\n"
            "--lod-quality-test <folder>\n"
            "  Once the model has fully streamed in, render it at every LOD switch distance, compare each level against level 0 at the same distance (PSNR, SSIM), write the images and report.md to the folder, and quit.\n"
            "\n"
            "--stream-test\n"
            "  Once the model has fully streamed in, select the 5G profile, press Evict at the launch view, let the refill run for 60 s and quit. The trace log records it: a repeatable streaming baseline.\n"
            "\n"
            "--lod-calibration <folder>\n"
            "  Once the model has fully streamed in, render the launch view at three distances with level 0 everywhere, the model-wide level rule and per-block levels at a range of pixel thresholds, compare each with level 0 (PSNR, SSIM, splats drawn) and write report.md, results.csv and the images to the folder.\n"
            "\n"
            "--capture-views <folder> [--reference <folder>]\n"
            "  Once the model has fully streamed in, render level 0 from eight directions around it (and the first again, the noise floor),\n"
            "  write view_<n>.rgba to the folder, and quit. With --reference, also compare each view with the same view in that folder\n"
            "  (another package's capture) and write report.txt: PSNR over RGB and SSIM over luma, inside the model's bounds.\n"
            "\n"
            "--bake-to <out.gstx> [--sh-degree <1-3>] [--sh-bits <8|6|5|4>] [--remember-settings]\n"
            "  (--remember-settings also remembers the settings for the scan in bake_tunings.json, as a bake from the Package Bake tab does.)\n"
            "  Bake the scan given on the command line with its default parameters (SH Quality degree and SH Precision bits as given;\n"
            "  degree 3 and 8 bits otherwise), save the package to <out.gstx> with its shadow mesh, and quit. The trace log records the size.\n"
            "\n"
            "--help, -h, /?\n"
            "  Show this text.\n";
    }

    // The mode switches and --help are handled before the app exists (RunSplatPakApp in AppEntry.cpp);
    // what arrives here is the rest: an optional file path and --render-path.
    void SplatPakApp::OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight)
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
            else if (a == "--stream-test")
            {
                m_streamTest = true;
                LogTransitionTrace("Command line: --stream-test");
            }
            else if (a == "--capture-views" && i + 1 < args.size())
            {
                m_viewCapFolder = args[++i];
                LogTransitionTrace("Command line: --capture-views %s", m_viewCapFolder.c_str());
            }
            else if (a == "--reference" && i + 1 < args.size())
            {
                m_viewCapReference = args[++i];
            }
            else if (a == "--bake-to" && i + 1 < args.size())
            {
                m_cliBakeOutput = args[++i];
                LogTransitionTrace("Command line: --bake-to %s", m_cliBakeOutput.c_str());
            }
            else if (a == "--remember-settings")
            {
                m_cliRemember = true;
            }
            else if (a == "--sh-degree" && i + 1 < args.size())
            {
                m_splatSHQuality = std::clamp(atoi(args[++i].c_str()), 1, 3) - 1;
                LogTransitionTrace("Command line: --sh-degree %d", m_splatSHQuality + 1);
            }
            else if (a == "--sh-bits" && i + 1 < args.size())
            {
                const int bits = atoi(args[++i].c_str());
                int precision = 0;
                for (int p = 0; p < 4; p++) if (kSHPrecisionBits[p] == bits) precision = p;
                SetSHPrecision(precision);
                LogTransitionTrace("Command line: --sh-bits %d", (int)SHBitsFor(m_shPrecision));
            }
            else if (a == "--lod-calibration" && i + 1 < args.size())
            {
                m_lodCalFolder = args[++i];
                LogTransitionTrace("Command line: --lod-calibration %s", m_lodCalFolder.c_str());
            }
            else if (!a.empty() && a[0] != '-' && a[0] != '/')
            {
                strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), a.c_str(), _TRUNCATE);
            }
        }
    }

    // Places to look for a file given relative to the repo layout (e.g. "assets/cthulu/cthulu.gstx" or
    // "config.json"): relative to the working directory at a few depths, then relative to the folder
    // the executable lives in (bin/) at the same depths. The working directory depends on how the app
    // was launched -- bin/ from the launch scripts, the repo root or a build sub-folder from Visual
    // Studio, anywhere at all from a shortcut -- so the executable's own location is the one anchor
    // that is always right. Absolute paths are returned as-is.
    std::vector<std::string> SplatPakApp::ResolveRelativeCandidates(const std::string& relativePath) const
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
    std::string SplatPakApp::GetProjectRootFolder() const
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
    std::string SplatPakApp::GetDialogDefaultFolder() const
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
    void SplatPakApp::RememberDialogFolder(const std::string& filePath)
    {
        if (filePath.empty()) return;
        size_t slash = filePath.find_last_of("\\/");
        if (slash == std::string::npos) return;
        std::string folder = filePath.substr(0, slash);
        if (folder == m_lastDialogFolder) return;
        m_lastDialogFolder = folder;
        SaveConfigFile();
    }

    // Reads config.json or config.ini for dev mode, the startup dataset path, and other app settings
    void SplatPakApp::LoadConfigFile()
    {
        std::vector<std::string> configPaths;
        for (const char* name : { "config.json", "config.ini" })
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
                        size_t hPos = line.find("\"lod_per_block\":");
                        size_t eqPos = std::string::npos;
                        if (hPos != std::string::npos) eqPos = line.find(':', hPos);
                        else if (line.find("lod_per_block=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            m_lodPerBlock = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
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
                        // A config from before a section was added lists fewer: its order is kept and the new sections follow.
                        const std::vector<int> order = readList("\"right_panel_order\":", "right_panel_order=");
                        if (!order.empty() && (int)order.size() <= kRightSectionCount)
                        {
                            bool seen[kRightSectionCount] = {};
                            bool valid = true;
                            for (int v : order) { if (v >= kRightSectionCount || seen[v]) valid = false; else seen[v] = true; }
                            if (valid)
                            {
                                int n = 0;
                                for (int v : order) m_rightPanelOrder[n++] = v;
                                for (int v = 0; v < kRightSectionCount; v++) if (!seen[v]) m_rightPanelOrder[n++] = v;
                            }
                        }
                        const std::vector<int> open = readList("\"right_panel_open\":", "right_panel_open=");
                        if ((int)open.size() <= kRightSectionCount)
                            for (int i = 0; i < (int)open.size(); i++) m_rightPanelOpen[i] = (open[i] != 0);
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

                    // Side panels folded (JSON or INI); see RecordSidePanel.
                    for (int side = 0; side < 2; side++)
                    {
                        const char* jsonKey = side == 0 ? "\"left_panel_collapsed\":" : "\"right_panel_collapsed\":";
                        const char* iniKey  = side == 0 ? "left_panel_collapsed=" : "right_panel_collapsed=";
                        size_t kPos = line.find(jsonKey);
                        size_t eqPos = std::string::npos;
                        if (kPos != std::string::npos) eqPos = line.find(':', kPos);
                        else if (line.find(iniKey) != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            (side == 0 ? m_leftPanelCollapsed : m_rightPanelCollapsed) = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }

                    {
                        size_t kPos = line.find("\"present_panel_collapsed\":");
                        if (kPos != std::string::npos)
                        {
                            std::string val = line.substr(line.find(':', kPos) + 1);
                            m_presentPanelCollapsed = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                        }
                    }

                    // Display toggles that are handy to pin for a test run (JSON or INI): the occlusion volume ("Show
                    // Occlusion Volume") and the arrival glow ("Refinement Visualizer"), both on by default, and the
                    // View Shadow Mesh debug view, off by default.
                    for (int toggle = 0; toggle < 3; toggle++)
                    {
                        const char* jsonKey = toggle == 0 ? "\"show_occlusion_volume\":" : toggle == 1 ? "\"refinement_visualizer\":" : "\"show_shadow_mesh\":";
                        const char* iniKey  = toggle == 0 ? "show_occlusion_volume=" : toggle == 1 ? "refinement_visualizer=" : "show_shadow_mesh=";
                        size_t kPos = line.find(jsonKey);
                        size_t eqPos = std::string::npos;
                        if (kPos != std::string::npos) eqPos = line.find(':', kPos);
                        else if (line.find(iniKey) != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            const bool on = !(val.find("false") != std::string::npos || val.find('0') != std::string::npos);
                            if (toggle == 0) m_enableOcclusionCulling = m_showOcclusionSetting = on;
                            else if (toggle == 1) m_showChunkStream = on;
                            else m_showShadowMesh = on;
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

                    // Render path (JSON or INI): "auto" (default), "mesh", "vs6" (vertex shaders, Shader
                    // Model 6.0) or "vs5" (vertex shaders through the legacy Shader Model 5.1 compiler).
                    // Anything but auto forces that fallback on capable hardware, for testing; a path the
                    // hardware cannot run is ignored. See SplatRenderer::RenderPath.
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
                    // Menu state (JSON or INI): the tab shown, and each collapsing header's open state as
                    // "label=1;label=0" (see MenuHeader)
                    {
                        size_t kPos = line.find("\"active_tab\":");
                        size_t eqPos = std::string::npos;
                        if (kPos != std::string::npos) eqPos = line.find(':', kPos);
                        else if (line.find("active_tab=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos) m_activeTab = std::clamp(atoi(line.c_str() + eqPos + 1), m_mode == Mode::Viewer ? 1 : 0, 2);
                    }
                    {
                        size_t kPos = line.find("\"menu_open\":");
                        size_t eqPos = std::string::npos;
                        if (kPos != std::string::npos) eqPos = line.find(':', kPos);
                        else if (line.find("menu_open=") != std::string::npos) eqPos = line.find('=');
                        if (eqPos != std::string::npos)
                        {
                            std::string val = line.substr(eqPos + 1);
                            const size_t q1 = val.find('"'), q2 = q1 == std::string::npos ? std::string::npos : val.find('"', q1 + 1);
                            if (q2 != std::string::npos) val = val.substr(q1 + 1, q2 - q1 - 1);
                            size_t start = 0;
                            while (start < val.size())
                            {
                                size_t end = val.find(';', start);
                                if (end == std::string::npos) end = val.size();
                                const std::string entry = val.substr(start, end - start);
                                const size_t eq = entry.rfind('=');
                                if (eq != std::string::npos && eq > 0) m_menuOpen[entry.substr(0, eq)] = entry[eq + 1] == '1';
                                start = end + 1;
                            }
                        }
                    }

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


    // Persists the current config-file-backed settings back to disk
    void SplatPakApp::SaveConfigFile()
    {
        // A scripted run changes settings for its own purposes (no glow in captures, no loupe): never the user's
        if (Unattended()) return;
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
            out << "  \"left_panel_collapsed\": " << (m_leftPanelCollapsed ? "true" : "false") << ",\n";
            out << "  \"right_panel_collapsed\": " << (m_rightPanelCollapsed ? "true" : "false") << ",\n";
            out << "  \"present_panel_collapsed\": " << (m_presentPanelCollapsed ? "true" : "false") << ",\n";
            out << "  \"refinement_visualizer\": " << (m_showChunkStream ? "true" : "false") << ",\n";
            out << "  \"show_shadow_mesh\": " << (m_showShadowMesh ? "true" : "false") << ",\n";
            out << "  \"splat_disc_match_level0\": " << (m_splatDiscMatchLevel0 ? "true" : "false") << ",\n";
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
            out << "  \"lod_per_block\": " << (m_lodPerBlock ? "true" : "false") << ",\n";
            out << "  \"lod_pixel_spacing\": " << m_lodPixelSpacing << ",\n";
            out << "  \"render_path\": \"" << m_renderPathConfig << "\",\n";
            out << "  \"active_tab\": " << m_activeTab << ",\n";
            {
                std::string menus;
                for (const auto& m : m_menuOpen) menus += (menus.empty() ? "" : ";") + m.first + "=" + (m.second ? "1" : "0");
                out << "  \"menu_open\": \"" << menus << "\",\n";
            }
            out << "  \"last_dialog_folder\": \"" << m_lastDialogFolder << "\"\n";
            out << "}\n";
            out.close();
        }
    }

    // Boots the renderer, loads config, and auto-loads the startup dataset (or waits for the user to pick one)
    void SplatPakApp::OnCreate()
    {
        StreamRuntime::SetTrace([](const char* line) { LogTransitionTrace("%s", line); });
        {
            FILE* fp = fopen(kTraceLogFilename, "w");
            if (fp)
            {
                fprintf(fp, "=== Splat LOD Transition Trace Started ===\n");
                fclose(fp);
            }
        }
        LogTransitionTrace("SplatPakApp::OnCreate initialized (%s mode).", m_mode == Mode::Viewer ? "Viewer" : "Studio");
        SplatRenderer::SetAppTitle(AppTitle()); // The renderer's own dialogs name the product too
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
            s_tooltipFont = fontIo.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 14.0f * dpiScale); // ShowTooltip's face
        }
        m_pRenderer = new SplatRenderer();
        m_pRenderer->SetRenderPathOverride(m_renderPathOverride);
        m_pRenderer->SetQuietStartup(!m_lodTestFolder.empty() || !m_cliBakeOutput.empty() || !m_viewCapFolder.empty()); // An unattended test run or bake has nobody to dismiss the fallback notice
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
        // via config.json or config.ini's "startup_dataset" key -- see LoadConfigFile() above,
        // already called earlier in OnCreate() -- so a different default can be swapped in without
        // recompiling). This intentionally does NOT fall back to any external "datasets" scratch
        // folder the user might separately have configured, so a default launch keeps working even if
        // that external folder gets cleared out. The default points at a pre-built .gstx package (fast,
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
                if (!std::ifstream(path, std::ios::binary).good()) continue;
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
                m_statusMessage = IsViewerOnly() ? "Ready. Use File -> Open to load a .gstx package."
                                                 : "Ready. Use File -> Open to load a .ply or .splat dataset.";
                m_statusIsSuccess = true;
            }
        }

        m_swapChain.SetVSync(m_vsync);
    }

    // Saves the config file and tears down the renderer
    void SplatPakApp::OnDestroy()
    {
        m_op.Abandon(); // Cancel and wait for a background operation before anything it reads goes away
        CancelShadowMeshBuild();
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
            Trace("SplatPakApp::OnDestroy: GPUFlush failed (device suspended/removed?); skipping flush and tearing down anyway\n");
        }

        m_pRenderer->OnDestroyWindowSizeDependentResources();
        m_pRenderer->OnDestroy();
        delete m_pRenderer;
        m_pRenderer = nullptr;

        DestroyShaderCache(&m_device);
    }

    // Forwards raw window messages to ImGui
    bool SplatPakApp::OnEvent(MSG msg)
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
            else if (ctrlPressed && msg.wParam == 'E' && !m_rawSplats.empty() && !IsViewerOnly())
            {
                m_pendingAction = PendingAction::ExportStream;
                return true;
            }
        }
        return true;
    }

    // Detach Camera on: the culling camera freezes where the view is, and the view steps to the side,
    // farther out, so the frozen frustum can be inspected. Off: the view returns to the frozen camera.
    void SplatPakApp::SetDetachCamera(bool on)
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
    void SplatPakApp::OnResize(bool resizeRender)
    {
        if (m_pRenderer)
            m_pRenderer->OnCreateWindowSizeDependentResources(&m_swapChain, m_Width, m_Height);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)m_Width, (float)m_Height);
        m_stream.m_streamStateDirty = true;

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

    void SplatPakApp::ApplyFitDistance()
    {
        m_distance = FitDistanceForViewport();
        m_lastFitDistance = m_distance;
    }

    // Rebuilds display-dependent renderer resources (e.g. after a format change)
    void SplatPakApp::OnUpdateDisplay()
    {
        if (m_pRenderer)
            m_pRenderer->OnUpdateDisplayDependentResources(&m_swapChain);
    }

    // Clears every in-memory dataset/streaming structure back to the empty state
    void SplatPakApp::CloseDataset()
    {
        m_sourceHash = 0;
        CancelShadowMeshBuild();
        SetShadowMesh(nullptr);
        SetOccluderMesh(nullptr);
        m_rawSplats.clear();
        m_chunks.clear();
        m_waveletResult.lodLevels.clear();
        m_rendererSplats.clear();
        m_rendererRawSplats.clear();
        m_stream.m_rendererMeshletChunks.clear();
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
        m_compressedSizeMB = 0.0f; m_transformedMB = m_transformedSHMB = 0.0f;
        m_compressionRatio = 0.0f;
        m_deadbandZeroPercent = 0.0f;
        m_state.pPoints = nullptr;
        m_state.pRawSplats = nullptr;
        m_state.pChunks = nullptr;
        m_state.splatCount = 0;
        m_state.chunkCount = 0;
        if (m_pRenderer) m_pRenderer->FlushGPU();
        m_statusMessage = "Dataset closed. Use File -> Open to load a model.";
        m_statusIsSuccess = true;
    }

    // Shows a standard Win32 "Open File" dialog; returns the chosen path, or empty if cancelled
    std::string SplatPakApp::OpenFileDialog(const char* filter, const char* title, const char* defaultExt)
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
    std::string SplatPakApp::SaveFileDialog(const char* filter, const char* defaultExt, const char* title)
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
            std::wstring wDefExt = defaultExt ? std::wstring(defaultExt, defaultExt + strlen(defaultExt)) : L"gstx";
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
    bool SplatPakApp::LoadFile(const std::string& filepath)
    {
        std::string lowerPath = filepath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        if (lowerPath.size() >= 5 && lowerPath.substr(lowerPath.size() - 5) == ".gstx")
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

    // Loads a pre-compressed .gstx package and restores the full multi-LOD chunk hierarchy from it.
    // An invalid package (bad signature, newer format version, truncated file, payload the linked codec
    // cannot decode) raises a Retry / Cancel dialog that explains the fault and how to re-bake; Retry
    // re-reads the file, which is what a user wants after re-copying or re-saving it.
    bool SplatPakApp::LoadSFLWFile(const std::string& filepath)
    {
        for (;;)
        {
            m_statusMessage = "Loading compressed 3DGS stream package (.gstx)...";
            std::string reason;
            // Loaded into a temporary: only a package that loads replaces the current one
            StreamPackager::SFLWPackageData pkg;
            if (StreamPackager::LoadPackage(filepath, pkg, &reason))
            {
                CancelShadowMeshBuild();
                SetShadowMesh(pkg.shadowMesh.Empty() ? nullptr : std::make_shared<const ShadowMesh>(std::move(pkg.shadowMesh)));
                SetOccluderMesh(pkg.occluderMesh.Empty() ? nullptr : std::make_shared<const ShadowMesh>(std::move(pkg.occluderMesh)));
                m_loadedPackage = std::move(pkg);
                break;
            }

            m_statusMessage = "Failed to load compressed package: " + filepath + " -- " + reason;
            m_statusIsSuccess = false;
            LogTransitionTrace("LoadSFLWFile: LoadPackage failed for '%s': %s", filepath.c_str(), reason.c_str());

            const std::string dialogText =
                std::string(AppTitle()) + " could not load this package:\n\n" + filepath + "\n\n" + reason + "\n\n" +
                SFLWRebakeAdvice() + "\n\nRetry loads the file again. Cancel leaves the current model in place.";
            const std::string dialogTitle = std::string(AppTitle()) + " - Invalid .gstx Package";
            if (Unattended()) return false; // A scripted run has nobody to answer the dialog: the trace has the reason
            const int choice = MessageBoxA(nullptr, dialogText.c_str(), dialogTitle.c_str(), MB_RETRYCANCEL | MB_ICONWARNING);
            if (choice != IDRETRY)
                return false;
            LogTransitionTrace("LoadSFLWFile: user chose Retry for '%s'", filepath.c_str());
        }
        LogTransitionTrace("LoadSFLWFile: '%s' v%u: %zu chunks, %llu LOD0 splats, %zu baked occluder blocks in %u mips, source %llu bytes",
            filepath.c_str(), m_loadedPackage.header.version, m_loadedPackage.chunkManifests.size(),
            (unsigned long long)m_loadedPackage.totalSplats, m_loadedPackage.occlusionVoxels.size(), m_loadedPackage.occlusionMips.mipCount,
            (unsigned long long)m_loadedPackage.header.sourceFileBytes);

        m_loadedFilePath = filepath;
        m_rendererSourceDescription = filepath;
        strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);
        m_isLoadedFromSFLW = true;
        m_sourceHash = 0; // A package: nothing to remember bake settings against
        m_hasBakedParams = false; // Until the load finishes, the previous model's bake does not apply
        m_colourEstimate = ColourEstimate{}; // The figures describe a bake; a package shows what it stores
        m_entropy = EntropyProfile{};

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
        // and LoadPackage already falls back to 1.0 (SplatRenderer::State's own default) in that case.
        m_state.splatRadius = m_loadedPackage.header.splatRadius;
        m_state.orientMode = 0; // Default to Normal-Oriented Surface Tangent Discs, same as a raw PLY/SPLAT load

        // Start from the volume baked into the package (v3+ only; empty otherwise -- see
        // StreamPackager::LoadPackage). A baked volume was built from the ORIGINAL raw point cloud, which
        // is strictly better data than anything reconstructable here (the .gstx round-trip through
        // 8-byte quantization is lossy -- Venus's dense-interior splats visibly regressed the first time
        // this unconditionally rebuilt on every load). So keep the baked cubes as-is when present; only
        // fall back to generating one here if the package genuinely didn't ship with one (see further
        // down, after the reconstructed splats exist).
        m_occlusionVoxels = m_loadedPackage.occlusionVoxels;
        m_occlusionMips = m_loadedPackage.occlusionMips;
        m_occlusionVoxelsVersion++;
        m_generateOcclusionVolume = true;

        // Collect all raw splats from package
        m_rendererSplats.clear();
        m_rendererOctreeChunks.clear();
        m_chunks.clear();

        // Splat packages (splat format 1) decode through SplatCodec: every level's records become
        // splats plus their Gaussians, the latter appended to m_splatAttributes with each splat's
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
        std::vector<SplatPoint> decodedLOD0; // Splat packages: LOD 0 across every chunk, in chunk order
        auto decodeSplatLevel = [&](size_t c, size_t l, std::vector<SplatPoint>& outSplats)
        {
            std::vector<SplatAttributes> attrs;
            const std::vector<uint8_t>& sh = m_loadedPackage.chunkLODSH[c][l];
            SplatCodec::Decode(m_loadedPackage.chunkLODSplats[c][l], sh.empty() ? nullptr : sh.data(), sh.size(),
                               m_loadedPackage.splatParams, (uint32_t)l, (uint32_t)m_splatAttributes.size(), outSplats, attrs);
            m_splatAttributes.insert(m_splatAttributes.end(), attrs.begin(), attrs.end());
        };

        for (size_t c = 0; c < m_loadedPackage.chunkManifests.size(); c++)
        {
            const auto& cm = m_loadedPackage.chunkManifests[c];

            std::vector<SplatPoint> chunkDecoded;
            if (!m_loadedPackage.chunkLODSplats[c].empty()) decodeSplatLevel(c, 0, chunkDecoded);
            decodedLOD0.insert(decodedLOD0.end(), chunkDecoded.begin(), chunkDecoded.end());

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
            cd.splats = std::move(chunkDecoded);
            m_chunks.push_back(cd);
        }

        // Unquantize splats to raw format (splat packages were decoded above)
        m_rendererRawSplats = std::move(decodedLOD0);

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
            m_detailGrid = DetailGrid::Build(m_rendererRawSplats, m_aabbMin, m_aabbMax);
            LogTransitionTrace("DetailHeatmap: %ux%ux%u cells (%.4f m), %u occupied, rebuilt from LOD 0 (none in the package)",
                m_detailGrid.nx, m_detailGrid.ny, m_detailGrid.nz, m_detailGrid.cellSize, m_detailGrid.occupiedCells);
        }

        // Partition into GPU micro-meshlets (64 splats per meshlet chunk)
        m_stream.m_rendererMeshletChunks.clear();
        SpatialOctree::PartitionIntoMeshletChunks(
            m_rendererRawSplats,
            m_stream.m_rendererMeshletChunks,
            64,
            m_enableMortonOrder
        );
        // Sorted Gaussian pipeline: dilationMorph is the level 0 Gaussian fade-in weight (see AppendChunkToRenderer);
        // chunks resident at load show Gaussians at once, so they carry 1. Drawn as splats: the splat value (0).
        if (m_splatMode && !m_splatDrawAsDiscs) for (auto& mc : m_stream.m_rendererMeshletChunks) mc.dilationMorph = 1.0f;

        // Re-quantize to guarantee exact alignment with meshlet ordering
        m_rendererSplats = Quantizer::QuantizePoints(m_rendererRawSplats, m_aabbMin, m_aabbMax);

        // Populate wavelet result metadata for the full LOD hierarchy. The package stores every LOD
        // level per chunk (m_loadedPackage.chunkManifests[c].lods / chunkLODSplats[c][lvl]); the residency
        // equalizer, decay, silhouette refinement and PrecacheResidentLODs/InitStreamingSimulation all key off
        // m_waveletResult.lodLevels.size(). Reassemble each level by concatenating that level's decoded
        // splats across every chunk that has it (chunks can have fewer levels than the global max, e.g. very
        // small chunks whose wavelet decomposition terminated early).
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
            for (const auto& cd : m_chunks) chunkPoints += cd.splats.size();
            LogTransitionTrace("LoadSFLW: %zu chunks carry %zu points for re-export, %zu levels in package, Max Wavelet LODs set to %d", m_chunks.size(), chunkPoints, numLODs, m_maxLODLevels);
        }

        for (size_t lvl = 0; lvl < numLODs; lvl++)
        {
            std::vector<SplatPoint> levelDecoded;
            float errorSum = 0.0f;
            size_t errorCount = 0;

            for (size_t c = 0; c < m_loadedPackage.chunkManifests.size(); c++)
            {
                const auto& cm = m_loadedPackage.chunkManifests[c];
                if (lvl >= cm.lods.size() || lvl >= m_loadedPackage.chunkLODSplats[c].size()) continue;
                if (lvl > 0)
                {
                    std::vector<SplatPoint> tmp;
                    decodeSplatLevel(c, lvl, tmp);
                    levelDecoded.insert(levelDecoded.end(), tmp.begin(), tmp.end());
                }
                errorSum += cm.lods[lvl].geometricError;
                errorCount++;
            }

            WaveletLODLevel wLod;
            wLod.level = (int)lvl;
            // Level 0 was decoded once already (the renderer's raw points, meshlet-ordered)
            wLod.splats = lvl == 0 ? m_rendererRawSplats : std::move(levelDecoded);
            wLod.geometricError = errorCount > 0 ? (errorSum / (float)errorCount) : 0.0f;
            m_waveletResult.lodLevels.push_back(std::move(wLod));
        }

        // Compression ratio = original input file vs this package file. The original's size is
        // carried in the header (0 = unknown: the 248-byte-per-splat 3DGS baseline).
        m_sourceFileBytes = m_loadedPackage.header.sourceFileBytes;
        m_rawFileSizeMB = m_sourceFileBytes > 0
            ? (float)m_sourceFileBytes / (1024.0f * 1024.0f)
            : (m_rendererSplats.size() * 248.0f) / (1024.0f * 1024.0f);
        {
            std::ifstream pkg(ResolvePackagePath(filepath), std::ios::ate | std::ios::binary);
            uint64_t packageBytes = pkg.is_open() ? (uint64_t)pkg.tellg() : (uint64_t)m_loadedPackage.totalCompressedBytes;
            m_compressedSizeMB = (float)packageBytes / (1024.0f * 1024.0f);
            uint64_t recordBytes = 0, shBytes = 0;
            for (size_t c = 0; c < m_loadedPackage.chunkManifests.size(); c++)
            {
                for (const auto& lod : m_loadedPackage.chunkManifests[c].lods) recordBytes += lod.uncompressedByteSize;
                if (c < m_loadedPackage.chunkLODSH.size())
                    for (const auto& sh : m_loadedPackage.chunkLODSH[c]) shBytes += sh.size();
            }
            m_transformedMB = (float)(recordBytes + shBytes) / (1024.0f * 1024.0f);
            m_transformedSHMB = (float)shBytes / (1024.0f * 1024.0f);
        }
        m_compressionRatio = (m_rawFileSizeMB > 0 && m_compressedSizeMB > 0) ? (m_rawFileSizeMB / std::max(0.001f, m_compressedSizeMB)) : 0.0f;
        MeasurePackageEntropy();

        m_rawSplats = m_rendererRawSplats;

        PrecacheResidentLODs();
        InitStreamingSimulation();
        UpdatePreviewSplats();
        RebuildHeatmapClusterCubes();

        // Only bake a volume here when the package didn't already ship with one (an older file, or one
        // exported with generation off) -- see the comment above where m_occlusionVoxels is restored.
        // The cached grid is invalidated either way; a rebuild uses these reconstructed points, since
        // packages don't retain the original unquantized cloud.
        m_occlusionGrid.valid = false;
        if (m_occlusionVoxels.empty() && !HasOccluderMesh())
        {
            RefreshOcclusionVolume();
        }
        else
        {
            m_enableOcclusionCulling = m_showOcclusionSetting; // Show Occlusion Volume: on whenever one exists, unless switched off
        }

        // Setup active renderer state pointing to renderer's dedicated buffers
        m_state.pPoints = m_rendererSplats.data();
        m_state.pRawSplats = m_rendererRawSplats.data();
        m_state.splatCount = (uint32_t)m_rendererSplats.size();
        m_state.pChunks = m_stream.m_rendererMeshletChunks.data();
        m_state.chunkCount = (uint32_t)m_stream.m_rendererMeshletChunks.size();
        m_state.aabbMin = m_aabbMin;
        m_state.aabbExtents = m_extents;
        m_state.renderMode = m_splatMode ? SplatRenderMode() : (m_enableQuantization ? 1 : 2);

        if (m_pRenderer)
        {
            m_pRenderer->FlushGPU();
        }

        // Stay on the Package Bake tab: it is the primary workflow. The Renderer tab is one click away.

        {
            char detail[96];
            snprintf(detail, sizeof(detail), " (%zu Gaussians, %.1f MB)", m_rendererSplats.size(), m_compressedSizeMB);
            m_statusMessage = "Successfully loaded compressed model: " + filepath + detail;
        }
        m_bakedParams = CurrentBakeParams(); // A package is the baked state of the tab's settings
        m_packagePath = ResolvePackagePath(filepath);
        {
            // The RDO analysis the package was encoded with, when it carries one (PackageRdoFooter)
            std::string text, error;
            RdoProfile p;
            if (PackageRdoFooter::Read(m_packagePath, text))
            {
                std::istringstream in(text);
                if (p.Parse(in, error))
                {
                    AdoptRdoAnalysis(p);
                    m_rdoPackagePath = m_packagePath;
                    LogTransitionTrace("LoadSFLW: RDO profile embedded (%zu encodings, scan %016llx)", p.analysis.points.size(), (unsigned long long)p.sourceHash);
                }
                else LogTransitionTrace("LoadSFLW: the embedded RDO profile could not be read: %s", error.c_str());
            }
        }
        m_hasBakedParams = true;
        m_statusIsSuccess = true;
        return true;
    }

    // Opens a raw .splat or .ply scan (StartOpenScan)
    bool SplatPakApp::LoadSPLATFile(const std::string& filepath) { return StartOpenScan(filepath, true); }

    bool SplatPakApp::LoadPLYFile(const std::string& filepath) { return StartOpenScan(filepath, false); }

    // Opens a raw .ply or .splat: reads it and bakes it on the worker with defaults chosen for its size. Only a
    // finished bake replaces the displayed model, so Cancel keeps the previous one.
    bool SplatPakApp::StartOpenScan(const std::string& filepath, bool splatFile)
    {
        struct Opened
        {
            std::vector<SplatPoint> points;
            std::vector<SplatAttributes> attributes;
            uint64_t fileBytes = 0;
            uint32_t sourceSHDegree = 0;
            BakeParams params;
            BakeResult bake;
            uint64_t hash = 0;     // The file's contents (BakeTuningCache)
            bool tuned = false;    // params are the ones remembered for this file
        };
        auto opened = std::make_shared<Opened>();
        const BakeParams keep = CurrentBakeParams();
        const int shQuality = m_splatSHQuality;
        SyncStreamInputs();
        const StreamSettings settings = m_stream.settings;
        const StreamCamera camera = m_stream.camera;
        const std::string name = filepath.substr(filepath.find_last_of("/\\") + 1);
        m_statusMessage = "Opening " + name + "...";
        const bool useTunings = !Unattended(); // A scripted run bakes with what it was given
        return RunOperation("Opening " + name, [this, filepath, splatFile, keep, shQuality, settings, camera, opened, useTunings](Progress& progress)
        {
            progress.Stage(splatFile ? "Reading the .splat file" : "Reading the .ply file", 0.0f);
            double origin[3] = { 0, 0, 0 };
            const bool read = splatFile ? SPLATLoader::LoadSPLAT(filepath, opened->points, origin, &opened->attributes)
                                        : PLYLoader::LoadPLY(filepath, opened->points, origin, &opened->attributes);
            if (!read || opened->points.empty() || progress.Cancelled()) return false;
            std::ifstream in(filepath, std::ios::ate | std::ios::binary);
            opened->fileBytes = in.is_open() ? (uint64_t)in.tellg() : 0;
            // Everything is 3D Gaussians: a plain point cloud gets an isotropic Gaussian at every point, sized from
            // its point spacing (PointCloudGaussians.h), before it is cut into chunks
            if (opened->attributes.empty())
            {
                progress.Stage("Gaussians for the point cloud", 0.0f);
                const float sigma = PointCloudGaussians::Build(opened->points, opened->attributes);
                LogTransitionTrace("Point cloud: %zu points shown as Gaussians of sigma %.5f (half the median point spacing)", opened->points.size(), sigma);
            }
            opened->sourceSHDegree = SourceSHDegreeOf(opened->attributes);
            opened->params = DefaultBakeParams(opened->points, keep);
            // The settings last baked from this very file, when there are any
            if (useTunings)
            {
                progress.Stage("Checking for remembered bake settings", 0.0f);
                opened->hash = BakeTuningCache::HashFile(filepath);
                const auto tunings = BakeTuningCache::Load();
                const auto it = tunings.find(opened->hash);
                if (it != tunings.end())
                {
                    const BakeTuning& t = it->second;
                    opened->params.chunkSize = t.chunkSize;
                    opened->params.maxLODs = t.maxLODs;
                    opened->params.shQuality = std::clamp(t.shQuality, 0, 2);
                    opened->params.shPrecision = std::clamp(t.shPrecision, 0, 3);
                    for (int b = 0; b < 3; b++)
                        opened->params.shBandBits[b] = (uint8_t)(t.shBits[b] > 0 ? std::clamp(t.shBits[b], 4, 8) : SHBitsFor(opened->params.shPrecision));
                    opened->params.deadbandMM = t.deadbandMM;
                    opened->params.occlusion = t.occlusion;
                    opened->tuned = true;
                }
            }
            (void)shQuality;
            BakeSource source;
            source.points = &opened->points;
            source.attributes = &opened->attributes;
            source.splatMode = !opened->attributes.empty();
            source.shDegree = std::min((uint32_t)std::clamp(opened->params.shQuality, 0, 2) + 1u, opened->sourceSHDegree);
            memcpy(source.shBits, opened->params.shBandBits, 3);
            return RunBake(opened->params, source, settings, camera, opened->bake, progress);
        },
        [this, filepath, opened](bool ok, bool cancelled)
        {
            if (!ok)
            {
                m_statusMessage = cancelled ? "Open cancelled: the previous model is still loaded." : "Failed to load: " + filepath;
                m_statusIsSuccess = false;
                LogTransitionTrace("Open of '%s' %s", filepath.c_str(), cancelled ? "cancelled" : "failed");
                return;
            }
            ReleaseModelMeshes(); // The build reads the scan about to be replaced, and the old model's meshes must not be drawn over the new one
            // A package loaded before brought its own render data, which the metrics panels prefer: it goes with it
            m_rendererSplats.clear();
            m_rendererRawSplats.clear();
            m_rendererOctreeChunks.clear();
            m_rawSplats = std::move(opened->points);
            m_splatAttributes = std::move(opened->attributes);
            m_sourceHasSplatAttributes = !m_splatAttributes.empty();
            m_splatMode = m_sourceHasSplatAttributes;
            m_sourceSHDegree = opened->sourceSHDegree;
            LogTransitionTrace("Source spherical harmonics: degree %u (SH Quality keeps degree %u)", m_sourceSHDegree, BakeSHDegree());
            m_loadedFilePath = filepath;
            m_rendererSourceDescription = filepath; // The viewport draws the raw cloud now (Input Model Metrics)
            m_isLoadedFromSFLW = false;
            strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);
            m_sourceFileBytes = opened->fileBytes;
            m_rawFileSizeMB = opened->fileBytes > 0 ? (float)opened->fileBytes / (1024.0f * 1024.0f) : (m_rawSplats.size() * 32.0f) / (1024.0f * 1024.0f);
            m_chunkSize = opened->params.chunkSize;
            m_maxLODLevels = opened->params.maxLODs;
            m_deadbandThresholdMM = opened->params.deadbandMM;
            m_splatSHQuality = opened->params.shQuality;
            m_shPrecision = opened->params.shPrecision;
            memcpy(m_shBandBits, opened->params.shBandBits, 3);
            m_generateOcclusionVolume = opened->params.occlusion;
            m_sourceHash = opened->hash;
            m_packagePath.clear(); // A scan: no package until it is saved
            if (opened->tuned)
                LogTransitionTrace("Bake settings remembered for this file (%016llx): chunk %.3f, %d levels, SH degree %d, %u bits, deadband %.2f mm",
                    (unsigned long long)opened->hash, opened->params.chunkSize, opened->params.maxLODs, opened->params.shQuality + 1,
                    (unsigned)SHBitsFor(opened->params.shPrecision), opened->params.deadbandMM);
            ApplyBake(opened->bake, opened->params, true);
            m_statusMessage = "Successfully loaded " + std::to_string(m_rawSplats.size()) + " Gaussian splats from " + filepath;
            m_statusIsSuccess = true;
        });
    }

    // Loads the built-in benchmark dataset, generating it from scratch if not found on disk
    void SplatPakApp::GenerateSyntheticScene(uint32_t count)
    {
        ReleaseModelMeshes(); // The build reads the scan about to be replaced; the old model's meshes go with it
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
            if (ext == ".gstx")
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
                m_statusMessage = "Successfully loaded benchmark dataset (" + std::to_string(m_rawSplats.size()) + " points from " + foundPath + ").";
                m_statusIsSuccess = true;
                return;
            }
        }

        // Fallback to procedural generator if disk file is missing
        m_rawSplats = SyntheticGenerator::GenerateUrbanStreetScene(count);
        m_splatAttributes.clear();
        m_sourceHasSplatAttributes = false;
        m_splatMode = false;
        m_loadedFilePath = "Synthetic Benchmark (" + std::to_string(count / 1000) + "K points)";
        m_rendererSourceDescription = m_loadedFilePath;
        m_isLoadedFromSFLW = false;
        m_sourceFileBytes = 0;
        m_rawFileSizeMB = (m_rawSplats.size() * sizeof(SplatPoint)) / (1024.0f * 1024.0f);

        // A plain point cloud: Gaussians sized from its point spacing, defaults for its size, then a bake
        PointCloudGaussians::Build(m_rawSplats, m_splatAttributes);
        m_sourceHasSplatAttributes = !m_splatAttributes.empty();
        m_splatMode = m_sourceHasSplatAttributes;
        DetectSourceSHDegree();
        const BakeParams defaults = DefaultBakeParams(m_rawSplats, CurrentBakeParams());
        m_chunkSize = defaults.chunkSize;
        m_maxLODLevels = defaults.maxLODs;
        m_deadbandThresholdMM = defaults.deadbandMM;
        StartBake(true);
        m_statusMessage = "Generated " + std::to_string(m_rawSplats.size()) + " synthetic benchmark splats.";
        m_statusIsSuccess = true;
    }

    // Re-chunks and re-decomposes the loaded raw point cloud after a preprocessing parameter changes
    // ---- Bake ---------------------------------------------------------------------------------------------

    SplatPakApp::BakeParams SplatPakApp::CurrentBakeParams() const
    {
        BakeParams p;
        p.chunkSize = m_chunkSize;
        p.maxLODs = m_maxLODLevels;
        p.shQuality = m_splatSHQuality;
        p.shPrecision = m_shPrecision;
        memcpy(p.shBandBits, m_shBandBits, 3);
        p.deadbandMM = m_deadbandThresholdMM;
        p.occlusion = m_generateOcclusionVolume;
        return p;
    }

    std::string SplatPakApp::PendingBakeChanges() const
    {
        if (!m_hasBakedParams) return std::string();
        const BakeParams c = CurrentBakeParams(), b = m_bakedParams;
        std::string s;
        auto add = [&](bool changed, const char* name) { if (changed) { if (!s.empty()) s += ", "; s += name; } };
        add(c.chunkSize != b.chunkSize, "Octree Chunk");
        add(c.maxLODs != b.maxLODs, "Max Wavelet LODs");
        add(c.shQuality != b.shQuality, "SH Quality");
        add(c.shPrecision != b.shPrecision, "SH Precision");
        add(c.shPrecision == b.shPrecision && memcmp(c.shBandBits, b.shBandBits, 3) != 0, "SH bits per band");
        add(c.deadbandMM != b.deadbandMM, "Deadband Zero");
        add(c.occlusion != b.occlusion, "Occlusion Volume");
        return s;
    }

    // Per scan, when it is loaded: about 4 x 4 x 4 octree chunks, a level count from the point count and a
    // deadband in proportion to the model's size. A bake then uses whatever the tab holds.
    SplatPakApp::BakeParams SplatPakApp::DefaultBakeParams(const std::vector<SplatPoint>& points, BakeParams keep)
    {
        XMFLOAT3 mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
        for (const auto& s : points)
        {
            mn.x = std::min(mn.x, s.position.x); mn.y = std::min(mn.y, s.position.y); mn.z = std::min(mn.z, s.position.z);
            mx.x = std::max(mx.x, s.position.x); mx.y = std::max(mx.y, s.position.y); mx.z = std::max(mx.z, s.position.z);
        }
        const float maxDim = points.empty() ? 1.0f : std::max(mx.x - mn.x, std::max(mx.y - mn.y, mx.z - mn.z));
        const size_t n = points.size();
        keep.chunkSize = std::max(0.10f, maxDim / 4.0f);
        keep.maxLODs = n > 2000000 ? 5 : n > 500000 ? 4 : n > 100000 ? 3 : 2;
        keep.deadbandMM = std::max(0.5f, maxDim * 1.5f);
        return keep;
    }

    SplatPakApp::BakeSource SplatPakApp::LoadedBakeSource() const
    {
        BakeSource s;
        s.points = &m_rawSplats;
        s.attributes = &m_splatAttributes;
        s.splatMode = m_splatMode;
        s.shDegree = BakeSHDegree();
        memcpy(s.shBits, m_shBandBits, 3);
        return s;
    }

    // Bakes the loaded scan with the tab's parameters: in the background with the progress dialog, or inline for
    // scripted runs. fitCamera frames the model, for a scan just loaded.
    void SplatPakApp::StartBake(bool fitCamera)
    {
        if (m_rawSplats.empty()) return;
        const BakeParams params = CurrentBakeParams();
        const BakeSource source = LoadedBakeSource();
        SyncStreamInputs();
        const StreamSettings settings = m_stream.settings;
        const StreamCamera camera = m_stream.camera;
        auto result = std::make_shared<BakeResult>();
        RunOperation("Baking", [this, params, source, settings, camera, result](Progress& progress)
        {
            return RunBake(params, source, settings, camera, *result, progress);
        },
        [this, params, result, fitCamera](bool ok, bool cancelled)
        {
            if (ok)
            {
                ApplyBake(*result, params, fitCamera);
                m_statusMessage = "Baked: " + std::to_string(m_chunks.size()) + " chunks, " + std::to_string(m_waveletResult.lodLevels.size()) + " levels.";
                // Remembered for this scan file: opening it again bakes this way (BakeTuningCache)
                if (m_sourceHash != 0 && !m_isLoadedFromSFLW && !Unattended())
                {
                    BakeTuning t;
                    t.chunkSize = params.chunkSize; t.maxLODs = params.maxLODs; t.shQuality = params.shQuality; t.shPrecision = params.shPrecision;
                    for (int b = 0; b < 3; b++) t.shBits[b] = params.shBandBits[b];
                    t.deadbandMM = params.deadbandMM; t.occlusion = params.occlusion; t.file = m_loadedFilePath;
                    BakeTuningCache::Store(m_sourceHash, t);
                    m_statusMessage += " These settings are remembered for this scan.";
                }
                m_statusIsSuccess = true;
            }
            else
            {
                m_statusMessage = cancelled ? "Bake cancelled: the model is unchanged and the changes are still pending."
                                            : "Bake failed: the model is unchanged.";
                m_statusIsSuccess = false;
                LogTransitionTrace("Bake %s; the displayed model is unchanged", cancelled ? "cancelled" : "failed");
            }
        });
    }

    // The whole bake, on the worker thread: reads only the source, the parameters and settings that cannot change
    // while the modal dialog is up, and writes only out. ApplyBake swaps the result in on the UI thread.
    bool SplatPakApp::RunBake(const BakeParams& params, const BakeSource& source, const StreamSettings& streamSettings,
                              const StreamCamera& streamCamera, BakeResult& out, Progress& progress) const
    {
        const auto& points = *source.points;
        if (points.empty()) return false;
        const auto tBake = std::chrono::high_resolution_clock::now();

        // Stage fractions follow the stage times on Cthulhu (3.3 M Gaussians, the "Bake stages" trace line)
        progress.Stage("Measuring the model", 0.0f);
        out.aabbMin = XMFLOAT3(1e9f, 1e9f, 1e9f);
        out.aabbMax = XMFLOAT3(-1e9f, -1e9f, -1e9f);
        for (const auto& s : points)
        {
            out.aabbMin.x = std::min(out.aabbMin.x, s.position.x); out.aabbMin.y = std::min(out.aabbMin.y, s.position.y); out.aabbMin.z = std::min(out.aabbMin.z, s.position.z);
            out.aabbMax.x = std::max(out.aabbMax.x, s.position.x); out.aabbMax.y = std::max(out.aabbMax.y, s.position.y); out.aabbMax.z = std::max(out.aabbMax.z, s.position.z);
        }
        out.center = XMFLOAT3((out.aabbMin.x + out.aabbMax.x) * 0.5f, (out.aabbMin.y + out.aabbMax.y) * 0.5f, (out.aabbMin.z + out.aabbMax.z) * 0.5f);
        out.extents = XMFLOAT3(out.aabbMax.x - out.aabbMin.x, out.aabbMax.y - out.aabbMin.y, out.aabbMax.z - out.aabbMin.z);

        // Detail grid over the raw cloud: an input to the streaming order (see StreamOrder.h) that ships inside the package
        progress.Stage("Detail grid", 0.005f);
        out.detailGrid = DetailGrid::Build(points, out.aabbMin, out.aabbMax);
        LogTransitionTrace("DetailHeatmap: %ux%ux%u cells (%.4f m), %u occupied, built from %zu raw points",
            out.detailGrid.nx, out.detailGrid.ny, out.detailGrid.nz, out.detailGrid.cellSize, out.detailGrid.occupiedCells, points.size());
        if (progress.Cancelled()) return false;

        progress.Stage("Octree chunks", 0.01f);
        out.chunks = SpatialOctree::PartitionIntoChunks(points, params.chunkSize);
        if (progress.Cancelled()) return false;

        progress.Stage("Wavelet levels", 0.02f);
        out.waveletResult = LiftingWavelet::DecomposeChunk(points, params.maxLODs, params.deadbandMM / 1000.0f);
        // Splat mode: every coarse point takes the Gaussian merged from its two children (as the package will)
        if (source.splatMode && source.attributes) SplatCodec::MergeLevels(out.waveletResult, *source.attributes);
        if (progress.Cancelled()) return false;

        // Splat mode: the encode parameters every level shares (positions span the model bounds; the scale range
        // covers every source Gaussian plus the growth of the coarser levels). The packager derives the same values
        // from the same inputs when the package is written.
        out.splatParams = SplatEncodeParams{};
        out.splatParams.shDegree = source.shDegree;
        out.splatParams.aabbMin = out.aabbMin;
        out.splatParams.aabbMax = out.aabbMax;
        if (source.splatMode)
            SplatCodec::ComputeScaleRange(*source.attributes, (uint32_t)params.maxLODs, out.splatParams.scaleLog2Min, out.splatParams.scaleLog2Max);

        progress.Stage("Estimating the package size", 0.04f);
        // Compression estimates
        // Package size as the bake would write it: every level ships in full inside the .gstx, so every
        // level's records (plus the SH stream in splat mode) are byte-plane shuffled and put through the
        // payload codec at the package quality (the header, manifest and occlusion volume are negligible
        // next to the payloads). Brotli at that quality runs well under 1 MB/s per core, so the estimate
        // compresses a sample: windows of about kWindowBytes of contiguous records, spread over the levels
        // in proportion to their bytes (every level gets one), each shuffled and compressed like a chunk
        // level's payload, and each class (records, SH) scaled by its sampled ratio. A window holds every
        // byte plane in its true proportion, which a slice of a shuffled stream does not (the planes
        // compress very differently, so slices put the estimate 10% out). It lands within a few percent
        // of the written package. The ratio shown is the original file against that.
        // In splat mode each window is also measured as the package stores it, colour as YCoCg-R (ColourTransform.h) and
        // every level's SH but the coarsest as residuals from the next coarser level (SHCoding.h; here over the whole
        // model rather than per chunk, close enough for an estimate): entropy of the interleaved window and Brotli size
        // of the shuffled one, both without and with the transforms, for the Statistics row. The estimate is the transformed size. The plain
        // comparison compresses every other window only (against the same windows transformed), so the figure
        // costs about half a sample's compression.
        const auto tEstimateStart = std::chrono::high_resolution_clock::now();
        constexpr size_t kWindowBytes = 512u << 10;
        constexpr int kWindowsPerClass = 6;
        struct LevelRecords { std::vector<PackedSplatGPU> splats; std::vector<PackedPointGPU> points; std::vector<uint8_t> sh; };
        std::vector<LevelRecords> levels;
        auto encodeLevel = [&](const std::vector<SplatPoint>& splats, uint32_t level)
        {
            LevelRecords lr;
            if (source.splatMode)
            {
                SplatCodec::Encode(splats, *source.attributes, level, out.splatParams, lr.splats, lr.sh, &out.waveletResult.mergedAttributes);
                const uint8_t* bits = source.shBits;
                RequantiseSH(lr.sh.data(), lr.splats.size(), SplatCodec::SHRecordBytes(out.splatParams.shDegree), bits);
            }
            else lr.points = Quantizer::QuantizePoints(splats, out.aabbMin, out.aabbMax);
            levels.push_back(std::move(lr));
        };
        encodeLevel(points, 0);
        for (size_t lvl = 1; lvl < out.waveletResult.lodLevels.size(); lvl++)
            encodeLevel(out.waveletResult.lodLevels[lvl].splats, (uint32_t)lvl);
        const float encodeSeconds = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - tEstimateStart).count();

        const uint32_t shStride = source.splatMode ? SplatCodec::SHRecordBytes(out.splatParams.shDegree) : 0u;
        // SH residuals as the package stores them; the absolute SH is kept for the plain comparison
        std::vector<std::vector<uint8_t>> shAbsolute;
        const bool shResidual = source.splatMode && shStride > 0 && levels.size() >= 2;
        if (shResidual)
        {
            std::vector<const uint8_t*> recs(levels.size());
            std::vector<size_t> counts(levels.size());
            std::vector<std::vector<uint8_t>*> shs(levels.size());
            for (size_t l = 0; l < levels.size(); l++)
            {
                shAbsolute.push_back(levels[l].sh);
                recs[l] = reinterpret_cast<const uint8_t*>(levels[l].splats.data());
                counts[l] = levels[l].splats.size();
                shs[l] = &levels[l].sh;
            }
            const uint8_t* bits = source.shBits;
            SHCoding::EncodeResiduals(recs, counts, sizeof(PackedSplatGPU), shs, shStride, bits);
        }
        const uint32_t recordStride = source.splatMode ? (uint32_t)sizeof(PackedSplatGPU) : (uint32_t)sizeof(PackedPointGPU);
        size_t packageBytes = sizeof(SFLWFileHeader);
        double plainEstimate = (double)sizeof(SFLWFileHeader), entropyPlain = 0.0, entropyYCoCg = 0.0;
        out.colourEstimate = ColourEstimate{};
        for (int cls = 0; cls < 2; cls++)
        {
            const bool sh = (cls == 1);
            const uint32_t stride = sh ? shStride : recordStride;
            if (stride == 0) continue;
            auto levelBytes = [&](const LevelRecords& lr) -> const uint8_t*
            {
                return sh ? lr.sh.data() : source.splatMode ? reinterpret_cast<const uint8_t*>(lr.splats.data()) : reinterpret_cast<const uint8_t*>(lr.points.data());
            };
            auto levelCount = [&](const LevelRecords& lr) -> size_t { return sh ? lr.sh.size() / stride : source.splatMode ? lr.splats.size() : lr.points.size(); };
            size_t total = 0;
            for (const auto& lr : levels) total += levelCount(lr) * stride;
            if (total == 0) continue;
            const size_t window = std::max<size_t>(1, kWindowBytes / stride);
            std::vector<std::vector<uint8_t>> samples, plainSamples;
            std::vector<size_t> pairedSample; // Index in samples of each plain window's transformed twin
            double classEntropyPlain = 0.0, classEntropyYCoCg = 0.0, classEntropyBytes = 0.0;
            for (size_t li = 0; li < levels.size(); li++)
            {
                const LevelRecords& lr = levels[li];
                const size_t count = levelCount(lr);
                if (count == 0) continue;
                const bool residualLevel = sh && shResidual && li + 1 < levels.size(); // Stored as residuals, in YCoCg already
                const int n = std::max(1, (int)std::lround((double)kWindowsPerClass * (double)(count * stride) / (double)total));
                const size_t w = std::min(window, count);
                for (int j = 0; j < n; j++)
                {
                    const size_t centre = (size_t)(((double)j + 0.5) * (double)count / (double)n);
                    const size_t start = std::min(count - w, centre > w / 2 ? centre - w / 2 : (size_t)0);
                    if (!source.splatMode)
                    {
                        samples.push_back(ByteShuffle::Shuffle(levelBytes(lr) + start * stride, w, stride));
                        continue;
                    }
                    // The plain window: the absolute RGB bytes
                    const uint8_t* plainBytes = residualLevel ? shAbsolute[li].data() : levelBytes(lr);
                    std::vector<uint8_t> win(plainBytes + start * stride, plainBytes + (start + w) * stride);
                    const double wb = (double)win.size();
                    classEntropyPlain += wb * ColourTransform::MeanPlaneEntropy(win.data(), w, stride);
                    if (samples.size() % 2 == 0)
                    {
                        pairedSample.push_back(samples.size());
                        plainSamples.push_back(ByteShuffle::Shuffle(win.data(), w, stride));
                    }
                    if (residualLevel) win.assign(levelBytes(lr) + start * stride, levelBytes(lr) + (start + w) * stride);
                    else if (sh) ColourTransform::ApplySH(win.data(), win.size());
                    else ColourTransform::ApplyRecords(win.data(), w, stride);
                    classEntropyYCoCg += wb * ColourTransform::MeanPlaneEntropy(win.data(), w, stride);
                    classEntropyBytes += wb;
                    samples.push_back(ByteShuffle::Shuffle(win.data(), w, stride));
                }
            }
            size_t sampled = 0;
            for (const auto& s : samples) sampled += s.size();
            std::vector<std::vector<uint8_t>*> jobs;
            for (auto& s : samples) jobs.push_back(&s);
            for (auto& s : plainSamples) jobs.push_back(&s);
            PayloadCodec::CompressInPlace(jobs);
            size_t compressed = 0, compressedPlain = 0, compressedPaired = 0;
            for (const auto& s : samples) compressed += s.size();
            for (const auto& s : plainSamples) compressedPlain += s.size();
            for (size_t i : pairedSample) compressedPaired += samples[i].size();
            const double ratio = sampled > 0 ? (double)compressed / (double)sampled : 1.0;
            packageBytes += (size_t)((double)total * ratio);
            out.transformedMB += (float)((double)total / (1024.0 * 1024.0));
            if (sh) out.transformedSHMB += (float)((double)total / (1024.0 * 1024.0));
            if (source.splatMode && classEntropyBytes > 0.0 && classEntropyPlain > 0.0 && compressedPlain > 0)
            {
                ColourEstimate::Stream& s = sh ? out.colourEstimate.sh : out.colourEstimate.records;
                s.entropyChangePercent = 100.0 * (classEntropyYCoCg / classEntropyPlain - 1.0);
                const double plainOverYCoCg = (double)compressedPlain / (double)compressedPaired;
                s.sizeChangePercent = 100.0 * (1.0 / plainOverYCoCg - 1.0);
                plainEstimate += (double)total * ratio * plainOverYCoCg;
                entropyPlain += (double)total * classEntropyPlain / classEntropyBytes;
                entropyYCoCg += (double)total * classEntropyYCoCg / classEntropyBytes;
                out.colourEstimate.valid = true;
            }
            else
            {
                plainEstimate += (double)total * ratio;
            }
        }
        if (out.colourEstimate.valid)
        {
            out.colourEstimate.entropyChangePercent = 100.0 * (entropyYCoCg / entropyPlain - 1.0);
            out.colourEstimate.sizeChangePercent = 100.0 * ((double)packageBytes / plainEstimate - 1.0);
            LogTransitionTrace("Colour transform (YCoCg and SH residuals, sampled): entropy %+.1f%%, size %+.1f%% (records: entropy %+.1f%%, size %+.1f%%; SH: entropy %+.1f%%, size %+.1f%%)",
                out.colourEstimate.entropyChangePercent, out.colourEstimate.sizeChangePercent,
                out.colourEstimate.records.entropyChangePercent, out.colourEstimate.records.sizeChangePercent,
                out.colourEstimate.sh.entropyChangePercent, out.colourEstimate.sh.sizeChangePercent);
        }
        // Tiers 2-4 in bits per byte (EntropyProfile.h): a few runs of every level in stored order, the harmonics measured
        // from their absolute values against the next coarser level as a whole (the package codes them per chunk)
        if (source.splatMode)
        {
            const auto t0 = std::chrono::high_resolution_clock::now();
            constexpr size_t kRun = 16384;
            constexpr int kRunsPerLevel = 4;
            std::vector<EntropySample> samples;
            for (size_t li = 0; li < levels.size(); li++)
            {
                const size_t count = levels[li].splats.size();
                if (count == 0) continue;
                const int runs = (int)std::min<size_t>(kRunsPerLevel, (count + kRun - 1) / kRun);
                const size_t w = std::min(kRun, count);
                for (int j = 0; j < runs; j++)
                {
                    const size_t centre = (size_t)(((double)j + 0.5) * (double)count / (double)runs);
                    const size_t start = std::min(count - w, centre > w / 2 ? centre - w / 2 : (size_t)0);
                    EntropySample s;
                    s.records = reinterpret_cast<const uint8_t*>(levels[li].splats.data() + start);
                    s.count = w;
                    if (shStride > 0) s.sh = (shResidual ? shAbsolute[li].data() : levels[li].sh.data()) + start * shStride;
                    if (shResidual && li + 1 < levels.size())
                    {
                        s.parentRecords = reinterpret_cast<const uint8_t*>(levels[li + 1].splats.data());
                        s.parentCount = levels[li + 1].splats.size();
                        s.parentSH = &shAbsolute[li + 1];
                    }
                    s.weight = (double)count / (double)(w * runs);
                    samples.push_back(s);
                }
            }
            out.entropy = Entropy::Measure(samples, sizeof(PackedSplatGPU), shStride, source.shBits);
            out.entropy.seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
            LogTransitionTrace("Entropy (bits per byte): Tier 2 %.3f, Tier 3 %.3f, Tier 4 %.3f (records %.3f / %.3f / %.3f, SH %.3f / %.3f / %.3f) in %.2f s",
                out.entropy.tier2, out.entropy.tier3, out.entropy.tier4, out.entropy.records[0], out.entropy.records[1], out.entropy.records[2],
                out.entropy.sh[0], out.entropy.sh[1], out.entropy.sh[2], out.entropy.seconds);
        }
        out.compressedSizeMB = (float)packageBytes / (1024.0f * 1024.0f);
        LogTransitionTrace("Package size estimate: %.2f MB (%s, sampled) in %.2f s (%.2f s encoding every level, the rest compressing the sample)",
            out.compressedSizeMB, PayloadCodec::Name(PayloadCodec::kDefault),
            std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - tEstimateStart).count(), encodeSeconds);
        if (progress.Cancelled()) return false;

        progress.Stage("Preparing the levels", 0.48f);
        BuildResidentLODs(source, out.waveletResult, out.aabbMin, out.aabbMax, out.splatParams, out.residentLODs);
        if (progress.Cancelled()) return false;

        // A new streaming runtime, built here and swapped in whole by ApplyBake (see InitStreamingSimulation)
        progress.Stage("Building the stream", 0.78f);
        StreamRuntime& rt = out.stream;
        rt.settings = streamSettings;
        rt.camera = streamCamera;
        rt.m_levels = &out.residentLODs;
        rt.m_waveletLevels = (int)out.waveletResult.lodLevels.size();
        rt.m_aabbMin = out.aabbMin; rt.m_aabbMax = out.aabbMax; rt.m_center = out.center; rt.m_extents = out.extents;
        rt.m_lodSortFrameMin = out.aabbMin; rt.m_lodSortFrameMax = out.aabbMax;
        rt.m_detailGrid = out.detailGrid;
        rt.m_enableMortonOrder = m_enableMortonOrder;
        rt.m_splatMode = source.splatMode;
        rt.m_splatParams = out.splatParams;
        rt.m_bytesPerRenderPoint = source.splatMode ? (sizeof(PackedSplatGPU) + SplatCodec::SHRecordBytes(out.splatParams.shDegree))
                                                    : (m_enableQuantization ? sizeof(PackedPointGPU) : sizeof(SplatPoint));
        rt.InitStreamingSimulation();
        if (progress.Cancelled()) return false;

        if (params.occlusion)
        {
            progress.Stage("Occlusion volume", 0.92f);
            std::string trace;
            OcclusionVolume::BuildGrid(points, out.aabbMin, out.aabbMax, out.occlusionGrid, trace, 0, m_occlusionShaveBiasCells);
            if (!trace.empty()) LogTransitionTrace("%s", trace.c_str());
            if (progress.Cancelled()) return false;
            trace.clear();
            OcclusionVolume::Bake(out.occlusionGrid, 0.0f, OcclusionVolume::ColorGrade{}, out.occlusionVoxels, out.occlusionMips, trace);
            if (!trace.empty()) LogTransitionTrace("%s", trace.c_str());
        }

        progress.Stage("Heatmap", 0.99f);
        BuildHeatmapClusterCubes(points, out.aabbMin, out.aabbMax, out.detailGrid, out.heatmapCubes);
        progress.Fraction(1.0f);
        LogTransitionTrace("Bake stages: %s", progress.Timings().c_str());
        LogTransitionTrace("Bake: chunk %.3f m, %d levels, SH degree %u, deadband %.1f mm, occlusion %s: %zu chunks, %zu levels, %zu occluder blocks in %.2f s",
            params.chunkSize, params.maxLODs, out.splatParams.shDegree, params.deadbandMM, params.occlusion ? "on" : "off",
            out.chunks.size(), out.waveletResult.lodLevels.size(), out.occlusionVoxels.size(),
            std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - tBake).count());
        return !progress.Cancelled();
    }

    // Swaps a finished bake into the app and the renderer (UI thread).
    void SplatPakApp::ApplyBake(BakeResult& r, const BakeParams& params, bool fitCamera)
    {
        m_aabbMin = r.aabbMin; m_aabbMax = r.aabbMax; m_center = r.center; m_extents = r.extents;
        if (fitCamera)
        {
            ApplyFitDistance();      // Frame the whole model between the control panels
            m_fitViewPending = true; // Re-fit on the first frame, when the window is sized and the LOD points exist (tighter than the box)
            m_target = m_center;
            m_state.splatRadius = 1.0f;
            m_state.orientMode = 0;
        }
        m_detailGrid = std::move(r.detailGrid);
        m_chunks = std::move(r.chunks);
        m_waveletResult = std::move(r.waveletResult);
        m_splatParams = r.splatParams;
        // The level count may have shrunk: BuildUI and the LOD export index lodLevels[m_selectedPreviewLOD] directly
        m_selectedPreviewLOD = 0;
        m_compressedSizeMB = r.compressedSizeMB;
        m_transformedMB = r.transformedMB;
        m_transformedSHMB = r.transformedSHMB;
        m_colourEstimate = r.colourEstimate;
        m_entropy = r.entropy;
        m_compressionRatio = (m_rawFileSizeMB > 0.0f && m_compressedSizeMB > 0.0f) ? (m_rawFileSizeMB / std::max(0.001f, m_compressedSizeMB)) : 0.0f;
        m_deadbandZeroPercent = 64.5f; // Measured planar surface coefficient sparsification
        m_residentLODs = std::move(r.residentLODs);
        m_lodSortFrameMin = m_aabbMin;
        m_lodSortFrameMax = m_aabbMax;
        m_stream = std::move(r.stream);
        m_stream.m_levels = &m_residentLODs; // The runtime was built against the result's copy of the levels
        ComputeSplatSoftness();
        m_ringBufferCapacityMB = m_stream.settings.ringBufferCapacityMB;
        m_heatmapClusterCubes = std::move(r.heatmapCubes);
        m_occlusionGrid = std::move(r.occlusionGrid);
        m_occlusionVoxels = std::move(r.occlusionVoxels);
        m_occlusionMips = r.occlusionMips;
        m_enableOcclusionCulling = (params.occlusion && !m_occlusionVoxels.empty()) ? m_showOcclusionSetting : false;
        m_occlusionVoxelsVersion++;
        m_bakedParams = params;
        m_hasBakedParams = true;
        // Settings analysed before: the stored analysis (rdo-profiles) comes back with them
        if (!m_isLoadedFromSFLW && m_sourceHash != 0 && RdoStale())
        {
            RdoProfile stored;
            std::string path;
            if (FindRdoProfile(RdoProfileFolder(), m_sourceHash, params.maxLODs, params.deadbandMM, stored, path))
            {
                AdoptRdoAnalysis(stored);
                m_rdoSourcePath = m_loadedFilePath;
                m_lastRdoProfilePath = path;
                LogTransitionTrace("RDO: stored analysis %s matches the bake: restored", path.c_str());
            }
        }
        // The renderer still points at the old stream's buffers: give it the new model's before the next draw
        UpdatePreviewSplats();
        if (m_enableStreamingSimulation) UpdateStreamingSimulation(0.0);
        LogTransitionTrace("Bake applied: %zu chunks, %zu levels, %zu occluder blocks", m_chunks.size(), m_waveletResult.lodLevels.size(), m_occlusionVoxels.size());
        StartShadowMeshBuild();
    }

    // ---- Shadow mesh --------------------------------------------------------------------------------------

    // Builds the shadow mesh of the scan just baked on its own thread, so the bake is no slower. It reads m_rawSplats and
    // m_splatAttributes, which only an open, a package load, closing or the synthetic benchmark replace: each cancels it
    // first (CancelShadowMeshBuild). A save waits for it.
    void SplatPakApp::StartShadowMeshBuild()
    {
        CancelShadowMeshBuild();
        if (m_rawSplats.empty() || !m_splatMode)
        {
            SetShadowMesh(nullptr);
            return;
        }
        auto job = std::make_shared<ShadowMeshJob>();
        const XMFLOAT3 mn = m_aabbMin, mx = m_aabbMax;
        const std::vector<SplatAttributes>* attributes = m_splatAttributes.empty() ? nullptr : &m_splatAttributes;
        SetOccluderMesh(nullptr); // The bake's cubes cull until the new occluder mesh is ready
        auto voxels = std::make_shared<std::vector<OcclusionVoxelGPU>>(m_occlusionVoxels);
        const OcclusionMipTable mips = m_occlusionMips;
        job->thread = std::thread([this, job, mn, mx, attributes, voxels, mips]()
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto mesh = std::make_shared<ShadowMesh>();
            const bool ok = ShadowMeshBuilder::Build(m_rawSplats, attributes, mn, mx, *mesh, job->trace,
                                                     [job](float) { return !job->cancel.load(); });
            if (ok) job->result = mesh;
            auto occluder = std::make_shared<ShadowMesh>();
            if (ok && !job->cancel.load() && ShadowMeshBuilder::BuildOccluder(*voxels, mips, *occluder, job->trace)) job->occluder = occluder;
            job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            job->done = true;
        });
        m_shadowMeshJob = job;
    }

    void SplatPakApp::CancelShadowMeshBuild()
    {
        if (!m_shadowMeshJob) return;
        m_shadowMeshJob->cancel = true;
        if (m_shadowMeshJob->thread.joinable()) m_shadowMeshJob->thread.join();
        m_shadowMeshJob.reset();
    }

    // Once per frame: publishes a finished build
    void SplatPakApp::PollShadowMeshBuild()
    {
        if (!m_shadowMeshJob || !m_shadowMeshJob->done.load()) return;
        auto job = m_shadowMeshJob;
        if (job->thread.joinable()) job->thread.join();
        m_shadowMeshJob.reset();
        if (!job->trace.empty()) LogTransitionTrace("%s", job->trace.c_str());
        if (job->cancel.load()) return;
        SetShadowMesh(job->result);
        SetOccluderMesh(job->occluder);
        LogTransitionTrace("Shadow mesh: %s in %.2f s (built after the bake)", ShadowMeshSummary().c_str(), job->seconds);
    }

    void SplatPakApp::SetShadowMesh(std::shared_ptr<const ShadowMesh> mesh)
    {
        m_shadowMesh = std::move(mesh);
        m_shadowMeshLevel = 0;
        m_shadowMeshSerial++;
    }

    void SplatPakApp::ReleaseModelMeshes()
    {
        CancelShadowMeshBuild();
        SetShadowMesh(nullptr);
        SetOccluderMesh(nullptr);
    }

    void SplatPakApp::SetOccluderMesh(std::shared_ptr<const ShadowMesh> mesh)
    {
        m_occluderMesh = std::move(mesh);
        m_occluderMeshGpu = ShadowMeshDisplay{};
        if (HasOccluderMesh())
        {
            BuildShadowMeshDisplay(m_occluderMesh->levels[0], 0, m_occluderMeshGpu);
            if (m_hasBakedParams ? m_bakedParams.occlusion : true) m_enableOcclusionCulling = m_showOcclusionSetting;
        }
        m_occluderMeshVersion++;
    }

    std::string SplatPakApp::ShadowMeshSummary() const
    {
        if (m_shadowMeshJob) return "building";
        if (!m_shadowMesh || m_shadowMesh->Empty()) return "none";
        std::string s;
        for (size_t l = 0; l < m_shadowMesh->levels.size(); l++) s += (l ? " / " : "") + std::to_string(m_shadowMesh->levels[l].TriangleCount());
        return s + " triangles";
    }

    // ---- Long operations: the worker, the dialog ---------------------------------------------------------

    bool SplatPakApp::UseBackgroundOps() const
    {
        return m_framesRendered > 1 && m_lodCalFolder.empty() && m_lodTestFolder.empty() && !m_streamTest && m_cliBakeOutput.empty() && m_viewCapFolder.empty();
    }

    bool SplatPakApp::RunOperation(const std::string& title, BackgroundOp::Work work, BackgroundOp::Done done)
    {
        if (!UseBackgroundOps()) return BackgroundOp::RunNow(std::move(work), std::move(done));
        m_op.Start(title, std::move(work), std::move(done));
        return true;
    }

    // The modal progress dialog. It opens as soon as an operation starts so it blocks the UI at once, but stays
    // invisible (off screen, no dimming) until the operation has run for a second, so quick ones never flash it.
    void SplatPakApp::DrawOperationDialog()
    {
        static const char* kPopup = "##Operation";
        if (!m_op.Busy() && !ImGui::IsPopupOpen(kPopup)) return;
        const bool visible = m_op.DialogVisible();
        if (m_op.Busy() && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDarkening, visible ? ImVec4(0.0f, 0.0f, 0.0f, 0.45f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (visible)
            ImGui::SetNextWindowPos(ImVec2((float)m_Width * 0.5f, (float)m_Height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        else
            ImGui::SetNextWindowPos(ImVec2(-10000.0f, -10000.0f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        {
            if (!m_op.Busy())
            {
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ImGui::Text("%s", m_op.Title().c_str());
                ImGui::TextDisabled("%s", m_op.StageName().c_str());
                const float barWidth = 380.0f * m_uiScale;
                {
                    char pct[16];
                    snprintf(pct, sizeof(pct), "%.0f%%", m_op.Fraction() * 100.0f);
                    ProgressBarCentred(m_op.Fraction(), ImVec2(barWidth, 0.0f), pct);
                }
                ImGui::Dummy(ImVec2(0.0f, 10.0f * m_uiScale));
                const float buttonWidth = 120.0f * m_uiScale;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + barWidth - buttonWidth);
                if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f))) m_op.RequestCancel();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
    }

    // Pre-quantizes and pre-chunks every LOD level up front, for instant hitch-free LOD switching later
    void SplatPakApp::PrecacheResidentLODs()
    {
        m_residentLODs.clear();
        if (m_rawSplats.empty() && m_rendererRawSplats.empty()) return;
        // Every level is sorted in one spatial order over the model's bounds, so a region sits at the same place
        // in the order at every level (the streaming runtime's block hierarchy relies on it)
        m_lodSortFrameMin = m_aabbMin;
        m_lodSortFrameMax = m_aabbMax;
        BakeSource source = LoadedBakeSource();
        if (m_rawSplats.empty()) source.points = &m_rendererRawSplats;
        BuildResidentLODs(source, m_waveletResult, m_aabbMin, m_aabbMax, m_splatParams, m_residentLODs);
    }

    // Every level of a source as resident, pre-quantized and pre-chunked render data (also used on the worker by a bake)
    void SplatPakApp::BuildResidentLODs(const BakeSource& source, const WaveletDecompositionResult& wavelet, const XMFLOAT3& aabbMin,
                                        const XMFLOAT3& aabbMax, const SplatEncodeParams& splatParams, std::vector<ResidentLOD>& out) const
    {
        out.clear();
        const auto& points = *source.points;
        if (points.empty()) return;
        const uint8_t* shBits = source.shBits;
        if (m_enableWavelet && !wavelet.lodLevels.empty())
        {
            int numLODs = (int)wavelet.lodLevels.size();
            out.resize(numLODs);

            for (int lodIdx = 0; lodIdx < numLODs; lodIdx++)
            {
                std::vector<SplatPoint> lodPoints;

                if (m_cascadeLOD)
                {
                    if (lodIdx == 0)
                    {
                        lodPoints = points;
                    }
                    else
                    {
                        for (int lvl = numLODs - 1; lvl >= lodIdx; lvl--)
                        {
                            const auto& levelData = wavelet.lodLevels[lvl];
                            lodPoints.insert(lodPoints.end(), levelData.splats.begin(), levelData.splats.end());
                        }
                    }
                }
                else
                {
                    lodPoints = wavelet.lodLevels[lodIdx].splats;
                }

                if (lodPoints.empty() && !points.empty())
                {
                    lodPoints = points;
                }

                out[lodIdx].rawSplats = std::move(lodPoints);
                SpatialOctree::PartitionIntoMeshletChunks(out[lodIdx].rawSplats, out[lodIdx].meshletChunks, 64, m_enableMortonOrder, &aabbMin, &aabbMax);
                for (auto& mc : out[lodIdx].meshletChunks) mc.lodLevel = (uint32_t)lodIdx; // The shader draws by level (splat mode: discs from kSplatDiscMinLevel), as the stream does
                out[lodIdx].packedPoints = Quantizer::QuantizePoints(out[lodIdx].rawSplats, aabbMin, aabbMax);
                if (source.splatMode)
                {
                    SplatCodec::Encode(out[lodIdx].rawSplats, *source.attributes, (uint32_t)lodIdx, splatParams, out[lodIdx].splats, out[lodIdx].splatSH, &wavelet.mergedAttributes);
                    RequantiseSH(out[lodIdx].splatSH.data(), out[lodIdx].splats.size(), SplatCodec::SHRecordBytes(splatParams.shDegree), shBits); // Draw what the package will hold
                    if (!m_splatDrawAsDiscs) for (auto& mc : out[lodIdx].meshletChunks) mc.dilationMorph = 1.0f; // Resident at load, sorted Gaussians: Gaussians at once (level 0 fade-in weight, see AppendChunkToRenderer); drawn as splats: the splat value (0)
                }
            }
        }
        else
        {
            out.resize(1);
            out[0].rawSplats = points;
            SpatialOctree::PartitionIntoMeshletChunks(out[0].rawSplats, out[0].meshletChunks, 64, m_enableMortonOrder, &aabbMin, &aabbMax);
            out[0].packedPoints = Quantizer::QuantizePoints(out[0].rawSplats, aabbMin, aabbMax);
            if (source.splatMode)
            {
                SplatCodec::Encode(out[0].rawSplats, *source.attributes, 0, splatParams, out[0].splats, out[0].splatSH);
                RequantiseSH(out[0].splatSH.data(), out[0].splats.size(), SplatCodec::SHRecordBytes(splatParams.shDegree), shBits);
                if (!m_splatDrawAsDiscs) for (auto& mc : out[0].meshletChunks) mc.dilationMorph = 1.0f; // Resident at load, sorted Gaussians: Gaussians at once (level 0 fade-in weight, see AppendChunkToRenderer); drawn as splats: the splat value (0)
            }
        }
    }

    // ---- Streaming runtime host (libs/gstx/StreamRuntime.h) -------------------------------------

    void SplatPakApp::SyncStreamInputs()
    {
        StreamSettings& s = m_stream.settings;
        s.autoLOD = m_autoLOD; s.selectedPreviewLOD = m_selectedPreviewLOD;
        s.lodScreenSpace = m_lodScreenSpace; s.lodPixelSpacing = m_lodPixelSpacing; s.lodPerBlock = m_lodPerBlock;
        s.enableStreamingSimulation = m_enableStreamingSimulation; s.isStreamingPaused = m_isStreamingPaused;
        s.unthrottledBandwidth = m_unthrottledBandwidth; s.bandwidthThrottleMBps = m_bandwidthThrottleMBps; s.ringBufferCapacityMB = m_ringBufferCapacityMB;
        s.enableStreamDecay = m_enableStreamDecay; s.streamDecayRate = m_streamDecayRate; s.demoteChunks = m_demoteChunks; s.releaseUnusedSegments = m_releaseUnusedSegments;
        s.streamingPolicy = m_streamingPolicy == 0 ? StreamSettings::Policy::Conservative : StreamSettings::Policy::Greedy;
        s.enableDitheredTransitions = m_enableDitheredTransitions; s.ditherTransitionDurationSec = m_ditherTransitionDurationSec;
        s.prioritizeFrustumAndProximity = m_prioritizeFrustumAndProximity; s.neighborBufferMargin = m_neighborBufferMargin;
        s.enableSilhouetteLOD0 = m_enableSilhouetteLOD0; s.highlightSilhouetteChunks = m_highlightSilhouetteChunks;
        s.silhouetteLODBias = m_silhouetteLODBias; s.silhouetteThreshold = m_silhouetteThreshold; s.dilationMorphAmount = m_dilationMorphAmount;
        s.showChunkStream = m_showChunkStream; s.chunkStreamDuration = m_chunkStreamDuration; s.splatDrawAsDiscs = m_splatDrawAsDiscs;
        StreamCamera& c = m_stream.camera;
        c.pitch = m_pitch; c.yaw = m_yaw; c.distance = m_distance; c.target = m_target;
        c.detachCamera = m_detachCamera; c.detachedPitch = m_detachedPitch; c.detachedYaw = m_detachedYaw;
        c.detachedDistance = m_detachedDistance; c.detachedTarget = m_detachedTarget;
        c.width = m_Width; c.height = m_Height;
    }

    void SplatPakApp::ApplyStreamFrame()
    {
        if (!m_stream.m_frameProduced) return;
        m_frameTraversalMs = m_stream.m_frameTraversalMs;
        m_state.splatCount = (uint32_t)(m_splatMode ? m_stream.m_unifiedSplats.size() : m_enableQuantization ? (!m_stream.m_unifiedPackedSplats.empty() ? m_stream.m_unifiedPackedSplats.size() : m_rendererSplats.size()) : (!m_stream.m_unifiedRawSplats.empty() ? m_stream.m_unifiedRawSplats.size() : m_rendererRawSplats.data() ? m_rendererRawSplats.size() : 0));
        m_state.chunkCount = m_stream.FrameChunkCount();
        m_state.pPoints = !m_stream.m_unifiedPackedSplats.empty() ? m_stream.m_unifiedPackedSplats.data() : m_rendererSplats.data();
        m_state.pRawSplats = !m_stream.m_unifiedRawSplats.empty() ? m_stream.m_unifiedRawSplats.data() : m_rendererRawSplats.data();
        m_state.pSplats = m_stream.m_unifiedSplats.empty() ? nullptr : m_stream.m_unifiedSplats.data();
        m_state.pSplatSH = m_stream.m_unifiedSH.empty() ? nullptr : m_stream.m_unifiedSH.data();
        m_state.pChunks = m_stream.m_rendererMeshletChunks.data();
        m_state.useChunkedPipeline = (m_state.chunkCount > 0);
    }

    void SplatPakApp::InitStreamingSimulation()
    {
        if (m_residentLODs.empty()) PrecacheResidentLODs();
        m_stream.m_levels = &m_residentLODs;
        m_stream.m_waveletLevels = (int)m_waveletResult.lodLevels.size();
        m_stream.m_aabbMin = m_aabbMin; m_stream.m_aabbMax = m_aabbMax; m_stream.m_center = m_center; m_stream.m_extents = m_extents;
        m_stream.m_lodSortFrameMin = m_lodSortFrameMin; m_stream.m_lodSortFrameMax = m_lodSortFrameMax;
        m_stream.m_detailGrid = m_detailGrid;
        m_stream.m_enableMortonOrder = m_enableMortonOrder;
        m_stream.m_splatMode = m_splatMode;
        m_stream.m_splatParams = m_splatParams;
        m_stream.m_bytesPerRenderPoint = BytesPerRenderPoint();
        SyncStreamInputs();
        m_stream.InitStreamingSimulation();
        ComputeSplatSoftness();
        m_ringBufferCapacityMB = m_stream.settings.ringBufferCapacityMB; // The runtime defaults the ring buffer to its slider ceiling
    }

    void SplatPakApp::ResetStreamingSimulation() { SyncStreamInputs(); m_stream.ResetStreamingSimulation(); ApplyStreamFrame(); }
    void SplatPakApp::ClearResidentStream() { SyncStreamInputs(); m_stream.ClearResidentStream(); ApplyStreamFrame(); }
    void SplatPakApp::UpdateStreamingSimulation(double dtSeconds) { SyncStreamInputs(); m_stream.UpdateStreamingSimulation(dtSeconds); ApplyStreamFrame(); }
    void SplatPakApp::TriggerSilhouetteEdgeMorphTest() { SyncStreamInputs(); m_stream.TriggerSilhouetteEdgeMorphTest(); }
    void SplatPakApp::CalibrateDecay() { SyncStreamInputs(); m_stream.CalibrateDecay(); }

    // Highest spherical-harmonics band with a non-zero coefficient in any source Gaussian (bands 1, 2, 3 are
    // coefficients 0-2, 3-7 and 8-14, three channels each), over a sample of up to 200k of them: a 3DGS file
    // without f_rest carries none, a degree-1 export only band 1.
    uint32_t SplatPakApp::SourceSHDegreeOf(const std::vector<SplatAttributes>& attributes)
    {
        uint32_t degree = 0;
        const size_t n = attributes.size();
        const size_t stride = std::max<size_t>(1, n / 200000);
        for (size_t i = 0; i < n && degree < 3; i += stride)
        {
            const float* sh = attributes[i].sh;
            for (int k = 14; k >= 0; k--)
            {
                if (sh[3 * k] != 0.0f || sh[3 * k + 1] != 0.0f || sh[3 * k + 2] != 0.0f)
                {
                    degree = std::max(degree, k < 3 ? 1u : (k < 8 ? 2u : 3u));
                    break;
                }
            }
        }
        return degree;
    }

    void SplatPakApp::DetectSourceSHDegree()
    {
        m_sourceSHDegree = SourceSHDegreeOf(m_splatAttributes);
        LogTransitionTrace("Source spherical harmonics: degree %u (SH Quality keeps degree %u)", m_sourceSHDegree, BakeSHDegree());
    }

    // Splat mode: one level's records and SH stream from its points and the source Gaussians.
    void SplatPakApp::EncodeSplatLevel(const std::vector<SplatPoint>& splats, uint32_t level, std::vector<PackedSplatGPU>& outRecords, std::vector<uint8_t>& outSH) const
    {
        SplatCodec::Encode(splats, m_splatAttributes, level, m_splatParams, outRecords, outSH, &m_waveletResult.mergedAttributes);
    }

    // Splat mode: the renderer state that does not depend on which level or stream is active.
    void SplatPakApp::ApplySplatRenderState()
    {
        m_state.shDegree = m_splatParams.shDegree;
        m_state.shRecordBytes = SplatCodec::SHRecordBytes(m_splatParams.shDegree);
        m_state.splatScaleLog2Min = m_splatParams.scaleLog2Min;
        m_state.splatScaleLog2Max = m_splatParams.scaleLog2Max;
        m_state.splatBlendSpace = (uint32_t)m_splatBlendSpace;
    }

    // Refreshes the CPU-side preview buffer for the currently selected LOD level
    void SplatPakApp::UpdatePreviewSplats()
    {
        LogTransitionTrace("UpdatePreviewSplats: m_selectedPreviewLOD=%d, m_autoLOD=%d, isStreaming=%d",
            m_selectedPreviewLOD, m_autoLOD ? 1 : 0, m_enableStreamingSimulation ? 1 : 0);

        if (m_rawSplats.empty() && m_rendererRawSplats.empty()) return;

        if (m_enableStreamingSimulation)
        {
            m_stream.m_streamStateDirty = true;
            return;
        }

        if (m_residentLODs.empty())
        {
            PrecacheResidentLODs();
        }

        m_stream.m_lastStreamCamPos = { 1e9f, 1e9f, 1e9f };
        m_stream.m_streamStateDirty = true;

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
            m_state.renderMode = SplatRenderMode(); // Splat mode: 20-byte Gaussians, as splats (4) or sorted Gaussians (3)
            ApplySplatRenderState();
            m_state.pSplats = resident.splats.data();
            m_state.pSplatSH = resident.splatSH.empty() ? nullptr : resident.splatSH.data();
            m_state.pPoints = nullptr;
            m_state.pRawSplats = nullptr;
            m_state.splatCount = (uint32_t)resident.splats.size();
        }
        else if (m_enableQuantization)
        {
            m_state.renderMode = 1; // Quantized 8-byte GPU stream
            m_state.pPoints = resident.packedPoints.data();
            m_state.pRawSplats = nullptr;
            m_state.splatCount = (uint32_t)resident.packedPoints.size();
        }
        else
        {
            m_state.renderMode = 2; // Raw Float32 direct stream
            m_state.pRawSplats = resident.rawSplats.data();
            m_state.pPoints = nullptr;
            m_state.splatCount = (uint32_t)resident.rawSplats.size();
        }
    }







    // Runs the full wavelet pipeline over m_chunks and writes the resulting single-file .gstx package
    // Writes the displayed model as a single-file .gstx package, on the worker: into <name>.partial.gstx, renamed
    // when complete, so a cancelled or failed save never leaves a broken package behind.
    void SplatPakApp::ProcessAndExport(const std::string& outputPath)
    {
        if (m_rawSplats.empty()) return;
        if (BakePending())
        {
            m_statusMessage = "Bake your changes first: a package is written from the displayed model.";
            m_statusIsSuccess = false;
            return;
        }
        const std::string partial = outputPath + ".partial";
        const std::string name = outputPath.substr(outputPath.find_last_of("/\\") + 1) + ".gstx";
        m_statusMessage = "Saving " + name + "...";
        auto splatInput = std::make_shared<SplatBakeInput>();
        splatInput->attributes = &m_splatAttributes;
        splatInput->shDegree = BakeSHDegree();
        memcpy(splatInput->shBits, m_bakedParams.shBandBits, 3); // What the displayed model was baked with
        auto transformStats = std::make_shared<TransformStats>();
        auto meshJob = m_shadowMeshJob;
        auto meshHeld = std::make_shared<std::shared_ptr<const ShadowMesh>>(m_shadowMesh);
        auto occluderHeld = std::make_shared<std::shared_ptr<const ShadowMesh>>(m_occluderMesh);
        RunOperation("Saving " + name, [this, partial, splatInput, transformStats, meshJob, meshHeld, occluderHeld](Progress& progress)
        {
            // A shadow mesh still being built after the bake goes into this package too: wait for it
            if (meshJob)
            {
                progress.Stage("Finishing the shadow mesh", 0.0f);
                while (!meshJob->done.load())
                {
                    if (progress.Cancelled()) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                *meshHeld = meshJob->result;
                *occluderHeld = meshJob->occluder;
            }
            // With an occluder mesh (format 13) the package carries no voxel volume
            const bool occluderMesh = *occluderHeld && !(*occluderHeld)->Empty();
            const std::vector<OcclusionVoxelGPU> noVoxels;
            const OcclusionMipTable noMips;
            progress.Stage("Encoding the levels", 0.0f);
            return StreamPackager::PackageDataset(partial, m_chunks, m_maxLODLevels, m_deadbandThresholdMM / 1000.0f, m_state.splatRadius,
                occluderMesh ? noVoxels : m_occlusionVoxels, m_sourceFileBytes, occluderMesh ? &noMips : &m_occlusionMips, &m_detailGrid, m_splatMode ? splatInput.get() : nullptr, PayloadCodec::kDefault,
                [&progress, transformStats](float fraction)
                {
                    // The packager's stages by their share of the bar (StreamPackager.h): encoding, Brotli, writing.
                    // The transform figures are complete once encoding is (the packager fills them before compressing).
                    std::string stage = fraction < 0.03f ? "Encoding the levels" : fraction < 0.97f ? "Compressing the payloads" : "Writing the package";
                    if (fraction >= 0.03f && fraction < 0.97f && transformStats->shBytes + transformStats->recordBytes > 0.0)
                    {
                        char buf[64];
                        snprintf(buf, sizeof(buf), " (YCoCg: entropy %+.1f%%)", transformStats->TotalChangePercent());
                        stage += buf;
                    }
                    if (progress.StageName() != stage) progress.Stage(stage, fraction);
                    progress.Fraction(fraction);
                    return !progress.Cancelled();
                }, PAYLOAD_TRANSFORMS_DEFAULT, transformStats.get(), meshHeld->get(), occluderHeld->get());
        },
        [this, outputPath, partial, transformStats, meshHeld](bool ok, bool cancelled)
        {
            const std::string finalPath = outputPath + ".gstx", partialPath = partial + ".gstx";
            if (transformStats->recordBytes > 0.0)
                LogTransitionTrace("Save: colour transform YCoCg, entropy %+.1f%% (records %.3f -> %.3f, SH %.3f -> %.3f bits/byte)",
                    transformStats->TotalChangePercent(), transformStats->recordEntropyBefore, transformStats->recordEntropyAfter,
                    transformStats->shEntropyBefore, transformStats->shEntropyAfter);
            if (!ok)
            {
                if (!cancelled) std::remove(partialPath.c_str()); // A cancelled writer deletes its own file when it stops
                m_statusMessage = cancelled ? "Save cancelled: no package was written." : "Error: Failed to export stream package.";
                m_statusIsSuccess = false;
                return;
            }
            std::remove(finalPath.c_str());
            if (std::rename(partialPath.c_str(), finalPath.c_str()) != 0)
            {
                std::remove(partialPath.c_str());
                m_statusMessage = "Error: could not write " + finalPath + ".";
                m_statusIsSuccess = false;
                return;
            }
            // The analysis behind this encoding goes into the package (PackageRdoFooter): a fresh one of this scan, or the one
            // the package it was re-saved from carried
            if (m_rdo.Valid() && (!RdoStale() || (!m_rdoPackagePath.empty() && m_rdoParams.maxLODs == m_bakedParams.maxLODs)))
            {
                RdoProfile p = MakeRdoProfile();
                p.choice.shDegree = BakeSHDegree();
                memcpy(p.choice.shBits, m_bakedParams.shBandBits, 3);
                const bool embedded = PackageRdoFooter::Append(finalPath, p.Text());
                LogTransitionTrace("Save: RDO profile %s the package", embedded ? "embedded in" : "could not be embedded in");
            }
            std::ifstream pkg(finalPath, std::ios::ate | std::ios::binary);
            if (pkg.is_open())
            {
                m_compressedSizeMB = (float)(uint64_t)pkg.tellg() / (1024.0f * 1024.0f);
                m_compressionRatio = (m_rawFileSizeMB > 0.0f && m_compressedSizeMB > 0.0f) ? (m_rawFileSizeMB / std::max(0.001f, m_compressedSizeMB)) : 0.0f;
            }
            m_packagePath = finalPath;
            m_statusMessage = "Success! Created " + finalPath + " (" + std::to_string(m_compressedSizeMB) + " MB, " + std::to_string(m_compressionRatio) + "x vs source";
            if (*meshHeld && !(*meshHeld)->Empty())
            {
                char mesh[96];
                snprintf(mesh, sizeof(mesh), "; shadow mesh %zu / %zu / %zu triangles", (*meshHeld)->levels[0].TriangleCount(),
                         (*meshHeld)->levels.size() > 1 ? (*meshHeld)->levels[1].TriangleCount() : 0, (*meshHeld)->levels.size() > 2 ? (*meshHeld)->levels[2].TriangleCount() : 0);
                m_statusMessage += mesh;
            }
            m_statusMessage += ")";
            m_statusIsSuccess = true;
        });
    }

    // Executes whatever file-dialog/action was queued this frame (kept off the ImGui callback stack)
    void SplatPakApp::ExecutePendingAction()
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
            std::string file = OpenFileDialog("GSTx Package (*.gstx)\0*.gstx\0", "Open Compressed Model (.gstx)", "gstx");
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
            std::string savePath = SaveFileDialog("GSTx Package (*.gstx)\0*.gstx\0", "gstx", "Save Compressed Package (.gstx)");
            if (!savePath.empty())
            {
                if (savePath.size() > 5 && savePath.substr(savePath.size() - 5) == ".gstx")
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
                if (SyntheticGenerator::ExportToPLY(savePath, curLOD.splats))
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
                if (SPLATLoader::ExportToSPLAT(savePath, curLOD.splats))
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
            CancelShadowMeshBuild();
            SetShadowMesh(nullptr);
            SetOccluderMesh(nullptr);
            m_rawSplats.clear();
            m_chunks.clear();
            m_rendererSplats.clear();
            m_rendererRawSplats.clear();
            m_stream.m_rendererMeshletChunks.clear();
            m_rendererOctreeChunks.clear();
            m_waveletResult = WaveletDecompositionResult();
            m_loadedFilePath = "No dataset loaded";
            m_rendererSourceDescription = "No model active";
            m_isLoadedFromSFLW = false;
            m_rawFileSizeMB = 0.0f;
            m_compressedSizeMB = 0.0f; m_transformedMB = m_transformedSHMB = 0.0f;
            m_compressionRatio = 0.0f;
            m_deadbandZeroPercent = 0.0f;
            m_state.splatCount = 0;
            m_state.chunkCount = 0;
            m_state.pPoints = nullptr;
            m_state.pRawSplats = nullptr;
            m_state.pChunks = nullptr;
            if (m_pRenderer) m_pRenderer->FlushGPU();
            m_statusMessage = "Dataset closed.";
            m_statusIsSuccess = true;
            break;
        }
        case PendingAction::LocateReencodeSource:
        {
            std::string file = OpenFileDialog("3D Point Clouds & Splats (*.ply; *.splat)\0*.ply;*.splat\0All Files (*.*)\0*.*\0", "Locate the Package's Source Scan", "ply");
            if (!file.empty())
            {
                m_reencodeSource = file;
                m_packageSourceFor = m_packagePath; // Remembered for this package
                m_packageSource = file;
                StartReencodeProfilePlan();
            }
            break;
        }
        case PendingAction::ReencodeSaveAs:
        {
            std::string savePath = SaveFileDialog("GSTx Package (*.gstx)\0*.gstx\0", "gstx", "Re-encode To");
            if (savePath.size() > 5 && savePath.substr(savePath.size() - 5) == ".gstx") savePath.resize(savePath.size() - 5);
            if (!savePath.empty()) m_reencodeOutput = savePath;
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








    void SplatPakApp::UpdateCamera(const ImGuiIO& io)
    {
        SyncStreamInputs(); // The auto LOD below asks the runtime's LOD rule, which reads the settings and the viewport
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
                // A drag works on the inset where it is drawn (a panel may have displaced it from its home), and where it is
                // left becomes its home
                if (m_loupeDrag & (kLoupeDragMove | kLoupeDragLeft | kLoupeDragTop)) m_loupePos = ImVec2(m_loupeRect.x, m_loupeRect.y);
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

        // A scripted run (--lod-calibration, --stream-test) owns the camera: a key or a mouse wheel reaching
        // the window from someone working beside it moved the camera mid-measurement and spoiled the run
        const bool scriptedCamera = !m_lodCalFolder.empty() || m_streamTest;

        // Keyboard Controls: Arrow Keys (Shift=Pan, No Shift=Rotate), W/S/PageUp/PageDown Zoom
        if (!io.WantCaptureKeyboard && !scriptedCamera)
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
                const float turn = ((GetKeyState(VK_RIGHT) & 0x8000) ? keyRotSpeed : 0.0f) - ((GetKeyState(VK_LEFT) & 0x8000) ? keyRotSpeed : 0.0f);
                const float tilt = ((GetKeyState(VK_UP) & 0x8000) ? keyRotSpeed : 0.0f) - ((GetKeyState(VK_DOWN) & 0x8000) ? keyRotSpeed : 0.0f);
                // Detach Camera (culling view): the arrow keys turn the model only, never the scene view. Left/Right turn it
                // about the vertical axis through its centre, Up/Down tilt it about the observer's horizontal axis through
                // its centre. Turning the model is done as the opposite turn of both cameras about that centre, so on screen
                // the frustum stays put, the model turns, and what the frozen camera culls follows.
                if (m_detachCamera && (turn != 0.0f || tilt != 0.0f))
                {
                    auto turnCamera = [&](XMFLOAT3& target, float& yaw, float& pitch, FXMVECTOR axis, float angle)
                    {
                        const XMVECTOR q = XMQuaternionRotationAxis(axis, angle);
                        const XMVECTOR c = XMLoadFloat3(&m_center);
                        const XMVECTOR offset = XMVectorSet(cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw), 0.0f);
                        XMStoreFloat3(&target, XMVectorAdd(c, XMVector3Rotate(XMVectorSubtract(XMLoadFloat3(&target), c), q)));
                        XMFLOAT3 o;
                        XMStoreFloat3(&o, XMVector3Normalize(XMVector3Rotate(offset, q)));
                        pitch = std::clamp(asinf(std::clamp(o.y, -1.0f, 1.0f)), -1.55f, 1.55f);
                        if (fabsf(o.x) + fabsf(o.z) > 1e-5f) yaw = atan2f(o.x, o.z);
                    };
                    const XMVECTOR vertical = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                    const XMVECTOR across = XMVectorSet(cosf(m_yaw), 0.0f, -sinf(m_yaw), 0.0f); // The observer's right, horizontal
                    if (turn != 0.0f)
                    {
                        turnCamera(m_target, m_yaw, m_pitch, vertical, turn);
                        turnCamera(m_detachedTarget, m_detachedYaw, m_detachedPitch, vertical, turn);
                    }
                    if (tilt != 0.0f)
                    {
                        turnCamera(m_target, m_yaw, m_pitch, across, -tilt);
                        turnCamera(m_detachedTarget, m_detachedYaw, m_detachedPitch, across, -tilt);
                    }
                }
                else
                {
                    m_yaw += turn;
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
                if (((GetKeyState(VK_UP) & 0x8000) && !m_detachCamera) || wZooms || (GetKeyState(VK_PRIOR) & 0x8000) || (GetKeyState(VK_ADD) & 0x8000) || (GetKeyState(VK_OEM_PLUS) & 0x8000))
                {
                    m_distance *= expf(-zoomRate * dtSeconds); // Smooth fast Zoom In
                }
                if (((GetKeyState(VK_DOWN) & 0x8000) && !m_detachCamera) || sZooms || (GetKeyState(VK_NEXT) & 0x8000) || (GetKeyState(VK_SUBTRACT) & 0x8000) || (GetKeyState(VK_OEM_MINUS) & 0x8000))
                {
                    m_distance *= expf(+zoomRate * dtSeconds); // Smooth fast Zoom Out
                }
            }

            m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
        }

        if (!io.WantCaptureMouse && m_loupeDrag == 0 && !scriptedCamera)
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
                const bool screenSpace = m_lodScreenSpace && (int)m_stream.m_lodSpacing.size() > maxLODIndex && maxLODIndex > 0 && m_stream.m_lodSpacing[0] > 0.0f;
                const float targetFloatLOD = m_stream.ContinuousLodAtDistance(lodDistance, screenSpace);

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
                        UpdatePreviewSplats();
                    }
                    else
                    {
                        m_stream.m_streamStateDirty = true;
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

        uint32_t totalBasePoints = (uint32_t)(!m_residentLODs.empty() ? m_residentLODs[0].rawSplats.size() : (!m_rawSplats.empty() ? m_rawSplats.size() : m_rendererSplats.size()));
        uint32_t totalBaseChunks = (uint32_t)(!m_residentLODs.empty() ? m_residentLODs[0].meshletChunks.size() : (!m_chunks.empty() ? m_chunks.size() : m_stream.m_rendererMeshletChunks.size()));
        m_state.totalDatasetSplats = totalBasePoints;
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
                m_state.pSplats = m_stream.m_unifiedSplats.empty() ? nullptr : m_stream.m_unifiedSplats.data();
                m_state.pSplatSH = m_stream.m_unifiedSH.empty() ? nullptr : m_stream.m_unifiedSH.data();
                m_state.pPoints = nullptr;
                m_state.pRawSplats = nullptr;
                m_state.splatCount = (uint32_t)m_stream.m_unifiedSplats.size();
            }
            else if (m_enableQuantization)
            {
                m_state.renderMode = 1;
                m_state.pPoints = !m_stream.m_unifiedPackedSplats.empty() ? m_stream.m_unifiedPackedSplats.data() : m_rendererSplats.data();
                m_state.pRawSplats = nullptr;
                m_state.splatCount = (uint32_t)(!m_stream.m_unifiedPackedSplats.empty() ? m_stream.m_unifiedPackedSplats.size() : m_rendererSplats.size());
            }
            else
            {
                m_state.renderMode = 2;
                m_state.pRawSplats = !m_stream.m_unifiedRawSplats.empty() ? m_stream.m_unifiedRawSplats.data() : m_rendererRawSplats.data();
                m_state.pPoints = nullptr;
                m_state.splatCount = (uint32_t)(!m_stream.m_unifiedRawSplats.empty() ? m_stream.m_unifiedRawSplats.size() : m_rendererRawSplats.size());
            }
            m_state.pChunks = m_stream.m_rendererMeshletChunks.data();
            m_state.chunkCount = (uint32_t)m_stream.m_rendererMeshletChunks.size();
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
                    m_state.pPoints = nullptr;
                    m_state.pRawSplats = nullptr;
                    m_state.splatCount = (uint32_t)resident.splats.size();
                }
                else if (m_enableQuantization)
                {
                    m_state.renderMode = 1;
                    m_state.pPoints = resident.packedPoints.data();
                    m_state.pRawSplats = nullptr;
                    m_state.splatCount = (uint32_t)resident.packedPoints.size();
                }
                else
                {
                    m_state.renderMode = 2;
                    m_state.pRawSplats = resident.rawSplats.data();
                    m_state.pPoints = nullptr;
                    m_state.splatCount = (uint32_t)resident.rawSplats.size();
                }
            }
        }
    }

    // The entire ImGui frame: the left tool tabs (Generator/Renderer/Streaming) and the right statistics panel
    void SplatPakApp::BuildUI()
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
                if (ImGui::MenuItem("Open Compressed Model... (.gstx)", IsViewerOnly() ? "Ctrl+O" : "Ctrl+L"))
                {
                    m_pendingAction = PendingAction::OpenCompressedFile;
                }
                if (!IsViewerOnly() && ImGui::MenuItem("Generate Synthetic Benchmark", "Ctrl+G"))
                {
                    m_pendingAction = PendingAction::GenerateBenchmark;
                }

                ImGui::Separator();
                bool hasModel = !m_rawSplats.empty();

                if (!IsViewerOnly() && ImGui::MenuItem("Save Compressed Package (.gstx)...", "Ctrl+S", false, hasModel && !BakePending()))
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
                if (ImGui::MenuItem(IsViewerOnly() ? "About Splat Viewer..." : "About Splat Generator..."))
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
        // Both side panels fold to their title bar with the arrow on it (remembered: left_panel_collapsed,
        // right_panel_collapsed); a folded panel's rectangle is just that bar, so the loupe's box and inset,
        // which keep out of the panels' rectangles, get the freed space.
        ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(leftPanelWidth, panelHeight), ImGuiCond_Always);
        SetNextSidePanelCollapse(m_leftPanelRect, m_leftPanelCollapsed);
        ImGui::Begin("Model Menu###LeftPanel", nullptr, 0);

        // Tab Selector Buttons. Studio shows the three numbered tabs; the viewer has no generator, so
        // it shows Renderer and Streaming, unnumbered, across the same width.
        {
            struct TabDef { const char* label; int index; };
            const TabDef studioTabs[] = { { "Package/Bake", 0 }, { "Renderer", 1 }, { "Streaming", 2 } };
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
                if (ImGui::Button(tabs[t].label, ImVec2(tabWidth, 28)) && m_activeTab != tabs[t].index)
                {
                    m_activeTab = tabs[t].index;
                    SaveConfigFile(); // Remembered (active_tab)
                }
                ImGui::PopStyleColor(2);
            }
        }

        ImGui::Separator();

        // =========================================================================
        // TAB 1: GSTX BAKE (Raw Model -> Octree -> Wavelet Decimation -> GSTx Export)
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
            if (ImGui::IsItemHovered()) ShowTooltip("Loads the configured benchmark dataset (default: %s) or generates synthetic points.", m_benchmarkDatasetPath.c_str());

            if (!m_rawSplats.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Save Compressed Model:");
                const bool savePending = BakePending();
                if (savePending) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
                if (ImGui::Button("Save Compressed Package (.gstx)...", ImVec2(-1, 28)) && !savePending)
                {
                    m_pendingAction = PendingAction::ExportStream;
                }
                if (savePending) ImGui::PopStyleVar();
                if (ImGui::IsItemHovered()) ShowTooltip(savePending ? "Bake your changes first" : "Compresses and saves the displayed model, with its occlusion volume, into a single self-contained .gstx stream package.");
            }

            ImGui::Spacing();
            ImGui::Separator();

            if (m_rawSplats.empty())
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
                if ((m_hasBakedParams ? m_bakedParams.occlusion : m_generateOcclusionVolume) && !m_occlusionGrid.valid && m_occlusionVoxels.empty() && !HasOccluderMesh()
                    && m_occlusionSafetyNetVersion != m_occlusionVoxelsVersion)
                {
                    RefreshOcclusionVolume(); // Bumps m_occlusionVoxelsVersion whether or not cubes came out
                    m_occlusionSafetyNetVersion = m_occlusionVoxelsVersion;
                }

                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Preprocessor Parameters:");

                // The bake parameters wait for Bake: moving them changes only the settings, and the
                // displayed model, its metrics and Save stay as last baked (the colour grade stays live).
                float maxChunkSize = std::max(1.0f, std::max(m_extents.x, std::max(m_extents.y, m_extents.z)));
                ImGui::SliderFloat("Octree Chunk (m)", &m_chunkSize, 0.05f, maxChunkSize, "%.2f meters");
                if (ImGui::IsItemHovered()) ShowTooltip("Spatial octree voxel bounding box diameter for streaming chunk partitioning. Applied by Bake.");

                ImGui::SliderInt("Max Wavelet LODs", &m_maxLODLevels, 1, 6);
                if (ImGui::IsItemHovered()) ShowTooltip("Maximum number of multi-resolution LOD decimation levels in the wavelet pyramid. Applied by Bake.");

                ImGui::SliderFloat("Deadband Zero (mm)", &m_deadbandThresholdMM, 0.0f, 50.0f, "%.1f mm");
                if (ImGui::IsItemHovered()) ShowTooltip("Sparsification deadband: wavelet detail coefficients below this threshold are zeroed out. Applied by Bake.");

                // Every point bakes as a 3D Gaussian (a plain point cloud's are made for it, see PointCloudGaussians.h); how
                // much view-dependent colour to keep, and at what precision, is the Compression (RDO) menu's choice
                if (m_splatMode) DrawRdoMenu();


                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Checkbox("Generate Interior Occlusion Volume", &m_generateOcclusionVolume);
                if (ImGui::IsItemHovered()) ShowTooltip("Bakes solid occluder cubes into the interior of the model so the renderer can depth-test against them to hide far-side splats visible through gaps in a sparse near side. The volume is a closed voxel proxy of the whole model (surface shell plus everything it encloses). On by default; applied by Bake.");
                // The volume's diagnostics, when one has been built
                {
                    if (m_occlusionGrid.valid)
                    {
                        ImGui::TextDisabled("Auto resolution: %d cells along the longest axis (%.3f m per cell)", m_occlusionGrid.resolution, m_occlusionGrid.cellSize);
                        ImGui::TextDisabled("Shell padding: %d cell(s)   Poke-through cull: %zu of %zu cubes removed   Bias: %+.1f cells (config.json)", m_occlusionGrid.gapClose, m_occlusionGrid.pokeCount, m_occlusionGrid.solidCount, m_occlusionShaveBiasCells);
                        if (ImGui::IsItemHovered()) ShowTooltip("Cubes whose corners fall outside the splat surface (tested against the centroid and mean normal of the neighbouring splats). Always removed, so the volume never clips outside the model.");
                        if (ImGui::IsItemHovered()) ShowTooltip("Chosen from the point density so a few points span every surface cell. Interior blocks are merged into larger cubes automatically.");
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                {
                    const bool pending = BakePending();
                    // The theme's button blue, a little brighter so Bake stands out; with changes waiting it is taller
                    // and pulses slowly (every 2.5 s) out to a pastel blue until it is pressed
                    auto brighter = [](ImVec4 c, float k) { return ImVec4(std::min(1.0f, c.x * k), std::min(1.0f, c.y * k), std::min(1.0f, c.z * k), 1.0f); };
                    const ImVec4 base = brighter(ImGui::GetStyle().Colors[ImGuiCol_Button], 1.25f);
                    ImVec4 face = base;
                    const ImVec4 pastel(0.58f, 0.72f, 0.95f, 1.0f);
                    // A slow breath (every 6 s) a third of the way to pastel, with a soft glow around the button
                    const float pulse = pending ? 0.5f - 0.5f * cosf((float)ImGui::GetTime() * 6.2832f / 6.0f) : 0.0f;
                    if (pending)
                    {
                        const float k = 0.35f * pulse;
                        face = ImVec4(base.x + (pastel.x - base.x) * k, base.y + (pastel.y - base.y) * k, base.z + (pastel.z - base.z) * k, 1.0f);
                    }
                    ImGui::PushStyleColor(ImGuiCol_Button, face);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighter(face, 1.15f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighter(base, 0.85f));
                    if (ImGui::Button(pending ? "Bake Changes" : "Bake", ImVec2(-1, pending ? 44.0f : 32.0f)) && pending)
                        StartBake(false);
                    if (pending)
                    {
                        // Glow: rings of pastel fading outward, always faintly there and brightest at the top of the breath
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                        const float rounding = ImGui::GetStyle().FrameRounding;
                        dl->PushClipRectFullScreen();
                        for (int i = 1; i <= 7; i++)
                        {
                            const float fade = 1.0f - (float)(i - 1) / 7.0f;
                            const float alpha = (0.05f + 0.13f * pulse) * fade * fade;
                            dl->AddRect(ImVec2(a.x - i, a.y - i), ImVec2(b.x + i, b.y + i), ImGui::GetColorU32(ImVec4(pastel.x, pastel.y, pastel.z, alpha)), rounding + i, ~0, 1.0f);
                        }
                        dl->PopClipRect();
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) ShowTooltip(pending ? "Rebuilds the model with the parameters above. Until then the displayed model and Save use the last bake."
                                                                          : "Nothing to bake: the displayed model matches the parameters above.");
                    if (pending) ImGui::TextColored(ImVec4(0.65f, 0.78f, 1.0f, 1.0f), "Changed, not yet baked: %s", PendingBakeChanges().c_str());
                }
                ImGui::Spacing();
                if (HasOccluderMesh())
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Baked: occluder mesh, %zu triangles", m_occluderMesh->levels[0].TriangleCount());
                    if (ImGui::IsItemHovered()) ShowTooltip("The occlusion volume as a smooth mesh just inside the splats: far-side splats are depth-tested against it, and the shadow mesh fills the gaps between the splats with the model's colour.");
                }
                else if (!m_occlusionVoxels.empty())
                {
                    std::string chain;
                    for (uint32_t k = 0; k < m_occlusionMips.mipCount; k++)
                        chain += (k ? " / " : "") + std::to_string(m_occlusionMips.blockCount[k]);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Baked: %zu occluder cubes in %u mips (%s)", m_occlusionVoxels.size(), m_occlusionMips.mipCount, chain.c_str());
                    if (ImGui::IsItemHovered()) ShowTooltip("Mip 0 is the fine skin, %.3f m cells. Each further mip is a coarser level that lies inside the one below it, so no mip ever protrudes. The renderer draws one mip per frame, chosen from how many pixels a cell covers at the model's nearest point, so the volume coarsens together with the splat LOD in view.", m_occlusionMips.mipCount > 0 ? m_occlusionMips.cellSize[0] : 0.0f);
                }
                else if (m_generateOcclusionVolume)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Baked: no cubes survived (point cloud too sparse/thin at this resolution)");
                // Show Occlusion Volume, Hide Splat Layer and View Shadow Mesh are rendering options: Renderer tab

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("Preprocessor Analytics:");
                ImGui::Text("• Total Raw Vertices: %u", (uint32_t)m_rawSplats.size());
                ImGui::Text("• Partitioned Chunks: %u", (uint32_t)m_chunks.size());
                ImGui::Text("• Raw Uncompressed:   %.2f MB", m_rawFileSizeMB);
                ImGui::Text("• Tier 5 Compressed:  %.2f MB (%.1fx)", m_compressedSizeMB, m_compressionRatio);
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
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Source: Loaded .gstx Package");
            }
            else if (!m_rawSplats.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Source: In-Memory Preprocessed Stream");
            }
            else
            {
                ImGui::TextDisabled("No active model loaded in renderer.");
            }

            if (ImGui::Button("Load Compressed Model (.gstx)...", ImVec2(-1, 26)))
            {
                m_pendingAction = PendingAction::OpenCompressedFile;
            }
            if (ImGui::IsItemHovered()) ShowTooltip("Directly loads and streams a pre-compressed .gstx model from disk, overriding memory data structures.");

            ImGui::Separator();

            if (!m_rawSplats.empty() || m_isLoadedFromSFLW)
            {
                // Section 1: Runtime LOD Settings
                if (MenuHeader("1. Runtime LOD & Quality"))
                {
                    int maxLODIndex = std::max(0, (int)m_waveletResult.lodLevels.size() - 1);
                    if (maxLODIndex > 0)
                    {
                        m_selectedPreviewLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));

                        if (ImGui::Checkbox("Auto Distance LOD", &m_autoLOD))
                        {
                            UpdatePreviewSplats();
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Automatically adapts active LOD level dynamically based on distance from camera.");
                        if (m_autoLOD)
                        {
                            ImGui::Indent(12.0f);
                            if (ImGui::Checkbox("Screen-Space LOD", &m_lodScreenSpace)) SaveConfigFile();
                            if (ImGui::IsItemHovered()) ShowTooltip("On: a coarser level is used only once its point spacing projects to at most the threshold below, in pixels, at the model's near side -- so the switch happens where the coarser level can no longer be told apart from full resolution, whatever the window size. Off: the old rule, a fixed ramp from 1x to 4x the model's size. Remembered across sessions.");
                            if (m_lodScreenSpace)
                            {
                                ImGui::SameLine();
                                ImGui::PushItemWidth(140.0f);
                                if (ImGui::SliderFloat("##LodPixelSpacing", &m_lodPixelSpacing, 0.25f, 4.0f, "max %.2f px")) SaveConfigFile();
                                ImGui::PopItemWidth();
                                if (ImGui::IsItemHovered()) ShowTooltip("Largest on-screen spacing between a level's points before the next finer level is used. Lower keeps full resolution longer (more splats drawn); 1 px is where a level's points just tile the pixels.");
                                if (ImGui::Checkbox("Per-Block Levels", &m_lodPerBlock)) SaveConfigFile();
                                if (ImGui::IsItemHovered()) ShowTooltip("On: every block of the model takes its own level from its own distance to the eye, by the rule above -- the near side refines, the far side and the edges turning away stay coarser, so fewer splats are streamed and drawn for the same look. Off: one level for the whole model, set by its near side. Remembered across sessions.");
                            }
                            const int lvlNow = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                            if (lvlNow < (int)m_stream.m_lodSpacing.size() && m_stream.m_lodSpacing[lvlNow] > 0.0f)
                                ImGui::TextDisabled("Level %d spacing on screen: %.2f px", lvlNow, m_stream.m_lodSpacing[lvlNow] * m_stream.LodFocalPixels() / m_stream.LodEffectiveDistance(m_detachCamera ? m_detachedDistance : m_distance));
                            ImGui::Unindent(12.0f);
                        }

                        ImGui::PushItemWidth(ImGui::GetContentRegionAvailWidth() - 95.0f);
                        if (ImGui::SliderInt("##LODSlider", &m_selectedPreviewLOD, 0, maxLODIndex, "LOD %.0f"))
                        {
                            m_selectedPreviewLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                            m_autoLOD = false;
                            m_enableWavelet = true;
                            UpdatePreviewSplats();
                        }
                        ImGui::PopItemWidth();

                        ImGui::SameLine();
                        if (ImGui::Checkbox("Cascade", &m_cascadeLOD))
                        {
                            PrecacheResidentLODs();
                            UpdatePreviewSplats();
                        }

                        ImGui::Separator();
                    }

                    if (ImGui::Checkbox("Dithered LOD Transitions", &m_enableDitheredTransitions))
                    {
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Stochastic screen-space Bayer dithering to smoothly blend LOD transitions and eliminate popping.");

                    if (m_enableDitheredTransitions)
                    {
                        ImGui::SameLine();
                        ImGui::PushItemWidth(100.0f);
                        if (ImGui::SliderFloat("##DitherDurationTab", &m_ditherTransitionDurationSec, 0.05f, 5.0f, "%.2f s"))
                        {
                            m_stream.m_streamStateDirty = true;
                        }
                        ImGui::PopItemWidth();
                    }
                    if (ImGui::Checkbox("Demote Chunks on Zoom Out", &m_demoteChunks))
                    {
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Off: once a chunk has refined into its children it stays refined when you zoom out or its edge flag drops, so detail never fades back to the coarser level (Decay still reclaims memory). On: children cross-fade back to the parent and are evicted, as in v1.2.0.");

                    ImGui::Separator();
#if 0 // DYNAMIC SILHOUETTE: GPU silhouette chunk processing switched off for now (re-enable by restoring this block)
                    if (ImGui::Checkbox("Highlight Edge Chunks", &m_highlightSilhouetteChunks))
                    {
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Visualizer toggle: highlights the active silhouette edge chunks.");
#endif // DYNAMIC SILHOUETTE

                    DrawRefinementVisualizerControls("##Renderer");

                    if (ImGui::Checkbox("Show ONLY Locked Chunks (Transition / Edge)", &m_showOnlyLockedChunks))
                    {
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Isolates active dynamic workload: hides static model geometry and renders ONLY chunks that are currently locked in transition or detected on the silhouette edge.");

#if 0 // DYNAMIC SILHOUETTE: GPU silhouette chunk processing switched off for now (re-enable by restoring this block)
                    uint32_t liveEdgeChunks = 0;
                    uint32_t totalActiveChunks = 0;
                    if (m_enableStreamingSimulation)
                    {
                        totalActiveChunks = (uint32_t)m_stream.m_rendererSourceChunks.size();
                        for (const auto* pChunk : m_stream.m_rendererSourceChunks)
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
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Recovers Level 0 fine chunks along 2D outer silhouettes and interior self-occlusion edges detected on GPU.");

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
                            m_stream.m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("LOD level bias for silhouette edges: renders fine edges using Level N - Bias (minimum Level 0).");

                        if (ImGui::SliderFloat("Silhouette Angle", &m_silhouetteThreshold, 0.10f, 0.70f, "%.2f"))
                        {
                            m_stream.m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Grazing angle dot product threshold |N . V| to classify boundary chunks as silhouette.");

                        if (ImGui::Button("Test Silhouette Edge Refinement", ImVec2(-1, 26)))
                        {
                            TriggerSilhouetteEdgeMorphTest();
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Unloads and immediately refreshes ONLY the silhouette edge chunks without touching or waving the rest of the model.");
                    }
#endif // DYNAMIC SILHOUETTE

                }

                // Interior Occlusion Volume and shadow mesh (rendering options, not bake parameters) -- only when the
                // loaded model actually has a baked volume or mesh (see LoadSFLWFile/ApplyBake).
                if (!m_occlusionVoxels.empty() || HasOccluderMesh() || (m_shadowMesh && !m_shadowMesh->Empty()) || m_shadowMeshJob)
                {
                    if (MenuHeader("Interior Occlusion Volume"))
                    {
                        if (ImGui::IsItemHovered())
                        {
                            if (HasOccluderMesh()) ShowTooltip("The baked occluder packaged with this model (a mesh of %zu triangles). It is built by the bake, from the splats, and fixed here.", m_occluderMesh->levels[0].TriangleCount());
                            else ShowTooltip("The baked occluder solid packaged with this model (%zu cubes). It is built by the bake, from the splats, and fixed here.", m_occlusionVoxels.size());
                        }
                        DrawOcclusionVolumeControls();
                    }
                }

                // Section 3: 3D Viewport & Splat Sizing
                if (MenuHeader("2. 3D Viewport & Splat Sizing"))
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Active Display: %u points", m_state.splatCount);

                    // Splat are drawn as normal-oriented tangent discs and splat packages as sorted Gaussians;
                    // the Splat Orientation dropdown and Draw Splats as Splat were removed from the panel
                    // (the LOD quality test still switches a splat package to splats for its comparison shots)
                    if (m_splatMode)
                    {
                        static const char* s_blendNames[] = { "Display space (reference viewer)", "Linear (sRGB target)" };
                        int blendShown = m_splatBlendSpace;
                        if (ImGui::Combo("Splat Blending", &blendShown, s_blendNames, IM_ARRAYSIZE(s_blendNames)))
                            m_splatBlendSpace = blendShown;
                        if (ImGui::IsItemHovered()) ShowTooltip("Where the Gaussians are blended. Display space blends the stored colours as they are, which is what the training rasterizer and SuperSplat do. Linear decodes them first and lets the sRGB target encode the result: physically tidier, slightly different from the reference.");
                        // Greyed out while kSplatGaussianLevel0 is false (it refers to the Gaussian level 0): shown, but
                        // edits go to a copy and are dropped; the saved value still sizes the discs of every level
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, kSplatGaussianLevel0 ? 1.0f : 0.45f);
                        bool matchShown = m_splatDiscMatchLevel0;
                        if (ImGui::Checkbox("Match Level 0 Softness", &matchShown) && kSplatGaussianLevel0)
                        {
                            m_splatDiscMatchLevel0 = matchShown;
                            SaveConfigFile();
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Sizes the coarse levels' discs from the level 0 Gaussians themselves: the median footprint of the level 0 records relative to their spacing gives a softness ratio, and every disc level takes the radius that gives its discs the same ratio to their own spacing, so a streamed model reads as one surface and the hand-over from discs to Gaussians is less visible. Discs of level 1 and up also take an opacity floor so a low-opacity record does not read as a hole. Off: discs are 1.5 x the level spacing with the record's opacity as is. Remembered across sessions.");
                        ImGui::TextDisabled("Level 0 softness %.2f x spacing -> disc radius %.2f x spacing", m_splatSoftnessRatio, m_splatDiscMatchFactor);
                        if (ImGui::IsItemHovered()) ShowTooltip("The level 0 records' median in-plane footprint sqrt(s_max x s_mid) divided by the level 0 point spacing, and the disc radius factor sqrt(8) x that ratio (a disc with the splat falloff of radius R is a Gaussian of sigma R / sqrt(8)), clamped to 1.5..4.0.");
                        ImGui::PopStyleVar();
                        if (ImGui::Checkbox("Coarse Levels as Gaussians", &m_splatCoarseAsGaussians))
                        {
                            SaveConfigFile();
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Draws levels 1 and up with their own Gaussians (the coarse records carry grown scales and orientations) instead of camera-facing discs of the level spacing, which read as grain. The two disc settings above then only matter while this is off. Remembered across sessions.");
                        if (m_splatCoarseAsGaussians)
                        {
                            ImGui::SameLine();
                            ImGui::PushItemWidth(140.0f);
                            if (ImGui::SliderFloat("##CoarseGaussianCap", &m_splatCoarseGaussianCap, 0.0f, 4.0f, m_splatCoarseGaussianCap > 0.0f ? "cap %.2f x spacing" : "no cap")) SaveConfigFile();
                            ImGui::PopItemWidth();
                            if (ImGui::IsItemHovered()) ShowTooltip("Largest scale a coarse Gaussian may take, per axis, as a multiple of its level's point spacing. The coarse records are grown well past that and bloom into a blur around the model without a cap; 1.5 measured best on cthulu (0.75 is a little grainier). 0 = no cap. Remembered across sessions.");
                        }
                    }
                    // Splat Radius Scale, Auto Splat Size and its Coverage sized the splat discs, which are gone;
                    // their defaults still size the silhouette prepass's coarse discs (m_state.lodRadius).
                    if (!m_stream.m_lodSpacing.empty())
                    {
                        std::string sp = "Level spacing:";
                        for (size_t l = 0; l < m_stream.m_lodSpacing.size(); l++) { char b[40]; snprintf(b, sizeof(b), " L%zu %.4g", l, m_stream.m_lodSpacing[l]); sp += b; }
                        ImGui::TextDisabled("%s", sp.c_str());
                        if (ImGui::IsItemHovered()) ShowTooltip("Median nearest-neighbour distance of each level's points, in model units (measured on load).");
                    }
                    ImGui::Separator();

                    ImGui::Checkbox("Auto Rotate Model##Viewport", &m_autoRotate);
                    m_state.autoRotate = m_autoRotate;

                    static const char* s_splatVizNames[] = { "Normal", "Splat outlines", "Random colour per splat", "Splat centres" };
                    ImGui::PushItemWidth(180.0f);
                    ImGui::Combo("Splat Display (V)##Viewport", &m_splatViz, s_splatVizNames, IM_ARRAYSIZE(s_splatVizNames));
                    ImGui::PopItemWidth();
                    if (ImGui::IsItemHovered()) ShowTooltip("How every splat of the whole model is drawn. Normal: as rendered, blending into one smooth surface. Splat outlines: each splat's ellipse at half its peak, over a faint fill -- its true size, shape and orientation. Random colour per splat: every splat filled flat out to that same contour in its own colour, so coverage and overlap show. Splat centres: a mark at every centre, the sampling density. V cycles it; Shift+V cycles the loupe's own (L).");

                    if (ImGui::Checkbox("Show Camera Control Hints##Viewport", &m_showControlHints))
                    {
                        SaveConfigFile();
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Shows or hides the faint rotate / zoom / pan reminder in the bottom-right corner of the viewport. Remembered across sessions.");

                    if (ImGui::Checkbox("VSync (Lock Framerate to Display)", &m_vsync))
                    {
                        m_swapChain.SetVSync(m_vsync);
                    }

                    if (m_pRenderer && m_pRenderer->GetRenderPath() != SplatRenderer::RenderPath::MeshShaders)
                    {
                        // No amplification (task) shader stage on this render path: the control is greyed out and
                        // inert. Chunk backface culling still happens, per splat in the vertex shader, always on.
                        bool fixedOn = true;
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f);
                        ImGui::Checkbox("Meshlet Backface Cone Culling (Task Shader)", &fixedOn);
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemHovered()) ShowTooltip("Unavailable on this GPU path (%s): there is no amplification (task) shader stage. Chunk backface culling runs per splat in the vertex shader instead and is always on.", m_pRenderer->GetRenderPathDescription());
                    }
                    else
                    {
                        ImGui::Checkbox("Meshlet Backface Cone Culling (Task Shader)", &m_enableConeCulling);
                        if (ImGui::IsItemHovered()) ShowTooltip("Task Shader (mainAS) culls ~50% of meshlet chunks facing away from the camera before mesh shaders and rasterization ever execute.");
                    }

                    ImGui::Checkbox("Use DX12 CopyQueue (Async DMA)", &m_useCopyQueue);
                    if (ImGui::IsItemHovered()) ShowTooltip("Uses a dedicated D3D12_COMMAND_LIST_TYPE_COPY hardware DMA queue for PCIe buffer uploads in parallel with 3D rendering.");

                    bool detachToggle = m_detachCamera;
                    if (ImGui::Checkbox("Detach Camera (Freeze Culling Frustum)", &detachToggle))
                        SetDetachCamera(detachToggle);
                    if (ImGui::IsItemHovered()) ShowTooltip("Ctrl+D. Freezes the culling camera where the view is and steps the view aside so the culling can be inspected from any angle. The model is exactly what the frozen camera would render, its level of detail included: splats it would draw keep their colour (mid grey where the viewer sees their back), everything it would have culled -- chunks outside its frustum or whose normal cone faces away, and the far half of the model beyond its centre -- stays on screen as a translucent red volume, depth-tested against your view so it never builds up. Streaming keeps running from the frozen camera's point of view. The occlusion volume shows only the faces turned toward the frozen camera.");
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
                    if (ImGui::SliderFloat("UI & Font Scale", &m_uiScale, 0.70f, 2.00f, "%.2fx"))
                    {
                        m_uiScale = std::max(0.50f, std::min(2.50f, m_uiScale));
                        io.FontGlobalScale = m_uiScale;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Scales all UI panels, controls, and font sizes dynamically.");
                }

                // Section 3: Temporal Filtering & Anti-Aliasing
                if (MenuHeader("3. Temporal Filtering & Anti-Aliasing (TAA)"))
                {
                    if (ImGui::Checkbox("Enable Temporal Dither Resolver (TAA)", &m_enableTemporalFiltering))
                    {
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Smooths stochastic Bayer dither patterns into seamless photo-realistic alpha dissolves,\nand eliminates splat edge shimmer via temporal history accumulation.");

                    if (m_enableTemporalFiltering)
                    {
                        if (ImGui::SliderFloat("Temporal Blend Weight (Alpha)", &m_temporalBlendWeight, 0.05f, 0.50f, "%.2f (Lower = Smoother)"))
                        {
                            m_stream.m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Blend weight for current frame. 0.10-0.15 delivers optimal 6-frame smooth dither dissipation without ghosting.");

                        if (ImGui::Checkbox("Sub-Pixel Camera Jitter (Halton 2,3)", &m_enableSubpixelJitter))
                        {
                            m_stream.m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Applies sub-pixel projection jitter to reconstruct crisp anti-aliased splat edges when stationary.");

                        if (ImGui::Checkbox("Variance Color Box Clamping (Anti-Ghosting)", &m_enableVarianceClamping))
                        {
                            m_stream.m_streamStateDirty = true;
                        }
                        if (ImGui::IsItemHovered()) ShowTooltip("Clamps reprojected history to 3x3 neighborhood YCoCg bounding box to eliminate ghosting during rapid camera motion.");
                    }
                }

                // Section 4: Hardware Accelerators & Pipeline
                if (MenuHeader("4. Accelerators & Meshlet Pipeline"))
                {
                    if (ImGui::Checkbox("GPU Radix Sort", &m_gpuRadixSort))
                    {
                        if (m_pRenderer) m_pRenderer->FlushGPU();
                        m_state.gpuRadixSort = m_gpuRadixSort;
                    }
                    m_state.gpuRadixSort = m_gpuRadixSort;
                    if (ImGui::IsItemHovered()) ShowTooltip("Executes parallel 32-bit depth key sorting directly on GPU compute shader threads.");

                    ImGui::SameLine();
                    ImGui::TextColored(m_gpuRadixSort ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        m_gpuRadixSort ? "[Compute Shader]" : "[CPU Threads]");

                    if (ImGui::Checkbox("Morton Spatial Curve Ordering", &m_enableMortonOrder))
                    {
                        PrecacheResidentLODs();
                        UpdatePreviewSplats();
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Reorders points along a 3D Morton Z-order space-filling curve.");

                    if (ImGui::Checkbox("Micro-Chunking", &m_useChunkedPipeline))
                    {
                        UpdatePreviewSplats();
                    }
                    m_state.useChunkedPipeline = m_useChunkedPipeline;
                    if (ImGui::IsItemHovered()) ShowTooltip("Hierarchical two-level sorting: 64-splat meshlet clusters with Amplification Shader (AS) frustum culling.");

                    ImGui::SameLine();
                    ImGui::TextColored(m_useChunkedPipeline ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                        m_useChunkedPipeline ? "[Amplification Shader]" : "[Disabled]");
                }

                // Section 5: Spatial & Cluster Visualizers
                if (MenuHeader("5. Spatial & Cluster Visualizers"))
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
                        if (ImGui::IsItemHovered()) ShowTooltip("What the cluster cubes colour by. Point Density: points per cube. Detail (Stream Order): the detail grid baked into the package, which the streaming scheduler reads when it decides what to deliver next: hottest cubes first.");
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
                    if (ImGui::IsItemHovered()) ShowTooltip("Colors chunks/cubes the culling frustum rejects. Only has something to color when 'Show Density Heatmap Cluster Cubes' or 'Show Macro Clusters' is also on, and only has anything VISIBLE to show when 'Detach Camera' is also on -- otherwise the culling frustum is the exact one you're viewing through, so anything culled is off-screen by definition.");
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

            if (!m_rawSplats.empty() || m_isLoadedFromSFLW)
            {
                if (ImGui::Checkbox("Simulate Network Streaming", &m_enableStreamingSimulation))
                {
                    if (m_enableStreamingSimulation)
                    {
                        InitStreamingSimulation();
                    }
                    else
                    {
                        UpdatePreviewSplats();
                    }
                }

                if (m_enableStreamingSimulation)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(m_isStreamingPaused ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                        m_isStreamingPaused ? "[PAUSED]" : "[STREAMING]");

                    // The tab in three groups, each under a heading: the network, playback, and the scheduler
                    const float gap = ImGui::GetStyle().ItemSpacing.x;
                    auto heading = [](const char* text)
                    {
                        ImGui::Spacing();
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "%s", text);
                        ImGui::Separator();
                        ImGui::Spacing();
                    };
                    auto dimWrapped = [](const char* fmt, ...)
                    {
                        char text[512];
                        va_list args;
                        va_start(args, fmt);
                        vsnprintf(text, sizeof(text), fmt, args);
                        va_end(args);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        ImGui::TextWrapped("%s", text);
                        ImGui::PopStyleColor();
                    };

                    // ---- Network Bandwidth: the presets fill the row; the selected one is lit like the active tab ----
                    heading("Network Bandwidth");
                    {
                        struct Preset { const char* label; const char* rate; float mbps; const char* tip; };
                        static const Preset s_presets[] = {
                            { "3G", "0.2 MB/s", 0.2f, "3G: a slow mobile link, 0.2 MB/s." },
                            { "4G", "2 MB/s", 2.0f, "4G: a typical mobile link, 2 MB/s." },
                            { "5G", "7.5 MB/s", 7.5f, "5G: a fast mobile link, 7.5 MB/s." },
                            { "Full", "uncapped", 0.0f, "No throttle: streams as fast as the simulation delivers." } };
                        const int count = IM_ARRAYSIZE(s_presets);
                        const float width = (ImGui::GetContentRegionAvailWidth() - gap * (count - 1)) / (float)count;
                        for (int p = 0; p < count; p++)
                        {
                            const Preset& pr = s_presets[p];
                            const bool selected = pr.mbps == 0.0f ? m_unthrottledBandwidth : (!m_unthrottledBandwidth && fabsf(m_bandwidthThrottleMBps - pr.mbps) < 0.01f);
                            if (p > 0) ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.18f, 0.45f, 0.75f, 1.0f) : ImVec4(0.20f, 0.20f, 0.23f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.24f, 0.53f, 0.85f, 1.0f) : ImVec4(0.28f, 0.28f, 0.32f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.62f, 0.62f, 0.66f, 1.0f));
                            // Two lines, each centred: the name, and its rate in a dimmer tone
                            const float lineH = ImGui::GetTextLineHeight();
                            char id[32];
                            snprintf(id, sizeof(id), "##preset%d", p);
                            if (ImGui::Button(id, ImVec2(width, 2.0f * lineH + 10.0f)))
                            {
                                if (pr.mbps == 0.0f) m_unthrottledBandwidth = true;
                                else { m_bandwidthThrottleMBps = pr.mbps; m_unthrottledBandwidth = false; }
                            }
                            {
                                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                                const float cx = 0.5f * (a.x + b.x), top = 0.5f * (a.y + b.y) - lineH;
                                ImDrawList* dl = ImGui::GetWindowDrawList();
                                const ImU32 nameColour = ImGui::GetColorU32(ImGuiCol_Text);
                                const ImU32 rateColour = selected ? IM_COL32(205, 225, 255, 255) : IM_COL32(130, 130, 140, 255);
                                dl->AddText(ImVec2(cx - 0.5f * ImGui::CalcTextSize(pr.label).x, top), nameColour, pr.label);
                                dl->AddText(ImVec2(cx - 0.5f * ImGui::CalcTextSize(pr.rate).x, top + lineH), rateColour, pr.rate);
                            }
                            ImGui::PopStyleColor(3);
                            if (selected)
                            {
                                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                                ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - 1.0f, a.y - 1.0f), ImVec2(b.x + 1.0f, b.y + 1.0f), IM_COL32(150, 200, 255, 255), ImGui::GetStyle().FrameRounding, ~0, 2.0f);
                            }
                            if (ImGui::IsItemHovered()) ShowTooltip("%s", pr.tip);
                        }
                    }
                    ImGui::Spacing();
                    // Labels on the left, the sliders filling the rest of the row
                    const float labelWidth = ImGui::CalcTextSize("GPU Ring Buffer").x + 2.0f * gap;
                    auto leftLabel = [&](const char* text)
                    {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(text);
                        ImGui::SameLine(labelWidth);
                        ImGui::PushItemWidth(-1.0f);
                    };
                    leftLabel("Bandwidth");
                    if (m_unthrottledBandwidth)
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        float dummyVal = 100.0f;
                        ImGui::SliderFloat("##bandwidth", &dummyVal, 0.2f, 100.0f, "Full (Uncapped)");
                        ImGui::PopStyleVar();
                    }
                    else
                    {
                        if (ImGui::SliderFloat("##bandwidth", &m_bandwidthThrottleMBps, 0.2f, 100.0f, "%.1f MB/s"))
                        {
                            m_unthrottledBandwidth = false;
                        }
                    }
                    ImGui::PopItemWidth();
                    if (ImGui::IsItemHovered()) ShowTooltip("The simulated link's bandwidth: the presets above, or any rate.");
                    const float maxRingMB = m_stream.MaxRingBufferMB();
                    leftLabel("GPU Ring Buffer");
                    ImGui::SliderFloat("##ringBuffer", &m_ringBufferCapacityMB, 4.0f, maxRingMB, "%.0f MB");
                    ImGui::PopItemWidth();
                    if (ImGui::IsItemHovered()) ShowTooltip("GPU memory the streamed blocks may occupy; past it the least needed blocks are evicted.");

                    // ---- Stream Load ----
                    heading("Stream Load");
                    float deliveredMB = m_stream.m_simulatedBytesDelivered / (1024.0f * 1024.0f);
                    float totalMB = m_stream.m_totalStreamBytes / (1024.0f * 1024.0f);
                    char progressOverlay[128];
                    snprintf(progressOverlay, sizeof(progressOverlay), "%.1f / %.1f MB (%.0f%%)", deliveredMB, totalMB, m_stream.m_streamRefinementProgress * 100.0f);
                    ProgressBarCentred(m_stream.m_streamRefinementProgress, ImVec2(-1, 22), progressOverlay);
                    if (ImGui::IsItemHovered())
                    {
                        if (m_stream.m_evictedSplatCount > 0) ShowTooltip("Delivered so far, of the whole stream.\n%u splats in the ring buffer, %zu evicted.", m_state.splatCount, m_stream.m_evictedSplatCount);
                        else ShowTooltip("Delivered so far, of the whole stream.\n%u splats resident.", m_state.splatCount);
                    }
                    ImGui::Spacing();
                    {
                        const float width = (ImGui::GetContentRegionAvailWidth() - gap * 2.0f) / 3.0f;
                        if (ImGui::Button("Re-Stream (Reset)", ImVec2(width, 26)))
                        {
                            ResetStreamingSimulation();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(m_isStreamingPaused ? "Resume" : "Pause", ImVec2(width, 26)))
                        {
                            m_isStreamingPaused = !m_isStreamingPaused;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Instant Full Load", ImVec2(width, 26)))
                        {
                            m_stream.m_simulatedBytesDelivered = m_stream.m_totalStreamBytes;
                            m_stream.m_streamRefinementProgress = 1.0f;
                            UpdateStreamingSimulation(1.0);
                        }
                    }
                    ImGui::Spacing();
                    if (ImGui::Checkbox("Freeze Rendering & Memory (Eliminate Flickering)", &m_freezeRenderingAndMemory))
                    {
                        m_stream.m_streamStateDirty = true;
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Completely freezes streaming simulation, LOD transitions, memory evictions, and edge recalculations.\nEliminates all flickering and jitter artifacts when movement is paused.");

                    // ---- Scheduler ----
                    heading("Scheduler");
                    ImGui::Checkbox("Prioritize View Frustum & Proximity", &m_prioritizeFrustumAndProximity);
                    if (ImGui::IsItemHovered()) ShowTooltip("On: blocks inside the view frustum are delivered before those outside it (then the neighbour band, then the rest). Off: the scheduler's model-wide order, regardless of the camera.");
                    dimWrapped("Order: base envelope > edge chunks > visible octahedron faces > rest");
                    if (ImGui::IsItemHovered()) ShowTooltip("The two coarsest levels always land first so there is a solid envelope. Silhouette edge chunks come next. Then the streaming scheduler serves the faces of the bounding octahedron the camera can see, together, while faces out of view wait their turn; the remaining view requests follow.");
                    ImGui::Spacing();
                    DrawOctahedronGlyph();
                    ImGui::Spacing();
                    dimWrapped("Re-deliveries since reset: %u blocks streamed more than once (%u extra deliveries)", m_stream.m_redeliveredBlocks, m_stream.m_extraDeliveries);
                    if (ImGui::IsItemHovered()) ShowTooltip("Diagnostic. A block counts here when it is delivered, later evicted, and delivered again. With a still camera this should stay at zero; it rises only when something evicts blocks the view still wants (decay, or a demotion that used to evict).");
                    if (m_stream.m_refineStartBudget == UINT32_MAX)
                        dimWrapped("Active transitions: %u. Refinement starts deferred: %u (budget unlimited)", m_stream.m_lastActiveTransitions, m_stream.m_refineStartsDeferred);
                    else
                        dimWrapped("Active transitions: %u. Refinement starts deferred: %u (budget %u chunks)", m_stream.m_lastActiveTransitions, m_stream.m_refineStartsDeferred, m_stream.m_refineStartBudget);
                    if (ImGui::IsItemHovered()) ShowTooltip("Chunks mid cross-fade last frame, and the refinements this frame held back to a later frame so the model refines gradually. A region that waits keeps showing its coarser level; the stream is not slowed.");
                    ImGui::Spacing();
                }
            }
            else
            {
                ImGui::TextDisabled("No active model loaded -- nothing to stream.");
            }
            if (!m_rawSplats.empty() || m_isLoadedFromSFLW)
            {
                ImGui::Spacing();
                ImGui::Spacing();
                DrawEncodingPanel();
            }
        }

        RecordSidePanel(m_leftPanelRect, m_leftPanelCollapsed);
        ImGui::End();

        // 3. Right Statistics & Telemetry Panel. Its height fits the open sections (0 = auto-fit), up to the
        // full panel height, where it scrolls; the space below it stays free for the viewport (and the loupe).
        ImGui::SetNextWindowPos(ImVec2((float)m_Width - rightPanelWidth - 10.0f, 30.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(rightPanelWidth, 0.0f), ImGuiCond_Always);
        // The Presentation Mode panel sits under it: the analytics panel leaves room for that panel's last height
        const float presentHeight = (m_presentPanelRect.w > m_presentPanelRect.y) ? (m_presentPanelRect.w - m_presentPanelRect.y) : ImGui::GetFrameHeight();
        const float analyticsMax = std::max(ImGui::GetFrameHeight() * 4.0f, panelHeight - presentHeight - 6.0f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(rightPanelWidth, 0.0f), ImVec2(rightPanelWidth, analyticsMax));
        SetNextSidePanelCollapse(m_rightPanelRect, m_rightPanelCollapsed);
        ImGui::Begin("Statistics & Compression Analytics", nullptr, 0);

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
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  • Splat/Chunk Buffer Upload:      %.2f ms", uploadDisplay);
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

                    ImGui::Text(m_pRenderer->GetRenderPath() == SplatRenderer::RenderPath::MeshShaders ? "  • Main Splat Mesh Shader Dispatch:  %.2f ms" : "  • Main Splat Vertex Shader Draw:    %.2f ms", m_pRenderer->GetSmoothMainDispatchMs());

                    if (m_enableTemporalFiltering)
                    {
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "  • TAA Resolve:                      %.2f ms", m_pRenderer->GetSmoothTaaMs());
                    }

                    // Where the frame time actually goes. The stage lines above are the CPU cost of recording each
                    // pass; this is the whole frame on the CPU wall clock, so the sum approaches the total.
                    ImGui::Separator();
                    ImGui::Text("Frame Budget (CPU wall clock, ms):");
                    if (ImGui::IsItemHovered()) ShowTooltip("Where the frame time goes, measured on the CPU. The stage lines above are command-recording costs only. GPU wait is the CPU blocked on the swapchain fence, i.e. waiting for the GPU (or VSync): a large value there means the GPU is the bottleneck.");
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
                    uint32_t pointCount = (uint32_t)(!m_rendererSplats.empty() ? m_rendererSplats.size() : m_rawSplats.size());
                    const float bytesPerPoint = (float)BytesPerRenderPoint();
                    float tier2MB = (pointCount * bytesPerPoint) / (1024.0f * 1024.0f);
                    float tier2Reduction = (tier2MB > 0.001f && m_rawFileSizeMB > 0.0f) ? (m_rawFileSizeMB / tier2MB) : 0.0f; // 0x: nothing to compare

                    // A three-column table, with a header, shaded alternate rows and borders: the tier, what it measures, and its
                    // ratio to the tier before (the tier before over this one, as a percentage: 2x smaller is 200%). Every cell has
                    // its own tooltip. Tier 3 is the layout (record order, byte planes), Tier 4 the value transforms.
                    const char* tier3Name = "Tier 3: Morton / Planar";
                    const char* tier4Name = "Tier 4: YCoCg / SH Res";
                    auto format = [](const char* fmt, ...)
                    {
                        char text[1024];
                        va_list args;
                        va_start(args, fmt);
                        vsnprintf(text, sizeof(text), fmt, args);
                        va_end(args);
                        return std::string(text);
                    };
                    auto ratioText = [&](double before, double after) { return (before > 0.0 && after > 0.0) ? format("%.0f%%", 100.0 * before / after) : std::string("-"); };
                    auto ratioTip = [&](const char* before, const char* after, double b, double a, const char* unit)
                    {
                        return (b > 0.0 && a > 0.0) ? format("%s over %s: %.2f / %.2f %s = %.2fx, %.0f%%.", before, after, b, a, unit, b / a, 100.0 * b / a) : std::string();
                    };

                    ImDrawList* tableDl = ImGui::GetWindowDrawList();
                    const float tableX0 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
                    const float tableX1 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                    const ImU32 borderColour = ImGui::GetColorU32(ImGuiCol_Border);
                    const float tableY0 = ImGui::GetCursorScreenPos().y;
                    int rowIndex = 0;
                    // Each row's band: the header darker, then alternate rows lightly shaded, with a rule under every row
                    auto rowBackground = [&](bool header)
                    {
                        const float y = ImGui::GetCursorScreenPos().y - 0.5f * ImGui::GetStyle().ItemSpacing.y;
                        tableDl->PushClipRect(ImVec2(tableX0, y), ImVec2(tableX1, y + rowHeight), false);
                        if (header) tableDl->AddRectFilled(ImVec2(tableX0, y), ImVec2(tableX1, y + rowHeight), IM_COL32(60, 70, 110, 200));
                        else if (rowIndex % 2 == 1) tableDl->AddRectFilled(ImVec2(tableX0, y), ImVec2(tableX1, y + rowHeight), IM_COL32(255, 255, 255, 14));
                        tableDl->PopClipRect();
                        tableDl->PushClipRectFullScreen();
                        tableDl->AddLine(ImVec2(tableX0, y + rowHeight), ImVec2(tableX1, y + rowHeight), borderColour);
                        tableDl->PopClipRect();
                        if (!header) rowIndex++;
                    };
                    auto cell = [](const ImVec4* colour, const std::string& text, const std::string& tip)
                    {
                        if (colour) ImGui::TextColored(*colour, "%s", text.c_str()); else ImGui::TextUnformatted(text.c_str());
                        if (ImGui::IsItemHovered() && !tip.empty()) ShowTooltip("%s", tip.c_str());
                        ImGui::NextColumn();
                    };
                    auto row = [&](const char* name, const ImVec4* colour, const std::string& nameTip, const std::string& value, const std::string& valueTip,
                                   const std::string& ratio, const std::string& rTip)
                    {
                        rowBackground(false);
                        cell(colour, name, nameTip);
                        cell(colour, value, valueTip);
                        cell(colour, ratio, rTip);
                    };

                    ImGui::Columns(3, "CompressionTiers", true);
                    const float pad = 2.0f * ImGui::GetStyle().ItemSpacing.x;
                    const float ratioWidth = ImGui::CalcTextSize("0000%").x + pad;
                    ImGui::SetColumnOffset(1, ImGui::CalcTextSize(tier3Name).x + pad);
                    ImGui::SetColumnOffset(2, ImGui::GetWindowContentRegionWidth() - ratioWidth);
                    {
                        const ImVec4 head(0.85f, 0.9f, 1.0f, 1.0f);
                        rowBackground(true);
                        cell(&head, "Tier", "");
                        cell(&head, "Value", "Sizes in MB; the layout and transform tiers in bits per byte (entropy), as they only rearrange or recode bytes.");
                        cell(&head, "Ratio", "The tier before over this one, as a percentage: 200% is half the size (or entropy) of the tier before.");
                    }

                    row("Raw point cloud", nullptr, "The input scan, uncompressed (a 3D Gaussian Splatting .ply: 248 bytes per splat).",
                        format("%.2f MB", m_rawFileSizeMB),
                        m_sourceFileBytes > 0 ? format("The source file's size, %.2f MB, stored in the package so it survives a reload. Every tier below is compared with it.", m_rawFileSizeMB)
                                              : format("%.2f MB: the source file's size is not known, so this is %u splats at 248 bytes each. Every tier below is compared with it.", m_rawFileSizeMB, pointCount),
                        "100%", "The baseline.");

                    char tier2Name[48];
                    snprintf(tier2Name, sizeof(tier2Name), "Tier 2: %u-byte GPU", (unsigned)bytesPerPoint);
                    row(tier2Name, nullptr, m_splatMode
                        ? "Tier 2 Quantization, splat mode: a 20-byte Gaussian record (16:16:16 position, 8-bit log scales, opacity, 8:8:8 colour, quaternion) plus the spherical-harmonics stream."
                        : "Tier 2 Quantization: 8-byte packed GPU format (10:10:10:2 position, Oct16 normal, RGB565 color).",
                        format("%.2f MB", tier2MB),
                        format("%u splats x %u bytes = %.2f MB, level 0 only: what the GPU holds at full detail.", pointCount, (unsigned)bytesPerPoint, tier2MB),
                        ratioText(m_rawFileSizeMB, tier2MB), ratioTip("The raw point cloud", "Tier 2", m_rawFileSizeMB, tier2MB, "MB"));

                    row(tier3Name, nullptr, "Tier 3 Layout: the records sorted along a Morton (Z-order) curve, so neighbouring splats sit next to each other, then transposed into byte planes (every record's first byte, then every second byte, ...), so each plane holds one field's similar values for the codec.",
                        m_entropy.valid ? format("%.2f bit/B", m_entropy.tier3) : std::string("Z-Order/Byte"),
                        m_entropy.valid
                            ? format("Bits per byte a simple coder needs once the records are in Morton order and split into byte planes: per plane, the better of its own entropy and that of its deltas from the previous record.\n"
                                     "Interleaved, as Tier 2 holds them: %.2f bit/B. In layout: %.2f bit/B (records %.2f -> %.2f, harmonics %.2f -> %.2f).\n"
                                     "Order-0 entropy of the whole stream would not move: the layout only reorders bytes. Lossless.",
                                     m_entropy.tier2, m_entropy.tier3, m_entropy.records[0], m_entropy.records[1], m_entropy.sh[0], m_entropy.sh[1])
                            : std::string("Z-order record order, byte planes. Lossless: it moves bytes and removes none. Its entropy figure appears for a 3D Gaussian package or bake."),
                        m_entropy.valid ? ratioText(m_entropy.tier2, m_entropy.tier3) : std::string("-"),
                        m_entropy.valid ? ratioTip("Tier 2 interleaved", "Tier 3", m_entropy.tier2, m_entropy.tier3, "bit/B") : std::string());

                    {
                        std::string valueTip = "-";
                        if (m_entropy.valid)
                            valueTip = format("Bits per byte, measured as for Tier 3, after colour as YCoCg and the harmonics as residuals from their parents: %.2f bit/B (records %.2f -> %.2f, harmonics %.2f -> %.2f).\n\n",
                                              m_entropy.tier4, m_entropy.records[1], m_entropy.records[2], m_entropy.sh[1], m_entropy.sh[2]);
                        if (m_transformedMB > 0.0f)
                        {
                            valueTip += format("%.2f MB: what the codec receives -- every level's records and harmonics, uncompressed, so about twice the level 0 data of Tier 2.\n"
                                              "Records %.2f MB, harmonics %.2f MB (%.0f%%).", m_transformedMB, m_transformedMB - m_transformedSHMB, m_transformedSHMB,
                                              100.0f * m_transformedSHMB / std::max(0.001f, m_transformedMB));
                            // What the colour transform bought: entropy and size side by side (entropy alone can mislead)
                            if (m_colourEstimate.valid)
                                valueTip += format("\n\nAgainst plain absolute RGB, on the bake's size sample (entropy: mean order-0 of the byte planes; size: after Brotli, what the file pays):\n"
                                                   "Both: entropy %+.1f%%, size %+.1f%%\nRecords: entropy %+.1f%%, size %+.1f%%\nHarmonics: entropy %+.1f%%, size %+.1f%%",
                                                   m_colourEstimate.entropyChangePercent, m_colourEstimate.sizeChangePercent,
                                                   m_colourEstimate.records.entropyChangePercent, m_colourEstimate.records.sizeChangePercent,
                                                   m_colourEstimate.sh.entropyChangePercent, m_colourEstimate.sh.sizeChangePercent);
                        }
                        row(tier4Name, nullptr, "Tier 4 Transforms: colour stored as YCoCg, and each level's harmonics as the difference from the coarser level's. Both are lossless and leave the codec small, regular values.",
                            m_entropy.valid ? format("%.2f bit/B", m_entropy.tier4) : m_transformedMB > 0.0f ? format("%.2f MB", m_transformedMB) : std::string("-"),
                            valueTip,
                            m_entropy.valid ? ratioText(m_entropy.tier3, m_entropy.tier4) : std::string("-"),
                            m_entropy.valid ? ratioTip("Tier 3", "Tier 4", m_entropy.tier3, m_entropy.tier4, "bit/B") : std::string());
                    }

                    row("Tier 5: Brotli", nullptr, "Tier 5 Entropy Codec: the transformed byte planes Brotli-compressed at quality 11: the package as it is written.",
                        format("%.2f MB", m_compressedSizeMB),
                        format("%.2f MB: %s.%s", m_compressedSizeMB, m_isLoadedFromSFLW ? "the package file's real size" : "estimated from a sample of the bake (Save writes the real file)",
                               m_transformedMB > 0.0f ? format("\n%.2f bits per byte of the %.2f MB the codec receives%s.", 8.0f * m_compressedSizeMB / std::max(0.001f, m_transformedMB), m_transformedMB,
                                                               m_entropy.valid ? format(", against the %.2f Tier 4 estimates", m_entropy.tier4).c_str() : "").c_str() : ""),
                        m_transformedMB > 0.0f ? ratioText(m_transformedMB, m_compressedSizeMB) : std::string("-"),
                        m_transformedMB > 0.0f ? ratioTip("What the codec receives", "the package", m_transformedMB, m_compressedSizeMB, "MB") : std::string());

                    const ImVec4 green(0.3f, 1.0f, 0.4f, 1.0f);
                    row("Total compression", &green, "Pinned to the Tier 2 (GPU quantization) ratio: the reduction every splat gets from the fixed-size packed format, independent of how well the codec compresses a given model's bytes.",
                        format("%.2f MB", tier2MB),
                        format("The raw point cloud, %.2f MB, against Tier 2, %.2f MB.", m_rawFileSizeMB, tier2MB),
                        ratioText(m_rawFileSizeMB, tier2MB), ratioTip("The raw point cloud", "Tier 2", m_rawFileSizeMB, tier2MB, "MB"));
                    ImGui::Columns(1);
                    {
                        // The outer border
                        const float tableY1 = ImGui::GetCursorScreenPos().y - 0.5f * ImGui::GetStyle().ItemSpacing.y;
                        tableDl->PushClipRectFullScreen();
                        tableDl->AddRect(ImVec2(tableX0, tableY0 - 0.5f * ImGui::GetStyle().ItemSpacing.y), ImVec2(tableX1, tableY1), borderColour);
                        tableDl->PopClipRect();
                    }
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
                        float percent = (lod.splats.size() * 100.0f) / std::max(1ULL, (unsigned long long)m_rawSplats.size());

                        // Highlight the active visible LOD level in green/asterisk
                        bool isVisible = ((int)i == m_selectedPreviewLOD);

                        char label[64];
                        sprintf_s(label, "LOD %d%s", lod.level, isVisible ? " (ACTIVE)" : "");

                        if (ImGui::Selectable(label, isVisible, ImGuiSelectableFlags_SpanAllColumns))
                        {
                            m_selectedPreviewLOD = (int)i;
                            m_autoLOD = false; // Disable auto when user clicks manual table row
                            m_enableWavelet = true;
                            UpdatePreviewSplats();
                        }
                        ImGui::NextColumn();

                        ImGui::Text("%u", (uint32_t)lod.splats.size()); ImGui::NextColumn();
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
                    uint32_t totalVertices = (uint32_t)(!m_rendererSplats.empty() ? m_rendererSplats.size() : m_rawSplats.size());
                    ImGui::Text("Active Source:   %s", m_rendererSourceDescription.c_str());
                    ImGui::Text("Total Vertices:  %u points", totalVertices);
                    ImGui::Text("Uncompressed:    %.2f MB", m_rawFileSizeMB);
                    ImGui::Text("Bounding Box Min:[%.2f, %.2f, %.2f]", m_aabbMin.x, m_aabbMin.y, m_aabbMin.z);
                    ImGui::Text("Bounding Box Max:[%.2f, %.2f, %.2f]", m_aabbMax.x, m_aabbMax.y, m_aabbMax.z);
                    ImGui::Text("Spatial Extents: %.1f x %.1f x %.1f m", m_extents.x, m_extents.y, m_extents.z);
                    ImGui::Text("Shadow Mesh:     %s", ShadowMeshSummary().c_str());
                    if (ImGui::IsItemHovered()) ShowTooltip("The smooth mesh following the visible surface, baked into the package in three levels of detail: Unreal uses it for shadows, Lumen and collision; View Shadow Mesh draws it.");
                }
                break;

                // Geometry Optimisations & Culling Stats
                case kRightCulling:
                {
                    const auto& cStats = m_pRenderer->GetCullStats();

                    uint32_t totalSplats = cStats.totalDatasetSplats;
                    uint32_t drawnSplats = cStats.msDrawnSplats;
                    uint32_t culledSplats = (totalSplats >= drawnSplats) ? (totalSplats - drawnSplats) : 0;
                    float reductionPct = totalSplats > 0 ? (float)culledSplats / (float)totalSplats * 100.0f : 0.0f;
                    float reductionFactor = drawnSplats > 0 ? (float)totalSplats / (float)drawnSplats : 1.0f;

                    // Summary Badges / KPI Cards
                    ImGui::Columns(3, "CullKpiCols", false);
                    ImGui::TextDisabled("TOTAL SPLATS");
                    ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "%u", totalSplats);
                    ImGui::NextColumn();

                    ImGui::TextDisabled("CULLED / SAVED");
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%u (%.1f%%)", culledSplats, reductionPct);
                    ImGui::NextColumn();

                    ImGui::TextDisabled("DRAWN TO SCREEN");
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%u (%.1f%%)", drawnSplats, 100.0f - reductionPct);
                    ImGui::NextColumn();
                    ImGui::Columns(1);

                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Optimization Efficiency: %.1fx reduction (%.1f%% eliminated)", reductionFactor, reductionPct);

                    ImGui::Spacing();

                    // Visual Proportional Multi-Segment Breakdown Bar
                    if (totalSplats > 0)
                    {
                        ImVec2 barSize = ImVec2(ImGui::GetContentRegionAvailWidth(), 16.0f);
                        ImVec2 barPos = ImGui::GetCursorScreenPos();
                        ImDrawList* drawList = ImGui::GetWindowDrawList();

                        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), IM_COL32(30, 30, 30, 255), 2.0f);

                        float fracDrawn   = (float)drawnSplats / (float)totalSplats;
                        float fracFrustum = (float)cStats.asFrustumCulledSplats / (float)totalSplats;
                        float fracCone    = (float)cStats.asConeCulledSplats / (float)totalSplats;
                        float fracLod     = (float)cStats.lodPrunedSplats / (float)totalSplats;

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
                    ImGui::Text("Splat"); ImGui::NextColumn();
                    ImGui::Text("Chunks"); ImGui::NextColumn();
                    ImGui::Text("Share / Rule"); ImGui::NextColumn();
                    ImGui::Separator();

                    // 1. LOD Decimation
                    float lodShare = totalSplats > 0 ? (float)cStats.lodPrunedSplats / (float)totalSplats * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.75f, 0.5f, 0.95f, 1.0f), "1. LOD Multi-Res"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.lodPrunedSplats); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.totalDatasetChunks > cStats.lodActiveChunks ? (cStats.totalDatasetChunks - cStats.lodActiveChunks) : 0); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (Pixel error)", lodShare); ImGui::NextColumn();

                    // 2. Task Shader Frustum Culling
                    float frustumShare = totalSplats > 0 ? (float)cStats.asFrustumCulledSplats / (float)totalSplats * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "2. Task Frustum Cull"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asFrustumCulledSplats); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asFrustumCulledChunks); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (6 Planes AABB)", frustumShare); ImGui::NextColumn();

                    // 3. Task Shader Normal Cone Culling
                    float coneShare = totalSplats > 0 ? (float)cStats.asConeCulledSplats / (float)totalSplats * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "3. Task Backface Cone"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asConeCulledSplats); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asConeCulledChunks); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (Cone Axis . Ray)", coneShare); ImGui::NextColumn();

                    // 4. Occlusion: chunks hidden behind the occluder mesh, dropped by the splat sort
                    float occShare = totalSplats > 0 ? (float)cStats.occlusionCulledSplats / (float)totalSplats * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "4. Occlusion Mesh"); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.occlusionCulledSplats); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.occlusionCulledChunks); ImGui::NextColumn();
                    ImGui::Text("%.1f%% (Behind occluder)", occShare); ImGui::NextColumn();

                    // 5. Mesh Shader & Drawn
                    float drawnShare = totalSplats > 0 ? (float)drawnSplats / (float)totalSplats * 100.0f : 0.0f;
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "5. Mesh Shader Emitted"); ImGui::NextColumn();
                    ImGui::Text("%u", drawnSplats); ImGui::NextColumn();
                    ImGui::Text("%u", cStats.asPassedChunks); ImGui::NextColumn();
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%.1f%% (SURVIVED)", drawnShare); ImGui::NextColumn();

                    ImGui::Columns(1);
                    ImGui::Separator();

                    // On-Chip Amplification & Bandwidth Savings
                    ImGui::Text("Hardware Amplification & Bandwidth:");
                    ImGui::BulletText("Generated Vertices:   %u  (4 per splat)", cStats.generatedVertices);
                    ImGui::BulletText("Generated Triangles:  %u  (2 per splat)", cStats.generatedTriangles);
                    ImGui::BulletText("VRAM Bandwidth Saved: %.2f MB/frame", cStats.vramBandwidthSavedMB);

                    uint32_t statsEdgeChunks = 0;
                    uint32_t statsTotalChunks = 0;
                    if (m_enableStreamingSimulation)
                    {
                        statsTotalChunks = (uint32_t)m_stream.m_rendererSourceChunks.size();
                        for (const auto* pChunk : m_stream.m_rendererSourceChunks)
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
                    uint32_t totalVertices = (uint32_t)(!m_rendererSplats.empty() ? m_rendererSplats.size() : m_rawSplats.size());
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

        RecordSidePanel(m_rightPanelRect, m_rightPanelCollapsed);
        ImGui::End();

        // 3b. Presentation Mode, under the analytics panel
        DrawPresentationPanel((float)m_Width - rightPanelWidth - 10.0f, rightPanelWidth, std::max(ImGui::GetFrameHeight() * 2.0f, 30.0f + panelHeight - m_rightPanelRect.w));

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
            if (ImGui::Begin(IsViewerOnly() ? "About Splat Viewer" : "About Splat Generator", &m_showAboutDialog, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), IsViewerOnly() ? "Splat Viewer" : "Splat Generator & Splat Cruncher");
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
                    ? "The standalone viewer for .gstx Gaussian splat packages: progressive wavelet streaming with DirectX 12 Gaussian rendering. The same renderer and streaming tabs as SplatPak, without the generator."
                    : "A multi-resolution point cloud preprocessor and progressive wavelet streaming pipeline built for high-performance DirectX 12 Mesh Shader splat rendering.");
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




    ImVec4 SplatPakApp::LevelTint(int lod, float saturation, float value) const
    {
        // Hue offset per level, mirrored from Splat.hlsl: consecutive levels across the colour wheel from one another
        static const float kArrivalGlowLevelHue[8] = { 0.0f, 180.0f, 90.0f, 270.0f, 315.0f, 135.0f, 45.0f, 225.0f };
        const float hue = std::fmod(45.0f + m_arrivalGlowHue + kArrivalGlowLevelHue[std::clamp(lod, 0, 7)] + 720.0f, 360.0f) / 360.0f;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, saturation, value, r, g, b);
        return ImVec4(r, g, b, 1.0f);
    }

    // Draws the per-LOD-level residency bar graph plus the Evict/Decay Rate/Policy controls beside it. Each level's
    // resident segments take the level's refinement tint (LevelTint), as the chunks do in the viewport while they arrive.
    void SplatPakApp::DrawLODResidencyEqualizer()
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

        if (m_stream.m_smoothedLodResidentPct.size() != (size_t)numLODs)
        {
            m_stream.m_smoothedLodResidentPct.assign(numLODs, 0.0f);
            m_stream.m_smoothedLodResidentBlocks.assign(numLODs, 0);
        }

        m_stream.m_equalizerUpdateTimer += (float)(m_deltaTime / 1000.0f);
        bool updateTextMetrics = (m_stream.m_equalizerUpdateTimer >= 0.15f);
        if (updateTextMetrics)
        {
            m_stream.m_equalizerUpdateTimer = 0.0f;
        }

        // Base segment count on the top row (Coarsest Base LOD)
        int baseSegments = (numLODs <= 2) ? 8 : 4;

        // Display rows: Top row = Highest LOD / Coarsest Base (numLODs - 1), moving down to LOD 0 (Fine)
        for (int rowIdx = 0; rowIdx < numLODs; rowIdx++)
        {
            int lodIdx = numLODs - 1 - rowIdx;
            const auto& lodData = m_residentLODs[lodIdx];
            uint32_t totalBlocks = (uint32_t)lodData.meshletChunks.size();
            size_t totalPts = lodData.rawSplats.size();
            float totalMB = (float)(totalPts * (BytesPerRenderPoint())) / (1024.0f * 1024.0f);

            // Segments count doubles for each row down (width divides by 2)
            int numSegments = baseSegments * (1 << rowIdx);
            const float barHeight = 14.0f;

            const auto* pLevelChunks = (lodIdx < (int)m_stream.m_lodStreamChunks.size()) ? &m_stream.m_lodStreamChunks[lodIdx] : nullptr;
            size_t T = pLevelChunks ? pLevelChunks->size() : 0;

            // Lower level mip for child refinement calculation
            int childLodIdx = lodIdx - 1;
            const auto* pChildChunks = (childLodIdx >= 0 && childLodIdx < (int)m_stream.m_lodStreamChunks.size()) ? &m_stream.m_lodStreamChunks[childLodIdx] : nullptr;
            size_t TChild = pChildChunks ? pChildChunks->size() : 0;

            // Calculate residency stats
            size_t residentPts = 0;
            uint32_t exactResidentBlocks = 0;
            if (m_enableStreamingSimulation)
            {
                residentPts = (lodIdx < (int)m_stream.m_lodResidentSplats.size()) ? m_stream.m_lodResidentSplats[lodIdx] : 0;
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

            size_t totalLevelPts = (lodIdx < (int)m_stream.m_lodTotalSplats.size() && m_stream.m_lodTotalSplats[lodIdx] > 0) ? m_stream.m_lodTotalSplats[lodIdx] : totalPts;
            float rawResidentPct = (totalLevelPts > 0) ? std::min(100.0f, (float)residentPts / (float)totalLevelPts * 100.0f) : 100.0f;

            if (updateTextMetrics || m_stream.m_smoothedLodResidentBlocks[lodIdx] == 0)
            {
                m_stream.m_smoothedLodResidentPct[lodIdx] = rawResidentPct;
                m_stream.m_smoothedLodResidentBlocks[lodIdx] = exactResidentBlocks;
            }

            float residentPct = m_stream.m_smoothedLodResidentPct[lodIdx];
            uint32_t residentBlocks = m_stream.m_smoothedLodResidentBlocks[lodIdx];

            bool isActiveLOD = (lodIdx == m_selectedPreviewLOD);

            // Row Header
            // The row's label in the level's tint, brighter on the level the view draws
            ImGui::TextColored(LevelTint(lodIdx, isActiveLOD ? 0.45f : 0.55f, isActiveLOD ? 1.0f : 0.85f), "LOD %d%s [%.0f%% Resident] - %u / %u blocks (%zu pts | %.1f MB)%s",
                lodIdx, (lodIdx == 0 ? " (Fine 100%)" : (lodIdx == numLODs - 1 ? " (Coarse Base)" : "")),
                residentPct, residentBlocks, totalBlocks, totalPts, totalMB, isActiveLOD ? " [ACTIVE VIEW]" : "");

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
                    // Resident: a pastel of the level's tint (soft, still clearly its hue). Transition lock: the same hue near white, like a chunk's glow as it
                    // arrives. Silhouette (edge) lock: the level's tint inside a white outline.
                    ImU32 colBase = ImGui::ColorConvertFloat4ToU32(LevelTint(lodIdx, 0.45f, 0.88f));
                    ImU32 colHighlight = ImGui::ColorConvertFloat4ToU32(LevelTint(lodIdx, 0.28f, 1.0f));
                    if (isTrans && !isSil)
                    {
                        colBase = ImGui::ColorConvertFloat4ToU32(LevelTint(lodIdx, 0.12f, 1.0f));
                        colHighlight = IM_COL32(255, 255, 255, 255);
                    }

                    drawList->AddRectFilled(segMin, segMax, colBase, 1.0f);
                    if (isSil)
                    {
                        drawList->AddRect(segMin, segMax, IM_COL32(255, 255, 255, 255), 1.0f, 0, 1.5f);
                    }
                    else if (segMax.x - segMin.x > 3.0f)
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

        // 1. Resident (a square split into the levels' tints + "Resident")
        {
            const int legendLevels = std::clamp(numLODs, 1, 8);
            for (int l = 0; l < legendLevels; l++)
            {
                const float y0 = legP.y + 3.5f + sqSize * l / legendLevels, y1 = legP.y + 3.5f + sqSize * (l + 1) / legendLevels;
                drawList->AddRectFilled(ImVec2(legP.x, y0), ImVec2(legP.x + sqSize, y1), ImGui::ColorConvertFloat4ToU32(LevelTint(legendLevels - 1 - l, 0.45f, 0.88f)));
            }
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sqSize + 4.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Resident");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8.0f, 0.0f));
        ImGui::SameLine();

        // 2. Transition Lock (a near-white square + "Transition Lock")
        legP = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), ImGui::ColorConvertFloat4ToU32(LevelTint(0, 0.12f, 1.0f)), 1.0f);
        drawList->AddRect(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(255, 255, 255, 255), 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sqSize + 4.0f);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Transition Lock");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8.0f, 0.0f));
        ImGui::SameLine();

        // 3. Silhouette Lock (a tinted square in a white outline + "Silhouette Lock") - Always shown
        legP = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), ImGui::ColorConvertFloat4ToU32(LevelTint(0, 0.45f, 0.88f)), 1.0f);
        drawList->AddRect(ImVec2(legP.x, legP.y + 3.5f), ImVec2(legP.x + sqSize, legP.y + 3.5f + sqSize), IM_COL32(255, 255, 255, 255), 1.0f, 0, 1.5f);
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
            ShowTooltip("Evicts refined detail stream chunks (LOD 0..max-2) while keeping the top two base mip levels (max & max-1) permanently resident.");
        }

        ImGui::SameLine();
        ImGui::PushItemWidth(availWidth - 165.0f);
        if (ImGui::SliderFloat("##DecaySlider", &m_streamDecayRate, 0.0f, StreamRuntime::kMaxDecayRate, "Decay Rate: %.2f"))
        {
            m_stream.m_streamStateDirty = true;
            CalibrateDecay();
        }
        if (ImGui::IsItemHovered())
        {
            float drainSeconds = m_stream.DecayFullDrainSeconds();
            if (m_enableStreamDecay && m_streamDecayRate > 0.0f && m_stream.m_decayBytesPerSec > 0.0f)
            {
                const float inflow = m_unthrottledBandwidth ? 0.0f : m_bandwidthThrottleMBps;
                const float outflowMB = m_stream.m_decayBytesPerSec / (1024.0f * 1024.0f);
                char ref[48];
                if (m_stream.m_decayReferenceBandwidthBps > 0.0f) snprintf(ref, sizeof(ref), "%.1f MB/s", m_stream.m_decayReferenceBandwidthBps / (1024.0f * 1024.0f)); else snprintf(ref, sizeof(ref), "Full");
                ShowTooltip("Decay outflow %.1f MB/s, fixed when the toggle or slider last changed (reference bandwidth %s: a full drain of every evictable level in ~%.1f s at that bandwidth).\nThe bandwidth throttle is now the knob: current inflow %s, so the net drain is %.1f MB/s.",
                    outflowMB, ref, drainSeconds, m_unthrottledBandwidth ? "Full (uncapped)" : "", outflowMB - inflow);
            }
            else if (m_streamDecayRate <= 0.0f)
                ShowTooltip("0 = no decay. Drag right to drain unused detail out of memory.\nIndependent of bandwidth: at 10 it clears everything but the two pinned\ncoarsest levels in %.0f s on any bandwidth setting.", StreamRuntime::kDecayFullDrainSecondsAtMaxRate);
            else
                ShowTooltip("Full drain of all evictable levels in ~%.1f s, on any bandwidth setting.\nAt 10 it clears everything but the two pinned coarsest levels in %.0f s.",
                    drainSeconds, StreamRuntime::kDecayFullDrainSecondsAtMaxRate);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Checkbox("Decay", &m_enableStreamDecay))
        {
            m_stream.m_streamStateDirty = true;
            CalibrateDecay();
        }
        if (ImGui::IsItemHovered())
        {
            ShowTooltip("Toggle LRU chunk memory reclamation over time.\nHigher decay rates reclaim memory faster from the lowest priority/least needed LODs.\nDrain speed is independent of the bandwidth throttle.");
        }
        if (ImGui::Checkbox("Release Unused LOD Segments", &m_releaseUnusedSegments))
        {
            m_stream.m_streamStateDirty = true;
        }
        if (ImGui::IsItemHovered())
        {
            ShowTooltip("Evict a level's segments as soon as nothing draws them any more: zoom out, or turn away, and the finer segments fade back to their coarser parents, then leave memory, and the residency map frees up. The two coarsest levels always stay resident.\nConservative: streaming covers only what is in view (plus a small margin) down to each block's level, so released segments are not fetched again until the view needs them.\nGreedy: every block of every level down to the finest level in view is loaded, model-wide, and only what lies finer than that is released.\nOff: everything that arrives stays.");
        }
        // Streaming policy: what streams besides the view's own requests (StreamSettings::Policy)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Policy:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Conservative", m_streamingPolicy == 0)) { m_streamingPolicy = 0; m_stream.m_streamStateDirty = true; }
        if (ImGui::IsItemHovered()) ShowTooltip("Loads only what the view needs: the blocks in view (plus a small margin), each down to its own level. Nothing streams in the background.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Greedy", m_streamingPolicy == 1)) { m_streamingPolicy = 1; m_stream.m_streamStateDirty = true; }
        if (ImGui::IsItemHovered()) ShowTooltip("Keeps loading in the background regardless of the view, faces in view first. With Release Unused LOD Segments it loads every block of every level down to the finest level a visible block renders at (now level %d), model-wide, and releases only what lies finer.", m_stream.GreedyFloor());

        ImGui::Spacing();
    }

    // Refinement visualizer controls on the Renderer tab (idSuffix keeps the widget IDs distinct should the
    // block ever be drawn in a second place): the arrival-glow checkbox, and when it is on, the fade
    // duration, glow intensity and hue sliders.
    void SplatPakApp::DrawRefinementVisualizerControls(const char* idSuffix)
    {
        std::string label = std::string("Refinement Visualizer") + idSuffix;
        if (ImGui::Checkbox(label.c_str(), &m_showChunkStream))
        {
            m_stream.m_streamStateDirty = true;
        }
        if (ImGui::IsItemHovered()) ShowTooltip("Visualizer for progressive loading: every chunk that arrives is filled with a semi-transparent tint. The newest chunks -- the leading edge of the growing model -- glow bright, then settle to the regular tint and fade out over the duration below. Press Evict to watch the whole model grow back in the scheduler's order. Named in the top-left banner while it is on.");
        if (!m_showChunkStream) return;
        ImGui::Indent(12.0f);
        ImGui::PushItemWidth(150.0f);
        ImGui::SliderFloat((std::string("Fade Duration") + idSuffix).c_str(), &m_chunkStreamDuration, 0.5f, 10.0f, "%.1f s");
        if (ImGui::IsItemHovered()) ShowTooltip("How long a delivered chunk glows. It arrives a bright white-gold, cools to gold and fades out smoothly over this time.");
        ImGui::SliderFloat((std::string("Glow Intensity") + idSuffix).c_str(), &m_arrivalGlowIntensity, 0.0f, 2.0f, "%.2fx");
        if (ImGui::IsItemHovered()) ShowTooltip("Strength of the tint and of the bright leading-edge bloom. 0.25 = default; 0 hides the effect without turning the visualizer off.");
        ImGui::SliderFloat((std::string("Glow Hue") + idSuffix).c_str(), &m_arrivalGlowHue, -180.0f, 180.0f, "%.0f deg");
        if (ImGui::IsItemHovered()) ShowTooltip("Rotates every level's glow colour together around the colour wheel. At 0, level 0 is gold and level 1 its complement, azure.");
        ImGui::PopItemWidth();
        // Legend: each level's tint (Splat.hlsl kArrivalGlowLevelHue), coarsest first, as they stream in
        {
            const int levels = std::clamp((int)m_waveletResult.lodLevels.size(), 1, 8);
            ImGui::TextDisabled("Level tint:");
            for (int l = levels - 1; l >= 0; l--)
            {
                ImGui::SameLine();
                ImGui::TextColored(LevelTint(l, 0.85f, 1.0f), "L%d", l);
            }
            if (ImGui::IsItemHovered()) ShowTooltip("Each level of detail arrives in its own tint, consecutive levels across the colour wheel from one another, so you can see which level a chunk brings.");
        }
        ImGui::Unindent(12.0f);
    }


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
    void SplatPakApp::ComputeSplatSoftness()
    {
        m_splatLevel0Footprint = 0.0f;
        m_splatSoftnessRatio = 0.0f;
        m_splatDiscMatchFactor = 1.5f;
        if (!m_splatMode || m_residentLODs.empty() || m_residentLODs[0].splats.empty() || m_stream.m_lodSpacing.empty()) return;
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
        const float spacing0 = m_stream.m_lodSpacing[0];
        m_splatSoftnessRatio = (spacing0 > 0.0f) ? m_splatLevel0Footprint / spacing0 : 0.0f;
        m_splatDiscMatchFactor = std::clamp(sqrtf(8.0f) * m_splatSoftnessRatio, 1.5f, 4.0f);
        LogTransitionTrace("SplatSoftness: level 0 median footprint %.5f, spacing %.5f, ratio %.2f, disc radius factor %.2f", m_splatLevel0Footprint, spacing0, m_splatSoftnessRatio, m_splatDiscMatchFactor);
    }



    // Streaming tab: the bounding octahedron as two diamonds. Seen from above, an octahedron projects to
    // a square standing on a corner whose diagonals split it into its four upper faces; the left diamond
    // (green) shows those, the right one (pastel blue) the four lower faces, both with +X right and +Z
    // down. Faces the camera sees are lit, brighter the more directly they face it; faces streamed with them as the
    // margin (turned just away, StreamOrder::VisibleFaceMask) are half lit; hidden faces dim. A yellow dot marks the
    // camera's direction on whichever half it lies in (a dim dot on the other).
    void SplatPakApp::DrawOctahedronGlyph()
    {
        const uint8_t frontMask = m_stream.m_streamOrder.FrontFaceMask(), streamMask = m_stream.m_streamOrder.VisibleFaceMask();
        uint32_t visible = 0, margin = 0;
        for (int f = 0; f < kOctahedronFaces; f++)
        {
            if (frontMask & (1u << f)) visible++;
            else if (streamMask & (1u << f)) margin++;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (margin > 0)
            ImGui::TextWrapped("Bounding octahedron: %u of %d faces visible, %u more streamed with them at the edge; %zu blocks in this frame's load list", visible, kOctahedronFaces, margin, m_stream.m_scratchLoadList.size());
        else
            ImGui::TextWrapped("Bounding octahedron: %u of %d faces visible; %zu blocks in this frame's load list", visible, kOctahedronFaces, m_stream.m_scratchLoadList.size());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ShowTooltip("Faces of the octahedron around the model that the camera can currently see (lit). Each owns the part of the model behind it; the scheduler streams the visible faces together and the hidden ones afterwards, so the model completes in the background. Faces turned just away from the camera are streamed with the visible ones (half lit), so a face is already loading as it turns into view. The inner triangles show each face's fill progress.");

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
                // Lit by how squarely a front face faces the camera; a margin face half lit at the low end
                const bool front = (frontMask & (1u << f)) != 0, edge = !front && (streamMask & (1u << f)) != 0;
                const float t = front ? std::max(0.0f, std::min(1.0f, m_stream.m_streamOrder.FaceFacing(f))) : 0.0f;
                // Bevelled look: each face is shaded as one side of a low pyramid seen from above, lit from
                // the upper left (both halves are drawn that way so they read alike); its two ridge edges
                // (centre to rim) are light on the lit side and dark on the shaded side, its rim edge dark.
                const float lx = -0.55f, ly = 0.65f, lz = -0.52f;           // Light direction: from the upper left, above
                const float nx = sx * 0.577f, ny = 0.577f, nz = sz * 0.577f;  // Face normal of the pyramid side
                const float ndotl = nx * lx + ny * ly + nz * lz;
                const float shade = 0.55f + 0.45f * std::max(0.0f, std::min(1.0f, ndotl + 0.2f)); // 0.55 .. 1.0
                float br, bg, bb;                                            // Base colour of this face
                if (upper) { br = front ? 55.0f + 30.0f * t : 60.0f;  bg = front ? 170.0f + 70.0f * t : 66.0f; bb = front ? 85.0f + 35.0f * t : 74.0f; }
                else       { br = front ? 100.0f + 30.0f * t : 60.0f; bg = front ? 150.0f + 45.0f * t : 64.0f; bb = front ? 225.0f + 30.0f * t : 78.0f; }
                if (edge) // Half way between hidden and a front face at its dimmest
                {
                    if (upper) { br = 58.0f; bg = 118.0f; bb = 80.0f; }
                    else       { br = 80.0f; bg = 107.0f; bb = 152.0f; }
                }
                auto shaded = [&](float k, int alpha) { return IM_COL32((int)std::min(255.0f, br * k), (int)std::min(255.0f, bg * k), (int)std::min(255.0f, bb * k), alpha); };
                // Fill opacity is the face's residency: 0% with nothing loaded, 80% with every block loaded,
                // every grade between. The outline and cross bars below are always fully opaque.
                float done = 0.0f;
                if (m_stream.m_streamOrder.FaceTotal(f) > 0) done = 1.0f - (float)std::min(m_stream.m_streamOrder.FaceRemaining(f), m_stream.m_streamOrder.FaceTotal(f)) / (float)m_stream.m_streamOrder.FaceTotal(f);
                const ImU32 fill = shaded(shade, (int)(204.0f * std::max(0.0f, std::min(1.0f, done))));
                if (pass == 1)
                {
                    const bool lit = ndotl > 0.05f;
                    const ImU32 ridge = lit ? shaded(1.35f, 255) : shaded(0.45f, 255);
                    const ImU32 rim = shaded(0.40f, 255);
                    const float w = front ? 2.0f : edge ? 1.5f : 1.0f;
                    dl->AddLine(c, px, ridge, w);
                    dl->AddLine(c, pz, ridge, w);
                    dl->AddLine(px, pz, rim, w);
                    continue;
                }
                dl->AddTriangleFilled(c, px, pz, fill);
            }
            // Camera marker: its horizontal direction, on this half if the camera is on this side of the equator.
            const bool camHere = (m_stream.m_streamOrder.CameraDirection()[1] >= 0.0f) == upper;
            const float h = sqrtf(m_stream.m_streamOrder.CameraDirection()[0] * m_stream.m_streamOrder.CameraDirection()[0] + m_stream.m_streamOrder.CameraDirection()[2] * m_stream.m_streamOrder.CameraDirection()[2]);
            const float mx = (h > 1e-4f) ? m_stream.m_streamOrder.CameraDirection()[0] / h : 0.0f, mz = (h > 1e-4f) ? m_stream.m_streamOrder.CameraDirection()[2] / h : 0.0f;
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
                char b[160];
                snprintf(b, sizeof(b), "\n  face %d (%c%c%c): %u / %u, %zu edge requests %s", f, (f & 1) ? '+' : '-', (f & 2) ? '+' : '-', (f & 4) ? '+' : '-',
                    m_stream.m_streamOrder.FaceRemaining(f), m_stream.m_streamOrder.FaceTotal(f), m_stream.m_faceEdgeQueue[f].size() - m_stream.m_faceEdgeHead[f], (frontMask & (1u << f)) ? "(visible)" : (streamMask & (1u << f)) ? "(edge, streamed with the visible faces)" : "");
                tip += b;
            }
            ShowTooltip("%s", tip.c_str());
        }
    }

    // Overlay listing every toggle that currently alters what the viewport shows -- debug views, isolation
    // modes, frozen state, forced levels -- as "TAB NAME: <feature> enabled" lines, one per row, each in
    // its own colour, anchored at the top-left of the viewport next to the control panel. Its purpose is purely
    // to stop a user from reading a deliberately altered picture (splats hidden, a stale culling
    // frustum, lavender-tinted chunks, a frozen stream) as a rendering bug: whenever the model looks
    // wrong, the reason is written on the screen. Draws nothing when no such mode is active. Ordinary
    // quality/performance settings (TAA, dithering, cone culling, the occlusion volume itself) are not
    // listed -- they are the normal picture, not a departure from it.
    void SplatPakApp::DrawRenderModeBanner(float leftPanelWidth, float rightPanelWidth)
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
        if (m_showOcclusionVolumeOnly)     add(ImVec4(1.00f, 0.90f, 0.20f, 1.0f), "RENDERER: Hide Splat Layer enabled");
        if (m_showShadowMesh && m_shadowMesh && !m_shadowMesh->Empty()) add(ImVec4(1.00f, 0.90f, 0.20f, 1.0f), "RENDERER: View Shadow Mesh enabled");
        if (m_showOnlyLockedChunks)        add(ImVec4(1.00f, 0.60f, 0.20f, 1.0f), "RENDERER: Show ONLY Locked Chunks enabled");
        if (m_highlightSilhouetteChunks)   add(ImVec4(0.78f, 0.68f, 1.00f, 1.0f), "RENDERER: Highlight Edge Chunks enabled");
        if (m_showChunkStream)             add(ImVec4(1.00f, 0.62f, 0.20f, 1.0f), "RENDERER: Refinement Visualizer enabled");
        if (m_showClusterHeatmap)          add(ImVec4(1.00f, 0.45f, 0.35f, 1.0f), m_heatmapSource == 1 ? "RENDERER: Detail Heatmap Cluster Cubes enabled" : "RENDERER: Density Heatmap Cluster Cubes enabled");
        if (m_showHeatmapWireframe)        add(ImVec4(0.92f, 0.82f, 0.60f, 1.0f), "RENDERER: Cube Outlines enabled");
        if (m_showOctreeVisualizer)        add(ImVec4(1.00f, 0.75f, 0.20f, 1.0f), "RENDERER: Macro Clusters enabled");
        if (m_showGlobalBounds)            add(ImVec4(0.40f, 0.60f, 1.00f, 1.0f), "RENDERER: Global Model Bounds enabled");
        if (m_state.renderMode == 4)       add(ImVec4(0.55f, 0.85f, 1.00f, 1.0f), "SPLAT: 3D Gaussian package drawn as splats, SH degree %u, opaque discs", m_splatParams.shDegree);
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
        // machine without mesh shaders says exactly which stages it lacks (SplatRenderer::RenderPath).
        if (m_pRenderer && m_pRenderer->GetRenderPath() != SplatRenderer::RenderPath::MeshShaders)
        {
            const auto& caps = m_pRenderer->GetGpuCapabilities();
            const ImVec4 hw(1.00f, 0.40f, 0.30f, 1.0f);
            const bool forced = m_pRenderer->IsRenderPathForced();
            const char* why = forced ? "bypassed (forced)" : (caps.meshPipelineFailed ? "pipeline failed" : "unavailable");
            add(hw, "RENDERER: Amplification shader %s, VS chunk culling enabled", why);
            add(hw, "RENDERER: Mesh shader %s, instanced VS splats enabled", why);
            if (m_pRenderer->GetRenderPath() == SplatRenderer::RenderPath::VertexShadersSM5)
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
    float SplatPakApp::FitDistanceForViewport() const
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
        if (!m_residentLODs.empty() && !m_residentLODs.back().rawSplats.empty())
        {
            const auto& src = m_residentLODs.back().rawSplats;
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
    void SplatPakApp::DrawResetViewButton(float leftPanelWidth)
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
            if (ImGui::IsItemHovered()) ShowTooltip("Returns the camera to the launch view: the default angle, centred on the model, fitted between the control panels. The culling camera, if detached, is left where it is.");
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
    // A collapsing header that reopens as the user left it: its open state is kept by label in m_menuOpen and saved to
    // config.json (menu_open) whenever it changes
    bool SplatPakApp::MenuHeader(const char* label, bool defaultOpen)
    {
        auto it = m_menuOpen.find(label);
        const bool stored = it != m_menuOpen.end() ? it->second : defaultOpen;
        ImGui::SetNextTreeNodeOpen(stored, ImGuiCond_Always);
        const bool open = ImGui::CollapsingHeader(label);
        // A click toggles it under the mouse; a header the window skips (folded, not yet laid out) returns false unhovered
        if (open != stored && ImGui::IsItemHovered())
        {
            m_menuOpen[label] = open;
            SaveConfigFile();
        }
        return open;
    }

    bool SplatPakApp::RightPanelSectionHeader(int slot, int section, int& swapSlotA, int& swapSlotB)
    {
        static const char* s_titles[kRightSectionCount] = {
            "Real-Time Performance & Stage Timings", "5-Tier Compression Results", "LOD Residency",
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
            ShowTooltip(
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

    bool SplatPakApp::ProjectLiveCamera(const XMFLOAT3& world, ImVec2& screen, float& viewDepth) const
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
    void SplatPakApp::SetLoupePinned(bool pinned)
    {
        m_loupePinned = pinned;
        m_loupeAnchorValid = false;
        if (!pinned || m_Width == 0 || m_Height == 0) return;
        XMFLOAT3 eye, fwd; XMMATRIX viewProj;
        LiveCamera(m_target, m_distance, m_pitch, m_yaw, (float)m_Width / (float)m_Height, eye, fwd, viewProj);
        const float zoom = std::max(1.0f, m_loupeEffectiveZoom);
        const ImVec2 half(0.5f * m_loupeSize.x / zoom, 0.5f * m_loupeSize.y / zoom);
        const std::vector<SplatPoint>& pts = !m_rendererRawSplats.empty() ? m_rendererRawSplats : m_rawSplats;
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

    void SplatPakApp::DrawLoupeOverlay()
    {
        m_state.loupeEnabled = false;
        if (!m_loupeEnabled || m_Width == 0 || m_Height == 0) return;
        // A push that would leave the window is not taken (below the full-height left panel there is no room)
        const auto pushOutOfPanels = [this](ImVec2& c, ImVec2 half)
        {
            for (const ImVec4& p : { m_leftPanelRect, m_rightPanelRect, m_presentPanelRect })
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
        // m_loupePos is the inset's home, where it was put; it is drawn there pushed clear of the panels and held inside the
        // window, without moving the home, so a panel opening over it displaces it only while the panel needs the room and
        // it snaps back once the space is free again
        ImVec2 shown = m_loupePos;
        const float menuBar = 22.0f;
        // Held inside the window, then pushed clear of the panels, a few times over: a home below the window (saved with a
        // taller one) is only over a panel once it is pulled inside, and one push can land it against the next panel
        for (int pass = 0; pass < 3; pass++)
        {
            shown.x = std::clamp(shown.x, 0.0f, std::max(0.0f, (float)m_Width - inset.x));
            shown.y = std::clamp(shown.y, menuBar, std::max(menuBar, (float)m_Height - inset.y));
            // The inset with its 1-pixel frame (and a pixel to spare) is what stays clear of the panels
            const ImVec2 halfInset(0.5f * inset.x + 2.0f, 0.5f * inset.y + 2.0f);
            ImVec2 c(shown.x + 0.5f * inset.x, shown.y + 0.5f * inset.y);
            pushOutOfPanels(c, halfInset);
            shown = ImVec2(c.x - 0.5f * inset.x, c.y - 0.5f * inset.y);
        }
        shown.x = std::clamp(shown.x, 0.0f, std::max(0.0f, (float)m_Width - inset.x));
        shown.y = std::clamp(shown.y, menuBar, std::max(menuBar, (float)m_Height - inset.y));
        const float destX = std::floor(shown.x);
        const float destY = std::floor(shown.y);
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
            // Pinned: a faint frosted grey tint and a darker border (the inset still shows the area under it untinted)
            dl->AddRectFilled(boxMin, boxMax, IM_COL32(205, 208, 214, 28));
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
    void SplatPakApp::DrawControlHints(float leftPanelWidth, float rightPanelWidth)
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
    // Package Bake tab (right after baking) and the Renderer tab (while viewing). The shape is
    // set by the bake: the volume is baked into the .gstx, so the viewer shows exactly what was packaged
    // and offers nothing that would change it. "Hide
    // Splat Layer" is intentionally independent of "Show Occlusion Volume" -- wanting to
    // just look at the volume shouldn't require also turning on splat culling against it.
    void SplatPakApp::DrawOcclusionVolumeControls()
    {
        if (ImGui::Checkbox("Show Occlusion Volume", &m_enableOcclusionCulling))
        {
            m_showOcclusionSetting = m_enableOcclusionCulling;
            SaveConfigFile(); // Remembered (show_occlusion_volume): the next load or bake follows it
        }
        if (ImGui::IsItemHovered())
        {
            if (HasOccluderMesh())
                ShowTooltip("Draws the baked interior occlusion volume (a mesh of %zu triangles) and depth-tests the splats against it, so far-side splats don't show through gaps in the near side; the shadow mesh fills the gaps with the model's colour. On by default.", m_occluderMesh->levels[0].TriangleCount());
            else
                ShowTooltip("Draws the baked interior occlusion volume (%zu cubes) and depth-tests the splats against it, so far-side splats don't show through gaps in the near side. On by default.", m_occlusionVoxels.size());
        }

        // The model views in place of the splats: the occlusion volume alone, and the shadow mesh
        const bool modelHasMesh = m_shadowMesh && !m_shadowMesh->Empty();
        if (!m_occlusionVoxels.empty() || HasOccluderMesh())
        {
            if (ImGui::Checkbox("Hide Splat Layer", &m_showOcclusionVolumeOnly) && m_showOcclusionVolumeOnly) m_showShadowMesh = false;
            if (ImGui::IsItemHovered()) ShowTooltip("Debug view: hides the splats and draws only the occlusion volume (the smooth occluder mesh, or its cubes until that is built). Works regardless of 'Show Occlusion Volume' above.");
        }
        else
        {
            m_showOcclusionVolumeOnly = false;
        }

        // View Shadow Mesh: the package's shadow mesh, solid and coloured from the splats, in place of the splats
        if (modelHasMesh || m_shadowMeshJob)
        {
            const bool hasMesh = modelHasMesh;
            if (!hasMesh) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
            bool view = m_showShadowMesh && hasMesh;
            if (ImGui::Checkbox("View Shadow Mesh", &view) && hasMesh)
            {
                m_showShadowMesh = view;
                if (view) m_showOcclusionVolumeOnly = false;
                SaveConfigFile(); // Remembered (show_shadow_mesh)
            }
            if (!hasMesh) ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                if (hasMesh) ShowTooltip("Debug view: draws the shadow mesh baked into the package (%s), solid and in the splats' base colour, instead of the splats. Unreal uses it for shadows, Lumen and collision. It follows the visible surface; Hide Splat Layer shows the occlusion volume instead.", ShadowMeshSummary().c_str());
                else if (m_shadowMeshJob) ShowTooltip("The shadow mesh is being built after the bake; the view is available when it is done.");
                else ShowTooltip("This model has no shadow mesh: it could not be built from these splats.");
            }
            if (hasMesh && m_showShadowMesh && m_shadowMesh->levels.size() > 1)
            {
                ImGui::SameLine();
                ImGui::PushItemWidth(120.0f * m_uiScale);
                char preview[48];
                snprintf(preview, sizeof(preview), "Level %d (%zu tris)", m_shadowMeshLevel, m_shadowMesh->levels[std::min<size_t>(m_shadowMeshLevel, m_shadowMesh->levels.size() - 1)].TriangleCount());
                if (ImGui::BeginCombo("##shadowMeshLevel", preview))
                {
                    for (int l = 0; l < (int)m_shadowMesh->levels.size(); l++)
                    {
                        char item[48];
                        snprintf(item, sizeof(item), "Level %d (%zu tris)", l, m_shadowMesh->levels[l].TriangleCount());
                        if (ImGui::Selectable(item, l == m_shadowMeshLevel)) m_shadowMeshLevel = l;
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
            }
        }

        // Mip chain (v6+ packages): which level the renderer is drawing, with a manual override for
        // comparing levels. Automatic selection is by projected cell size (see
        // SplatRenderer::SelectOcclusionMip), never by per-chunk splat LOD.
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
            if (ImGui::IsItemHovered()) ShowTooltip("The volume is baked as %u nested mips (mip 0 = the fine skin; each further mip a coarser level lying inside it). Auto draws the finest mip whose cell still covers a few pixels at the model's nearest point, with hysteresis, so the cube size grows with the splat LOD in view. Force a mip here to compare levels.", m_occlusionMips.mipCount);
        }
    }

    // Rebuilds the occlusion volume, or clears it when generation is off. Called on every full pipeline
    // recompute. A freshly baked volume is shown straight
    // away unless Show Occlusion Volume was switched off (m_showOcclusionSetting), which a bake respects.
    void SplatPakApp::RefreshOcclusionVolume()
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

    void SplatPakApp::BuildOcclusionGrid()
    {
        const auto& sourcePoints = !m_rawSplats.empty() ? m_rawSplats : m_rendererRawSplats;
        std::string trace;
        OcclusionVolume::BuildGrid(sourcePoints, m_aabbMin, m_aabbMax, m_occlusionGrid, trace, 0, m_occlusionShaveBiasCells);
        if (!trace.empty()) LogTransitionTrace("%s", trace.c_str());
    }

    void SplatPakApp::BuildOcclusionVolume()
    {
        if (!m_occlusionGrid.valid) BuildOcclusionGrid();
        std::string trace;
        OcclusionVolume::Bake(m_occlusionGrid, 0.0f, OcclusionVolume::ColorGrade{}, m_occlusionVoxels, m_occlusionMips, trace);
        if (!trace.empty()) LogTransitionTrace("%s", trace.c_str());
    }

    // Voxelizes the loaded point cloud into a density heatmap for the cluster-cube visualizer
    void SplatPakApp::RebuildHeatmapClusterCubes()
    {
        BuildHeatmapClusterCubes(!m_rawSplats.empty() ? m_rawSplats : m_rendererRawSplats, m_aabbMin, m_aabbMax, m_detailGrid, m_heatmapClusterCubes);
    }

    void SplatPakApp::BuildHeatmapClusterCubes(const std::vector<SplatPoint>& points, const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax,
                                               const DetailGrid& detail, std::vector<HeatmapClusterCube>& out) const
    {
        out.clear();
        const auto& sourcePoints = points;
        if (sourcePoints.empty()) return;

        // 1. Determine global model extents
        XMFLOAT3 gMin = aabbMin;
        XMFLOAT3 gMax = aabbMax;
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

        out.reserve(rx * ry * rz);
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

                    out.push_back(cube);
                }
            }
        }

        // 5. Normalize densities using log scale for vibrant contrast across sparse and dense areas
        float logMin = std::log(std::max(1.0f, minDensity));
        float logMax = std::log(std::max(2.0f, maxDensity));
        float logRange = std::max(0.001f, logMax - logMin);

        for (auto& cube : out)
        {
            if (cube.pointCount > 0 && m_heatmapSource == 1)
            {
                // Detail source: the streaming order itself (mean detail score over the cube's cells)
                cube.normDensity = detail.Valid() ? detail.SampleBox(cube.aabbMin, cube.aabbMax) : 0.0f;
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
    void SplatPakApp::DrawOctreeVisualizer()
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
        if (m_showGlobalBounds && (!m_rawSplats.empty() || !m_rendererSplats.empty()))
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

    // Per-frame entry point: builds the UI, advances the streaming simulation and camera, then hands off to SplatRenderer
    void SplatPakApp::OnRender()
    {
        // A finished background operation swaps its result in before anything reads the model this frame
        m_op.Tick();
        PollShadowMeshBuild();
        m_framesRendered++;
        // Safely execute any modal/file operations before beginning the ImGui frame (never during an operation)
        if (!m_op.Busy()) ExecutePendingAction();

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
        if (!m_lodCalFolder.empty()) StepLodCalibration();
        if (m_streamTest) StepStreamTest();
        if (!m_cliBakeOutput.empty()) StepCliBake();
        if (!m_viewCapFolder.empty()) StepViewCapture();
        StepPresentationShot();
        StepPendingRdoProfile();
        StepReencode();
        // A scripted run waits for the whole model to stream in, which Release Unused LOD Segments (on by default) never allows
        if (Unattended()) { m_releaseUnusedSegments = false; m_streamingPolicy = 1; } // Scripted runs wait for the whole model
        const auto tUi = std::chrono::high_resolution_clock::now();
        BuildUI();
        DrawOperationDialog();
        m_frameUiMs = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - tUi).count();
        m_state.time += (float)(m_deltaTime / 1000.0);
        m_state.enableDithering = m_enableDitheredTransitions;
        m_state.highlightSilhouette = m_highlightSilhouetteChunks;
        m_state.showChunkStream = m_showChunkStream;
        m_state.arrivalGlowIntensity = m_arrivalGlowIntensity;
        m_state.arrivalGlowHue = m_arrivalGlowHue;
        m_state.autoSplatSize = m_autoSplatSize;
        for (int l = 0; l < 8; l++) m_state.lodRadius[l] = (l < (int)m_stream.m_lodSpacing.size()) ? m_stream.m_lodSpacing[l] * m_autoSplatCoverage : 0.0f;
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
            const float spacing = (l < (int)m_stream.m_lodSpacing.size()) ? m_stream.m_lodSpacing[l] : 0.0f;
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
        // the same open shell the splats do.
        m_state.enableOcclusionCulling = m_enableOcclusionCulling;
        m_state.showOcclusionVolumeOnly = m_showOcclusionVolumeOnly;
        // View Shadow Mesh: the drawn level, subdivided twice and smooth-shaded, rebuilt when the mesh or the level changes
        {
            const bool hasMesh = m_shadowMesh && !m_shadowMesh->Empty();
            m_state.showShadowMesh = m_showShadowMesh && hasMesh && !m_state.showOcclusionVolumeOnly;
            if (hasMesh)
            {
                const int level = std::clamp(m_shadowMeshLevel, 0, (int)m_shadowMesh->levels.size() - 1);
                if (m_shadowMeshGpuSerial != m_shadowMeshSerial || m_shadowMeshGpuLevel != level)
                {
                    BuildShadowMeshDisplay(m_shadowMesh->levels[level], 2, m_shadowMeshGpu);
                    m_shadowMeshGpuSerial = m_shadowMeshSerial;
                    m_shadowMeshGpuLevel = level;
                    m_shadowMeshVersion++;
                }
                m_state.pShadowMeshVertices = m_shadowMeshGpu.vertices.data();
                m_state.shadowMeshVertexCount = m_shadowMeshGpu.VertexCount();
                m_state.pShadowMeshIndices = m_shadowMeshGpu.indices.data();
                m_state.shadowMeshIndexCount = (uint32_t)m_shadowMeshGpu.indices.size();
            }
            else
            {
                if (m_shadowMeshGpuSerial != UINT32_MAX) { m_shadowMeshGpuSerial = UINT32_MAX; m_shadowMeshGpuLevel = -1; m_shadowMeshGpu = ShadowMeshDisplay{}; m_shadowMeshVersion++; }
                m_state.pShadowMeshVertices = nullptr; m_state.shadowMeshVertexCount = 0;
                m_state.pShadowMeshIndices = nullptr; m_state.shadowMeshIndexCount = 0;
            }
            m_state.shadowMeshVersion = m_shadowMeshVersion;
            const bool hasOccluder = HasOccluderMesh() && !m_occluderMeshGpu.vertices.empty();
            m_state.pOccluderMeshVertices = hasOccluder ? m_occluderMeshGpu.vertices.data() : nullptr;
            m_state.occluderMeshVertexCount = hasOccluder ? m_occluderMeshGpu.VertexCount() : 0;
            m_state.pOccluderMeshIndices = hasOccluder ? m_occluderMeshGpu.indices.data() : nullptr;
            m_state.occluderMeshIndexCount = hasOccluder ? (uint32_t)m_occluderMeshGpu.indices.size() : 0;
            m_state.occluderMeshVersion = m_occluderMeshVersion;
        }
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
            if (m_lodCalCaptureRequested && m_lodCalIndex < m_lodCalShots.size())
            {
                RgbaImage& img = m_lodCalShots[m_lodCalIndex].image;
                m_pRenderer->TakeSceneCapture(img.pixels, img.width, img.height);
            }
            if (m_presentShotRequested) SavePresentationShot();
            if (m_viewCapRequested && m_viewCapIndex < m_viewCapImages.size())
            {
                RgbaImage& img = m_viewCapImages[m_viewCapIndex];
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
            LogTransitionTrace("!!! SplatPakApp::OnRender std::exception: '%s', deviceRemovedReason=0x%08X !!!", e.what(), (uint32_t)removeReason);
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
            LogTransitionTrace("!!! SplatPakApp::OnRender UNKNOWN EXCEPTION CAUGHT, deviceRemovedReason=0x%08X !!!", (uint32_t)removeReason);
            if (removeReason != S_OK)
            {
                std::stringstream ss;
                ss << "GPU device removed / lost (0x" << std::hex << (uint32_t)removeReason << ").";
                ReportDeviceLostAndExit(ss.str());
            }
            else
            {
                Trace("SplatPakApp::OnRender: transient render/present glitch; continuing on next frame.\n");
            }
        }
    }

    void SplatPakApp::SetSplatDrawAsDiscs(bool asSplats)
    {
        m_splatDrawAsDiscs = asSplats;
        // The render mode follows SplatRenderMode() at the next state selection; re-stamp the chunks'
        // dilationMorph now (the sorted pipeline's fade weight, or the splat value) without a re-partition
        const float morph = m_splatDrawAsDiscs ? 0.0f : 1.0f;
        for (auto& mc : m_stream.m_rendererMeshletChunks) mc.dilationMorph = morph;
        for (auto& lod : m_residentLODs) for (auto& mc : lod.meshletChunks) mc.dilationMorph = morph;
        m_stream.m_streamStateDirty = true;
    }

    // LOD quality test (see SplatPakApp.h). Runs on the static per-level sets (streaming off), so each capture
    // is exactly one level everywhere -- the streamed view keeps refined detail when the target coarsens,
    // which would hide the level being measured. Before that it waits for the stream to land completely,
    // which doubles as the check that streaming carries on to level 0 (Release Unused LOD Segments off).
    void SplatPakApp::StepLodQualityTest()
    {
        const int maxLevel = (int)m_waveletResult.lodLevels.size() - 1;
        if (!m_lodTestRunning)
        {
            if (maxLevel <= 0 || (int)m_stream.m_lodSpacing.size() <= maxLevel || m_residentLODs.empty()) return;
            if (m_enableStreamingSimulation && (m_stream.m_totalStreamBytes <= 0.0f || m_stream.m_simulatedBytesDelivered < m_stream.m_totalStreamBytes * 0.999f))
            {
                m_lodTestShotSeconds += m_deltaTime / 1000.0;
                return;
            }
            LogTransitionTrace("LOD quality test: stream fully resident after %.1f s (%.1f MB); starting captures", m_lodTestShotSeconds, m_stream.m_simulatedBytesDelivered / (1024.0f * 1024.0f));
            for (int lvl = 0; lvl <= maxLevel; lvl++)
            {
                size_t resident = 0, total = 0;
                if (lvl < (int)m_stream.m_lodStreamChunks.size()) for (const auto& c : m_stream.m_lodStreamChunks[lvl]) { total++; if (c.isResident) resident++; }
                LogTransitionTrace("LOD quality test: level %d resident %zu / %zu blocks", lvl, resident, total);
            }

            m_autoLOD = false;
            m_autoRotate = false;
            m_enableStreamDecay = false;
            if (m_detachCamera) SetDetachCamera(false);
            m_enableStreamingSimulation = false;

            // Splat packages: both drawing modes (sorted Gaussians, then opaque splat discs); other packages draw one way.
            const int drawModes = m_splatMode ? 2 : 1;
            for (int mode = 0; mode < drawModes; mode++)
            for (int rule = 0; rule < 2; rule++)
            {
                const bool screenSpace = (rule == 1);
                for (int L = 1; L <= maxLevel; L++)
                {
                    const float d = m_stream.LodSwitchDistance(L, screenSpace) * 1.001f;
                    auto add = [&](int level, bool gauss, int repeat, float cap = 0.0f)
                    {
                        LodTestShot s;
                        s.screenSpaceRule = screenSpace; s.switchLevel = L; s.distance = d;
                        s.level = level; s.coarseAsGaussians = gauss; s.coarseGaussianCap = cap; s.repeat = repeat; s.asSplats = (mode == 1);
                        m_lodTestShots.push_back(std::move(s));
                    };
                    add(0, false, 0);
                    add(0, false, 1);
                    add(L - 1, false, 0);
                    add(L, false, 0);
                    if (mode == 0) // Coarse levels as their own Gaussians: a sorted-Gaussian option only (the splat pipeline sizes its discs from lodRadius)
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
            m_lodTestSavedDrawAsDiscs = m_splatDrawAsDiscs;
            LogTransitionTrace("LOD quality test: %zu captures at %ux%u, yaw %.3f pitch %.3f, pixel threshold %.2f", m_lodTestShots.size(), m_Width, m_Height, m_yaw, m_pitch, m_lodPixelSpacing);
        }

        if (m_lodTestIndex >= m_lodTestShots.size()) return;
        LodTestShot& shot = m_lodTestShots[m_lodTestIndex];

        if (m_lodTestCaptureRequested)
        {
            if (shot.image.Empty()) return; // Taken after this frame's render (see OnRender)
            LogTransitionTrace("LOD quality test: %zu/%zu %s, %s switch %d, d=%.3f, level %d%s%s captured after %.1f s",
                m_lodTestIndex + 1, m_lodTestShots.size(), shot.asSplats ? "splats" : "gaussians", shot.screenSpaceRule ? "screen" : "legacy", shot.switchLevel, shot.distance,
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
                if (m_splatDrawAsDiscs != m_lodTestSavedDrawAsDiscs) SetSplatDrawAsDiscs(m_lodTestSavedDrawAsDiscs);
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
            if (m_splatMode && m_splatDrawAsDiscs != shot.asSplats) SetSplatDrawAsDiscs(shot.asSplats);
            UpdatePreviewSplats();
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

    // Presentation Mode: its own panel under the analytics panel. Each subsection is a list of presets; picking one sets
    // the view, the visualizers and the loupe to demo a feature. Clear in each removes the model and resets the view.
    void SplatPakApp::DrawPresentationPanel(float x, float width, float maxHeight)
    {
        const float y = (m_rightPanelRect.w > m_rightPanelRect.y) ? m_rightPanelRect.w + 6.0f : 30.0f;
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
        // Down to just above the status bar at most (it scrolls beyond that)
        const float room = std::min(maxHeight, (float)m_Height - 40.0f - y);
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, std::max(ImGui::GetFrameHeight(), room)));
        SetNextSidePanelCollapse(m_presentPanelRect, m_presentPanelCollapsed);
        ImGui::Begin("Presentation Mode", nullptr, 0);

        if (MenuHeader("Splat Loading##Presentation"))
        {
            ImGui::Indent();
            // A press applies the preset, also on the one already chosen (to put things back after a change)
            auto preset = [this](const char* label, int value, const char* tip) -> bool
            {
                const bool pressed = ImGui::RadioButton(label, m_presentSplatPreset == value);
                if (ImGui::IsItemHovered()) ShowTooltip("%s", tip);
                if (pressed) m_presentSplatPreset = value;
                return pressed;
            };
            if (preset("Clear##SplatPreset", kPresetClear, "Removes the model and resets the view, ready to load one."))
            {
                ApplyPresentationClear();
                SchedulePresentationShot("Splat Loading - Clear");
            }
            if (preset("Points##SplatPreset", kPresetPoints, "Splat centres in the view, and the loupe in the bottom-right corner showing them too.\nA model loaded next comes up this way."))
            {
                ResetPresentationCamera();
                m_splatViz = 3; m_loupeViz = 3; m_loupeEnabled = true; PlaceLoupeBottomRight();
                SchedulePresentationShot("Splat Loading - Points");
            }
            if (preset("Outlines##SplatPreset", kPresetOutlines, "Splat outlines in the view and in the loupe (bottom right)."))
            {
                ResetPresentationCamera();
                m_splatViz = 1; m_loupeViz = 1; m_loupeEnabled = true; PlaceLoupeBottomRight();
                SchedulePresentationShot("Splat Loading - Outlines");
            }
            if (preset("Full##SplatPreset", kPresetFull, "The full splat render in the view; the loupe stays as it is."))
            {
                ResetPresentationCamera();
                m_splatViz = 0;
                SchedulePresentationShot("Splat Loading - Full");
            }
            if (preset("Culling##SplatPreset", kPresetCulling, "The full render with the culling camera detached where the view is: the observer looks at the frozen frustum,\nshowing in colour what that camera draws and in red what it culls (outside the frustum, the far half, hidden behind the occluder).\nThe arrow keys turn and tilt the model under it."))
            {
                m_splatViz = 0;
                m_loupeEnabled = false;
                if (!m_detachCamera) SetDetachCamera(true);
                m_autoRotate = false;
                SchedulePresentationShot("Splat Loading - Culling");
            }
            ImGui::Unindent();
        }
        if (MenuHeader("Streaming##Presentation"))
        {
            ImGui::Indent();
            if (ImGui::RadioButton("Clear##StreamPreset", m_presentStreamPreset == kPresetClear))
            {
                m_presentStreamPreset = kPresetClear;
                ApplyPresentationClear();
                SchedulePresentationShot("Streaming - Clear");
            }
            if (ImGui::IsItemHovered()) ShowTooltip("Removes the model and resets the view, ready to load one.");
            ImGui::Unindent();
        }

        RecordSidePanel(m_presentPanelRect, m_presentPanelCollapsed);
        ImGui::End();
    }

    void SplatPakApp::ResetPresentationCamera()
    {
        if (m_detachCamera) SetDetachCamera(false);
        m_autoRotate = false;
    }

    void SplatPakApp::ApplyPresentationClear()
    {
        CloseDataset();
        m_splatViz = 0;
        m_loupeViz = 0;
        m_loupeEnabled = false;
        ResetPresentationCamera();
        m_yaw = 0.6f;
        m_pitch = 0.35f;
        m_target = m_center;
        ApplyFitDistance();
    }

    // ---- Compression (RDO): docs/design/gstx-rdo.md, phase 1 ---------------------------------------------------------

    // The menu on the Package Bake tab: the progress of an analysis while one runs, the scan's size-against-quality curve
    // with the chosen point, the target (size, ratio, quality, or streaming time -- per view, the only bandwidth setting) with
    // Analyse at its bottom right, the result and the settings it implies, the override, and RDO profiles
    void SplatPakApp::DrawRdoMenu()
    {
        if (!MenuHeader("Compression (RDO)")) return;
        ImGui::Indent();
        const double mib = 1024.0 * 1024.0;
        const bool scanLoaded = !m_rawSplats.empty() && !m_isLoadedFromSFLW && m_sourceSHDegree > 0 && !m_waveletResult.lodLevels.empty();
        const bool analysing = m_op.Busy() && m_op.Title() == "Analysing the compression";
        // The analysis measures the baked pyramid: only a pending change to it (levels, deadband, chunks) must be baked first;
        // a pending SH change is what the analysis is for
        const BakeParams current = CurrentBakeParams();
        const bool pyramidPending = current.maxLODs != m_bakedParams.maxLODs || current.deadbandMM != m_bakedParams.deadbandMM || current.chunkSize != m_bakedParams.chunkSize;

        // Progress while it runs, otherwise where things stand
        if (analysing)
        {
            const float f = m_op.Fraction();
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_rdoStart).count();
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "%.0f%%", f * 100.0f);
            ProgressBarCentred(f, ImVec2(-1.0f, 0.0f), overlay);
            if (f > 0.02f) ImGui::TextDisabled("%s -- %.0f s elapsed, about %.0f s left", m_op.StageName().c_str(), elapsed, elapsed * (1.0 - f) / f);
            else ImGui::TextDisabled("%s -- %.0f s elapsed", m_op.StageName().c_str(), elapsed);
        }
        else if (!scanLoaded) ImGui::TextDisabled("Open a .ply scan with spherical harmonics to analyse it");
        else if (pyramidPending) ImGui::TextDisabled("Bake the pending level and deadband changes first");
        else if (!m_rdo.Valid()) ImGui::TextDisabled("Analyse draws this scan's size-against-quality curve");
        else if (RdoStale()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Measured for another scan or bake: analyse again");
        else ImGui::TextDisabled("%zu encodings measured in %.0f s", m_rdo.points.size(), m_rdo.seconds);

        bool changed = false;
        if (m_rdo.Valid())
        {
            DrawRdoPlot();
            int mode = (int)m_rdoTarget.mode;
            changed |= ImGui::RadioButton("Size##rdo", &mode, 0); ImGui::SameLine();
            changed |= ImGui::RadioButton("Ratio##rdo", &mode, 1); ImGui::SameLine();
            changed |= ImGui::RadioButton("Quality##rdo", &mode, 2); ImGui::SameLine();
            changed |= ImGui::RadioButton("Streaming time##rdo", &mode, 3);
            if (ImGui::IsItemHovered()) ShowTooltip("The only bandwidth setting: how fast a typical close view of the model streams in over a link.\n"
                                                          "A viewer downloads the blocks a view needs, coarse first, not the whole package.");
            m_rdoTarget.mode = (RdoAnalysis::TargetMode)mode;
        }
        // Analyse: at the bottom right of the graph, under the Streaming time toggle
        {
            const float buttonWidth = 110.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvailWidth() - buttonWidth));
            if (ImGui::Button("Analyse", ImVec2(buttonWidth, 0.0f)) && scanLoaded && !pyramidPending && !analysing) StartRdoAnalysis();
            if (ImGui::IsItemHovered())
                ShowTooltip("Measures every spherical-harmonics encoding of this scan (degree, and bits per band): its package size, exactly as Save\n"
                                  "would compress it, its colour fidelity against the source, and what a typical view downloads. About a minute.");
        }

        if (m_rdo.Valid())
        {
            ImGui::PushItemWidth(120.0f);
            auto inputD = [&](const char* label, double& v, float step, float fast, int decimals, const char* tip)
            {
                float f = (float)v;
                if (ImGui::InputFloat(label, &f, step, fast, decimals)) { v = std::max(0.0f, f); changed = true; }
                if (ImGui::IsItemHovered()) ShowTooltip("%s", tip);
            };
            switch (m_rdoTarget.mode)
            {
                case RdoAnalysis::TargetMode::Size:    inputD("MB, at most##rdoSize", m_rdoTarget.sizeMB, 1.0f, 10.0f, 1, "The largest package to accept: the best quality that fits is chosen."); break;
                case RdoAnalysis::TargetMode::Ratio:   inputD("x smaller than the source##rdoRatio", m_rdoTarget.ratio, 0.1f, 1.0f, 2, "The source file's size over the package's, at least."); break;
                case RdoAnalysis::TargetMode::Quality: inputD("dB colour fidelity, at least##rdoQ", m_rdoTarget.qualityDb, 0.5f, 2.0f, 1, "The smallest package that keeps at least this colour fidelity (the graph's vertical axis)."); break;
                case RdoAnalysis::TargetMode::Streaming:
                {
                    static const char* s_links[] = { "3G (0.2 MB/s)", "4G (2 MB/s)", "5G (7.5 MB/s)", "Custom" };
                    static const double s_linkMBps[] = { 0.2, 2.0, 7.5 };
                    if (ImGui::Combo("Network##rdoNet", &m_rdoNetwork, s_links, IM_ARRAYSIZE(s_links)))
                    {
                        if (m_rdoNetwork < 3) m_rdoTarget.bandwidthMBps = s_linkMBps[m_rdoNetwork];
                        changed = true;
                    }
                    if (m_rdoNetwork == 3) inputD("MB/s##rdoBw", m_rdoTarget.bandwidthMBps, 0.1f, 1.0f, 2, "The link's bandwidth.");
                    inputD("s to a view's first picture##rdoFirst", m_rdoTarget.firstViewSeconds, 0.5f, 2.0f, 1, "A typical close view's coarse picture (its share of the two coarsest levels) must arrive within this time.");
                    inputD("s to a view's full detail##rdoFull", m_rdoTarget.fullDetailSeconds, 1.0f, 10.0f, 1, "A typical close view's full detail (the blocks it needs, at every level) must arrive within this time.");
                    break;
                }
            }
            ImGui::PopItemWidth();
            if (changed && !m_rdoOverride) ApplyRdoChoice(RdoAnalysis::Pick(m_rdo, m_rdoTarget, (double)m_sourceFileBytes));

            if (m_rdoChoice >= 0 && m_rdoChoice < (int)m_rdo.points.size())
            {
                const RdoPoint& p = m_rdo.points[m_rdoChoice];
                const double ratio = p.bytes > 0.0 ? (double)m_sourceFileBytes / p.bytes : 0.0;
                ImGui::Text("-> %.1f MB (%.1fx), colour fidelity %.1f dB", p.bytes / mib, ratio, p.psnr);
                ImGui::TextDisabled("   %s%s", p.settings.Describe().c_str(), m_rdoOverride ? "" : "  (chosen by the target)");
                if (p.viewBytes > 0.0)
                {
                    const double bw = std::max(0.01, m_rdoTarget.bandwidthMBps) * mib;
                    ImGui::TextDisabled("   A typical view: first picture %.1f MB (%.1f s), full detail %.1f MB (%.1f s) at %.1f MB/s",
                        p.viewFirstBytes / mib, p.viewFirstBytes / bw, p.viewBytes / mib, p.viewBytes / bw, m_rdoTarget.bandwidthMBps);
                }
                if (p.regionSaving > 0.005)
                    ImGui::TextDisabled("   Per-region settings (format 15) would save about %.1f%% here", 100.0 * p.regionSaving);
            }
            if (ImGui::Checkbox("Override settings##rdo", &m_rdoOverride) && !m_rdoOverride)
                ApplyRdoChoice(RdoAnalysis::Pick(m_rdo, m_rdoTarget, (double)m_sourceFileBytes));
            if (ImGui::IsItemHovered()) ShowTooltip("Set the harmonics by hand; the curve marks where that lands.");
        }

        // By hand: before an analysis, or with Override on
        if (!m_rdo.Valid() || m_rdoOverride)
        {
            static const char* s_shNames[] = { "Low (degree 1, 9 bytes per splat)", "Medium (degree 2, 24 bytes per splat)", "High (degree 3, 45 bytes per splat)" };
            ImGui::Combo("SH Quality", &m_splatSHQuality, s_shNames, IM_ARRAYSIZE(s_shNames));
            if (ImGui::IsItemHovered()) ShowTooltip("How much of the view-dependent colour (spherical harmonics) the package keeps. Low keeps the first band, which carries most of the tint shift with viewing angle; Medium adds the second band (sharper highlights) at about 2.7 times the harmonics bytes; High keeps all three bands the source has, at five times the harmonics bytes.");
            static const char* s_precisionNames[] = { "8 bits (lossless)", "6 bits", "5 bits", "4 bits" };
            int precision = m_shPrecision;
            if (ImGui::Combo("SH Precision", &precision, s_precisionNames, IM_ARRAYSIZE(s_precisionNames))) SetSHPrecision(precision);
            if (ImGui::IsItemHovered()) ShowTooltip("Bits per spherical-harmonics coefficient, every band. Fewer bits round the view-dependent colour to coarser steps, which compresses much better.\n"
                                                          "Measured on Cthulhu (rendered views against 8 bits): 4 bits makes the package 41%% smaller with no measurable difference;\n"
                                                          "dropping to degree 2 costs about 11 dB. Analyse finds the best for your scan and target.");
            if (m_sourceSHDegree == 0)
                ImGui::TextDisabled("The source has no spherical harmonics (a point cloud): colour only");
            else if (BakeSHDegree() < SplatSHDegree())
                ImGui::TextDisabled("The source carries degree %u: the bake keeps degree %u", m_sourceSHDegree, BakeSHDegree());
        }

        // Profiles
        if (ImGui::Button("Save Profile...", ImVec2(110.0f, 0.0f)) && m_rdo.Valid() && !RdoStale()) SaveRdoProfile();
        if (ImGui::IsItemHovered()) ShowTooltip("Saves this scan's analysis, target and settings, with the scan's content hash, to a .rdoprofile file.");
        ImGui::SameLine();
        if (ImGui::Button("Load Profile...", ImVec2(110.0f, 0.0f)) && !m_op.Busy()) LoadRdoProfile();
        if (ImGui::IsItemHovered()) ShowTooltip("Loads a .rdoprofile: for the scan open now, or by opening the .ply it names when that file is unchanged.");
        ImGui::Unindent();
    }

    // The curve: package size (MB) across, colour fidelity (dB) up; every candidate a dot, the frontier (no candidate smaller
    // and better) joined, the chosen one ringed. Clicking or dragging on it picks the frontier point at that size.
    void SplatPakApp::DrawRdoPlot()
    {
        const double mib = 1024.0 * 1024.0;
        const float width = std::max(200.0f, ImGui::GetContentRegionAvailWidth()), height = 190.0f;
        ImGui::TextDisabled("Colour fidelity (dB)");
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##rdoPlot", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float left = 30.0f, bottom = 34.0f, top = 4.0f, right = 10.0f;
        const ImVec2 a(origin.x + left, origin.y + top), b(origin.x + width - right, origin.y + height - bottom);

        double xMin = 1e300, xMax = 0.0, yMin = 1e300, yMax = -1e300;
        for (const RdoPoint& p : m_rdo.points)
        {
            xMin = std::min(xMin, p.bytes / mib); xMax = std::max(xMax, p.bytes / mib);
            yMin = std::min(yMin, p.psnr); yMax = std::max(yMax, std::min(p.psnr, 70.0));
        }
        const double xPad = std::max(1.0, (xMax - xMin) * 0.05), yPad = std::max(0.5, (yMax - yMin) * 0.08);
        xMin -= xPad; xMax += xPad; yMin -= yPad; yMax += yPad;
        auto X = [&](double mb) { return a.x + (float)((mb - xMin) / (xMax - xMin)) * (b.x - a.x); };
        auto Y = [&](double db) { return b.y - (float)((std::min(db, 70.0) - yMin) / (yMax - yMin)) * (b.y - a.y); };

        const ImU32 axisText = IM_COL32(150, 155, 170, 255);
        dl->AddRectFilled(a, b, IM_COL32(18, 20, 26, 255));
        dl->AddRect(a, b, IM_COL32(70, 75, 90, 255));
        for (int i = 0; i <= 4; i++)
        {
            char label[24];
            const double mb = xMin + (xMax - xMin) * i / 4.0, db = yMin + (yMax - yMin) * i / 4.0;
            const float x = X(mb), y = Y(db);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(40, 44, 54, 255));
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(40, 44, 54, 255));
            snprintf(label, sizeof(label), "%.0f", mb);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(x - 0.5f * ts.x, b.y + 2.0f), axisText, label);
            snprintf(label, sizeof(label), "%.0f", db);
            dl->AddText(ImVec2(origin.x + 2.0f, y - 7.0f), axisText, label);
        }
        {
            const char* xTitle = "Package size (MB)";
            const ImVec2 ts = ImGui::CalcTextSize(xTitle);
            dl->AddText(ImVec2(0.5f * (a.x + b.x - ts.x), b.y + 17.0f), axisText, xTitle);
        }

        const std::vector<size_t> frontier = m_rdo.Frontier();
        for (size_t i = 1; i < frontier.size(); i++)
        {
            const RdoPoint& p0 = m_rdo.points[frontier[i - 1]], &p1 = m_rdo.points[frontier[i]];
            dl->AddLine(ImVec2(X(p0.bytes / mib), Y(p0.psnr)), ImVec2(X(p1.bytes / mib), Y(p1.psnr)), IM_COL32(90, 170, 255, 255), 2.0f);
        }
        int hoverIndex = -1; float hoverDist = 9.0f;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (size_t i = 0; i < m_rdo.points.size(); i++)
        {
            const RdoPoint& p = m_rdo.points[i];
            const ImVec2 c(X(p.bytes / mib), Y(p.psnr));
            dl->AddCircleFilled(c, p.frontier ? 3.5f : 2.5f, p.frontier ? IM_COL32(150, 205, 255, 255) : IM_COL32(120, 125, 140, 255));
            const float d = std::sqrt((c.x - mouse.x) * (c.x - mouse.x) + (c.y - mouse.y) * (c.y - mouse.y));
            if (hovered && d < hoverDist) { hoverDist = d; hoverIndex = (int)i; }
        }
        if (m_rdoChoice >= 0 && m_rdoChoice < (int)m_rdo.points.size())
        {
            const RdoPoint& p = m_rdo.points[m_rdoChoice];
            const ImVec2 c(X(p.bytes / mib), Y(p.psnr));
            dl->AddCircleFilled(c, 6.0f, IM_COL32(255, 160, 40, 255));
            dl->AddCircle(c, 9.0f, IM_COL32(255, 210, 120, 255), 16, 1.5f);
        }
        if (hoverIndex >= 0)
        {
            const RdoPoint& p = m_rdo.points[hoverIndex];
            ShowTooltip("%s\n%.1f MB, colour fidelity %.1f dB\nA typical view: %.1f MB to full detail, %.1f MB to its first picture%s", p.settings.Describe().c_str(),
                p.bytes / mib, p.psnr, p.viewBytes / mib, p.viewFirstBytes / mib, p.frontier ? "" : "\nNot on the frontier: another encoding is both smaller and better");
        }
        // A click or drag picks the frontier point nearest that size, as a size target
        if (active && !frontier.empty() && !m_rdoOverride)
        {
            const double mb = xMin + (double)(mouse.x - a.x) / (b.x - a.x) * (xMax - xMin);
            size_t best = frontier[0];
            for (size_t i : frontier)
                if (std::fabs(m_rdo.points[i].bytes / mib - mb) < std::fabs(m_rdo.points[best].bytes / mib - mb)) best = i;
            if ((int)best != m_rdoChoice)
            {
                m_rdoTarget.mode = RdoAnalysis::TargetMode::Size;
                m_rdoTarget.sizeMB = std::ceil(m_rdo.points[best].bytes / mib * 10.0) / 10.0;
                ApplyRdoChoice((int)best);
            }
        }
    }

    void SplatPakApp::ApplyRdoChoice(int choice)
    {
        if (choice < 0 || choice >= (int)m_rdo.points.size()) return;
        m_rdoChoice = choice;
        if (m_rdoOverride) return;
        const RdoSettings& s = m_rdo.points[choice].settings;
        m_splatSHQuality = (int)std::clamp(s.shDegree, 1u, 3u) - 1;
        memcpy(m_shBandBits, s.shBits, 3);
        m_shPrecision = 0;
        for (int p = 0; p < 4; p++) if (kSHPrecisionBits[p] == s.shBits[0]) m_shPrecision = p;
    }

    void SplatPakApp::StartRdoAnalysis()
    {
        auto result = std::make_shared<RdoAnalysisResult>();
        auto hash = std::make_shared<uint64_t>(m_sourceHash);
        auto reused = std::make_shared<std::string>(); // The stored profile used instead of analysing
        const std::string path = m_loadedFilePath;
        const std::string folder = RdoProfileFolder();
        const int maxLODs = m_bakedParams.maxLODs;
        const float deadbandMM = m_bakedParams.deadbandMM;
        m_rdoStart = std::chrono::steady_clock::now();
        LogTransitionTrace("RDO: analysing %s", path.c_str());
        RunOperation("Analysing the compression", [this, result, hash, reused, path, folder, maxLODs, deadbandMM](Progress& progress)
        {
            if (*hash == 0)
            {
                progress.Stage("Hashing the scan", 0.0f);
                *hash = BakeTuningCache::HashFile(path);
            }
            {
                progress.Stage("Looking for a stored analysis", 0.0f);
                RdoProfile stored;
                if (FindRdoProfile(folder, *hash, maxLODs, deadbandMM, stored, *reused))
                {
                    *result = stored.analysis;
                    return true;
                }
            }
            auto report = [&progress](float f, const char* stage)
            {
                if (progress.StageName() != stage) progress.Stage(stage, f); else progress.Fraction(f);
                return !progress.Cancelled();
            };
            // Through the workers when SplatPakWorker.exe is beside the app (RdoDispatcher.h: one job per SH degree, side by
            // side); otherwise here, in this process
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string worker = exe;
            worker = worker.substr(0, worker.find_last_of("\\/") + 1) + "SplatPakWorker.exe";
            if (GetFileAttributesA(worker.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                RdoJob base;
                base.scan = path;
                base.scanHash = *hash;
                base.levels = m_bakedParams.maxLODs;
                base.deadbandMM = m_bakedParams.deadbandMM;
                char temp[MAX_PATH] = {};
                GetTempPathA(MAX_PATH, temp);
                char sub[32];
                snprintf(sub, sizeof(sub), "%016llx", (unsigned long long)*hash);
                const std::string folder = std::string(temp) + "SplatPak RDO\\" + sub;
                CreateDirectoryA((std::string(temp) + "SplatPak RDO").c_str(), nullptr);
                std::string error;
                const bool ok = RdoDispatcher::Analyse(base, m_sourceSHDegree, worker, folder, report, *result, error);
                LogTransitionTrace("RDO: %s through %s%s%s", ok ? "analysed" : "failed", worker.c_str(), ok ? "" : ": ", ok ? "" : error.c_str());
                if (ok || error == "cancelled") return ok;
                LogTransitionTrace("RDO: falling back to this process");
            }
            std::vector<const std::vector<SplatPoint>*> levels;
            levels.push_back(&m_rawSplats);
            for (size_t l = 1; l < m_waveletResult.lodLevels.size(); l++) levels.push_back(&m_waveletResult.lodLevels[l].splats);
            return RdoAnalysis::Analyse(levels, m_splatAttributes, &m_waveletResult.mergedAttributes, m_splatParams, m_sourceSHDegree,
                report, *result);
        },
        [this, result, hash, reused, path](bool ok, bool cancelled)
        {
            if (!ok)
            {
                m_statusMessage = cancelled ? "Analysis cancelled." : "The analysis failed: see the trace log.";
                m_statusIsSuccess = false;
                return;
            }
            result->unitBytes.clear(); result->unitError.clear(); // Summed into each point's regionSaving
            m_rdo = std::move(*result);
            m_rdoHash = *hash;
            m_rdoSourcePath = path;
            m_rdoPackagePath.clear();
            if (m_sourceHash == 0) m_sourceHash = *hash;
            m_rdoParams = m_bakedParams;
            // A first target: the size of what is set now
            if (m_rdoTarget.sizeMB <= 0.0)
            {
                for (const RdoPoint& p : m_rdo.points)
                    if (p.settings.shDegree == BakeSHDegree() && memcmp(p.settings.shBits, m_shBandBits, std::min<uint32_t>(3u, p.settings.shDegree)) == 0)
                        m_rdoTarget.sizeMB = std::ceil(p.bytes / (1024.0 * 1024.0) * 10.0) / 10.0;
                if (m_rdoTarget.sizeMB <= 0.0) m_rdoTarget.sizeMB = std::ceil(m_rdo.points[0].bytes / (1024.0 * 1024.0));
            }
            ApplyRdoChoice(RdoAnalysis::Pick(m_rdo, m_rdoTarget, (double)m_sourceFileBytes));
            char msg[512];
            if (!reused->empty())
            {
                snprintf(msg, sizeof(msg), "Using the stored analysis %s (%zu encodings, identical settings): pick a point on the curve or set a target, then Bake.",
                    reused->substr(reused->find_last_of("/\\") + 1).c_str(), m_rdo.points.size());
                LogTransitionTrace("RDO: reused %s", reused->c_str());
                m_lastRdoProfilePath = *reused;
            }
            else
            {
                StoreRdoProfile();
                snprintf(msg, sizeof(msg), "Analysed %zu encodings in %.0f s: pick a point on the curve or set a target, then Bake.", m_rdo.points.size(), m_rdo.seconds);
            }
            m_statusMessage = msg;
            m_statusIsSuccess = true;
            for (const RdoPoint& p : m_rdo.points)
                LogTransitionTrace("RDO: %-24s %8.1f MB  first view %6.1f MB  %6.2f dB%s  region %.1f%%", p.settings.Describe().c_str(), p.bytes / (1024.0 * 1024.0),
                    p.coarseBytes / (1024.0 * 1024.0), p.psnr, p.frontier ? " *" : "  ", 100.0 * p.regionSaving);
        });
    }

    void SplatPakApp::SaveRdoProfile()
    {
        std::string path = SaveFileDialog("RDO Profile (*.rdoprofile)\0*.rdoprofile\0", "rdoprofile", "Save RDO Profile");
        if (path.empty()) return;
        const bool ok = MakeRdoProfile().Write(path);
        m_statusMessage = ok ? "RDO profile saved: " + path : "Could not write the RDO profile to " + path;
        m_statusIsSuccess = ok;
    }

    RdoProfile SplatPakApp::MakeRdoProfile() const
    {
        RdoProfile p;
        p.sourceHash = m_rdoHash;
        p.sourceFile = m_rdoSourcePath.empty() ? m_loadedFilePath : m_rdoSourcePath;
        p.chunkSize = m_bakedParams.chunkSize; p.maxLODs = m_rdoParams.maxLODs; p.deadbandMM = m_rdoParams.deadbandMM; p.occlusion = m_bakedParams.occlusion;
        p.target = m_rdoTarget;
        p.override = m_rdoOverride;
        p.choice.shDegree = BakeSHDegree();
        memcpy(p.choice.shBits, m_shBandBits, 3);
        p.analysis = m_rdo;
        return p;
    }

    bool SplatPakApp::FindRdoProfile(const std::string& folder, uint64_t hash, int maxLODs, float deadbandMM, RdoProfile& out, std::string& path)
    {
        std::error_code ec;
        if (hash == 0 || !std::filesystem::is_directory(folder, ec)) return false;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            if (entry.path().extension() != ".rdoprofile") continue;
            RdoProfile p;
            std::string error;
            if (!p.Read(entry.path().string(), error)) continue;
            if (p.sourceHash == hash && p.maxLODs == maxLODs && fabsf(p.deadbandMM - deadbandMM) < 0.0006f && p.ConstantsMatch())
            {
                out = std::move(p);
                path = entry.path().string();
                return true;
            }
        }
        return false;
    }

    // A fresh analysis, kept for the next time the same scan is analysed with the same pyramid: <scan>_L<levels>_D<deadband>.rdoprofile,
    // numbered when that name is taken
    std::string SplatPakApp::NewRdoProfilePath(const std::string& folder, const std::string& scan, int maxLODs, float deadbandMM)
    {
        std::error_code ec;
        char base[160];
        snprintf(base, sizeof(base), "%s_L%d_D%.2f", std::filesystem::path(scan).stem().string().c_str(), maxLODs, deadbandMM);
        std::string path = folder + "\\" + base + ".rdoprofile";
        for (int n = 2; std::filesystem::exists(path, ec); n++) path = folder + "\\" + base + " (" + std::to_string(n) + ").rdoprofile";
        return path;
    }

    void SplatPakApp::StoreRdoProfile()
    {
        if (!m_rdo.Valid() || m_rdoHash == 0) return;
        const std::string folder = RdoProfileFolder();
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const RdoProfile p = MakeRdoProfile();
        const std::string path = NewRdoProfilePath(folder, p.sourceFile, p.maxLODs, p.deadbandMM);
        const bool ok = p.Write(path);
        if (ok) m_lastRdoProfilePath = path;
        LogTransitionTrace("RDO: %s %s", ok ? "stored" : "could not store", path.c_str());
    }

    void SplatPakApp::AdoptRdoAnalysis(const RdoProfile& p)
    {
        m_rdo = p.analysis;
        m_rdoHash = p.sourceHash;
        m_rdoSourcePath = p.sourceFile;
        m_rdoPackagePath.clear();
        m_rdoParams = m_bakedParams;
        m_rdoParams.maxLODs = p.maxLODs;
        m_rdoParams.deadbandMM = p.deadbandMM;
        // The point on the curve is the encoding on screen, if the analysis measured it
        m_rdoChoice = -1;
        const uint32_t degree = m_isLoadedFromSFLW ? m_splatParams.shDegree : BakeSHDegree();
        uint8_t bits[3] = { 8, 8, 8 };
        if (m_isLoadedFromSFLW) { if (m_loadedPackage.header.payloadTransforms & PAYLOAD_TRANSFORM_SH_RESIDUAL) SHCoding::UnpackBits(m_loadedPackage.header.payloadTransforms, bits); }
        else memcpy(bits, m_bakedParams.shBandBits, 3);
        for (size_t i = 0; i < m_rdo.points.size(); i++)
            if (m_rdo.points[i].settings.shDegree == degree && memcmp(m_rdo.points[i].settings.shBits, bits, std::min<uint32_t>(3u, degree)) == 0) m_rdoChoice = (int)i;
    }

    void SplatPakApp::LoadRdoProfile()
    {
        std::string path = OpenFileDialog("RDO Profile (*.rdoprofile)\0*.rdoprofile\0", "Load RDO Profile", "rdoprofile");
        if (path.empty()) return;
        RdoProfile p;
        std::string error;
        if (!p.Read(path, error)) { m_statusMessage = "Cannot load " + path + ": " + error; m_statusIsSuccess = false; return; }
        // The scan open now, if it is the profile's
        if (!m_rawSplats.empty() && !m_isLoadedFromSFLW)
        {
            if (m_sourceHash == 0) m_sourceHash = BakeTuningCache::HashFile(m_loadedFilePath);
            if (m_sourceHash == p.sourceHash) { ApplyRdoProfile(p); return; }
        }
        // Otherwise the .ply it names, if that file is unchanged
        if (!p.sourceFile.empty() && BakeTuningCache::HashFile(p.sourceFile) == p.sourceHash)
        {
            m_pendingRdoProfile = std::make_unique<RdoProfile>(p);
            m_statusMessage = "Opening " + p.sourceFile + " for the profile...";
            m_statusIsSuccess = true;
            LoadFile(p.sourceFile);
            return;
        }
        m_statusMessage = "The profile is for another scan" + std::string(p.sourceFile.empty() ? "" : " (" + p.sourceFile + ", which is missing or has changed)") +
                          ": open that .ply, unchanged, first.";
        m_statusIsSuccess = false;
    }

    void SplatPakApp::ApplyRdoProfile(const RdoProfile& p)
    {
        m_chunkSize = p.chunkSize;
        m_maxLODLevels = p.maxLODs;
        m_deadbandThresholdMM = p.deadbandMM;
        m_generateOcclusionVolume = p.occlusion;
        m_rdo = p.analysis;
        m_rdoHash = p.sourceHash;
        m_rdoSourcePath = p.sourceFile;
        m_rdoPackagePath.clear();
        m_rdoParams = CurrentBakeParams();
        m_rdoTarget = p.target;
        m_rdoOverride = p.override;
        m_rdoChoice = -1;
        if (p.override)
        {
            m_splatSHQuality = (int)std::clamp(p.choice.shDegree, 1u, 3u) - 1;
            memcpy(m_shBandBits, p.choice.shBits, 3);
            for (size_t i = 0; i < m_rdo.points.size(); i++) if (m_rdo.points[i].settings == p.choice) m_rdoChoice = (int)i;
        }
        else ApplyRdoChoice(RdoAnalysis::Pick(m_rdo, m_rdoTarget, (double)m_sourceFileBytes));
        m_statusMessage = std::string("RDO profile loaded") + (p.ConstantsMatch() ? "" : " (measured by a build with other analysis settings: analyse again to refresh it)") +
                          (BakePending() ? "; baking with it." : ".");
        m_statusIsSuccess = true;
        if (BakePending()) StartBake(false);
    }

    // A profile loaded with its scan not open yet: applied once that scan has opened (and baked)
    void SplatPakApp::StepPendingRdoProfile()
    {
        if (!m_pendingRdoProfile || m_op.Busy()) return;
        if (m_sourceHash == m_pendingRdoProfile->sourceHash && !m_rawSplats.empty())
        {
            const RdoProfile p = *m_pendingRdoProfile;
            m_pendingRdoProfile.reset();
            ApplyRdoProfile(p);
        }
        else if (m_isLoadedFromSFLW || (m_sourceHash != 0 && m_sourceHash != m_pendingRdoProfile->sourceHash))
        {
            m_pendingRdoProfile.reset(); // Something else opened instead
        }
    }

    // Tiers 2-4 of a loaded package in bits per byte (EntropyProfile.h): a few chunks of every level, each against its own
    // coarser level, as the package codes the harmonics (they are held absolute once loaded)
    void SplatPakApp::MeasurePackageEntropy()
    {
        m_entropy = EntropyProfile{};
        const auto& pkg = m_loadedPackage;
        if (!pkg.IsSplatPackage() || pkg.chunkManifests.empty()) return;
        const auto t0 = std::chrono::high_resolution_clock::now();
        const size_t shStride = pkg.header.shRecordBytes;
        uint8_t bits[3] = { 8, 8, 8 };
        if (pkg.header.payloadTransforms & PAYLOAD_TRANSFORM_SH_RESIDUAL) SHCoding::UnpackBits(pkg.header.payloadTransforms, bits);
        const bool residual = (pkg.header.payloadTransforms & PAYLOAD_TRANSFORM_SH_RESIDUAL) != 0;
        constexpr size_t kRun = 16384;
        constexpr size_t kChunksPerLevel = 8;
        size_t levels = 0;
        for (const auto& cm : pkg.chunkManifests) levels = std::max(levels, cm.lods.size());
        std::vector<EntropySample> samples;
        for (size_t l = 0; l < levels; l++)
        {
            std::vector<size_t> chunks;
            size_t levelCount = 0;
            for (size_t c = 0; c < pkg.chunkManifests.size(); c++)
                if (l < pkg.chunkLODSplats[c].size() && !pkg.chunkLODSplats[c][l].empty()) { chunks.push_back(c); levelCount += pkg.chunkLODSplats[c][l].size(); }
            if (chunks.empty()) continue;
            const size_t n = std::min(kChunksPerLevel, chunks.size());
            std::vector<size_t> picked;
            size_t sampled = 0;
            for (size_t k = 0; k < n; k++)
            {
                const size_t c = chunks[(size_t)(((double)k + 0.5) * (double)chunks.size() / (double)n)];
                picked.push_back(c);
                sampled += std::min(kRun, pkg.chunkLODSplats[c][l].size());
            }
            for (size_t c : picked)
            {
                const auto& recs = pkg.chunkLODSplats[c][l];
                EntropySample s;
                s.records = reinterpret_cast<const uint8_t*>(recs.data());
                s.count = std::min(kRun, recs.size());
                if (shStride > 0 && c < pkg.chunkLODSH.size() && l < pkg.chunkLODSH[c].size() && pkg.chunkLODSH[c][l].size() >= recs.size() * shStride) s.sh = pkg.chunkLODSH[c][l].data();
                if (s.sh && residual && l + 1 < pkg.chunkLODSplats[c].size() && !pkg.chunkLODSplats[c][l + 1].empty() && pkg.chunkLODSH[c][l + 1].size() >= pkg.chunkLODSplats[c][l + 1].size() * shStride)
                {
                    s.parentRecords = reinterpret_cast<const uint8_t*>(pkg.chunkLODSplats[c][l + 1].data());
                    s.parentCount = pkg.chunkLODSplats[c][l + 1].size();
                    s.parentSH = &pkg.chunkLODSH[c][l + 1];
                }
                s.weight = (double)levelCount / (double)std::max<size_t>(1, sampled);
                samples.push_back(s);
            }
        }
        m_entropy = Entropy::Measure(samples, sizeof(PackedSplatGPU), shStride, bits);
        m_entropy.seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
        LogTransitionTrace("Package entropy (bits per byte): Tier 2 %.3f, Tier 3 %.3f, Tier 4 %.3f (records %.3f / %.3f / %.3f, SH %.3f / %.3f / %.3f) in %.2f s",
            m_entropy.tier2, m_entropy.tier3, m_entropy.tier4, m_entropy.records[0], m_entropy.records[1], m_entropy.records[2],
            m_entropy.sh[0], m_entropy.sh[1], m_entropy.sh[2], m_entropy.seconds);
    }

    static bool SameFile(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty()) return false;
        std::error_code ec;
        if (std::filesystem::equivalent(a, b, ec)) return true;
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    std::string SplatPakApp::FindSourceScan() const
    {
        if (m_packagePath.empty()) return std::string();
        if (m_packageSourceFor == m_packagePath) return m_packageSource;
        m_packageSourceFor = m_packagePath;
        m_packageSource.clear();
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path package(m_packagePath);
        for (const char* ext : { ".ply", ".splat" })
        {
            fs::path scan = package;
            scan.replace_extension(ext);
            if (fs::exists(scan, ec)) return m_packageSource = scan.string();
        }
        // A scan baked elsewhere: the one remembered with the same name
        for (const auto& kv : BakeTuningCache::Load())
            if (!kv.second.file.empty() && fs::path(kv.second.file).stem() == package.stem() && fs::exists(kv.second.file, ec))
                return m_packageSource = kv.second.file;
        return m_packageSource;
    }

    // The encoding on screen against the Streaming tab's network profile. The first picture is the two coarsest levels (the
    // stream sends that envelope of the whole model first); full detail is everything. Their share of the package is taken
    // from the stream's blocks. With an RDO analysis of this scan, both are a typical close view's instead (RdoPoint).
    SplatPakApp::EncodingReport SplatPakApp::AssessEncoding() const
    {
        EncodingReport r;
        if (m_waveletResult.lodLevels.empty() || m_stream.m_lodStreamChunks.empty() || m_compressedSizeMB <= 0.0f) return r;
        constexpr double kMiB = 1024.0 * 1024.0;
        r.valid = true;
        r.package = m_isLoadedFromSFLW;
        r.shDegree = m_splatParams.shDegree;
        if (r.package)
        {
            if (m_loadedPackage.header.payloadTransforms & PAYLOAD_TRANSFORM_SH_RESIDUAL) SHCoding::UnpackBits(m_loadedPackage.header.payloadTransforms, r.shBits);
        }
        else memcpy(r.shBits, m_bakedParams.shBandBits, 3);
        r.levels = (int)m_waveletResult.lodLevels.size();
        r.blocks = m_stream.m_lodStreamChunks[0].size();
        r.occlusion = HasOccluderMesh() || !m_occlusionVoxels.empty();
        r.packageBytes = m_compressedSizeMB * kMiB;
        {
            double total = 0.0, coarse = 0.0;
            const size_t levels = m_stream.m_lodStreamChunks.size();
            for (size_t l = 0; l < levels; l++)
                for (const StreamChunk& c : m_stream.m_lodStreamChunks[l])
                {
                    total += (double)c.byteSize;
                    if (l + 2 >= levels) coarse += (double)c.byteSize;
                }
            r.firstBytes = total > 0.0 ? r.packageBytes * coarse / total : 0.0;
            r.fullBytes = r.packageBytes;
        }
        r.bandwidthMBps = m_unthrottledBandwidth ? 0.0 : (double)m_bandwidthThrottleMBps;
        {
            char p[48];
            if (m_unthrottledBandwidth) snprintf(p, sizeof(p), "Full (uncapped)");
            else if (fabsf(m_bandwidthThrottleMBps - 0.2f) < 0.01f) snprintf(p, sizeof(p), "3G (0.2 MB/s)");
            else if (fabsf(m_bandwidthThrottleMBps - 2.0f) < 0.01f) snprintf(p, sizeof(p), "4G (2 MB/s)");
            else if (fabsf(m_bandwidthThrottleMBps - 7.5f) < 0.01f) snprintf(p, sizeof(p), "5G (7.5 MB/s)");
            else snprintf(p, sizeof(p), "%.1f MB/s", m_bandwidthThrottleMBps);
            r.profile = p;
        }
        auto bitsText = [](const uint8_t* bits, uint32_t degree)
        {
            std::string s;
            for (uint32_t b = 0; b < std::min(degree, 3u); b++) s += (b ? " / " : "") + std::to_string(bits[b]);
            return s.empty() ? std::string("none") : s;
        };
        char buf[512];

        // The RDO analysis of this scan, when there is one: what it measured for the encoding on screen, and what it picks
        const std::string source = r.package ? FindSourceScan() : m_loadedFilePath;
        const bool analysed = r.package ? (m_rdo.Valid() && ((!m_rdoPackagePath.empty() && SameFile(m_rdoPackagePath, m_packagePath)) ||
                                                             (SameFile(source, m_rdoSourcePath) && m_rdoParams.maxLODs == r.levels - 1)))
                                        : !RdoStale();
        int current = -1;
        if (analysed)
        {
            for (size_t i = 0; i < m_rdo.points.size(); i++)
            {
                const RdoSettings& s = m_rdo.points[i].settings;
                if (s.shDegree == r.shDegree && memcmp(s.shBits, r.shBits, std::min<uint32_t>(3u, s.shDegree)) == 0) current = (int)i;
            }
            if (current >= 0)
            {
                r.viewBased = true;
                r.firstBytes = m_rdo.points[current].viewFirstBytes;
                r.fullBytes = m_rdo.points[current].viewBytes;
            }
            RdoAnalysis::Target t = m_rdoTarget;
            t.mode = RdoAnalysis::TargetMode::Streaming;
            t.bandwidthMBps = r.bandwidthMBps > 0.0 ? r.bandwidthMBps : 1000.0;
            const int pick = RdoAnalysis::Pick(m_rdo, t, (double)m_sourceFileBytes);
            if (pick >= 0 && pick != current)
            {
                r.rdoPick = pick;
                const RdoPoint& p = m_rdo.points[pick];
                snprintf(buf, sizeof(buf), "For %s the RDO analysis picks degree %u, %s bits: %.1f MB at %.1f dB colour fidelity", r.profile.c_str(),
                    p.settings.shDegree, bitsText(p.settings.shBits, p.settings.shDegree).c_str(), p.bytes / kMiB, p.psnr);
                std::string why = buf;
                if (current >= 0)
                {
                    snprintf(buf, sizeof(buf), " (now %.1f MB at %.1f dB).", m_rdo.points[current].bytes / kMiB, m_rdo.points[current].psnr);
                    why += buf;
                }
                else why += ".";
                if (p.settings.shDegree != r.shDegree) r.warn[kEncDegree] = why;
                if (memcmp(p.settings.shBits, r.shBits, std::min<uint32_t>(3u, std::min(p.settings.shDegree, r.shDegree))) != 0 || p.settings.shDegree != r.shDegree) r.warn[kEncBits] = why;
            }
        }

        const double bw = r.bandwidthMBps * kMiB;
        const double firstSeconds = bw > 0.0 ? r.firstBytes / bw : 0.0, fullSeconds = bw > 0.0 ? r.fullBytes / bw : 0.0;
        if (bw > 0.0 && firstSeconds > m_rdoTarget.firstViewSeconds)
        {
            snprintf(buf, sizeof(buf), "At %s the first picture takes %.1f s; the target is %.1f s (Compression (RDO) menu).", r.profile.c_str(), firstSeconds, m_rdoTarget.firstViewSeconds);
            r.warn[kEncFirstView] = buf;
            if (r.levels - 1 < 6)
            {
                r.recMaxLODs = r.levels; // One more level: a smaller coarse envelope
                snprintf(buf, sizeof(buf), "One more level (%d) makes the coarse envelope that arrives first smaller.", r.levels + 1);
                r.warn[kEncLevels] = buf;
            }
            if (!analysed && r.shDegree > 0)
                r.warn[kEncBits] = "No RDO analysis of this scan yet: Re-encode analyses the compression and picks the spherical-harmonics bits that arrive in time.";
        }
        if (r.viewBased && bw > 0.0 && fullSeconds > m_rdoTarget.fullDetailSeconds)
        {
            snprintf(buf, sizeof(buf), "At %s a typical close view reaches full detail in %.1f s; the target is %.1f s (Compression (RDO) menu).", r.profile.c_str(), fullSeconds, m_rdoTarget.fullDetailSeconds);
            r.warn[kEncFullDetail] = buf;
        }
        if (bw <= 0.0 && !analysed && r.shDegree > 0 && (r.shBits[0] < 8 || r.shBits[1] < 8 || r.shBits[2] < 8))
            r.warn[kEncBits] = "Full bandwidth does not need the reduced spherical-harmonics precision: Re-encode analyses the compression and picks the best that streams in time.";
        if (r.levels < 3)
        {
            snprintf(buf, sizeof(buf), "Only %d level%s: the first picture is nearly the whole model. At least 3 levels give the stream a coarse envelope to send first.", r.levels, r.levels == 1 ? "" : "s");
            r.warn[kEncLevels] = buf;
            r.recMaxLODs = std::max(r.recMaxLODs, 3);
        }
        if (r.blocks < 8)
        {
            snprintf(buf, sizeof(buf), "Only %zu block%s at full detail: the stream cannot send what is in view first. A smaller Octree Chunk cuts the model into more blocks.", r.blocks, r.blocks == 1 ? "" : "s");
            r.warn[kEncChunks] = buf;
            r.recChunk = std::max(0.05f, std::max(m_extents.x, std::max(m_extents.y, m_extents.z)) / 4.0f);
        }
        if (!r.occlusion)
        {
            r.warn[kEncOcclusion] = "No occlusion volume: blocks hidden behind the model are streamed and sorted anyway. Re-encode bakes one.";
            r.recOcclusion = true;
        }
        return r;
    }

    void SplatPakApp::DrawEncodingPanel()
    {
        if (!MenuHeader("Encoding")) return;
        const EncodingReport rep = AssessEncoding();
        if (!rep.valid) { ImGui::TextDisabled("Nothing baked yet."); return; }
        constexpr double kMiB = 1024.0 * 1024.0;

        // A row, with a warning triangle in front of it when that part of the encoding should be baked again
        auto row = [&](int k, const char* text, const char* tip = nullptr)
        {
            const bool warned = k >= 0 && !rep.warn[k].empty();
            const float h = ImGui::GetTextLineHeight();
            ImGui::BeginGroup();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (warned)
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float s = h * 0.95f, top = p.y + 0.5f;
                const float cx = p.x + s * 0.5f;
                dl->AddTriangleFilled(ImVec2(cx, top), ImVec2(p.x, top + s), ImVec2(p.x + s, top + s), IM_COL32(255, 190, 40, 255));
                dl->AddLine(ImVec2(cx, top + s * 0.34f), ImVec2(cx, top + s * 0.68f), IM_COL32(40, 28, 0, 255), 1.6f);
                dl->AddCircleFilled(ImVec2(cx, top + s * 0.83f), 1.1f, IM_COL32(40, 28, 0, 255));
            }
            ImGui::Dummy(ImVec2(h + 4.0f, h));
            ImGui::SameLine(0.0f, 0.0f);
            if (warned) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", text);
            else ImGui::TextUnformatted(text);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
            {
                if (warned) ShowTooltip("%s%s%s", rep.warn[k].c_str(), tip ? "\n\n" : "", tip ? tip : "");
                else if (tip) ShowTooltip("%s", tip);
            }
        };
        char line[256];
        const std::string name = m_packagePath.empty() ? std::string() : m_packagePath.substr(m_packagePath.find_last_of("/\\") + 1);
        if (rep.package || !name.empty()) snprintf(line, sizeof(line), "Package: %s, %.1f MB", name.c_str(), m_compressedSizeMB);
        else snprintf(line, sizeof(line), "Baked from the scan, not saved: about %.1f MB", m_compressedSizeMB);
        row(-1, line, m_packagePath.empty() ? nullptr : m_packagePath.c_str());
        snprintf(line, sizeof(line), "SH degree: %u", rep.shDegree);
        row(kEncDegree, line, "Spherical-harmonics bands kept: the view-dependent colour.");
        {
            std::string bits;
            for (uint32_t b = 0; b < std::min(rep.shDegree, 3u); b++) bits += (b ? " / " : "") + std::to_string(rep.shBits[b]);
            snprintf(line, sizeof(line), "SH bits per band: %s", bits.empty() ? "none" : bits.c_str());
        }
        row(kEncBits, line, "Bits per spherical-harmonics coefficient, bands 1 / 2 / 3 (8 = as encoded).");
        snprintf(line, sizeof(line), "LOD levels: %d", rep.levels);
        row(kEncLevels, line);
        snprintf(line, sizeof(line), "Blocks at full detail: %zu", rep.blocks);
        row(kEncChunks, line, "What view-first streaming chooses between: the model's chunks at level 0.");
        if (HasOccluderMesh()) snprintf(line, sizeof(line), "Occlusion: mesh, %zu triangles", m_occluderMesh->levels[0].TriangleCount());
        else if (!m_occlusionVoxels.empty()) snprintf(line, sizeof(line), "Occlusion: %zu cubes", m_occlusionVoxels.size());
        else snprintf(line, sizeof(line), "Occlusion: none");
        row(kEncOcclusion, line);
        ImGui::Separator();
        snprintf(line, sizeof(line), "Checked against: %s", rep.profile.c_str());
        row(-1, line, "The Network Profile selected in the streaming simulation above, and the time targets of the Compression (RDO) menu.");
        const double bw = rep.bandwidthMBps * kMiB;
        const char* scope = rep.viewBased ? "a typical close view" : "whole model";
        if (bw > 0.0) snprintf(line, sizeof(line), "First picture: %.1f MB, %.1f s (target %.1f s)", rep.firstBytes / kMiB, rep.firstBytes / bw, m_rdoTarget.firstViewSeconds);
        else snprintf(line, sizeof(line), "First picture: %.1f MB", rep.firstBytes / kMiB);
        row(kEncFirstView, line, rep.viewBased ? "A typical close view's share of the two coarsest levels (RDO analysis)." : "The two coarsest levels of the whole model: the envelope the stream sends first.");
        if (bw > 0.0) snprintf(line, sizeof(line), "Full detail (%s): %.1f MB, %.1f s%s", scope, rep.fullBytes / kMiB, rep.fullBytes / bw, rep.viewBased ? "" : "");
        else snprintf(line, sizeof(line), "Full detail (%s): %.1f MB", scope, rep.fullBytes / kMiB);
        if (rep.viewBased && bw > 0.0)
        {
            const size_t n = strlen(line);
            snprintf(line + n, sizeof(line) - n, " (target %.1f s)", m_rdoTarget.fullDetailSeconds);
        }
        row(kEncFullDetail, line);
        if (!rep.viewBased)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("No RDO analysis of this scan: the times are the whole model's.");
            ImGui::PopStyleColor();
        }

        if (IsViewerOnly()) return;
        const bool idle = m_reencode == Reencode::Idle && !m_op.Busy();
        const bool any = rep.Any();
        if (!any || !idle) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
        if (any)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.42f, 0.08f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.52f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.35f, 0.06f, 1.0f));
        }
        char label[96];
        if (m_reencode != Reencode::Idle && m_reencode != Reencode::Confirm) snprintf(label, sizeof(label), "Re-encoding...###reencode");
        else snprintf(label, sizeof(label), "Re-encode for %s###reencode", rep.profile.c_str());
        const bool pressed = ImGui::Button(label, ImVec2(-1, 28));
        if (any) ImGui::PopStyleColor(3);
        if (!any || !idle) ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            ShowTooltip(any ? "Bakes the package again from its source scan with what the warnings recommend, analysing the compression\nfor this network first when it has not been, then saves it and opens it. Asks before replacing anything."
                                  : "Nothing to fix for this network profile.");
        if (pressed && any && idle)
        {
            m_reencodeReport = rep;
            m_reencodeReload = rep.package;
            m_reencodeSource = rep.package ? FindSourceScan() : m_loadedFilePath;
            std::string out = !m_packagePath.empty() ? m_packagePath : std::filesystem::path(m_loadedFilePath).replace_extension(".gstx").string();
            if (out.size() > 5 && _stricmp(out.c_str() + out.size() - 5, ".gstx") == 0) out.resize(out.size() - 5);
            m_reencodeOutput = out;
            m_reencode = Reencode::Confirm;
            StartReencodeProfilePlan();
            ImGui::OpenPopup("Re-encode##confirm");
        }

        if (ImGui::BeginPopupModal("Re-encode##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            const EncodingReport& r = m_reencodeReport;
            ImGui::Text("Re-encode for %s", r.profile.c_str());
            ImGui::Separator();
            static const char* s_rows[kEncRowCount] = { "SH degree", "SH bits", "LOD levels", "Blocks", "Occlusion", "First picture", "Full detail" };
            for (int k = 0; k < kEncRowCount; k++)
                if (!r.warn[k].empty())
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s:", s_rows[k]);
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", r.warn[k].c_str());
                    ImGui::PopTextWrapPos();
                }
            ImGui::Separator();
            if (m_reencodeSource.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "Source scan: not found beside the package.");
            else ImGui::Text("Source scan: %s", m_reencodeSource.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Locate...")) m_pendingAction = PendingAction::LocateReencodeSource;
            ImGui::Text("Writes: %s.gstx", m_reencodeOutput.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Change...")) m_pendingAction = PendingAction::ReencodeSaveAs;
            {
                std::error_code ec;
                if (std::filesystem::exists(m_reencodeOutput + ".gstx", ec)) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "That package is replaced.");
            }
            if (!m_reencodeProfileReady && m_reencodeProfilePlan.valid() && m_reencodeProfilePlan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                m_reencodeProfile = m_reencodeProfilePlan.get();
                m_reencodeProfileReady = true;
            }
            if (!m_reencodeProfileReady) ImGui::TextDisabled("Profile: checking the scan...");
            else if (m_reencodeProfile.path.empty()) ImGui::TextDisabled("Profile: locate the source scan first.");
            else if (m_reencodeProfile.reuse) ImGui::Text("Profile: %s (stored already, reused)", m_reencodeProfile.path.c_str());
            else ImGui::Text("Profile saved as: %s", m_reencodeProfile.path.c_str());
            if (ImGui::IsItemHovered()) ShowTooltip("The compression analysis this re-encode uses, kept in the rdo-profiles folder: an identical stored one (the same scan, levels and deadband) is reused instead of analysing again.");
            ImGui::TextDisabled("Steps: open the scan, bake, analyse the compression for this network (about a minute,\nunless it has been), bake with its choice, save%s.", m_reencodeReload ? ", open the new package" : "");
            ImGui::Spacing();
            if (m_reencodeSource.empty()) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
            if (ImGui::Button("Re-encode", ImVec2(140, 0)) && !m_reencodeSource.empty())
            {
                m_reencode = Reencode::Open;
                LogTransitionTrace("Re-encode: %s -> %s.gstx for %s", m_reencodeSource.c_str(), m_reencodeOutput.c_str(), r.profile.c_str());
                ImGui::CloseCurrentPopup();
            }
            if (m_reencodeSource.empty()) ImGui::PopStyleVar();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0)))
            {
                m_reencode = Reencode::Idle;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else if (m_reencode == Reencode::Confirm) m_reencode = Reencode::Idle; // Closed some other way
    }

    // Re-encode's confirmation: the profile it will store or reuse. The scan's hash (about a second), its remembered bake
    // settings (or the defaults, from the splat count and size), the levels the re-encode adds, then a stored identical profile
    // or a new name; worked out in the background while the confirmation is open.
    void SplatPakApp::StartReencodeProfilePlan()
    {
        const std::string source = m_reencodeSource, folder = RdoProfileFolder();
        const uint64_t knownHash = (!m_reencodeReport.package && m_sourceHash != 0) ? m_sourceHash : 0;
        const float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
        const size_t n = m_rawSplats.size();
        const int fallbackLODs = m_reencodeReport.package ? (n > 2000000 ? 5 : n > 500000 ? 4 : n > 100000 ? 3 : 2) : m_bakedParams.maxLODs;
        const float fallbackDeadband = m_reencodeReport.package ? std::max(0.5f, maxDim * 1.5f) : m_bakedParams.deadbandMM;
        const int rec = m_reencodeReport.recMaxLODs;
        m_reencodeProfileReady = false;
        m_reencodeProfile = ReencodeProfilePlan{};
        m_reencodeProfilePlan = std::async(std::launch::async, [source, folder, knownHash, fallbackLODs, fallbackDeadband, rec]()
        {
            ReencodeProfilePlan plan;
            if (source.empty()) return plan;
            const uint64_t hash = knownHash ? knownHash : BakeTuningCache::HashFile(source);
            int maxLODs = fallbackLODs;
            float deadband = fallbackDeadband;
            const auto tunings = BakeTuningCache::Load();
            const auto it = tunings.find(hash);
            if (it != tunings.end()) { maxLODs = it->second.maxLODs; deadband = it->second.deadbandMM; }
            if (rec > maxLODs) maxLODs = std::min(rec, 6);
            RdoProfile stored;
            if (FindRdoProfile(folder, hash, maxLODs, deadband, stored, plan.path)) plan.reuse = true;
            else plan.path = NewRdoProfilePath(folder, source, maxLODs, deadband);
            return plan;
        });
    }

    void SplatPakApp::FailReencode(const std::string& why)
    {
        m_reencode = Reencode::Idle;
        m_statusMessage = "Re-encode stopped: " + why;
        m_statusIsSuccess = false;
        LogTransitionTrace("Re-encode stopped: %s", why.c_str());
    }

    // One step of a re-encode per frame, each once the operation before it has finished: open the source scan, apply the
    // structural fixes and bake, analyse the compression for the network (unless done), bake with its pick, save, reopen
    void SplatPakApp::StepReencode()
    {
        if (m_reencode == Reencode::Idle || m_reencode == Reencode::Confirm || m_op.Busy()) return;
        const EncodingReport& r = m_reencodeReport;
        switch (m_reencode)
        {
            case Reencode::Open:
                m_reencode = Reencode::Structure;
                if (m_isLoadedFromSFLW || !SameFile(m_loadedFilePath, m_reencodeSource)) LoadFile(m_reencodeSource);
                return;
            case Reencode::Structure:
                if (m_isLoadedFromSFLW || !SameFile(m_loadedFilePath, m_reencodeSource) || m_rawSplats.empty()) { FailReencode("the source scan could not be opened."); return; }
                if (r.recMaxLODs > m_maxLODLevels) m_maxLODLevels = std::min(r.recMaxLODs, 6);
                if (r.recChunk > 0.0f && r.recChunk < m_chunkSize) m_chunkSize = r.recChunk;
                if (r.recOcclusion) m_generateOcclusionVolume = true;
                if (BakePending()) StartBake(false);
                m_reencode = Reencode::Analyse;
                return;
            case Reencode::Analyse:
                if (BakePending()) { FailReencode("the bake did not finish."); return; }
                m_rdoTarget.mode = RdoAnalysis::TargetMode::Streaming;
                m_rdoTarget.bandwidthMBps = r.bandwidthMBps > 0.0 ? r.bandwidthMBps : 1000.0;
                m_rdoNetwork = r.bandwidthMBps == 0.2 ? 0 : r.bandwidthMBps == 2.0 ? 1 : r.bandwidthMBps == 7.5 ? 2 : 3;
                m_rdoOverride = false;
                if (RdoStale()) StartRdoAnalysis();
                else
                {
                    RdoProfile stored;
                    std::string path;
                    if (FindRdoProfile(RdoProfileFolder(), m_rdoHash, m_rdoParams.maxLODs, m_rdoParams.deadbandMM, stored, path)) m_lastRdoProfilePath = path;
                    else StoreRdoProfile();
                }
                m_reencode = Reencode::Bake;
                return;
            case Reencode::Bake:
                if (RdoStale()) { FailReencode("the compression analysis did not finish."); return; }
                ApplyRdoChoice(RdoAnalysis::Pick(m_rdo, m_rdoTarget, (double)m_sourceFileBytes));
                if (BakePending()) StartBake(false);
                m_reencode = Reencode::Save;
                return;
            case Reencode::Save:
                if (BakePending()) { FailReencode("the bake did not finish."); return; }
                m_packagePath.clear();
                ProcessAndExport(m_reencodeOutput);
                m_reencode = Reencode::Reload;
                return;
            case Reencode::Reload:
            {
                m_reencode = Reencode::Idle;
                if (m_packagePath.empty()) { FailReencode("the package could not be written."); return; }
                // Remembered for the scan, so opening it again bakes the same way
                BakeTuning t;
                t.chunkSize = m_chunkSize; t.maxLODs = m_maxLODLevels; t.shQuality = m_splatSHQuality; t.shPrecision = m_shPrecision;
                for (int b = 0; b < 3; b++) t.shBits[b] = m_shBandBits[b];
                t.deadbandMM = m_deadbandThresholdMM; t.occlusion = m_generateOcclusionVolume; t.file = m_loadedFilePath;
                if (m_sourceHash != 0) BakeTuningCache::Store(m_sourceHash, t);
                const std::string written = m_packagePath;
                const std::string saved = m_statusMessage;
                if (m_reencodeReload && !LoadSFLWFile(written)) { FailReencode("the new package was written but could not be opened: " + written); return; }
                m_statusMessage = "Re-encoded for " + r.profile + ": " + saved + (m_lastRdoProfilePath.empty() ? std::string() : "; profile " + m_lastRdoProfilePath);
                m_statusIsSuccess = true;
                LogTransitionTrace("Re-encode: wrote %s", written.c_str());
                return;
            }
            default: return;
        }
    }

    void SplatPakApp::StepPresentationShot()
    {
        if (m_presentShotTimer < 0.0 || m_presentShotRequested) return;
        m_presentShotTimer -= m_deltaTime / 1000.0;
        if (m_presentShotTimer > 0.0) return;
        m_pRenderer->RequestSceneCapture(true);
        m_presentShotRequested = true;
    }

    void SplatPakApp::SavePresentationShot()
    {
        RgbaImage img;
        m_pRenderer->TakeSceneCapture(img.pixels, img.width, img.height);
        if (img.Empty()) return; // Not read back yet
        m_presentShotRequested = false;
        m_presentShotTimer = -1.0;
        char pictures[MAX_PATH] = {};
        std::string folder = (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYPICTURES, nullptr, 0, pictures)) ? std::string(pictures) : GetProjectRootFolder()) + "\\SplatPak Presentation";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d.%02d.%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        const std::string path = folder + "\\" + stamp + " " + m_presentShotName + ".png";
        const bool ok = ImageCompare::WritePng(path, img);
        m_statusMessage = ok ? "Presentation screenshot saved: " + path : "Could not save the presentation screenshot to " + path;
        m_statusIsSuccess = ok;
        LogTransitionTrace("Presentation: %s", m_statusMessage.c_str());
    }

    void SplatPakApp::PlaceLoupeBottomRight()
    {
        // DrawLoupeOverlay keeps it clear of the panels; 40 px leaves the status bar visible
        const ImVec2 inset(std::min(m_loupeSize.x, (float)m_Width), std::min(m_loupeSize.y, (float)m_Height));
        m_loupePos = ImVec2((float)m_Width - m_rightPanelWidth - inset.x - 24.0f, (float)m_Height - inset.y - 40.0f);
    }

    // Before a side panel's Begin: a click on the fold arrow at the left end of its title bar (last frame's rectangle)
    // folds or unfolds it. ImGui 1.53 draws the arrow but only folds on a double-click of the title bar, which still
    // works too; the saved state applies on the first frame.
    void SplatPakApp::SetNextSidePanelCollapse(const ImVec4& rect, bool collapsed)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float bar = ImGui::GetFrameHeight();
        const bool onArrow = rect.z > rect.x && io.MouseClicked[0]
            && io.MousePos.x >= rect.x && io.MousePos.x <= rect.x + 1.5f * bar && io.MousePos.y >= rect.y && io.MousePos.y <= rect.y + bar;
        if (onArrow) ImGui::SetNextWindowCollapsed(!collapsed, ImGuiCond_Always);
        else ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Once);
    }

    // Called inside a side panel's Begin/End: records the rectangle it occupies on screen (only the title bar while
    // folded) and remembers a fold or unfold in config.json
    void SplatPakApp::RecordSidePanel(ImVec4& rect, bool& collapsed)
    {
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const bool nowCollapsed = ImGui::IsWindowCollapsed();
        const float height = nowCollapsed ? ImGui::GetFrameHeight() : size.y;
        rect = ImVec4(pos.x, pos.y, pos.x + size.x, pos.y + height);
        if (nowCollapsed != collapsed)
        {
            collapsed = nowCollapsed;
            SaveConfigFile();
        }
    }

    // --stream-test: once fully resident, the launch view, 5G with Release Unused LOD Segments off, Evict, 60 s of refill, then quit
    void SplatPakApp::StepStreamTest()
    {
        if (m_stream.m_lodStreamChunks.empty()) return;
        if (m_streamTestPhase == 0)
        {
            m_unthrottledBandwidth = true;
            if (m_stream.m_totalStreamBytes <= 0.0f || m_stream.m_simulatedBytesDelivered < m_stream.m_totalStreamBytes * 0.999f) return;
            m_autoRotate = false; m_enableStreamDecay = false; m_loupeEnabled = false;
            if (m_detachCamera) SetDetachCamera(false);
            m_yaw = 0.6f; m_pitch = 0.35f; m_target = m_center; ApplyFitDistance();
            m_releaseUnusedSegments = false;
            m_unthrottledBandwidth = false; m_bandwidthThrottleMBps = 7.5f;
            m_streamTestSeconds = 0.0;
            m_streamTestPhase = 1;
            return;
        }
        m_streamTestSeconds += m_deltaTime / 1000.0;
        if (m_streamTestPhase == 1 && m_streamTestSeconds >= 3.0)
        {
            LogTransitionTrace("Stream test: Evict at 5G (7.5 MB/s), launch view");
            ClearResidentStream();
            m_streamTestSeconds = 0.0;
            m_streamTestPhase = 2;
        }
        else if (m_streamTestPhase == 2 && m_streamTestSeconds >= 60.0)
        {
            LogTransitionTrace("Stream test: done after 60 s");
            m_streamTestPhase = 3;
            PostQuitMessage(0);
        }
    }

    // --lod-calibration: see LodCalShot in SplatPakApp.h
    void SplatPakApp::StepLodCalibration()
    {
        const int maxLevel = (int)m_waveletResult.lodLevels.size() - 1;
        if (!m_lodCalRunning)
        {
            if (maxLevel <= 0 || (int)m_stream.m_lodSpacing.size() <= maxLevel || m_stream.m_lodStreamChunks.empty()) return;
            m_unthrottledBandwidth = true;
            if (m_stream.m_totalStreamBytes <= 0.0f || m_stream.m_simulatedBytesDelivered < m_stream.m_totalStreamBytes * 0.999f) return;

            m_autoRotate = false;
            m_enableStreamDecay = false;
            if (m_detachCamera) SetDetachCamera(false);
            m_demoteChunks = true;          // Blocks coarsen again when a setting asks for less detail
            m_showChunkStream = false;      // No arrival glow in the captures
            m_loupeEnabled = false;
            m_lodScreenSpace = true;
            m_yaw = 0.6f; m_pitch = 0.35f; m_target = m_center;
            m_lodCalFit = FitDistanceForViewport();
            static const float kFactors[] = { 0.55f, 1.0f, 1.8f };
            static const float kThresholds[] = { 1.0f, 0.85f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f };
            for (float f : kFactors)
            {
                auto add = [&](int kind, float px) { LodCalShot s; s.distanceFactor = f; s.kind = kind; s.pixelSpacing = px; m_lodCalShots.push_back(std::move(s)); };
                add(0, 1.0f);
                add(1, 1.0f);
                add(2, 1.0f);
                for (float px : kThresholds) add(3, px);
            }
            m_lodCalIndex = 0;
            m_lodCalStableFrames = -1;
            m_lodCalRunning = true;
            LogTransitionTrace("LOD calibration: %zu captures at %ux%u, fitted distance %.3f", m_lodCalShots.size(), m_Width, m_Height, m_lodCalFit);
        }

        if (m_lodCalIndex >= m_lodCalShots.size()) return;
        LodCalShot& shot = m_lodCalShots[m_lodCalIndex];

        if (m_lodCalCaptureRequested)
        {
            if (shot.image.Empty()) return; // Taken after this frame's render (see OnRender)
            LogTransitionTrace("LOD calibration: %zu/%zu x%.2f kind %d px %.2f: %llu splats, settled %d after %.1f s",
                m_lodCalIndex + 1, m_lodCalShots.size(), shot.distanceFactor, shot.kind, shot.pixelSpacing, (unsigned long long)shot.splats, shot.settled ? 1 : 0, shot.seconds);
            m_lodCalCaptureRequested = false;
            m_lodCalStableFrames = -1;
            if (++m_lodCalIndex >= m_lodCalShots.size() || m_lodCalShots[m_lodCalIndex].distanceFactor != shot.distanceFactor)
                FinishLodCalibration();
            return;
        }

        if (m_lodCalStableFrames < 0)
        {
            m_distance = m_lodCalFit * shot.distanceFactor;
            if (shot.kind <= 1)
            {
                m_autoLOD = false;
                m_selectedPreviewLOD = 0;
            }
            else
            {
                m_autoLOD = true;
                m_lodPerBlock = (shot.kind == 3);
                m_lodPixelSpacing = shot.pixelSpacing;
            }
            UpdatePreviewSplats();
            m_lodCalStableFrames = 0;
            m_lodCalShotSeconds = 0.0;
            m_lodCalLastSplats = ~0ull;
            m_lodCalStableSeconds = 0.0;
            return;
        }

        // Settled: no cross-fade running and the same render list for 2.5 s. Time, not frames: the model-wide rule
        // steps one level per cool-down (the dither duration, 0.75 s), so a frame count settled between its steps at
        // a high frame rate. A shot that never settles is captured after 60 s and marked
        m_lodCalShotSeconds += m_deltaTime / 1000.0;
        uint64_t splats = 0;
        for (const StreamChunk* pc : m_stream.m_rendererSourceChunks) splats += pc->rawSplats.size();
        const bool still = (m_stream.m_lastActiveTransitions == 0 && splats == m_lodCalLastSplats);
        m_lodCalStableSeconds = still ? m_lodCalStableSeconds + m_deltaTime / 1000.0 : 0.0;
        m_lodCalStableFrames = still ? m_lodCalStableFrames + 1 : 0;
        m_lodCalLastSplats = splats;
        if (m_lodCalStableSeconds >= 2.5 || m_lodCalShotSeconds > 60.0)
        {
            shot.splats = splats;
            shot.chunks = (uint32_t)m_stream.m_rendererSourceChunks.size();
            for (const StreamChunk* pc : m_stream.m_rendererSourceChunks) shot.perLevel[std::min(7, std::max(0, pc->lodLevel))] += pc->rawSplats.size();
            shot.seconds = (float)m_lodCalShotSeconds;
            shot.settled = (m_lodCalStableSeconds >= 2.5);
            m_pRenderer->RequestSceneCapture();
            m_lodCalCaptureRequested = true;
        }
    }

    void SplatPakApp::FinishLodCalibration()
    {
        const int maxLevel = (int)m_waveletResult.lodLevels.size() - 1;
        CreateDirectoryA(m_lodCalFolder.c_str(), nullptr);
        std::ofstream md(m_lodCalFolder + "\\report.md");
        std::ofstream csv(m_lodCalFolder + "\\results.csv");
        csv << "distance_factor,distance,setting,pixel_threshold,splats,splats_vs_modelwide,psnr_db,ssim,settled,seconds\n";
        md << "# LOD calibration\n\n";
        md << "Model: `" << m_inputPathBuf << "`, " << m_Width << "x" << m_Height << ", 45 degree vertical field of view, yaw " << m_yaw << ", pitch " << m_pitch
           << ", streaming on with Demote Chunks on. At each distance (a multiple of the fitted launch distance " << m_lodCalFit << ") every setting is compared with level 0 "
              "everywhere at the same distance (PSNR over RGB and SSIM over luma, inside the model's bounds). `level 0 again` is a second level 0 capture: the noise floor.\n\n";
        auto label = [](const LodCalShot& s) -> std::string
        {
            char b[64];
            if (s.kind == 0) return "level 0 (reference)";
            if (s.kind == 1) return "level 0 again";
            if (s.kind == 2) { snprintf(b, sizeof(b), "model-wide, %.2f px", s.pixelSpacing); return b; }
            snprintf(b, sizeof(b), "per-block, %.2f px", s.pixelSpacing); return b;
        };
        for (size_t i = 0; i < m_lodCalShots.size();)
        {
            const float f = m_lodCalShots[i].distanceFactor;
            size_t end = i;
            while (end < m_lodCalShots.size() && m_lodCalShots[end].distanceFactor == f && !m_lodCalShots[end].image.Empty()) end++;
            const LodCalShot* ref = nullptr; const LodCalShot* wide = nullptr;
            std::vector<const RgbaImage*> all;
            for (size_t k = i; k < end; k++)
            {
                if (m_lodCalShots[k].kind == 0) ref = &m_lodCalShots[k];
                if (m_lodCalShots[k].kind == 2) wide = &m_lodCalShots[k];
                all.push_back(&m_lodCalShots[k].image);
            }
            if (ref && end > i)
            {
                const PixelRect rect = ImageCompare::ContentBounds(all);
                md << "## Distance " << f << " x fitted (" << m_lodCalFit * f << ")\n\n";
                md << "| Setting | Splats drawn | vs model-wide | By level (thousands, L0..) | PSNR (dB) | SSIM | Settled |\n|---|---:|---:|---|---:|---:|:-:|\n";
                std::vector<RgbaImage> panels;
                for (size_t k = i; k < end; k++)
                {
                    const LodCalShot& s = m_lodCalShots[k];
                    const double psnr = (s.kind == 0) ? 0.0 : ImageCompare::Psnr(ref->image, s.image, rect);
                    const double ssim = (s.kind == 0) ? 1.0 : ImageCompare::Ssim(ref->image, s.image, rect);
                    const double vsWide = (wide && wide->splats > 0) ? (double)s.splats / (double)wide->splats : 0.0;
                    std::string levels;
                    for (int l = 0; l <= std::min(7, maxLevel); l++) { char b[24]; snprintf(b, sizeof(b), "%s%llu", l ? " / " : "", (unsigned long long)(s.perLevel[l] / 1000)); levels += b; }
                    char vs[16] = "", ps[16] = "", ss[16] = "";
                    if (vsWide > 0.0) snprintf(vs, sizeof(vs), "%d%%", (int)std::lround(vsWide * 100.0));
                    if (s.kind != 0) { snprintf(ps, sizeof(ps), "%.1f", psnr); snprintf(ss, sizeof(ss), "%.3f", ssim); }
                    char row[512];
                    snprintf(row, sizeof(row), "| %s | %llu | %s | %s | %s | %s | %s |\n", label(s).c_str(), (unsigned long long)s.splats, vs, levels.c_str(), ps, ss, s.settled ? "yes" : "no");
                    md << row;
                    csv << f << "," << m_lodCalFit * f << "," << label(s) << "," << s.pixelSpacing << "," << s.splats << "," << vsWide << "," << psnr << "," << ssim << "," << (s.settled ? 1 : 0) << "," << s.seconds << "\n";
                    panels.push_back(ImageCompare::Crop(s.image, rect));
                }
                std::vector<const RgbaImage*> panelPtrs;
                for (const RgbaImage& p : panels) panelPtrs.push_back(&p);
                char name[64]; snprintf(name, sizeof(name), "distance_%.2f.png", f);
                ImageCompare::WritePng(m_lodCalFolder + "\\" + name, ImageCompare::SideBySide(panelPtrs));
                md << "\nLeft to right, in the table's order: " << name << "\n\n![" << name << "](" << name << ")\n\n";
            }
            i = std::max(end, i + 1);
            while (i < m_lodCalShots.size() && m_lodCalShots[i].distanceFactor == f) i++;
        }
        LogTransitionTrace("LOD calibration: report written to %s", m_lodCalFolder.c_str());
    }

    static const float kViewCapYaws[] = { 0.0f, 0.785f, 1.571f, 2.356f, 3.142f, 3.927f, 4.712f, 5.498f };
    static constexpr size_t kViewCapViews = sizeof(kViewCapYaws) / sizeof(kViewCapYaws[0]);

    // --capture-views: once fully resident, level 0 from each direction in turn (each settled for 1.5 s), then the
    // first again; FinishViewCapture writes and compares them
    void SplatPakApp::StepViewCapture()
    {
        if (!m_viewCapRunning)
        {
            if (m_rawSplats.empty() && m_framesRendered > 30)
            {
                LogTransitionTrace("View capture: no model loaded (%s); quitting", m_statusMessage.c_str());
                m_viewCapFolder.clear();
                PostQuitMessage(0);
                return;
            }
            if (m_stream.m_lodStreamChunks.empty()) return;
            m_unthrottledBandwidth = true;
            if (m_stream.m_totalStreamBytes <= 0.0f || m_stream.m_simulatedBytesDelivered < m_stream.m_totalStreamBytes * 0.999f) return;
            m_autoRotate = false;
            m_enableStreamDecay = false;
            if (m_detachCamera) SetDetachCamera(false);
            m_showChunkStream = false; // No arrival glow
            m_loupeEnabled = false;
            m_autoLOD = false;
            m_selectedPreviewLOD = 0;
            m_target = m_center;
            m_viewCapFit = FitDistanceForViewport();
            m_viewCapImages.assign(kViewCapViews + 1, RgbaImage());
            m_viewCapIndex = 0;
            m_viewCapStableSeconds = -1.0;
            m_viewCapRunning = true;
            UpdatePreviewSplats();
            LogTransitionTrace("View capture: %zu views at %ux%u, distance %.3f", m_viewCapImages.size(), m_Width, m_Height, m_viewCapFit * 0.7f);
        }
        if (m_viewCapIndex >= m_viewCapImages.size()) return;
        if (m_viewCapRequested)
        {
            if (m_viewCapImages[m_viewCapIndex].Empty()) return; // Taken after this frame's render (see OnRender)
            m_viewCapRequested = false;
            m_viewCapStableSeconds = -1.0;
            if (++m_viewCapIndex >= m_viewCapImages.size()) FinishViewCapture();
            return;
        }
        if (m_viewCapStableSeconds < 0.0)
        {
            m_yaw = kViewCapYaws[m_viewCapIndex % kViewCapViews];
            m_pitch = 0.3f;
            m_distance = m_viewCapFit * 0.7f;
            m_viewCapStableSeconds = 0.0;
            m_viewCapLastSplats = ~0ull;
            return;
        }
        uint64_t splats = 0;
        for (const StreamChunk* pc : m_stream.m_rendererSourceChunks) splats += pc->rawSplats.size();
        const bool still = (m_stream.m_lastActiveTransitions == 0 && splats == m_viewCapLastSplats);
        m_viewCapStableSeconds = still ? m_viewCapStableSeconds + m_deltaTime / 1000.0 : 0.0;
        m_viewCapLastSplats = splats;
        if (m_viewCapStableSeconds >= 1.5)
        {
            m_pRenderer->RequestSceneCapture();
            m_viewCapRequested = true;
        }
    }

    void SplatPakApp::FinishViewCapture()
    {
        std::error_code dirError;
        std::filesystem::create_directories(m_viewCapFolder, dirError); // Parents too: CreateDirectoryA makes one level only
        auto file = [](const std::string& folder, size_t i) { return folder + "\\view_" + std::to_string(i) + ".rgba"; };
        size_t written = 0;
        for (size_t i = 0; i < m_viewCapImages.size(); i++)
        {
            const RgbaImage& img = m_viewCapImages[i];
            std::ofstream f(file(m_viewCapFolder, i), std::ios::binary);
            f.write((const char*)&img.width, 4);
            f.write((const char*)&img.height, 4);
            f.write((const char*)img.pixels.data(), (std::streamsize)img.pixels.size());
            if (f) written++;
        }
        if (written < m_viewCapImages.size())
            LogTransitionTrace("View capture: ERROR: wrote %zu of %zu views to %s", written, m_viewCapImages.size(), m_viewCapFolder.c_str());
        if (!m_viewCapReference.empty())
        {
            std::ofstream report(m_viewCapFolder + "\\report.txt");
            report << "view,psnr_db,ssim\n";
            double sumPsnr = 0.0, sumSsim = 0.0, minPsnr = 1e9;
            size_t n = 0;
            for (size_t i = 0; i < m_viewCapImages.size(); i++)
            {
                RgbaImage ref;
                std::ifstream f(file(m_viewCapReference, i), std::ios::binary);
                if (!f) continue;
                f.read((char*)&ref.width, 4);
                f.read((char*)&ref.height, 4);
                ref.pixels.resize((size_t)ref.width * ref.height * 4);
                f.read((char*)ref.pixels.data(), (std::streamsize)ref.pixels.size());
                const RgbaImage& img = m_viewCapImages[i];
                if (!f || ref.width != img.width || ref.height != img.height) continue;
                const PixelRect rect = ImageCompare::ContentBounds({ &ref, &img });
                const double psnr = ImageCompare::Psnr(ref, img, rect), ssim = ImageCompare::Ssim(ref, img, rect);
                report << (i < kViewCapViews ? std::to_string(i) : std::string("0 again")) << "," << psnr << "," << ssim << "\n";
                if (i < kViewCapViews) { sumPsnr += psnr; sumSsim += ssim; minPsnr = std::min(minPsnr, psnr); n++; }
            }
            if (n > 0)
            {
                report << "mean," << sumPsnr / n << "," << sumSsim / n << "\nworst," << minPsnr << ",\n";
                LogTransitionTrace("View capture: against %s, mean PSNR %.2f dB (worst %.2f), mean SSIM %.4f", m_viewCapReference.c_str(), sumPsnr / n, minPsnr, sumSsim / n);
            }
        }
        LogTransitionTrace("View capture: written to %s", m_viewCapFolder.c_str());
        m_viewCapFolder.clear();
        PostQuitMessage(0);
    }

    // --bake-to: the startup load baked the scan inline (scripted runs do not use the worker); save it, inline too, and quit
    void SplatPakApp::StepCliBake()
    {
        if (m_rawSplats.empty() || BakePending() || m_waveletResult.lodLevels.empty())
        {
            LogTransitionTrace("Bake to %s: nothing baked (%s); quitting", m_cliBakeOutput.c_str(), m_statusMessage.c_str());
        }
        else
        {
            std::string base = m_cliBakeOutput;
            if (base.size() > 5 && base.substr(base.size() - 5) == ".gstx") base.resize(base.size() - 5);
            ProcessAndExport(base);
            std::ifstream f(base + ".gstx", std::ios::ate | std::ios::binary);
            if (m_cliRemember)
            {
                BakeTuning t;
                t.chunkSize = m_chunkSize; t.maxLODs = m_maxLODLevels; t.shQuality = m_splatSHQuality; t.shPrecision = m_shPrecision;
                for (int b = 0; b < 3; b++) t.shBits[b] = m_shBandBits[b];
                t.deadbandMM = m_deadbandThresholdMM; t.occlusion = m_generateOcclusionVolume; t.file = m_loadedFilePath;
                const uint64_t hash = BakeTuningCache::HashFile(m_loadedFilePath);
                BakeTuningCache::Store(hash, t);
                LogTransitionTrace("Bake to: settings remembered for %s (%016llx)", m_loadedFilePath.c_str(), (unsigned long long)hash);
            }
            LogTransitionTrace("Bake to %s: SH degree %u, %u bits: %lld bytes (%s)", (base + ".gstx").c_str(), BakeSHDegree(), (unsigned)SHBitsFor(m_shPrecision),
                f ? (long long)f.tellg() : -1LL, m_statusMessage.c_str());
        }
        m_cliBakeOutput.clear();
        PostQuitMessage(0);
    }

    void SplatPakApp::FinishLodQualityTest()
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
        for (size_t l = 0; l < m_stream.m_lodSpacing.size(); l++) md << " L" << l << " " << m_stream.m_lodSpacing[l] * 1000.0f << " mm";
        md << "\n\n";

        bool asSplats = false;
        auto find = [&](bool screenSpace, int switchLevel, int level, bool gauss, int repeat, float cap = 0.0f) -> const LodTestShot*
        {
            for (const auto& s : m_lodTestShots)
                if (s.asSplats == asSplats && s.screenSpaceRule == screenSpace && s.switchLevel == switchLevel && s.level == level && s.coarseAsGaussians == gauss
                    && s.coarseGaussianCap == cap && s.repeat == repeat && !s.image.Empty())
                    return &s;
            return nullptr;
        };
        const int maxLevel = (int)m_waveletResult.lodLevels.size() - 1;
        std::vector<std::string> montages;
        for (int section = 0; section < 4; section++)
        {
            asSplats = (section >= 2);
            const bool screenSpace = (section & 1) != 0;
            if (!m_splatMode && asSplats) break;
            const char* modeName = !m_splatMode ? "" : (asSplats ? "Drawn as splats (mode 4): " : "Sorted Gaussians (mode 3): ");
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
                    const float spacingPx = (r.level < (int)m_stream.m_lodSpacing.size()) ? m_stream.m_lodSpacing[r.level] * m_stream.LodFocalPixels() / m_stream.LodEffectiveDistance(ref->distance) : 0.0f;
                    char line[256];
                    snprintf(line, sizeof(line), "| L%d | %.3f | L%d %s | %.2f px | %.2f | %.4f |\n", L, ref->distance, r.level, r.name.c_str(), spacingPx, psnr, ssim);
                    md << line;
                    csv << (asSplats ? "splats," : "gaussians,") << (screenSpace ? "screen" : "legacy") << "," << L << "," << ref->distance << "," << r.level << ",\"" << r.name << "\"," << spacingPx << "," << psnr << "," << ssim << "\n";
                }
                // Montage, sorted Gaussians: level 0 | new level as discs | as Gaussians | capped Gaussians (each cap).
                // Splat: level 0 | previous level | new level | its error against level 0.
                const LodTestShot* prev = find(screenSpace, L, L - 1, false, 0);
                const LodTestShot* discs = find(screenSpace, L, L, false, 0);
                if (prev && discs)
                {
                    char name[64];
                    snprintf(name, sizeof(name), "%s_%s_switch_L%d.png", asSplats ? "splats" : "gaussians", screenSpace ? "screen" : "legacy", L);
                    if (std::find(m_lodTestMontagesWritten.begin(), m_lodTestMontagesWritten.end(), name) == m_lodTestMontagesWritten.end())
                    {
                        std::vector<RgbaImage> panels;
                        panels.push_back(ImageCompare::Crop(ref->image, rect));
                        if (asSplats)
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
            md << (asSplats ? "\nMontages, left to right: level 0, the previous level, the new level, then the new level's error against level 0 (x4).\n\n"
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
    void SplatPakApp::ReportDeviceLostAndExit(const std::string& message)
    {
        if (m_deviceLost) return; // Already reported/exiting -- avoid stacking duplicate dialogs
        m_deviceLost = true;

        m_statusMessage = message;
        m_statusIsSuccess = false;
        Trace("%s\n", message.c_str());

        std::string dialogText = message + "\n\nThe application cannot recover from this and will now close.";
        const std::string dialogTitle = std::string(AppTitle()) + " - GPU Device Lost";
        if (!Unattended()) MessageBoxA(nullptr, dialogText.c_str(), dialogTitle.c_str(), MB_OK | MB_ICONERROR);
        PostQuitMessage(0);
    }
}

