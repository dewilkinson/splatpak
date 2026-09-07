// AppEntry.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// One entry point for both executables. SplatLab's main.cpp calls RunSurfelsApp() in Studio
// mode, the standalone viewer's in Viewer mode; everything else (crash handlers, the debug
// layer, COM, Cauldron's RunFramework) lives in AppEntry.cpp so it is written once.

#pragma once
#include "stdafx.h"
#include "SurfelsApp.h"

namespace Surfels
{
    // windowName is the window title / Cauldron app name. Returns the process exit code.
    int RunSurfelsApp(HINSTANCE hInstance, LPSTR lpCmdLine, int nCmdShow, const char* windowName, SurfelsApp::Mode mode);
}
