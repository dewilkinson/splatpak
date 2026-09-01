#include "stdafx.h"
#include "SurfelsSample.h"
#include "Wavelet/StreamingManager.h"
#include "Wavelet/LODSelector.h"
#include "../../tools/SurfelsPreprocess/SyntheticGenerator.h"
#include "../../tools/SurfelsPreprocess/SpatialOctree.h"
#include "../../tools/SurfelsPreprocess/StreamPackager.h"

// Cauldron vendors the Agility SDK (libs/cauldron/libs/DX12AgilitySDK) and copies its
// D3D12Core.dll/d3d12SDKLayers.dll to bin/D3D12/, but opting into it requires calling
// CAULDRON_APP_USE_DX12_AGILITY_SDK(version, ".\\D3D12\\") with the exact integer version
// matching that DLL -- get it wrong and device creation fails at runtime instead of build
// time. Left disabled here; the Windows-provided D3D12 runtime is enough for this sample.
// See the vendored SDK's folder name under libs/cauldron/libs/DX12AgilitySDK if you want it.

SurfelsSample::SurfelsSample(LPCSTR name) : FrameworkWindows(name)
{
}

void SurfelsSample::OnParseCommandLine(LPSTR /*lpCmdLine*/, uint32_t* pWidth, uint32_t* pHeight)
{
    *pWidth = 1600;
    *pHeight = 900;
    m_VsyncEnabled = false;
    m_isCpuValidationLayerEnabled = false;
    m_isGpuValidationLayerEnabled = false;
    m_stablePowerState = false;
}

void SurfelsSample::OnCreate()
{
    InitDirectXCompiler();
    CreateShaderCache();

    m_pRenderer = new SurfelsRenderer();
    m_pRenderer->OnCreate(&m_device, &m_swapChain);

    // Initialize Wavelet Streaming Manager
    if (!m_streamingManager.LoadDataset("scene"))
    {
        // Automatically generate synthetic urban street benchmark dataset if not found
        auto points = Surfels::SyntheticGenerator::GenerateUrbanStreetScene(250000);
        auto chunks = Surfels::SpatialOctree::PartitionIntoChunks(points, 16.0f);
        Surfels::StreamPackager::PackageDataset("scene", chunks, 4, 0.003f);
        m_streamingManager.LoadDataset("scene");
        m_hasGeneratedSample = true;
    }

    // Default to normal-oriented wavelet streamed mode
    m_state.renderMode = 1;
    m_state.orientMode = 0;
    m_state.splatRadius = 0.06f;

    ImGUI_Init((void*)m_windowHwnd);
}

void SurfelsSample::OnDestroy()
{
    m_streamingManager.Close();

    ImGUI_Shutdown();

    // If the GPU device was suspended/removed earlier (e.g. a TDR while paused in
    // the debugger), GPUFlush()'s fence Signal() throws right out of shutdown with
    // nothing upstream to catch it -- this runs from the main loop after WM_QUIT,
    // not from OnRender()'s guarded try/catch. Nothing left to wait for on a dead
    // device anyway, so just skip the flush and tear down.
    try
    {
        m_device.GPUFlush();
    }
    catch (...)
    {
        Trace("SurfelsSample::OnDestroy: GPUFlush failed (device suspended/removed?); skipping flush and tearing down anyway\n");
    }

    m_pRenderer->OnDestroyWindowSizeDependentResources();
    m_pRenderer->OnDestroy();
    delete m_pRenderer;
    m_pRenderer = nullptr;

    DestroyShaderCache(&m_device);
}

bool SurfelsSample::OnEvent(MSG msg)
{
    ImGUI_WndProcHandler(msg.hwnd, msg.message, msg.wParam, msg.lParam);
    return true;
}

void SurfelsSample::OnResize(bool resizeRender)
{
    if (resizeRender && m_Width && m_Height && m_pRenderer)
    {
        m_pRenderer->OnDestroyWindowSizeDependentResources();
        m_pRenderer->OnCreateWindowSizeDependentResources(&m_swapChain, m_Width, m_Height);
    }
}

void SurfelsSample::OnUpdateDisplay()
{
    if (m_pRenderer)
        m_pRenderer->OnUpdateDisplayDependentResources(&m_swapChain);
}

