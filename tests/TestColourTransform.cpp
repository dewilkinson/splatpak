// TestColourTransform.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The v11 colour decorrelation (libs/gstx/ColourTransform.h) is exactly invertible: every one of the 2^24 byte
// triples comes back unchanged, and a record or SH buffer round-trips byte for byte.
//
//   TestColourTransform
//
// Exit code 0 on success, 1 on any failure.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "../libs/gstx/ColourTransform.h"

using namespace Splat;

int main()
{
    size_t failures = 0;
    for (uint32_t v = 0; v < (1u << 24); v++)
    {
        uint8_t a = (uint8_t)v, b = (uint8_t)(v >> 8), c = (uint8_t)(v >> 16);
        ColourTransform::Forward(a, b, c);
        ColourTransform::Inverse(a, b, c);
        if (a != (uint8_t)v || b != (uint8_t)(v >> 8) || c != (uint8_t)(v >> 16))
        {
            if (failures++ < 5) printf("FAIL: triple %06x does not round-trip\n", v);
        }
    }

    std::vector<uint8_t> records(20 * 1000), sh(45 * 1000);
    uint32_t seed = 12345;
    auto next = [&]() { seed = seed * 1664525u + 1013904223u; return (uint8_t)(seed >> 24); };
    for (auto& x : records) x = next();
    for (auto& x : sh) x = next();
    const auto records0 = records, sh0 = sh;
    ColourTransform::ApplyRecords(records.data(), 1000, 20);
    ColourTransform::ApplySH(sh.data(), sh.size());
    if (records == records0 || sh == sh0) { printf("FAIL: the transform changed nothing\n"); failures++; }
    ColourTransform::UndoRecords(records.data(), 1000, 20);
    ColourTransform::UndoSH(sh.data(), sh.size());
    if (records != records0) { printf("FAIL: records do not round-trip\n"); failures++; }
    if (sh != sh0) { printf("FAIL: SH does not round-trip\n"); failures++; }

    printf("%s: %zu failures (2^24 triples, records and SH buffers)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
