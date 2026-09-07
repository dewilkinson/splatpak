// main.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// SplatLab entry point: the shared app (src/SurfelsCore) in Studio mode -- generator,
// renderer and streaming tabs. See AppEntry.h.

#include "AppEntry.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    return Surfels::RunSurfelsApp(hInstance, lpCmdLine, nCmdShow, "SplatLab", Surfels::SurfelsApp::Mode::Studio);
}
