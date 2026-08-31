#include "stdafx.h"
#include "SurfelsSample.h"

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

    ImGUI_Init((void*)m_windowHwnd);
}

void SurfelsSample::OnDestroy()
{
    ImGUI_Shutdown();

    m_device.GPUFlush();

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
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Surfels");

    ImGui::Text("GPU: %s", m_systemInfo.mGPUName.c_str());
    ImGui::Text("Frame: %.2f ms (%.0f FPS)", m_deltaTime, m_deltaTime > 0.0 ? 1000.0 / m_deltaTime : 0.0);
    ImGui::Separator();

    int count = (int)m_state.surfelCount;
    if (ImGui::SliderInt("Surfel Count", &count, 8, 20000))
        m_state.surfelCount = (uint32_t)count;

    ImGui::SliderFloat("Splat Radius", &m_state.splatRadius, 0.005f, 0.15f);
    ImGui::SliderFloat("Sphere Radius", &m_state.sphereRadius, 0.5f, 5.0f);
    ImGui::Checkbox("Auto Rotate", &m_state.autoRotate);
    ImGui::TextDisabled("Drag to orbit, wheel to zoom");

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
    BeginFrame();

    ImGUI_UpdateIO(m_Width, m_Height);
    ImGui::NewFrame();

    BuildUI();
    UpdateCamera(ImGui::GetIO());

    m_state.time += (float)(m_deltaTime / 1000.0);

    m_pRenderer->OnRender(&m_state, &m_swapChain);

    EndFrame();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    return RunFramework(hInstance, lpCmdLine, nCmdShow, new SurfelsSample("Surfels - Cauldron DX12 Sample"));
}
