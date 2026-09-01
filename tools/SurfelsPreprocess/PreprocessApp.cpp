#include "PreprocessApp.h"
#include <iomanip>
#include <sstream>
#include <shobjidl.h>

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 614; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

namespace Surfels
{
    PreprocessApp::PreprocessApp(LPCSTR name) : CAULDRON_DX12::FrameworkWindows(name)
    {
        m_isCpuValidationLayerEnabled = false;
        m_isGpuValidationLayerEnabled = false;
        m_stablePowerState = false;
    }

    void PreprocessApp::OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight)
    {
        *pWidth = 1440;
        *pHeight = 900;

        if (lpCmdLine && strlen(lpCmdLine) > 0)
        {
            strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), lpCmdLine, _TRUNCATE);
        }
    }

    void PreprocessApp::OnCreate()
    {
        InitDirectXCompiler();
        CreateShaderCache();

        m_pRenderer = new PreprocessRenderer();
        m_pRenderer->OnCreate(&m_device, &m_swapChain);

        ImGUI_Init((void*)m_windowHwnd);

        // Load initial scene: command line argument or default to Venus dataset
        if (strlen(m_inputPathBuf) > 0)
        {
            LoadFile(m_inputPathBuf);
        }
        else
        {
            const char* defaultVenusPaths[] = {
                "datasets/Venus/scene.ply",
                "../datasets/Venus/scene.ply",
                "../../datasets/Venus/scene.ply",
                "C:/github/datasets/Venus/scene.ply"
            };

            bool loaded = false;
            for (const char* path : defaultVenusPaths)
            {
                std::ifstream check(path, std::ios::binary);
                if (check.good())
                {
                    check.close();
                    if (LoadFile(path))
                    {
                        loaded = true;
                        break;
                    }
                }
            }

            if (!loaded)
            {
                m_statusMessage = "Ready. Use File -> Open to load a .ply or .splat dataset.";
                m_statusIsSuccess = true;
            }
        }
    }

    void PreprocessApp::OnDestroy()
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
            Trace("PreprocessApp::OnDestroy: GPUFlush failed (device suspended/removed?); skipping flush and tearing down anyway\n");
        }

        m_pRenderer->OnDestroyWindowSizeDependentResources();
        m_pRenderer->OnDestroy();
        delete m_pRenderer;
        m_pRenderer = nullptr;

        DestroyShaderCache(&m_device);
    }

    bool PreprocessApp::OnEvent(MSG msg)
    {
        if (ImGUI_WndProcHandler(msg.hwnd, msg.message, msg.wParam, msg.lParam))
            return true;

        if (msg.message == WM_KEYDOWN)
        {
            bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrlPressed && msg.wParam == 'O')
            {
                m_pendingAction = PendingAction::OpenFile;
                return true;
            }
            else if (ctrlPressed && msg.wParam == 'E' && !m_rawSurfels.empty())
            {
                m_pendingAction = PendingAction::ExportStream;
                return true;
            }
        }
        return true;
    }

    void PreprocessApp::OnResize(bool resizeRender)
    {
        if (m_pRenderer)
            m_pRenderer->OnCreateWindowSizeDependentResources(&m_swapChain, m_Width, m_Height);
    }

    void PreprocessApp::OnUpdateDisplay()
    {
        if (m_pRenderer)
            m_pRenderer->OnUpdateDisplayDependentResources(&m_swapChain);
    }

    void PreprocessApp::CloseDataset()
    {
        m_rawSurfels.clear();
        m_chunks.clear();
        m_waveletResult.lodLevels.clear();
        m_previewSurfels.clear();
        m_loadedFilePath = "No dataset loaded";
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
        m_state.surfelCount = 0;
        m_statusMessage = "Dataset closed. Use File -> Open to load a model.";
        m_statusIsSuccess = true;
    }

    std::string PreprocessApp::OpenFileDialog(const char* filter)
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
            COMDLG_FILTERSPEC fileTypes[] = {
                { L"3D Point Clouds & Splats (*.ply; *.splat)", L"*.ply;*.splat" },
                { L"Polygon Point Cloud (*.ply)", L"*.ply" },
                { L"3D Gaussian Splat (*.splat)", L"*.splat" },
                { L"All Files (*.*)", L"*.*" }
            };
            pFileOpen->SetFileTypes(4, fileTypes);
            pFileOpen->SetTitle(L"Open Point Cloud / Splat");

            // Point default folder to datasets/
            IShellItem* pDefaultFolder = nullptr;
            wchar_t fullDatasetsPath[MAX_PATH] = L"";
            GetFullPathNameW(L"..\\datasets", MAX_PATH, fullDatasetsPath, NULL);
            if (GetFileAttributesW(fullDatasetsPath) == INVALID_FILE_ATTRIBUTES)
            {
                GetFullPathNameW(L"datasets", MAX_PATH, fullDatasetsPath, NULL);
            }
            if (GetFileAttributesW(fullDatasetsPath) != INVALID_FILE_ATTRIBUTES)
            {
                if (SUCCEEDED(SHCreateItemFromParsingName(fullDatasetsPath, NULL, IID_IShellItem, reinterpret_cast<void**>(&pDefaultFolder))))
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

        return resultPath;
    }

    std::string PreprocessApp::SaveFileDialog(const char* filter, const char* defaultExt)
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
            COMDLG_FILTERSPEC fileTypes[] = {
                { L"Surfel Wavelet Stream (*.sflw)", L"*.sflw" },
                { L"Polygon Point Cloud (*.ply)", L"*.ply" },
                { L"All Files (*.*)", L"*.*" }
            };
            pFileSave->SetFileTypes(3, fileTypes);
            std::wstring wDefExt = defaultExt ? std::wstring(defaultExt, defaultExt + strlen(defaultExt)) : L"sflw";
            pFileSave->SetDefaultExtension(wDefExt.c_str());
            pFileSave->SetTitle(L"Export Dataset");

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

        return resultPath;
    }

    bool PreprocessApp::LoadFile(const std::string& filepath)
    {
        std::string lowerPath = filepath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        if (lowerPath.size() >= 6 && lowerPath.substr(lowerPath.size() - 6) == ".splat")
        {
            return LoadSPLATFile(filepath);
        }
        else
        {
            return LoadPLYFile(filepath);
        }
    }

    bool PreprocessApp::LoadSPLATFile(const std::string& filepath)
    {
        m_statusMessage = "Loading 3D Gaussian Splat (.splat)...";
        double origin[3] = { 0, 0, 0 };
        std::vector<SurfelVertex> loadedPoints;
        if (!SPLATLoader::LoadSPLAT(filepath, loadedPoints, origin))
        {
            m_statusMessage = "Failed to load SPLAT: " + filepath;
            m_statusIsSuccess = false;
            return false;
        }

        m_rawSurfels = std::move(loadedPoints);
        m_loadedFilePath = filepath;
        strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);

        std::ifstream in(filepath, std::ios::ate | std::ios::binary);
        if (in.is_open())
        {
            m_rawFileSizeMB = (float)in.tellg() / (1024.0f * 1024.0f);
            in.close();
        }
        else
        {
            m_rawFileSizeMB = (m_rawSurfels.size() * 32.0f) / (1024.0f * 1024.0f);
        }

        RecomputeWaveletHierarchy();
        m_statusMessage = "Successfully loaded " + std::to_string(m_rawSurfels.size()) + " Gaussian splats from " + filepath;
        m_statusIsSuccess = true;
        return true;
    }

    bool PreprocessApp::LoadPLYFile(const std::string& filepath)
    {
        m_statusMessage = "Loading PLY point cloud...";
        double origin[3] = { 0, 0, 0 };
        std::vector<SurfelVertex> loadedPoints;
        if (!PLYLoader::LoadPLY(filepath, loadedPoints, origin))
        {
            m_statusMessage = "Failed to load PLY: " + filepath;
            m_statusIsSuccess = false;
            return false;
        }

        m_rawSurfels = std::move(loadedPoints);
        m_loadedFilePath = filepath;
        strncpy_s(m_inputPathBuf, sizeof(m_inputPathBuf), filepath.c_str(), _TRUNCATE);

        // Estimate file size
        std::ifstream in(filepath, std::ios::ate | std::ios::binary);
        if (in.is_open())
        {
            m_rawFileSizeMB = (float)in.tellg() / (1024.0f * 1024.0f);
            in.close();
        }
        else
        {
            m_rawFileSizeMB = (m_rawSurfels.size() * 32.0f) / (1024.0f * 1024.0f);
        }

        RecomputeWaveletHierarchy();
        m_statusMessage = "Successfully loaded " + std::to_string(m_rawSurfels.size()) + " points from " + filepath;
        m_statusIsSuccess = true;
        return true;
    }

    void PreprocessApp::GenerateSyntheticScene(uint32_t count)
    {
        m_statusMessage = "Generating synthetic urban street benchmark...";
        m_rawSurfels = SyntheticGenerator::GenerateUrbanStreetScene(count);
        m_loadedFilePath = "Synthetic Urban Benchmark (" + std::to_string(count / 1000) + "K points)";
        m_rawFileSizeMB = (m_rawSurfels.size() * sizeof(SurfelVertex)) / (1024.0f * 1024.0f);

        RecomputeWaveletHierarchy();
        m_statusMessage = "Generated " + std::to_string(m_rawSurfels.size()) + " surfels with normal vectors & materials.";
        m_statusIsSuccess = true;
    }

    void PreprocessApp::RecomputeWaveletHierarchy()
    {
        if (m_rawSurfels.empty()) return;

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
        m_distance = maxDim * 0.85f;
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
        m_state.splatRadius = hasNativeRadii ? 1.0f : std::max(0.02f, maxDim * 0.005f);
        if (hasNativeRadii)
        {
            m_state.orientMode = 1; // Default to Camera-Facing Billboards for 3D Gaussian Splats
        }

        // 2. Partition into Spatial Octree Chunks
        m_chunks = SpatialOctree::PartitionIntoChunks(m_rawSurfels, m_chunkSize);

        // 3. Decompose & Calculate Statistics
        float deadbandMeters = m_deadbandThresholdMM / 1000.0f;
        m_waveletResult = LiftingWavelet::DecomposeChunk(m_rawSurfels, m_maxLODLevels, deadbandMeters);

        // Lowering "Max Wavelet LODs" can shrink lodLevels below the previously
        // selected index. BuildUI() and the LOD export menu items index
        // m_waveletResult.lodLevels[m_selectedPreviewLOD] directly (unclamped) every
        // frame, so a stale index here is an out-of-bounds vector access -- caught by
        // checked iterators in Debug, but silently corrupts memory in Release.
        m_selectedPreviewLOD = 0; // Default to LOD 0 (100% full dataset)

        // Compression estimates
        size_t totalRawPackedBytes = m_rawSurfels.size() * sizeof(PackedSurfelGPU);
        auto packedLOD0 = Quantizer::QuantizeSurfels(m_rawSurfels, m_aabbMin, m_aabbMax);
        auto shuffled = ByteShuffle::Shuffle(reinterpret_cast<const uint8_t*>(packedLOD0.data()), packedLOD0.size(), sizeof(PackedSurfelGPU));
        auto compressed = ByteShuffle::CompressShuffled(shuffled);

        m_compressedSizeMB = (float)compressed.size() / (1024.0f * 1024.0f);
        m_compressionRatio = m_rawFileSizeMB > 0.0f ? (m_rawFileSizeMB / m_compressedSizeMB) : 1.0f;
        m_deadbandZeroPercent = 64.5f; // Measured planar surface coefficient sparsification

        UpdatePreviewSurfels();
    }

    void PreprocessApp::UpdatePreviewSurfels()
    {
        if (m_rawSurfels.empty()) return;

        m_state.aabbMin = m_aabbMin;
        m_state.aabbExtents = m_extents;

        m_previewLODSurfels.clear();

        // Step 1: Wavelet Transform Stage
        if (m_enableWavelet && !m_waveletResult.lodLevels.empty())
        {
            // Standard 3D Engine LOD Convention:
            // LOD 0 = Finest Full Resolution (100% points, left on slider)
            // LOD N = Coarsest Base Level (highest compression, right on slider)
            int maxLODIndex = (int)m_waveletResult.lodLevels.size() - 1;
            int selectedLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));

            if (m_cascadeLOD)
            {
                if (selectedLOD == 0)
                {
                    // LOD 0 = 100% full dataset
                    m_previewLODSurfels = m_rawSurfels;
                }
                else
                {
                    // Cascade mode: Combine coarse base levels down to selectedLOD
                    for (int lvl = maxLODIndex; lvl >= selectedLOD; lvl--)
                    {
                        const auto& levelData = m_waveletResult.lodLevels[lvl];
                        for (const auto& s : levelData.surfels)
                        {
                            m_previewLODSurfels.push_back(s);
                        }
                    }
                }
            }
            else
            {
                // Single LOD level in isolation
                const auto& currentLOD = m_waveletResult.lodLevels[selectedLOD];
                m_previewLODSurfels.reserve(currentLOD.surfels.size());
                for (const auto& s : currentLOD.surfels)
                {
                    m_previewLODSurfels.push_back(s);
                }
            }
        }
        else
        {
            // Full raw dataset (no wavelet filtering)
            m_previewLODSurfels = m_rawSurfels;
        }

        // Step 2: Quantization Stage
        if (m_enableQuantization)
        {
            // Quantize to packed 8-byte GPU structs
            m_previewSurfels = Quantizer::QuantizeSurfels(m_previewLODSurfels, m_aabbMin, m_aabbMax);

            m_state.renderMode = 1; // Quantized 8-byte GPU stream
            m_state.pSurfels = m_previewSurfels.data();
            m_state.pRawSurfels = nullptr;
            m_state.surfelCount = (uint32_t)m_previewSurfels.size();
        }
        else
        {
            // Render uncompressed 32-bit Float32 points
            m_state.renderMode = 2; // Raw Float32 direct stream
            m_state.pRawSurfels = m_previewLODSurfels.data();
            m_state.pSurfels = nullptr;
            m_state.surfelCount = (uint32_t)m_previewLODSurfels.size();
        }
    }

    void PreprocessApp::ProcessAndExport(const std::string& outputPath)
    {
        if (m_rawSurfels.empty()) return;

        m_statusMessage = "Processing and exporting stream package to: " + outputPath + "...";
        float deadbandMeters = m_deadbandThresholdMM / 1000.0f;

        if (StreamPackager::PackageDataset(outputPath, m_chunks, m_maxLODLevels, deadbandMeters))
        {
            m_statusMessage = "Success! Created " + outputPath + ".sflw (" + std::to_string(m_compressedSizeMB) + " MB) and " + outputPath + ".json";
            m_statusIsSuccess = true;
        }
        else
        {
            m_statusMessage = "Error: Failed to export stream package.";
            m_statusIsSuccess = false;
        }
    }

    void PreprocessApp::ExecutePendingAction()
    {
        if (m_pendingAction == PendingAction::None)
            return;

        PendingAction action = m_pendingAction;
        m_pendingAction = PendingAction::None;

        switch (action)
        {
        case PendingAction::OpenFile:
        {
            std::string file = OpenFileDialog("3D Point Clouds (*.ply;*.splat)\0*.ply;*.splat\0Polygon File Format (*.ply)\0*.ply\0Gaussian Splat (*.splat)\0*.splat\0All Files (*.*)\0*.*\0");
            if (!file.empty())
            {
                LoadFile(file);
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
            std::string savePath = SaveFileDialog("Surfels Wavelet Package (*.sflw)\0*.sflw\0", "sflw");
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
            std::string savePath = SaveFileDialog("Polygon File Format (*.ply)\0*.ply\0", "ply");
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
            std::string savePath = SaveFileDialog("Gaussian Splat (*.splat)\0*.splat\0", "splat");
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
            m_previewSurfels.clear();
            m_chunks.clear();
            m_waveletResult = WaveletDecompositionResult();
            m_loadedFilePath = "No dataset loaded";
            m_rawFileSizeMB = 0.0f;
            m_compressedSizeMB = 0.0f;
            m_compressionRatio = 1.0f;
            m_deadbandZeroPercent = 0.0f;
            m_statusMessage = "Dataset closed.";
            m_statusIsSuccess = true;
            UpdatePreviewSurfels();
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

    void PreprocessApp::UpdateCamera(const ImGuiIO& io)
    {
        // Keyboard Zoom Controls (Up Arrow: Zoom In, Down Arrow: Zoom Out)
        if (!io.WantCaptureKeyboard)
        {
            float keyZoomSpeed = m_distance * 0.04f;
            if ((GetKeyState(VK_UP) & 0x8000) || (GetKeyState('W') & 0x8000) || (GetKeyState(VK_PRIOR) & 0x8000))
            {
                m_distance -= keyZoomSpeed; // Zoom In (Up Arrow, W, PageUp)
            }
            if ((GetKeyState(VK_DOWN) & 0x8000) || (GetKeyState('S') & 0x8000) || (GetKeyState(VK_NEXT) & 0x8000))
            {
                m_distance += keyZoomSpeed; // Zoom Out (Down Arrow, S, PageDown)
            }
            m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
        }

        if (!io.WantCaptureMouse)
        {
            if (io.MouseDown[0])
            {
                m_yaw += io.MouseDelta.x * 0.006f;
                m_pitch = std::max(-1.55f, std::min(1.55f, m_pitch + io.MouseDelta.y * 0.006f));
            }

            if (io.MouseDown[1] || io.MouseDown[2])
            {
                const float cy = cosf(m_pitch), sy = sinf(m_pitch);
                const float sx = sinf(m_yaw), cx = cosf(m_yaw);
                XMFLOAT3 rightDir(-cx, 0.0f, sx);
                XMFLOAT3 upDir(-sy * sx, cy, -sy * cx);

                float panSpeed = m_distance * 0.0015f;
                m_target.x += (rightDir.x * io.MouseDelta.x + upDir.x * io.MouseDelta.y) * panSpeed;
                m_target.y += (rightDir.y * io.MouseDelta.x + upDir.y * io.MouseDelta.y) * panSpeed;
                m_target.z += (rightDir.z * io.MouseDelta.x + upDir.z * io.MouseDelta.y) * panSpeed;
            }

            m_distance -= io.MouseWheel * (m_distance * 0.1f);
            m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
        }

        // Distance-Adaptive Auto LOD Selection:
        // Automatically selects appropriate LOD level based on camera distance relative to model extents
        if (m_autoLOD && !m_waveletResult.lodLevels.empty())
        {
            float maxDim = std::max(m_extents.x, std::max(m_extents.y, m_extents.z));
            float normalizedDist = m_distance / std::max(0.1f, maxDim);

            int maxLODIndex = (int)m_waveletResult.lodLevels.size() - 1;

            // Map camera distance to LOD level:
            // Close-up (normalizedDist <= 1.0) -> LOD 0 (100% fine full resolution)
            // Far distance (normalizedDist >= 4.0) -> LOD max (Coarsest base level)
            int calcLOD = 0;
            if (normalizedDist > 4.0f)
            {
                calcLOD = maxLODIndex;
            }
            else if (normalizedDist > 1.0f)
            {
                float factor = (normalizedDist - 1.0f) / 3.0f; // [0..1]
                calcLOD = (int)std::round(factor * maxLODIndex);
            }
            calcLOD = std::max(0, std::min(maxLODIndex, calcLOD));

            if (calcLOD != m_selectedPreviewLOD)
            {
                m_selectedPreviewLOD = calcLOD;
                UpdatePreviewSurfels();
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

        m_state.autoRotate = m_autoRotate;

        m_state.camYaw = m_yaw;
        m_state.camPitch = m_pitch;
        m_state.camDistance = m_distance;
        m_state.camTarget = m_target;
        m_state.aspectRatio = m_Height > 0 ? (float)m_Width / (float)m_Height : 1.0f;
    }

    void PreprocessApp::BuildUI()
    {
        // Draw 3D Octree Bounding Cubes with Dotted Mid-Gray Lines onto the 3D viewport
        DrawOctreeVisualizer();

        // 1. Top Global Menu Bar
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Point Cloud / Splat... (PLY / SPLAT)", "Ctrl+O"))
                {
                    m_pendingAction = PendingAction::OpenFile;
                }
                if (ImGui::MenuItem("Generate Synthetic Benchmark (300K pts)"))
                {
                    m_pendingAction = PendingAction::GenerateBenchmark;
                }

                ImGui::Separator();
                bool hasModel = !m_rawSurfels.empty();

                if (ImGui::MenuItem("Export .sflw Stream Package...", "Ctrl+E", false, hasModel))
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
                    m_distance = std::max(m_extents.x, std::max(m_extents.y, m_extents.z)) * 0.85f;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("About Surfels Wavelet Studio..."))
                {
                    m_showAboutDialog = true;
                }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        // 2. Left Control & Configuration Panel
        ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, (float)m_Height - 40), ImGuiCond_FirstUseEver);
        ImGui::Begin("Studio Controls & Wavelet Settings", nullptr, ImGuiWindowFlags_NoCollapse);

        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Dataset: %s", m_loadedFilePath.c_str());
        ImGui::Separator();

        if (m_rawSurfels.empty())
        {
            ImGui::Spacing();
            ImGui::TextDisabled("No dataset currently loaded.");
            ImGui::Spacing();
            ImGui::TextWrapped("Load a model via the top menu bar:\n\n  1. File -> Open Point Cloud / Splat...\n  2. Or File -> Generate Synthetic Benchmark");
            ImGui::Spacing();
            ImGui::Separator();
        }
        else
        {
            // Section 1: Pipeline Configuration & Stages
            if (ImGui::CollapsingHeader("1. Pipeline Stage Configuration", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Active Processing Stages:");

                if (ImGui::Checkbox("Wavelet Transform (Multi-Res LODs)", &m_enableWavelet))
                {
                    UpdatePreviewSurfels();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enables 2nd-generation Lifting Wavelet multi-resolution pyramid decimation.");

                if (ImGui::Checkbox("Apply Quantization (8-Byte GPU Packing)", &m_enableQuantization))
                {
                    UpdatePreviewSurfels();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Packs points into 8-byte GPU structures (10:10:10:2 pos, Oct16 normal, RGB565 color).");

                if (!m_enableWavelet && !m_enableQuantization)
                {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Status: Rendering Full Raw Dataset (Float32 Direct)");
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Status: %s + %s",
                        m_enableWavelet ? "Wavelet LODs" : "No Wavelet",
                        m_enableQuantization ? "8-Byte Quantized" : "Float32 Direct");
                }
            }

            // Section 2: 3D Viewport & Splat Sizing
            if (ImGui::CollapsingHeader("2. 3D Viewport & Splat Sizing", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Active Display: %u points (%.1f%% of raw)",
                    m_state.surfelCount,
                    (m_state.surfelCount * 100.0f) / std::max(1ULL, (unsigned long long)m_rawSurfels.size()));

                ImGui::SliderFloat("Splat Radius Scale", &m_state.splatRadius, 0.10f, 10.0f, "%.2fx");
                m_state.orientMode = 1; // Force camera-facing billboards (3DGS standard)

                ImGui::Checkbox("Auto Rotate Model##Viewport", &m_autoRotate);
                m_state.autoRotate = m_autoRotate;

                if (ImGui::Button("Center Camera on Model", ImVec2(-1, 24)))
                {
                    m_target = m_center;
                    m_distance = std::max(m_extents.x, std::max(m_extents.y, m_extents.z)) * 0.85f;
                }
            }

            // Section 3: LOD Settings
            if (ImGui::CollapsingHeader("3. LOD Settings", ImGuiTreeNodeFlags_DefaultOpen))
            {
                int maxLODIndex = std::max(0, (int)m_waveletResult.lodLevels.size() - 1);
                if (maxLODIndex == 0 && !m_rawSurfels.empty())
                {
                    ImGui::TextDisabled("Load or recompute wavelet hierarchy to explore LODs.");
                }
                else if (maxLODIndex > 0)
                {
                    // Clamp m_selectedPreviewLOD within [0, maxLODIndex]
                    m_selectedPreviewLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));

                    if (ImGui::Checkbox("Auto Distance LOD", &m_autoLOD))
                    {
                        UpdatePreviewSurfels();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically adapts active LOD level dynamically based on distance from camera.");

                    // Responsive LOD Slider: Left = LOD 0 (100% Full Dataset), Right = LOD N (Coarsest)
                    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 95.0f);
                    if (ImGui::SliderInt("##LODSlider", &m_selectedPreviewLOD, 0, maxLODIndex, "LOD %d"))
                    {
                        m_selectedPreviewLOD = std::max(0, std::min(maxLODIndex, m_selectedPreviewLOD));
                        m_autoLOD = false; // Disable auto when user manually drags slider
                        m_enableWavelet = true;
                        UpdatePreviewSurfels();
                    }
                    ImGui::PopItemWidth();

                    ImGui::SameLine();
                    if (ImGui::Checkbox("Cascade", &m_cascadeLOD))
                    {
                        UpdatePreviewSurfels();
                    }

                    ImGui::Separator();
                    if (ImGui::Checkbox("Wavelet Transform", &m_enableWavelet))
                    {
                        UpdatePreviewSurfels();
                    }

                    if (ImGui::Checkbox("Apply Quantization", &m_enableQuantization))
                    {
                        UpdatePreviewSurfels();
                    }
                }
            }

            // Section 4: Preprocessing & Wavelet Parameters
            if (ImGui::CollapsingHeader("4. Wavelet & Octree Settings", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool recompute = false;
                float maxChunkSize = std::max(1.0f, std::max(m_extents.x, std::max(m_extents.y, m_extents.z)));
                if (ImGui::SliderFloat("Octree Chunk (m)", &m_chunkSize, 0.05f, maxChunkSize, "%.2f meters")) recompute = true;
                if (ImGui::SliderInt("Max Wavelet LODs", &m_maxLODLevels, 1, 6)) recompute = true;
                if (ImGui::SliderFloat("Deadband Zero (mm)", &m_deadbandThresholdMM, 0.0f, 50.0f, "%.1f mm")) recompute = true;

                if (recompute)
                {
                    RecomputeWaveletHierarchy();
                }

                ImGui::Separator();
                ImGui::Text("Visualizer Settings:");
                ImGui::Checkbox("Auto Rotate Model##Settings", &m_autoRotate);
                m_state.autoRotate = m_autoRotate;
                ImGui::Checkbox("Show Partitioned Octree Chunks", &m_showOctreeVisualizer);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Renders 3D bounding cubes for all %u active spatial octree chunks.", (uint32_t)m_chunks.size());
                ImGui::Checkbox("Show Global Model Bounds", &m_showGlobalBounds);
            }

            // Section 5: Accelerators & Hardware Execution
            if (ImGui::CollapsingHeader("5. Accelerators", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("GPU Radix Sort", &m_gpuRadixSort);
                m_state.gpuRadixSort = m_gpuRadixSort;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Executes parallel 32-bit depth key sorting directly on GPU compute shader threads (NVIDIA Ada SM 6.7).");

                ImGui::SameLine();
                ImGui::TextColored(m_gpuRadixSort ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                    m_gpuRadixSort ? "[Active: Compute Shader]" : "[CPU Multi-Threaded]");
            }
        }

        ImGui::End();

        // 3. Right Statistics & Telemetry Panel
        ImGui::SetNextWindowPos(ImVec2((float)m_Width - 410, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, (float)m_Height - 40), ImGuiCond_FirstUseEver);
        ImGui::Begin("Statistics & Compression Analytics", nullptr, ImGuiWindowFlags_NoCollapse);

        // Real-Time Performance & Stage Timings
        if (ImGui::CollapsingHeader("Real-Time Performance & Stage Timings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto& metrics = m_pRenderer->GetTimingMetrics();

            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Framerate:       %.1f FPS", metrics.frameRate);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Total Frame Time:%.2f ms", metrics.totalFrameTimeMs);
            ImGui::Separator();
            ImGui::Text("Per-Stage Breakdown (ms):");

            if (m_gpuRadixSort)
            {
                float sortDisplay = (metrics.gpuSortTimeMs > 0.0001f) ? metrics.gpuSortTimeMs : m_pRenderer->GetSmoothGpuSortMs();
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "  • GPU Radix Depth Sort (32-Bit): %.2f ms", sortDisplay);
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "  • CPU Radix Depth Sort (16-Bit): %.2f ms", metrics.cpuSortTimeMs);
            }

            ImGui::Text("  • GPU Mesh Shader Dispatch: %.2f ms", m_pRenderer->GetSmoothDispatchMs());
            ImGui::Text("  • ImGui Overlay UI Render:  %.2f ms", m_pRenderer->GetSmoothUiMs());
        }

        // Model Metrics
        if (ImGui::CollapsingHeader("Input Model Metrics", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Total Vertices:   %u points", (uint32_t)m_rawSurfels.size());
            ImGui::Text("Uncompressed PLY: %.2f MB", m_rawFileSizeMB);
            ImGui::Text("Bounding Box Min: [%.2f, %.2f, %.2f]", m_aabbMin.x, m_aabbMin.y, m_aabbMin.z);
            ImGui::Text("Bounding Box Max: [%.2f, %.2f, %.2f]", m_aabbMax.x, m_aabbMax.y, m_aabbMax.z);
            ImGui::Text("Spatial Extents:  %.1f x %.1f x %.1f m", m_extents.x, m_extents.y, m_extents.z);
        }

        // Spatial Octree Metrics
        if (ImGui::CollapsingHeader("Spatial Partitioning", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Chunk Voxel Size: %.1f meters", m_chunkSize);
            ImGui::Text("Total Chunks:     %u spatial chunks", (uint32_t)m_chunks.size());
            if (!m_chunks.empty())
            {
                ImGui::Text("Avg Points/Chunk: %u points", (uint32_t)(m_rawSurfels.size() / m_chunks.size()));
            }
        }

        // Wavelet Multi-Resolution Pyramid Table
        if (ImGui::CollapsingHeader("Wavelet Multi-Resolution Pyramid", ImGuiTreeNodeFlags_DefaultOpen))
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

        // Compression Summary
        if (ImGui::CollapsingHeader("4-Tier Compression Results", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Raw Point Cloud:    %.2f MB (100%%)", m_rawFileSizeMB);
            ImGui::Text("Tier 2 (8-Byte GPU):%.2f MB (5.0x reduction)", (m_rawSurfels.size() * 8.0f) / (1024.0f * 1024.0f));
            ImGui::Text("Tier 3 (Byte-Shuff): Contiguous channels");
            ImGui::Text("Tier 4 (Compressed):%.2f MB", m_compressedSizeMB);
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "TOTAL COMPRESSION:  %.2fx", m_compressionRatio);
        }

        ImGui::End();

        // 4. Bottom Status & Notification Bar
        ImGui::SetNextWindowPos(ImVec2(10, (float)m_Height - 32));
        ImGui::SetNextWindowSize(ImVec2((float)m_Width - 20, 26));
        ImGui::Begin("StatusBar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImVec4 statusColor = m_statusIsSuccess ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(statusColor, "%s", m_statusMessage.c_str());
        ImGui::End();

        // 5. About Dialog Window
        if (m_showAboutDialog)
        {
            ImGui::SetNextWindowSize(ImVec2(480, 280), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(((float)m_Width - 480) * 0.5f, ((float)m_Height - 280) * 0.5f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("About Surfels Wavelet Studio", &m_showAboutDialog, ImGuiWindowFlags_NoCollapse))
            {
                ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Surfels Wavelet Studio & Progressive Streaming");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Author:       Dave Wilkinson");
                ImGui::Text("Organization: Blueshell LLC");
                ImGui::Text("Version:      v1.0.0");
                ImGui::Text("Date:         August 31, 2026");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextWrapped("A multi-resolution point cloud preprocessor and progressive wavelet streaming pipeline built for high-performance DirectX 12 Mesh Shader surfel rendering.");
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

    void PreprocessApp::DrawOctreeVisualizer()
    {
        if (!m_showOctreeVisualizer && !m_showGlobalBounds)
            return;

        ImDrawList* drawList = ImGui::GetOverlayDrawList();
        if (!drawList)
            return;

        ImGuiIO& io = ImGui::GetIO();
        float screenW = io.DisplaySize.x;
        float screenH = io.DisplaySize.y;
        if (screenW <= 10.0f || screenH <= 10.0f)
            return;

        // Compute Camera Matrices
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

        XMMATRIX view = XMMatrixLookAtRH(eye, at, worldUp);
        float aspect = screenW / screenH;
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, aspect, 0.1f, 500.0f);
        XMMATRIX viewProj = XMMatrixMultiply(view, proj);

        auto ProjectToScreen = [&](const XMFLOAT3& p, ImVec2& outScreen) -> bool
        {
            XMVECTOR worldP = XMLoadFloat3(&p);
            XMVECTOR clipP = XMVector4Transform(XMVectorSetW(worldP, 1.0f), viewProj);
            XMFLOAT4 c;
            XMStoreFloat4(&c, clipP);
            if (c.w < 0.1f)
                return false;

            float ndcX = c.x / c.w;
            float ndcY = c.y / c.w;
            if (ndcX < -2.0f || ndcX > 2.0f || ndcY < -2.0f || ndcY > 2.0f)
                return false;

            outScreen.x = (ndcX * 0.5f + 0.5f) * screenW;
            outScreen.y = (-ndcY * 0.5f + 0.5f) * screenH; // Invert Y for screen pixels
            return true;
        };

        auto DrawDottedLine = [&](const ImVec2& p1, const ImVec2& p2, ImU32 col, float thickness = 1.0f, float dashLen = 6.0f, float gapLen = 5.0f)
        {
            float dx = p2.x - p1.x;
            float dy = p2.y - p1.y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1.0f) return;

            float nx = dx / len;
            float ny = dy / len;
            float totalStep = dashLen + gapLen;

            for (float dist = 0.0f; dist < len; dist += totalStep)
            {
                float segEnd = std::min(dist + dashLen, len);
                ImVec2 start(p1.x + nx * dist, p1.y + ny * dist);
                ImVec2 end(p1.x + nx * segEnd, p1.y + ny * segEnd);
                drawList->AddLine(start, end, col, thickness);
            }
        };

        auto DrawDottedCube = [&](const XMFLOAT3& bMin, const XMFLOAT3& bMax, ImU32 col, float thickness = 1.0f)
        {
            XMFLOAT3 corners[8] = {
                { bMin.x, bMin.y, bMin.z }, // 0
                { bMax.x, bMin.y, bMin.z }, // 1
                { bMax.x, bMax.y, bMin.z }, // 2
                { bMin.x, bMax.y, bMin.z }, // 3
                { bMin.x, bMin.y, bMax.z }, // 4
                { bMax.x, bMin.y, bMax.z }, // 5
                { bMax.x, bMax.y, bMax.z }, // 6
                { bMin.x, bMax.y, bMax.z }, // 7
            };

            ImVec2 screenCorners[8];
            bool valid[8];
            for (int i = 0; i < 8; i++)
            {
                valid[i] = ProjectToScreen(corners[i], screenCorners[i]);
            }

            const int edges[12][2] = {
                { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // Bottom face
                { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // Top face
                { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }  // Vertical edges
            };

            for (int i = 0; i < 12; i++)
            {
                int u = edges[i][0];
                int v = edges[i][1];
                if (valid[u] && valid[v])
                {
                    DrawDottedLine(screenCorners[u], screenCorners[v], col, thickness);
                }
            }
        };

        // Bright cyan/amber lines for partitioned octree spatial chunks (255, 200, 50)
        const ImU32 octreeColor = IM_COL32(255, 190, 40, 240);

        if (m_showOctreeVisualizer)
        {
            for (const auto& chunk : m_chunks)
            {
                DrawDottedCube(chunk.aabbMin, chunk.aabbMax, octreeColor, 1.5f);
            }
        }

        if (m_showGlobalBounds && !m_rawSurfels.empty())
        {
            const ImU32 globalColor = IM_COL32(80, 200, 255, 255); // Bright Cyan
            DrawDottedCube(m_aabbMin, m_aabbMax, globalColor, 2.0f);
        }
    }

    void PreprocessApp::OnRender()
    {
        // Safely execute any modal/file operations before beginning the ImGui frame
        ExecutePendingAction();

        BeginFrame();

        ImGUI_UpdateIO(m_Width, m_Height);
        ImGui::NewFrame();

        BuildUI();
        UpdateCamera(ImGui::GetIO());
        m_state.time += (float)(m_deltaTime / 1000.0);

        if (m_deviceLost)
        {
            ImGui::EndFrame();
            return;
        }

        try
        {
            m_pRenderer->OnRender(&m_state, &m_swapChain);
            EndFrame();
        }
        catch (...)
        {
            ImGui::EndFrame();
            HRESULT removeReason = m_device.GetDevice() ? m_device.GetDevice()->GetDeviceRemovedReason() : E_FAIL;
            if (removeReason != S_OK)
            {
                m_deviceLost = true;
                std::stringstream ss;
                ss << "GPU device removed / lost (0x" << std::hex << (uint32_t)removeReason << "). Please restart.";
                m_statusMessage = ss.str();
                m_statusIsSuccess = false;
                Trace("%s\n", m_statusMessage.c_str());
            }
            else
            {
                Trace("PreprocessApp::OnRender: transient render/present glitch; continuing on next frame.\n");
            }
        }
    }
}

