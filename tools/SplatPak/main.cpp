// main.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// SplatPak entry point: the shared app (src/SplatCore) in Studio mode -- generator,
// renderer and streaming tabs. See AppEntry.h.

#include "AppEntry.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR lpCmdLine, int nCmdShow)
{
    return Splat::RunSplatPakApp(hInstance, lpCmdLine, nCmdShow, "SplatPak", Splat::SplatPakApp::Mode::Studio);
}
