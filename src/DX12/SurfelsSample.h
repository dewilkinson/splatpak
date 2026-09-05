// SurfelsSample.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The 'application' shell: window/input handling and per-frame state, delegating
// all GPU work to SurfelsRenderer. Mirrors the standard Cauldron sample split
// (see AMD's GLTFSample / FidelityFX-CAS sample apps) minus scene loading.
#pragma once
#include "SurfelsRenderer.h"
#include "Wavelet/StreamingManager.h"

class SurfelsSample : public CAULDRON_DX12::FrameworkWindows
{
public:
    explicit SurfelsSample(LPCSTR name);

    void OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight) override;
    void OnCreate() override;
    void OnDestroy() override;
    void OnRender() override;
    bool OnEvent(MSG msg) override;
    void OnResize(bool resizeRender) override;
    void OnUpdateDisplay() override;

private:
    void BuildUI();
    void BuildProfilerUI();
    void UpdateCamera(const ImGuiIO& io);

    struct CpuTimings
    {
        float waitGpuMs    = 0.0f;
        float updateMs     = 0.0f;
        float uiBuildMs    = 0.0f;
        float renderMs     = 0.0f;
        float totalCpuMs   = 0.0f;
    };

    struct ProfilerStats
    {
        bool showProfiler = true;
        bool paused       = false;

        static const int HISTORY_SIZE = 128;
        float cpuHistory[HISTORY_SIZE] = {};
        float gpuHistory[HISTORY_SIZE] = {};
        int   historyIndex             = 0;

        float avgCpuMs = 0.0f;
        float minCpuMs = 999.0f;
        float maxCpuMs = 0.0f;

        float avgGpuMs = 0.0f;
        float minGpuMs = 999.0f;
        float maxGpuMs = 0.0f;

        float avgFps = 0.0f;

        CpuTimings lastCpu;
        std::vector<TimeStamp> lastGpu;
    };

    ProfilerStats          m_profiler;
    CpuTimings             m_currentCpu;

    Surfels::StreamingManager m_streamingManager;
    std::vector<Surfels::PackedSurfelGPU> m_activeSurfels;

    float m_targetPixelError = 3.0f;
    bool  m_showTelemetry = true;
    bool  m_hasGeneratedSample = false;
    bool  m_showAboutDialog = false;
    bool  m_useCopyQueue = true; // Dedicated DX12 Hardware DMA Copy Queue

    SurfelsRenderer*       m_pRenderer = nullptr;
    SurfelsRenderer::State m_state;

    // Set once a GPU device-removed/suspended HRESULT is caught. Once true, every
    // frame stops attempting to render/Present entirely (see OnRender()) instead
    // of retrying and throwing the same ThrowIfFailed exception forever -- that
    // repeated throw is otherwise indistinguishable from a real hang if a debugger
    // is set to break on every C++ exception.
    bool m_deviceLost = false;

    float m_yaw = 0.6f;
    float m_pitch = 0.35f;
    float m_distance = 4.0f;
};
