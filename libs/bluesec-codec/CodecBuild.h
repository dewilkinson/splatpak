// CodecBuild.h
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Which build of the bluesec-codec library is linked: the proprietary algorithm core, or the open
// stand-in implementation the public repository compiles when the prebuilt library cannot be
// used. Both builds define these functions (CodecBuild.cpp in each), so an application can tell
// at runtime and warn the user about what the stand-in does not do. This header is the same in
// both builds and is Apache-2.0 like the other headers in this directory.

#pragma once

namespace Surfels
{
    struct CodecBuild
    {
        // True when the proprietary bluesec-codec is linked; false for the open stand-ins.
        static bool IsProprietary();

        // Short name of the linked build, for logs and About dialogs ("bluesec-codec" or
        // "open stand-in").
        static const char* Name();

        // One short line naming what is missing relative to the proprietary build, for a banner
        // row; empty when nothing is missing.
        static const char* MissingFeatures();
    };
}
