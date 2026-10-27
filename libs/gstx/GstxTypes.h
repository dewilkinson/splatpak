// GstxTypes.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
//
// The plain data types the library's public headers share with every host: the GPU splat and meshlet records, the
// CPU splat point and its 3D Gaussian attributes, and the occlusion volume's blocks. Self-contained (the standard
// library and DirectXMath), so a host that stages only libs/gstx (the UE5Splats plugin) can include the runtime's
// headers. src/SplatCore/WaveletTypes.h, the package format, includes it.

#pragma once
#include <cstdint>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>
#include <DirectXMath.h>

namespace Splat
{
    using namespace DirectX;

    // Upper bound on the occlusion volume mip chain: mip 0 plus up to three coarser levels. Blocks fall
    // by roughly 4x per level (the skin is a surface), so the whole chain costs about a third more than
    // mip 0 alone, and a fourth coarsening would be too blobby to hug any surface usefully.
    static constexpr uint32_t kMaxOcclusionMips = 4;

    #pragma pack(push, 1)
    // One baked occluder block of the interior occlusion volume (libs/gstx/OcclusionVolume.h).
    // The volume is a closed voxel solid of the model sitting just inside the splat shell, stored as an
    // adaptive octree: the finest cubes hug the surface while the interior is covered by a few big ones,
    // and blocks with no exposed face are dropped, since a closed occluder only needs its skin. halfSize
    // is half of (cellSize * 2^level), and faces of neighbouring blocks of different sizes still tile
    // exactly because every block is grid-aligned to its own size.
    //
    // packedColor layout:
    //   bits  0..15  RGB565 bake of the mean splat colour over a wide box of surface around the block
    //                (half-width ~1.5x its depth below the surface), i.e. the local albedo of the cap of
    //                surface facing it rather than one nearest patch (see Quantizer/UnpackColorRGB565).
    //   bits 16..21  exposed-face mask, one bit per face in -Z,+Z,-X,+X,-Y,+Y order: set when any fine
    //                cell across that face is not solid, i.e. some part of the face is visible.
    //   bit  22      mask-valid flag. Set by every current bake. Clear on legacy files baked before the
    //                mask existed, which the shader treats as "all six faces exposed".
    //
    // There is deliberately no live shrink/shave at draw time: the erosion is baked, because the volume
    // travels inside the .gstx and the viewer should show exactly what was packaged.
    struct OcclusionVoxelGPU
    {
        XMFLOAT3 center;
        float    halfSize;
        uint32_t packedColor; // Bits 0..15: RGB565 colour (see Quantizer/UnpackColorRGB565); bits 16..21: exposed-face mask
    };
    #pragma pack(pop)
    static_assert(sizeof(OcclusionVoxelGPU) == 20, "OcclusionVoxelGPU must be exactly 20 bytes");

    // Layout of the occlusion volume mip chain inside one OcclusionVoxelGPU array (in a package, and the
    // in-memory result of OcclusionVolume::Bake). Mip 0 is the finest volume, the skin just inside the
    // splat surface; each further mip is a coarser level that lies inside the one below it and can never
    // protrude, with its own exposed-face masks. The renderer draws exactly ONE mip per frame, chosen so
    // a cell still covers a few pixels at the model's nearest point (see SplatRenderer): at distance the
    // fine skin is sub-pixel work and too tight for the big coarse-LOD splats it is paired with, so the
    // cube size grows together with the splat LOD in view.
    //
    // The blocks of all mips are stored back to back, mip 0 first (the coarser mips lie hidden inside mip 0's
    // skin, so drawing the whole array still renders correctly).
    struct OcclusionMipTable
    {
        uint32_t mipCount = 0;                          // 0 = no volume; 1 = level-0 only
        uint32_t blockCount[kMaxOcclusionMips] = {};    // Blocks per mip, in array order (mip 0 first)
        float    cellSize[kMaxOcclusionMips] = {};      // Finest cube edge of each mip; cellSize[k] = cellSize[0] * 2^k

        uint32_t FirstBlock(uint32_t mip) const
        {
            uint32_t first = 0;
            for (uint32_t k = 0; k < mip && k < kMaxOcclusionMips; k++) first += blockCount[k];
            return first;
        }
        uint32_t TotalBlocks() const { return FirstBlock(kMaxOcclusionMips); }

