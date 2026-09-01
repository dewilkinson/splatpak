#include "../../src/DX12/stdafx.h"
#include "PreprocessApp.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = RunFramework(hInstance, lpCmdLine, nCmdShow, new Surfels::PreprocessApp("Surfels PLY Preprocessor & Wavelet Studio"));
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
    return result;
}

