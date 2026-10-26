// main.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// SplatLabViewer entry point: the standalone viewer. It is the shared app (src/SplatCore) in
// Viewer mode -- SplatLab's Renderer and Streaming tabs with the generator hidden -- so the
// two executables cannot drift apart. See AppEntry.h.

#include "AppEntry.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    return Splat::RunSplatLabApp(hInstance, lpCmdLine, nCmdShow, "Splat Viewer", Splat::SplatLabApp::Mode::Viewer);
}
