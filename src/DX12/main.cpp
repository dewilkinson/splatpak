// main.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Surfels_DX12 entry point: the standalone viewer. It is the shared app (src/SurfelsCore) in
// Viewer mode -- SplatLab's Renderer and Streaming tabs with the generator hidden -- so the
// two executables cannot drift apart. See AppEntry.h.

#include "AppEntry.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    return Surfels::RunSurfelsApp(hInstance, lpCmdLine, nCmdShow, "Surfels Viewer", Surfels::SurfelsApp::Mode::Viewer);
}
