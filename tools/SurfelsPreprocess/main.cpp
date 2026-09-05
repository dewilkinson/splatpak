// main.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Entry point for SurfelLab: installs crash-dump handlers (so a hard crash still
// leaves a trace-log entry and a minidump for post-mortem debugging) before handing off
// to Cauldron's RunFramework with a PreprocessApp instance.

#include "../../src/DX12/stdafx.h"
#include "PreprocessApp.h"

#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")

// Two exception codes without a standard WinNT.h macro name -- MSVC's own C++ exception code
// and the classic SetThreadName()-via-exception debugger trick, both benign and not real crashes.
static constexpr DWORD kMsvcCppExceptionCode = 0xE06D7363;
static constexpr DWORD kSetThreadNameExceptionCode = 0x406D1388;
static constexpr DWORD kRpcDebugNotificationCode = 0x40010006;

static constexpr const char* kCrashTextFilename = "crash.txt";
static constexpr const char* kCrashDumpFilename = "crash.dmp";

// Global unhandled-exception filter: logs the crash and writes a minidump before letting the
// exception continue unhandled (so Windows' own crash dialog / debugger still sees it).
LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* pEx)
{
    Surfels::LogTransitionTrace("!!! FATAL CRASH OCCURRED !!! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p",
        pEx->ExceptionRecord->ExceptionCode,
        pEx->ExceptionRecord->ExceptionAddress);
    Surfels::LogD3D12Messages();

    FILE* fp = fopen(kCrashTextFilename, "w");
    if (fp)
    {
        fprintf(fp, "Crash occurred! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p\n",
            pEx->ExceptionRecord->ExceptionCode,
            pEx->ExceptionRecord->ExceptionAddress);
        fclose(fp);
    }

    HANDLE hFile = CreateFileA(kCrashDumpFilename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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

// Vectored handler: sees exceptions before any SEH __try/__except unwinds them, so this catches
// genuine hard crashes (access violation, stack overflow, etc.) even if something downstream
// swallows them. STATUS_STACK_BUFFER_OVERRUN (0x0000087A) has no public EXCEPTION_ constant.
static constexpr DWORD kStatusStackBufferOverrun = 0x0000087A;

LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* pEx)
{
    DWORD code = pEx->ExceptionRecord->ExceptionCode;
    // Filter out common benign Windows debug notifications / RPC events
    if (code == kMsvcCppExceptionCode || code == kSetThreadNameExceptionCode || code == kRpcDebugNotificationCode)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INVALID_HANDLE ||
        code == EXCEPTION_BREAKPOINT || code == kStatusStackBufferOverrun)
    {
        Surfels::LogTransitionTrace("!!! VECTORED EXCEPTION CAUGHT !!! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p, Flags: 0x%X",
            code, pEx->ExceptionRecord->ExceptionAddress, pEx->ExceptionRecord->ExceptionFlags);
        Surfels::LogD3D12Messages();

        FILE* fp = fopen(kCrashTextFilename, "w");
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
    int result = RunFramework(hInstance, lpCmdLine, nCmdShow, new Surfels::PreprocessApp("SurfelLab - Surfel-based 3D Streaming Demo"));
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
    return result;
}

