// main.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// SplatPakViewer entry point: the standalone viewer. It is the shared app (src/SplatCore) in
// Viewer mode -- SplatPak's Renderer and Streaming tabs with the generator hidden -- so the
// two executables cannot drift apart. See AppEntry.h.

#include "AppEntry.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    return Splat::RunSplatPakApp(hInstance, lpCmdLine, nCmdShow, "Splat Viewer", Splat::SplatPakApp::Mode::Viewer);
}
