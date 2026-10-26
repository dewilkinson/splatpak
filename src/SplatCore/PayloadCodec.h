// PayloadCodec.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// The entropy stage of .gstx payloads (Clause 8 of docs/GSTX_FORMAT_SPECIFICATION.md). A level's
// records, or its spherical-harmonics stream, already transposed into byte planes by
// ByteShuffle::Shuffle, go through one of these coders, named by the header's payloadCodec field:
//
//   PAYLOAD_CODEC_LIBRARY (0)  the codec library's own stage (ByteShuffle::CompressShuffled): every
//                              package before format version 9, kept so they still load
//   PAYLOAD_CODEC_BROTLI  (1)  Brotli (RFC 7932, libs/brotli): what every package is written with now
//
// Measured on the Cthulhu scan (3.3 M splats, SH degree 1), the library's run-length stage took the
// shuffled records to 1.12x and the SH stream to 1.00x; Brotli at quality 11 takes them to 1.39x
// and 1.18x (the records to 89.5 MB against 111.7 MB). Brotli over the run-length output was
// slightly worse than Brotli alone, so it replaces that stage rather than following it. Decoding
// runs at ~150 MB/s per core; encoding at quality 11 at ~1 MB/s per core, which is why the packager
// hands every payload to CompressInPlace and lets it use the cores.

#pragma once
#include <vector>
#include <cstdint>
#include <thread>
#include <chrono>
#include <functional>
#include <atomic>
#include <algorithm>
#include "WaveletTypes.h"
#include "../../libs/gstx/ByteShuffle.h"
#include "../../libs/gstx/PayloadDecode.h"
#include <brotli/encode.h>

namespace Splat
{
    class PayloadCodec
    {
    public:
        static constexpr uint32_t kDefault = PAYLOAD_CODEC_BROTLI;
        static constexpr int kPackageQuality = BROTLI_MAX_QUALITY; // 11: what a package is written with
        static constexpr unsigned kMaxThreads = 16;                // Quality 11 holds tens of MB per thread in flight

        static const char* Name(uint32_t codec)
        {
            switch (codec)
            {
            case PAYLOAD_CODEC_LIBRARY: return "codec library";
            case PAYLOAD_CODEC_BROTLI:  return "Brotli";
            default:                    return "unknown";
            }
        }

        // One payload. Returns an empty vector for empty input, and (Brotli) on an allocation failure
        // with non-empty input, which a writer must treat as a failed bake. quality is Brotli's (0..11).
        static std::vector<uint8_t> Compress(const std::vector<uint8_t>& shuffled, uint32_t codec = kDefault, int quality = kPackageQuality)
        {
            if (codec == PAYLOAD_CODEC_LIBRARY) return ByteShuffle::CompressShuffled(shuffled);
            std::vector<uint8_t> out;
            if (shuffled.empty()) return out;
            // The window covers the whole payload where the standard windows allow (a level's records
            // run to a few MB); a smaller window keeps a small payload's encoder small
            int lgwin = BROTLI_MIN_WINDOW_BITS;
            while (lgwin < BROTLI_MAX_WINDOW_BITS && ((size_t)1 << lgwin) < shuffled.size()) lgwin++;
            size_t outSize = BrotliEncoderMaxCompressedSize(shuffled.size());
            out.resize(outSize);
            if (BrotliEncoderCompress(quality, lgwin, BROTLI_MODE_GENERIC, shuffled.size(), shuffled.data(), &outSize, out.data()) != BROTLI_TRUE)
            {
                out.clear();
                return out;
            }
            out.resize(outSize);
            return out;
        }

        // The inverse: false when the bytes are not a payload of this codec or do not decode to exactly dstSize bytes
        static bool Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize, uint32_t codec)
        {
            return PayloadDecode::Decompress(src, srcSize, dst, dstSize, codec); // The one reader, in the codec library
        }

        // Every payload replaced by its compressed form. Brotli runs across the cores (up to kMaxThreads);
        // the library stage runs one payload after another, as it is not known to be reentrant.
        // progress, when given, is polled on the calling thread with the fraction of payloads done; returning false
        // stops handing out payloads and returns false once the ones in flight finish (the rest stay uncompressed)
        static bool CompressInPlace(std::vector<std::vector<uint8_t>*>& payloads, uint32_t codec = kDefault, int quality = kPackageQuality,
                                    const std::function<bool(float)>& progress = nullptr)
        {
            // Progress counts input bytes: a full-resolution level takes far longer than a coarse one
            size_t totalBytes = 0;
            for (const auto* p : payloads) totalBytes += p->size();
            std::atomic<size_t> next{ 0 }, finished{ 0 }, finishedBytes{ 0 };
            auto work = [&]()
            {
                for (size_t i; (i = next.fetch_add(1)) < payloads.size(); finished++)
                {
                    const size_t bytes = payloads[i]->size();
                    *payloads[i] = Compress(*payloads[i], codec, quality);
                    finishedBytes += bytes;
                }
            };
            const unsigned threads = codec != PAYLOAD_CODEC_BROTLI ? 1u : std::max(1u, std::min({ std::thread::hardware_concurrency(), kMaxThreads, (unsigned)payloads.size() }));
            std::vector<std::thread> pool;
            for (unsigned t = progress ? 0 : 1; t < threads; t++) pool.emplace_back(work);
            bool keepGoing = true;
            if (!progress) work();
            else
                while (finished.load() < payloads.size() && next.load() < payloads.size() + pool.size())
                {
                    if (keepGoing && !progress((float)((double)finishedBytes.load() / (double)std::max<size_t>(1, totalBytes))))
                    {
                        keepGoing = false;
                        next = payloads.size() + pool.size(); // Nothing more is handed out
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            for (auto& t : pool) t.join();
            return keepGoing;
        }
    };
}
