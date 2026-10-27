// GstxTypes.cpp
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
//
// BulkAlloc / BulkFree (GstxTypes.h): 32 MB and more from VirtualAlloc, which hands back zeroed pages committed on first
// touch, so a large array costs nothing until the threads that fill it write it; smaller blocks from operator new.

#include "GstxTypes.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Splat
{
    static constexpr size_t kBulkThreshold = size_t(32) << 20;

    void* BulkAlloc(size_t bytes)
    {
#ifdef _WIN32
        if (bytes >= kBulkThreshold)
        {
            void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!p) throw std::bad_alloc();
            return p;
        }
#endif
        return ::operator new(bytes);
    }

    void BulkFree(void* p, size_t bytes)
    {
        if (!p) return;
#ifdef _WIN32
        if (bytes >= kBulkThreshold)
        {
            VirtualFree(p, 0, MEM_RELEASE);
            return;
        }
#endif
        ::operator delete(p);
    }
}
