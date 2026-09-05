#include "../../src/DX12/stdafx.h"
#include "PreprocessApp.h"

#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")

LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* pEx)
{
    Surfels::LogTransitionTrace("!!! FATAL CRASH OCCURRED !!! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p",
        pEx->ExceptionRecord->ExceptionCode,
        pEx->ExceptionRecord->ExceptionAddress);
    Surfels::LogD3D12Messages();

    FILE* fp = fopen("crash.txt", "w");
    if (fp)
    {
        fprintf(fp, "Crash occurred! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p\n",
            pEx->ExceptionRecord->ExceptionCode,
            pEx->ExceptionRecord->ExceptionAddress);
        fclose(fp);
    }

    HANDLE hFile = CreateFileA("crash.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = pEx;
        mdei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpWithDataSegs, &mdei, NULL, NULL);
        CloseHandle(hFile);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* pEx)
{
    DWORD code = pEx->ExceptionRecord->ExceptionCode;
    // Filter out common benign Windows debug notifications / RPC events
    if (code == 0xE06D7363 || code == 0x406D1388 || code == 0x40010006)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code == 0xC0000005 || code == 0xC00000FD || code == 0xC0000008 || code == 0x80000003 || code == 0x0000087A)
    {
        Surfels::LogTransitionTrace("!!! VECTORED EXCEPTION CAUGHT !!! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p, Flags: 0x%X",
            code, pEx->ExceptionRecord->ExceptionAddress, pEx->ExceptionRecord->ExceptionFlags);
        Surfels::LogD3D12Messages();

        FILE* fp = fopen("crash.txt", "w");
        if (fp)
        {
            fprintf(fp, "VectoredCrashHandler: ExceptionCode: 0x%08X, ExceptionAddress: 0x%p\n",
                code, pEx->ExceptionRecord->ExceptionAddress);
            fclose(fp);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    SetUnhandledExceptionFilter(CrashDumpHandler);
    AddVectoredExceptionHandler(1, VectoredCrashHandler);

#if defined(_DEBUG)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            Surfels::LogTransitionTrace("WinMain: D3D12 Debug Layer enabled.");
        }
    }
#endif

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = RunFramework(hInstance, lpCmdLine, nCmdShow, new Surfels::PreprocessApp("Surfel-based 3D Streaming Demo"));
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
    return result;
}

