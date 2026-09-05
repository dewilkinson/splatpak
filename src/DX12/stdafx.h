// stdafx.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Precompiled/shared header for the Surfels sample: pulls in Windows, DirectXMath, and
// the Cauldron framework base headers every DX12 source file in this project needs.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // otherwise windows.h's max()/min() macros break std::max/std::min call sites
#include <windows.h>
#include <windowsx.h>

#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <chrono>

#include <DirectXMath.h>
using namespace DirectX;

// Cauldron (from libs/cauldron/src/DX12 and libs/cauldron/src/Common, both
// added as PUBLIC include dirs by the Cauldron_DX12 CMake target).
#include "base/Device.h"
#include "base/SwapChain.h"
#include "base/FrameworkWindows.h"
#include "base/ResourceViewHeaps.h"
#include "base/UploadHeap.h"
#include "base/DynamicBufferRing.h"
#include "base/CommandListRing.h"
#include "base/ShaderCompilerHelper.h"
#include "base/DXCHelper.h" // InitDirectXCompiler() -- must be called before any shader compiles
#include "base/Helper.h"
#include "base/GPUTimestamps.h"
#include "base/Imgui.h"
#include "base/ImGuiHelper.h"

#include "Misc/Misc.h"

// Something transitively pulled in still leaves the windows.h min()/max() macros
// active despite NOMINMAX above (observed with the vendored Cauldron/AGS headers) --
// undef them defensively so std::max/std::min work in this and dependent files.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using namespace CAULDRON_DX12;
