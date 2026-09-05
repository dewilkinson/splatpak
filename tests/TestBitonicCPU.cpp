// TestBitonicCPU.cpp
// Surfels -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Standalone CPU reference implementation of the bitonic sort network used by
// GPURadixSortCS.hlsl -- verifies the algorithm's correctness in isolation, without a GPU.

#include <iostream>
#include <vector>
#include <random>
#include <algorithm>

int main()
{
    uint32_t numPoints = 100000;
    uint32_t numElements = 1;
    while (numElements < numPoints) numElements <<= 1;

    std::vector<float> data(numElements);
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

    for (uint32_t i = 0; i < numPoints; i++) data[i] = dist(rng);
    for (uint32_t i = numPoints; i < numElements; i++) data[i] = -1e9f; // Padding dummy elements

    // Bitonic Sort simulation
    for (uint32_t level = 2; level <= numElements; level <<= 1)
    {
        for (uint32_t levelMask = level >> 1; levelMask > 0; levelMask >>= 1)
        {
            for (uint32_t i = 0; i < numElements; i++)
            {
                uint32_t j = i ^ levelMask;
                if (j > i)
                {
                    bool dir = ((i & level) == 0);
                    // dir == true (final merge): want descending (dI >= dJ -> swap if dI < dJ)
                    // dir == false: want ascending (dI <= dJ -> swap if dI > dJ)
                    bool swap = dir ? (data[i] < data[j]) : (data[i] > data[j]);
                    if (swap)
                    {
                        std::swap(data[i], data[j]);
                    }
                }
            }
        }
    }

    uint32_t errors = 0;
    for (uint32_t i = 0; i < numPoints - 1; i++)
    {
        if (data[i] < data[i + 1])
        {
            if (errors < 10)
            {
                std::cout << "Inversion at index " << i << ": data[i]=" << data[i] << " < data[i+1]=" << data[i+1] << "\n";
            }
            errors++;
        }
    }

    std::cout << "Bitonic Simulation on " << numPoints << " elements (" << numElements << " padded):\n";
    std::cout << "Errors (Inversions): " << errors << "\n";
    return (errors == 0) ? 0 : 1;
}