        // A table describing a plain single-level array (legacy packages, or any caller that never
        // asked for mips): one mip holding every block, its cell size read off the smallest block.
        static OcclusionMipTable SingleLevel(const OcclusionVoxelGPU* blocks, uint32_t count)
        {
            OcclusionMipTable t;
            if (count == 0 || blocks == nullptr) return t;
            float minHalf = blocks[0].halfSize;
            for (uint32_t i = 1; i < count; i++) minHalf = (blocks[i].halfSize < minHalf) ? blocks[i].halfSize : minHalf;
            t.mipCount = 1;
            t.blockCount[0] = count;
            t.cellSize[0] = minHalf * 2.0f;
            return t;
        }
    };

    // Packed 8-byte GPU Splat structure
    // Layout:
    //  - packedPosRadius (32-bit):
    //      - pos.x: 10 bits [0..1023] relative to chunk AABB
    //      - pos.y: 10 bits [0..1023] relative to chunk AABB
    //      - pos.z: 10 bits [0..1023] relative to chunk AABB
    //      - radiusExp: 2 bits [0..3] coarse radius scaling
    //  - packedNormal (16-bit):
    //      - oct16 octahedral normal (2 x snorm8)
    //  - packedColor (16-bit):
    //      - RGB565 (5 bits red, 6 bits green, 5 bits blue)
    #pragma pack(push, 1)
    struct PackedPointGPU
    {
        uint32_t packedPosRadius; // 10:10:10:2
        uint16_t packedNormal;    // 8:8 Octahedral
        uint16_t packedColor;     // 5:6:5 RGB
    };
    #pragma pack(pop)

    static_assert(sizeof(PackedPointGPU) == 8, "PackedPointGPU must be exactly 8 bytes");

    // Meshlet / Micro-Chunk descriptor for 64/128-splat spatial clusters
    struct MeshletChunkGPU
    {
        XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        float    boundingRadius = 0.0f;
        XMFLOAT3 aabbMin = { 0.0f, 0.0f, 0.0f };
        uint32_t splatOffset = 0;
        XMFLOAT3 aabbExtents = { 0.0f, 0.0f, 0.0f };
        uint32_t splatCount = 0;
        float    blendWeight = 1.0f; // 1.0f = 100% solid opacity
        uint32_t lodLevel = 0;
        float    dilationMorph = 0.0f; // Morph dilation factor for silhouette reconstruction; splat mode: the level 0 Gaussian fade-in weight instead (0 = discs, 1 = Gaussians; see SplatPakApp::UpdateStreamingSimulation)
        float    isSilhouette = 0.0f;  // 1.0f if silhouette chunk, 0.0f otherwise
        XMFLOAT3 coneAxis = { 0.0f, 1.0f, 0.0f }; // Average unit normal vector of cluster
        float    coneCutoff = -1.0f;              // cos(theta_max) of cluster normal cone (-1.0 = cone culling disabled)
    };
    static_assert(sizeof(MeshletChunkGPU) == 80, "MeshletChunkGPU must be exactly 80 bytes");

    // Uncompressed intermediate splat representation for CPU / preprocessor
    struct SplatPoint
    {
        XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 normal   = { 0.0f, 1.0f, 0.0f };
        XMFLOAT3 color    = { 1.0f, 1.0f, 1.0f };
        float    radius   = 0.05f;
        // Splat mode: index of this point's entry in the source cloud's SplatAttributes array (the
        // Gaussian's rotation, scales, opacity and spherical harmonics). Survives the decimation into
        // coarser levels, so every level can be encoded from the original attributes. kNoSplatSource
        // marks a point with no such entry (a plain point cloud, a synthetic cloud, a decoded splat).
        uint32_t sourceIndex = 0xFFFFFFFFu;
    };
    static_assert(sizeof(SplatPoint) == 44, "SplatPoint size check (mirrored by RawSplat in the shaders)");
    static constexpr uint32_t kNoSplatSource = 0xFFFFFFFFu;

    // 3D Gaussian Splatting attributes of one source point, kept alongside SplatPoint during a bake
    // in splat mode (SplatPoint::sourceIndex points here). Scales and rotation are in the model's
    // frame after the import's axis change; the spherical-harmonics coefficients have had the same
    // change applied (see SplatCodec::ApplyAxisFlipToSH) so a shader evaluates them with a direction
    // in that frame. sh holds the bands above the DC term, coefficient-major: sh[3 * k + c] is band
    // coefficient k (0..14, bands 1..3 in the 3DGS order) of colour channel c. The DC colour itself is
    // SplatPoint::color (0.5 + C0 * f_dc, clamped to [0, 1]).
    static constexpr uint32_t kSplatSHMaxCoefficients = 45; // 15 coefficients x 3 channels (degree 3)
    struct SplatAttributes
    {
        XMFLOAT3 scale      = { 0.01f, 0.01f, 0.01f };    // Per-axis standard deviations (world units), exp() of the file's log scales
        XMFLOAT4 rotation   = { 0.0f, 0.0f, 0.0f, 1.0f }; // Unit quaternion (x, y, z, w)
        float    opacity    = 1.0f;                       // Sigmoid of the file's logit, 0..1
        uint32_t bakedLevel = 0;                          // Level whose footprint these scales already describe (0 for an import; see SplatCodec::AttributesForLevel)
        float    sh[kSplatSHMaxCoefficients] = {};
    };

