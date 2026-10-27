# PLY loader performance

How long `PLYLoader::LoadPLY` takes on the bundled 3D Gaussian scans, step by step, so every change to the loader is
measured against the last. Each step's output is checked against the original single-threaded loader, point for point
and Gaussian for Gaussian (`identical` in every row below).

**Machine:** AMD Ryzen 9 7950X3D (16 cores, 32 threads), WD Blue SN550-class NVMe (WDS100T2B0C), Windows 11.
**Method:** a scratch benchmark loads each scan with the committed loader and then with the new one, twice, and
compares the results. Times are wall clock for the whole `LoadPLY` call, with the file already in the system cache
(warm): they measure the parse and the memory traffic, not the drive. Cold-cache times are still to be measured.

| Scan | Points | File |
|---|---|---|
| bicycle (Mip-NeRF 360, 3DGS iteration 30,000) | 5,279,598 | 1,450 MB |
| Cthulhu | 3,322,060 | 748 MB |
| Venus | 4,668,773 | 1,076 MB |

## Steps

| # | Date | Change | bicycle | Cthulhu | Venus | vs. previous |
|---|---|---|---|---|---|---|
| 0 | 2026-10-26 | Single-threaded: one read into a buffer, one loop | 1.87 s | 0.99 s | 1.45 s | (baseline) |
| 1 | 2026-10-26 | Parallel read (a file handle per thread, each its own byte range) and parallel conversion into per-thread lists, joined in order | 1.52 s* | 0.61 s* | | |
| 2 | 2026-10-26 | Two passes: each range counts what it keeps, then converts straight into place (no per-thread lists, no join) | 0.95 s | 0.52 s | 0.72 s | -37% / -15% / |
| 3 | 2026-10-26 | No constructing `resize`: `SplatPointArray` / `SplatAttributeArray` on `BulkAllocator` (VirtualAlloc above 32 MB, zeroed pages committed on first touch); outputs sized inside `BulkAllocator::Uninitialized` and each thread placement-constructs its own range | 0.71 s | 0.35 s | 0.52 s | -25% / -33% / -28% |

\* Step 1 was timed once, right after the old loader had warmed the cache, and not on Venus; steps 2 and 3 are the
second of two runs each.

Overall, step 3 against step 0: bicycle 2.6x, Cthulhu 2.8x, Venus 2.8x.

- Hash the scan in the same pass (today `BakeTuningCache::HashFile` reads the whole file a second time after the load).
- Memory-map the file instead of reading it into a buffer (no 1.45 GB copy; parallel page faults, `PrefetchVirtualMemory`).
- Measure cold-cache times, split into read, parse and hash.
