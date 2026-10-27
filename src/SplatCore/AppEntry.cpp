// AppEntry.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The shared WinMain body behind both executables (see AppEntry.h): installs crash-dump
// handlers (so a hard crash still leaves a trace-log entry and a minidump for post-mortem
// debugging) before handing off to Cauldron's RunFramework with a SplatPakApp instance in
// the requested mode. Each exe's main.cpp is a few lines that call RunSplatPakApp().

#include "stdafx.h"
#include "AppEntry.h"
#include "SplatPakApp.h"

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
    Splat::LogTransitionTrace("!!! FATAL CRASH OCCURRED !!! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p",
        pEx->ExceptionRecord->ExceptionCode,
        pEx->ExceptionRecord->ExceptionAddress);
    Splat::LogD3D12Messages();

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
        Splat::LogTransitionTrace("!!! VECTORED EXCEPTION CAUGHT !!! ExceptionCode: 0x%08X, ExceptionAddress: 0x%p, Flags: 0x%X",
            code, pEx->ExceptionRecord->ExceptionAddress, pEx->ExceptionRecord->ExceptionFlags);
        Splat::LogD3D12Messages();

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

// Shows the usage text: on the parent console when there is one (these are WIN32 executables, so
// a console is only present when launched from a prompt), and always in a message box.
static void ShowUsage(const char* windowName, const std::string& problem)
{
    std::string text = problem.empty() ? std::string() : (problem + "\n\n");
    text += Splat::SplatPakApp::CommandLineUsage();
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        std::string consoleText = "\n" + text;
        WriteConsoleA(out, consoleText.c_str(), (DWORD)consoleText.size(), &written, nullptr);
        FreeConsole();
    }
    std::string title = std::string(windowName) + " - Command line";
    MessageBoxA(nullptr, text.c_str(), title.c_str(), MB_OK | (problem.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
}

int Splat::RunSplatPakApp(HINSTANCE hInstance, LPSTR lpCmdLine, int nCmdShow, const char* windowName, SplatPakApp::Mode mode)
{
    // Mode switches and --help are decided here, before the app object exists; everything else
    // (file path, --render-path) is handed on to SplatPakApp::OnParseCommandLine as a rebuilt
    // command line. An unknown option shows the usage and exits with code 2.
    std::string forwarded;
    {
        const std::vector<std::string> args = SplatPakApp::SplitCommandLine(lpCmdLine);
        for (size_t i = 0; i < args.size(); i++)
        {
            const std::string& a = args[i];
            if (a == "--viewer")      { mode = SplatPakApp::Mode::Viewer; windowName = "Splat Viewer"; continue; }
            if (a == "--studio")      { mode = SplatPakApp::Mode::Studio; windowName = "SplatPak"; continue; }
            if (a == "--help" || a == "-h" || a == "/?" || a == "-?") { ShowUsage(windowName, ""); return 0; }
            if (a == "--render-path")
            {
                if (i + 1 >= args.size() || (args[i + 1] != "auto" && args[i + 1] != "mesh" && args[i + 1] != "vs6" && args[i + 1] != "vs5"))
                {
                    ShowUsage(windowName, "--render-path needs one of: auto, mesh, vs6, vs5");
                    return 2;
                }
                forwarded += "--render-path " + args[++i] + " ";
                continue;
            }
            if (a == "--stream-test" || a == "--remember-settings" || a == "--level0" || a == "--no-taa" || a == "--no-subject" || a == "--ring-test")
            {
                forwarded += a + " ";
                continue;
            }
            if (a == "--lod-quality-test" || a == "--lod-calibration" || a == "--bake-to" || a == "--capture-views" || a == "--reference")
            {
                if (i + 1 >= args.size())
                {
                    ShowUsage(windowName, a + (a == "--bake-to" ? " needs an output file" : " needs a folder"));
                    return 2;
                }
                forwarded += a + " \"" + args[++i] + "\" ";
                continue;
            }
            if (a == "--camera-capture" || a == "--cameras" || a == "--camera" || a == "--camera-list" || a == "--size" || a == "--scene" || a == "--spin" || a == "--occlusion" || a == "--policy" || a == "--zoom" || a == "--occlusion-test" || a == "--distance" || a == "--occlusion-margin")
            {
                if (i + 1 >= args.size())
                {
                    ShowUsage(windowName, a + (a == "--size" ? " needs <W>x<H>" : a == "--spin" ? " needs degrees a second" : a == "--zoom" ? " needs a period in seconds" : a == "--occlusion" ? " needs on or off"
                                                : a == "--policy" ? " needs conservative or greedy" : a == "--camera" ? " needs a camera number" : a == "--camera-list" ? " needs camera numbers"
                                                : a == "--cameras" ? " needs a cameras.json" : " needs a folder"));
                    return 2;
                }
                forwarded += a + " \"" + args[++i] + "\" ";
                continue;
            }
            // --set-background <package.gstx> solid <r,g,b> | gradient <top r,g,b> <bottom r,g,b> | none: writes the
            // package's recommended background (PackageView, GSTXVEW1) in place and exits; no window
            if (a == "--set-background")
            {
                auto colour = [](const std::string& s, DirectX::XMFLOAT3& c)
                {
                    return sscanf_s(s.c_str(), "%f,%f,%f", &c.x, &c.y, &c.z) == 3 && c.x >= 0.0f && c.x <= 1.0f && c.y >= 0.0f && c.y <= 1.0f && c.z >= 0.0f && c.z <= 1.0f;
                };
                const std::string pkg = i + 1 < args.size() ? args[i + 1] : std::string(), how = i + 2 < args.size() ? args[i + 2] : std::string();
                Splat::PackageBackground bg;
                bg.basis = "chosen";
                bool ok = !pkg.empty();
                if (how == "solid") { ok = ok && i + 3 < args.size() && colour(args[i + 3], bg.top); bg.bottom = bg.top; bg.mode = how; }
                else if (how == "gradient") { ok = ok && i + 4 < args.size() && colour(args[i + 3], bg.top) && colour(args[i + 4], bg.bottom); bg.mode = how; }
                else ok = ok && how == "none";
                if (!ok)
                {
                    ShowUsage(windowName, "--set-background needs <package.gstx> solid <r,g,b> | gradient <r,g,b> <r,g,b> | none (0..1 each)");
                    return 2;
                }
                Splat::PackageView view;
                view.Read(pkg); // Kept: its other settings, if it has a view section
                view.background = bg;
                const bool written = view.Write(pkg);
                Splat::LogTransitionTrace("--set-background: %s %s %s", pkg.c_str(), how.c_str(), written ? "written" : "FAILED");
                return written ? 0 : 1;
            }
            if (a == "--sh-degree" || a == "--sh-bits")
            {
                const int v = (i + 1 < args.size()) ? atoi(args[i + 1].c_str()) : 0;
                const bool ok = (a == "--sh-degree") ? (v >= 1 && v <= 3) : (v == 8 || v == 6 || v == 5 || v == 4);
                if (!ok)
                {
                    ShowUsage(windowName, a == "--sh-degree" ? "--sh-degree needs 1, 2 or 3" : "--sh-bits needs 8, 6, 5 or 4");
                    return 2;
                }
                forwarded += a + " " + args[++i] + " ";
                continue;
            }
            if (!a.empty() && (a[0] == '-' || (a[0] == '/' && a.size() == 2)))
            {
                ShowUsage(windowName, "Unknown option: " + a);
                return 2;
            }
            forwarded += "\"" + a + "\" ";
        }
    }
    std::vector<char> cmdBuf(forwarded.begin(), forwarded.end());
    cmdBuf.push_back('\0');

    SetUnhandledExceptionFilter(CrashDumpHandler);
    AddVectoredExceptionHandler(1, VectoredCrashHandler);

#if defined(_DEBUG)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            Splat::LogTransitionTrace("WinMain: D3D12 Debug Layer enabled.");
        }
    }
#endif

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = RunFramework(hInstance, cmdBuf.data(), nCmdShow, new Splat::SplatPakApp(windowName, mode));
    if (SUCCEEDED(hrCom))
    {
        CoUninitialize();
    }
    return result;
}