    // Which per-point record a package's LOD payloads hold (SFLWFileHeader::recordFormat, v8+).
    enum SplatFormat : uint32_t
    {
        RECORD_FORMAT_POINT8  = 0, // PackedPointGPU: 8-byte oriented disc (no longer read)
        RECORD_FORMAT_SPLAT20  = 1, // PackedSplatGPU: 20-byte 3D Gaussian, plus an optional spherical-harmonics stream
    };

    // Splat mode's per-point GPU record: a full 3D Gaussian in 20 bytes. Five little-endian 32-bit words:
    //   word 0  bits  0..15 position x, 16..31 position y      -- 16-bit unorm inside the global bounds
    //   word 1  bits  0..15 position z, 16..23 scale x, 24..31 scale y
    //   word 2  bits  0..7  scale z, 8..15 opacity, 16..23 colour R, 24..31 colour G
    //   word 3  bits  0..7  colour B, 8..15 flags, 16..31 SH scale (IEEE 754 binary16)
    //   word 4  rotation: smallest-three unit quaternion, bits 0..1 index of the dropped (largest) component,
    //           bits 2..11 / 12..21 / 22..31 the other three in order, 10-bit unorm over [-1/sqrt2, +1/sqrt2]
    // Scales are 8-bit unorm over the package's [splatScaleLog2Min, splatScaleLog2Max] in log2 space.
    // Opacity is 8-bit unorm. Colour is the DC term as 8-bit display-space RGB. The SH scale is the
    // per-point dequantisation factor of the record's entry in the package's SH stream (0 = no SH).
    #pragma pack(push, 1)
    struct PackedSplatGPU
    {
        uint32_t word0;
        uint32_t word1;
        uint32_t word2;
        uint32_t word3;
        uint32_t word4;
    };
    #pragma pack(pop)
    static_assert(sizeof(PackedSplatGPU) == 20, "PackedSplatGPU must be exactly 20 bytes");
    // Big arrays of points and Gaussians (a scan's run to gigabytes). BulkAlloc takes 32 MB and more straight from the OS
    // (VirtualAlloc on Windows): zeroed pages, committed when first touched, never written up front. GstxTypes.cpp.
    void* BulkAlloc(size_t bytes);
    void  BulkFree(void* p, size_t bytes);

    // The allocator of SplatPointArray and SplatAttributeArray. Inside an Uninitialized scope, a resize on this thread
    // constructs nothing: the caller constructs every new element itself (PLYLoader does, on many threads at once), so the
    // memory is written once, in parallel, instead of first by a single-threaded resize. Every other resize constructs
    // as usual.
    template <class T>
    struct BulkAllocator
    {
        using value_type = T;
        BulkAllocator() noexcept = default;
        template <class U> BulkAllocator(const BulkAllocator<U>&) noexcept {}
        T* allocate(size_t n) { return static_cast<T*>(BulkAlloc(n * sizeof(T))); }
        void deallocate(T* p, size_t n) noexcept { BulkFree(p, n * sizeof(T)); }
        template <class U, class... Args> void construct(U* p, Args&&... args) { ::new ((void*)p) U(std::forward<Args>(args)...); }
        template <class U> void construct(U* p) { if (!SkipConstruction()) ::new ((void*)p) U(); }
        template <class U> bool operator==(const BulkAllocator<U>&) const noexcept { return true; }
        template <class U> bool operator!=(const BulkAllocator<U>&) const noexcept { return false; }

        static bool& SkipConstruction() { thread_local bool skip = false; return skip; }
        struct Uninitialized
        {
            Uninitialized() { SkipConstruction() = true; }
            ~Uninitialized() { SkipConstruction() = false; }
        };
    };

    using SplatPointArray = std::vector<SplatPoint, BulkAllocator<SplatPoint>>;
    using SplatAttributeArray = std::vector<SplatAttributes, BulkAllocator<SplatAttributes>>;
}