void SurfelsSample::BuildUI()
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Surfels");

    ImGui::Text("GPU: %s", m_systemInfo.mGPUName.c_str());
    ImGui::Text("Frame: %.2f ms (%.1f FPS)", m_profiler.avgCpuMs, m_profiler.avgFps);
    ImGui::Separator();

    ImGui::Text("Render Pipeline Mode:");
    int rMode = (int)m_state.renderMode;
    if (ImGui::RadioButton("Wavelet Streamed", &rMode, 1)) m_state.renderMode = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Procedural Sphere", &rMode, 0)) m_state.renderMode = 0;

    ImGui::Text("Primitive Orientation:");
    int oMode = (int)m_state.orientMode;
    if (ImGui::RadioButton("Normal-Oriented Discs", &oMode, 0)) m_state.orientMode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Camera-Facing", &oMode, 1)) m_state.orientMode = 1;

    ImGui::Checkbox("GPU Bitonic Depth Sort", &m_state.gpuRadixSort);

    ImGui::Separator();

    if (m_state.renderMode == 0)
    {
        int count = (int)m_state.surfelCount;
        if (ImGui::SliderInt("Surfel Count", &count, 8, 20000))
            m_state.surfelCount = (uint32_t)count;

        ImGui::SliderFloat("Sphere Radius", &m_state.sphereRadius, 0.5f, 5.0f);
    }
    else
    {
        ImGui::SliderFloat("AutoLOD Target Error", &m_targetPixelError, 0.5f, 15.0f, "%.1f px");
        ImGui::TextDisabled("Lower = finer LOD, Higher = coarser LOD");
    }

    ImGui::SliderFloat("Splat Radius Scale", &m_state.splatRadius, 0.005f, 0.20f);
    ImGui::Checkbox("Auto Rotate", &m_state.autoRotate);
    ImGui::TextDisabled("Drag to orbit, wheel to zoom");

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Wavelet Streaming Telemetry", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const auto& telem = m_streamingManager.GetTelemetry();
        ImGui::Text("Active Chunks:   %u / %u", telem.activeChunksRendered, telem.totalChunksInDataset);
        ImGui::Text("Rendered Splats: %u", telem.totalSurfelsRendered);
        ImGui::Text("Resident Memory: %.2f MB", telem.residentMemoryMB);
        ImGui::Text("Cache Hit Rate:  %.1f%% (%u hits, %u misses)",
            (telem.cacheHits + telem.cacheMisses > 0) ? (telem.cacheHits * 100.0f / (telem.cacheHits + telem.cacheMisses)) : 100.0f,
            telem.cacheHits, telem.cacheMisses);

        ImGui::Text("LOD Pyramid Breakdown:");
        for (int l = 0; l < 5; l++)
        {
            ImGui::Text("  LOD %d: %u chunks", l, telem.lodDistribution[l]);
        }

        if (ImGui::Button("Regenerate Benchmark (300K pts)"))
        {
            auto points = Surfels::SyntheticGenerator::GenerateUrbanStreetScene(300000);
            auto chunks = Surfels::SpatialOctree::PartitionIntoChunks(points, 16.0f);
            Surfels::StreamPackager::PackageDataset("scene", chunks, 4, 0.003f);
            m_streamingManager.LoadDataset("scene");
        }
    }

    ImGui::Separator();
    ImGui::Checkbox("Show Frame Profiler", &m_profiler.showProfiler);
    if (ImGui::Button("About Surfels..."))
    {
        m_showAboutDialog = true;
    }

    ImGui::End();

    if (m_showAboutDialog)
    {
        ImGui::SetNextWindowSize(ImVec2(460, 270), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(((float)m_Width - 460) * 0.5f, ((float)m_Height - 270) * 0.5f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("About Surfels Viewer", &m_showAboutDialog, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Surfels - DirectX 12 Mesh Shader Viewer");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Author:       Dave Wilkinson");
            ImGui::Text("Organization: Blueshell LLC");
            ImGui::Text("Version:      v1.0.0");
            ImGui::Text("Date:         August 31, 2026");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextWrapped("High-performance DirectX 12 point-cloud and progressive wavelet surfel streaming renderer with hardware mesh shaders and real-time AutoLOD.");
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

void SurfelsSample::BuildProfilerUI()
{
    ImGui::SetNextWindowPos(ImVec2(360, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Frame Profiler", &m_profiler.showProfiler))
    {
        ImGui::End();
        return;
    }

    // --- Header & Global Metrics ---
    ImGui::Text("FPS: %.1f (Min: %.1f, Max: %.1f)",
        m_profiler.avgFps,
        m_profiler.maxCpuMs > 0.0f ? 1000.0f / m_profiler.maxCpuMs : 0.0f,
        m_profiler.minCpuMs > 0.0f ? 1000.0f / m_profiler.minCpuMs : 0.0f);

    float latestGpu = m_profiler.gpuHistory[(m_profiler.historyIndex - 1 + ProfilerStats::HISTORY_SIZE) % ProfilerStats::HISTORY_SIZE];
    ImGui::Text("Frame Time: CPU %.2f ms (avg: %.2f) | GPU %.2f ms (avg: %.2f)",
        m_profiler.lastCpu.totalCpuMs, m_profiler.avgCpuMs,
        latestGpu, m_profiler.avgGpuMs);

    ImGui::Checkbox("Pause Capture", &m_profiler.paused);
    ImGui::SameLine();
    if (ImGui::Button("Reset Stats"))
    {
        memset(m_profiler.cpuHistory, 0, sizeof(m_profiler.cpuHistory));
        memset(m_profiler.gpuHistory, 0, sizeof(m_profiler.gpuHistory));
        m_profiler.minCpuMs = 999.0f;
        m_profiler.maxCpuMs = 0.0f;
        m_profiler.minGpuMs = 999.0f;
        m_profiler.maxGpuMs = 0.0f;
    }

    ImGui::Separator();

    // --- History Graphs ---
    if (ImGui::CollapsingHeader("Frame Time History", ImGuiTreeNodeFlags_DefaultOpen))
    {
        char cpuOverlay[64];
        sprintf_s(cpuOverlay, "CPU: %.2f ms", m_profiler.lastCpu.totalCpuMs);
        ImGui::PlotLines("##CPUHistory", m_profiler.cpuHistory, ProfilerStats::HISTORY_SIZE,
            m_profiler.historyIndex, cpuOverlay, 0.0f, 33.3f, ImVec2(0, 55));

        char gpuOverlay[64];
        sprintf_s(gpuOverlay, "GPU: %.2f ms", latestGpu);
        ImGui::PlotLines("##GPUHistory", m_profiler.gpuHistory, ProfilerStats::HISTORY_SIZE,
            m_profiler.historyIndex, gpuOverlay, 0.0f, 33.3f, ImVec2(0, 55));

        ImGui::TextDisabled("Scale: 0 - 33.3 ms (30 FPS at top, 60 FPS at midpoint)");
    }

    // --- GPU Pass Breakdown ---
    if (ImGui::CollapsingHeader("GPU Passes Breakdown", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float totalGpuMicroseconds = 0.0f;
        for (const auto& ts : m_profiler.lastGpu)
        {
            if (ts.m_label == "Total GPU Time")
            {
                totalGpuMicroseconds = ts.m_microseconds;
                break;
            }
        }

        // Draw Visual Stacked Proportional Bar
        if (totalGpuMicroseconds > 0.0f)
        {
            ImVec2 barSize = ImVec2(ImGui::GetContentRegionAvailWidth(), 14.0f);
            ImVec2 barPos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            drawList->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), IM_COL32(40, 40, 40, 255), 2.0f);

            static const ImU32 passColors[] = {
                IM_COL32(70, 130, 180, 255),   // Steel Blue
                IM_COL32(50, 205, 50, 255),    // Lime Green
                IM_COL32(255, 165, 0, 255),    // Orange
                IM_COL32(186, 85, 211, 255),   // Orchid
                IM_COL32(220, 20, 60, 255),    // Crimson
            };

            float curX = barPos.x;
            int colorIdx = 0;
            for (const auto& ts : m_profiler.lastGpu)
            {
                if (ts.m_label == "Total GPU Time" || ts.m_label == "Frame Begin") continue;
                float fraction = ts.m_microseconds / totalGpuMicroseconds;
                float segmentWidth = fraction * barSize.x;
                if (segmentWidth > 0.5f)
                {
                    ImU32 col = passColors[colorIdx % 5];
                    drawList->AddRectFilled(ImVec2(curX, barPos.y), ImVec2(curX + segmentWidth, barPos.y + barSize.y), col, 2.0f);
                    curX += segmentWidth;
                }
                colorIdx++;
            }
            ImGui::Dummy(barSize);
        }

        ImGui::Columns(4, "GpuPassColumns", true);
        ImGui::Text("Pass"); ImGui::NextColumn();
        ImGui::Text("Time (ms)"); ImGui::NextColumn();
        ImGui::Text("Time (us)"); ImGui::NextColumn();
        ImGui::Text("Share"); ImGui::NextColumn();
        ImGui::Separator();

        for (const auto& ts : m_profiler.lastGpu)
        {
            if (ts.m_label == "Frame Begin") continue;

            bool isTotal = (ts.m_label == "Total GPU Time");
            if (isTotal) ImGui::Separator();

            float ms = ts.m_microseconds / 1000.0f;
            float share = totalGpuMicroseconds > 0.0f ? (ts.m_microseconds / totalGpuMicroseconds * 100.0f) : 0.0f;

            if (isTotal)
            {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", ts.m_label.c_str()); ImGui::NextColumn();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.3f", ms); ImGui::NextColumn();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.1f", ts.m_microseconds); ImGui::NextColumn();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "100.0%%"); ImGui::NextColumn();
            }
            else
            {
                ImGui::Text("%s", ts.m_label.c_str()); ImGui::NextColumn();
                ImGui::Text("%.3f", ms); ImGui::NextColumn();
                ImGui::Text("%.1f", ts.m_microseconds); ImGui::NextColumn();
                ImGui::Text("%.1f%%", share); ImGui::NextColumn();
            }
        }
        ImGui::Columns(1);
    }

    // --- CPU Pipeline Breakdown ---
    if (ImGui::CollapsingHeader("CPU Pipeline Breakdown", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Columns(4, "CpuStageColumns", true);
        ImGui::Text("Stage"); ImGui::NextColumn();
        ImGui::Text("Time (ms)"); ImGui::NextColumn();
        ImGui::Text("Time (us)"); ImGui::NextColumn();
        ImGui::Text("Share"); ImGui::NextColumn();
        ImGui::Separator();

        auto drawCpuRow = [&](const char* name, float ms, float total) {
            float us = ms * 1000.0f;
            float share = total > 0.0f ? (ms / total * 100.0f) : 0.0f;
            ImGui::Text("%s", name); ImGui::NextColumn();
            ImGui::Text("%.3f", ms); ImGui::NextColumn();
            ImGui::Text("%.1f", us); ImGui::NextColumn();
            ImGui::Text("%.1f%%", share); ImGui::NextColumn();
        };

        float total = m_profiler.lastCpu.totalCpuMs;
        drawCpuRow("UI Build (ImGui)", m_profiler.lastCpu.uiBuildMs, total);
        drawCpuRow("Camera & Update", m_profiler.lastCpu.updateMs, total);
        drawCpuRow("Render Prep & Record", m_profiler.lastCpu.renderMs, total);

        float otherMs = total - (m_profiler.lastCpu.uiBuildMs + m_profiler.lastCpu.updateMs + m_profiler.lastCpu.renderMs);
        if (otherMs > 0.0f)
        {
            drawCpuRow("Framework & Present", otherMs, total);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Total CPU Frame"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%.3f", total); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%.1f", total * 1000.0f); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "100.0%%"); ImGui::NextColumn();

        ImGui::Columns(1);
    }

    // --- Mesh Shader Workload Metrics ---
    if (ImGui::CollapsingHeader("Mesh Shader Workload Stats", ImGuiTreeNodeFlags_DefaultOpen))
    {
        uint32_t surfels = m_state.surfelCount;
        uint32_t groups = (surfels + 31) / 32;
        uint32_t vertices = surfels * 4;
        uint32_t triangles = surfels * 2;

        float meshShaderMs = 0.0f;
        for (const auto& ts : m_profiler.lastGpu)
        {
            if (ts.m_label == "Surfels Mesh Shader")
            {
                meshShaderMs = ts.m_microseconds / 1000.0f;
                break;
            }
        }

        ImGui::Text("Active Surfels:        %u", surfels);
        ImGui::Text("Threadgroups (32/grp): %u", groups);
        ImGui::Text("Generated Vertices:    %u", vertices);
        ImGui::Text("Generated Triangles:   %u", triangles);

        if (meshShaderMs > 0.0001f)
        {
            float mSurfelsPerSec = (surfels / 1000000.0f) / (meshShaderMs / 1000.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Surfel Throughput:     %.2f M splats/sec", mSurfelsPerSec);
        }
        else
        {
            ImGui::Text("Surfel Throughput:     N/A");
        }
    }

    ImGui::End();
}

void SurfelsSample::UpdateCamera(const ImGuiIO& io)
{
    if (!io.WantCaptureMouse)
    {
        if (io.MouseDown[0])
        {
            m_yaw -= io.MouseDelta.x * 0.01f;
            m_pitch += io.MouseDelta.y * 0.01f;
            m_pitch = std::max(-1.5f, std::min(1.5f, m_pitch));
        }
        m_distance -= io.MouseWheel * 0.25f;
        m_distance = std::max(1.0f, std::min(20.0f, m_distance));
    }

    if (m_state.autoRotate)
        m_yaw += (float)(m_deltaTime * 0.0003);

    m_state.camYaw = m_yaw;
    m_state.camPitch = m_pitch;
    m_state.camDistance = m_distance;
    m_state.aspectRatio = m_Height > 0 ? (float)m_Width / (float)m_Height : 1.0f;
}

void SurfelsSample::OnRender()
{
    // Once the device is confirmed suspended/removed it never recovers on its own --
    // every future Present() would fail with the exact same HRESULT. Stop doing any
    // per-frame work at all instead of retrying and throwing that same exception
    // every single frame forever: a debugger set to break on C++ exceptions turns
    // that into what looks and feels like a hang, and even without a debugger
    // attached it's still pure wasted CPU/GPU work for a frame nothing can present.
    if (m_deviceLost)
        return;

    auto frameStart = std::chrono::high_resolution_clock::now();

    BeginFrame();

    ImGUI_UpdateIO(m_Width, m_Height);
    ImGui::NewFrame();

    auto uiStart = std::chrono::high_resolution_clock::now();
    BuildUI();
    if (m_profiler.showProfiler)
    {
        BuildProfilerUI();
    }
    auto uiEnd = std::chrono::high_resolution_clock::now();
    m_currentCpu.uiBuildMs = std::chrono::duration<float, std::milli>(uiEnd - uiStart).count();

    auto updateStart = std::chrono::high_resolution_clock::now();
    UpdateCamera(ImGui::GetIO());
    m_state.time += (float)(m_deltaTime / 1000.0);

    // AutoLOD Selection & Streaming Update
    if (m_state.renderMode == 1 && m_streamingManager.IsLoaded())
    {
        const float cy = cosf(m_pitch), sy = sinf(m_pitch);
        const float sx = sinf(m_yaw), cx = cosf(m_yaw);
        XMFLOAT3 camPos(m_distance * cy * sx, m_distance * sy, m_distance * cy * cx);
        XMVECTOR eye = XMLoadFloat3(&camPos);
        XMVECTOR at = XMVectorZero();
        XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMMATRIX view = XMMatrixLookAtRH(eye, at, worldUp);
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, m_state.aspectRatio, 0.1f, 100.0f);
        XMMATRIX viewProj = XMMatrixMultiply(view, proj);

        auto selections = Surfels::LODSelector::SelectLODs(
            m_streamingManager.GetChunks(),
            camPos,
            viewProj,
            (float)m_Height,
            XM_PIDIV4,
            m_targetPixelError
        );

        m_streamingManager.Update(selections, m_activeSurfels);
        m_state.pStreamedSurfels = m_activeSurfels.data();
        m_state.streamedSurfelCount = (uint32_t)m_activeSurfels.size();

        const auto& hdr = m_streamingManager.GetHeader();
        m_state.aabbMin = hdr.globalBoundsMin;
        m_state.aabbExtents = XMFLOAT3(
            hdr.globalBoundsMax.x - hdr.globalBoundsMin.x,
            hdr.globalBoundsMax.y - hdr.globalBoundsMin.y,
            hdr.globalBoundsMax.z - hdr.globalBoundsMin.z
        );
    }
    else
    {
        m_state.pStreamedSurfels = nullptr;
        m_state.streamedSurfelCount = 0;
    }

    auto updateEnd = std::chrono::high_resolution_clock::now();
    m_currentCpu.updateMs = std::chrono::duration<float, std::milli>(updateEnd - updateStart).count();

    auto renderStart = std::chrono::high_resolution_clock::now();

    // GPU work (mesh shader dispatch + Present) can transiently fail if the display
    // device is suspended/removed by the driver (e.g. a TDR from sitting paused in a
    // debugger, GPU contention, power state changes, remote session hiccups).
    // Cauldron's SwapChain::Present throws on any failed HRESULT; without this guard
    // that exception has nothing to catch it and takes down the whole process on the
    // very next frame instead of just skipping this one. See PreprocessApp::OnRender
    // for the same pattern.
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
                Trace("SurfelsSample::OnRender: GPU device was removed / lost (0x%08X). Rendering stopped -- please restart the app.\n", (uint32_t)removeReason);
            }
            else
            {
                Trace("SurfelsSample::OnRender: transient render/present glitch; continuing on next frame.\n");
            }
        }

    auto renderEnd = std::chrono::high_resolution_clock::now();
    m_currentCpu.renderMs = std::chrono::duration<float, std::milli>(renderEnd - renderStart).count();

    auto frameEnd = std::chrono::high_resolution_clock::now();
    m_currentCpu.totalCpuMs = std::chrono::duration<float, std::milli>(frameEnd - frameStart).count();

    if (!m_profiler.paused)
    {
        m_profiler.lastCpu = m_currentCpu;
        m_profiler.lastGpu = m_pRenderer->GetGPUTimestamps();

        float totalGpuMs = 0.0f;
        for (const auto& ts : m_profiler.lastGpu)
        {
            if (ts.m_label == "Total GPU Time")
            {
                totalGpuMs = ts.m_microseconds / 1000.0f;
                break;
            }
        }

        m_profiler.cpuHistory[m_profiler.historyIndex] = m_currentCpu.totalCpuMs;
        m_profiler.gpuHistory[m_profiler.historyIndex] = totalGpuMs;
        m_profiler.historyIndex = (m_profiler.historyIndex + 1) % ProfilerStats::HISTORY_SIZE;

        float sumCpu = 0.0f, minCpu = 999.0f, maxCpu = 0.0f;
        float sumGpu = 0.0f, minGpu = 999.0f, maxGpu = 0.0f;
        for (int i = 0; i < ProfilerStats::HISTORY_SIZE; i++)
        {
            float c = m_profiler.cpuHistory[i];
            float g = m_profiler.gpuHistory[i];
            if (c > 0.0f)
            {
                sumCpu += c;
                if (c < minCpu) minCpu = c;
                if (c > maxCpu) maxCpu = c;
            }
            if (g > 0.0f)
            {
                sumGpu += g;
                if (g < minGpu) minGpu = g;
                if (g > maxGpu) maxGpu = g;
            }
        }

        m_profiler.avgCpuMs = sumCpu / (float)ProfilerStats::HISTORY_SIZE;
        m_profiler.minCpuMs = (minCpu < 900.0f) ? minCpu : 0.0f;
        m_profiler.maxCpuMs = maxCpu;

        m_profiler.avgGpuMs = sumGpu / (float)ProfilerStats::HISTORY_SIZE;
        m_profiler.minGpuMs = (minGpu < 900.0f) ? minGpu : 0.0f;
        m_profiler.maxGpuMs = maxGpu;

        m_profiler.avgFps = m_profiler.avgCpuMs > 0.0f ? (1000.0f / m_profiler.avgCpuMs) : 0.0f;
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = RunFramework(hInstance, lpCmdLine, nCmdShow, new SurfelsSample("Surfels - Cauldron DX12 Sample"));
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
    return result;
}
