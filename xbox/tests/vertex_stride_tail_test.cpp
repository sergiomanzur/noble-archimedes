#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Verifies the AMD GCN vertex-buffer re-striding math used by AssembleRaw and
// D3D12Backend::UploadVertexBuffers (ported from jmafort/CastlevaniaCOD-PC ccd5000).
namespace {

struct ElementSpec {
    uint32_t offset;
    uint32_t bytes;
};

struct UploadedView {
    std::vector<uint8_t> data;
    uint32_t size_in_bytes = 0;
    uint32_t stride_in_bytes = 0;
};

uint32_t ComputeTail(uint32_t stride, const std::vector<ElementSpec>& elems) {
    uint32_t tail = 0;
    for (const ElementSpec& e : elems) {
        if (e.offset + e.bytes > stride) {
            tail = std::max(tail, e.offset + e.bytes - stride);
        }
    }
    return tail;
}

UploadedView UploadRestrided(const uint8_t* src, uint32_t bytes, uint32_t stride, uint32_t tail) {
    UploadedView out;
    uint32_t n = stride ? bytes / stride : 0;
    if (tail && n) {
        uint32_t wide = stride + tail;
        out.data.resize(size_t(n) * wide);
        for (uint32_t v = 0; v < n; v++) {
            uint8_t* dst = out.data.data() + size_t(v) * wide;
            size_t at = size_t(v) * stride;
            size_t have = std::min<size_t>(wide, bytes - at);
            std::memcpy(dst, src + at, have);
            if (have < wide) std::memset(dst + have, 0, wide - have);
        }
        out.size_in_bytes = n * wide;
        out.stride_in_bytes = wide;
        return out;
    }
    out.data.resize(size_t(bytes) + tail);
    if (bytes) std::memcpy(out.data.data(), src, bytes);
    if (tail) std::memset(out.data.data() + bytes, 0, tail);
    out.size_in_bytes = bytes + tail;
    out.stride_in_bytes = stride;
    return out;
}

void TestCastlevania2DQuadLayout() {
    // Castlevania: Curse of Darkness 2D draws: 4 vertices, 28-byte stride,
    // unused second float2 texcoord declared at byte 28 (reaching byte 36).
    const std::vector<ElementSpec> elems = {
        {0, 16},   // position float4
        {16, 4},   // color D3DCOLOR
        {20, 8},   // texcoord0 float2 (valid, bytes 20..27)
        {28, 8},   // texcoord1 float2 (unused, reaches past stride 28)
    };
    const uint32_t stride = 28;
    const uint32_t tail = ComputeTail(stride, elems);
    assert(tail == 8);

    std::vector<uint8_t> guest(4 * stride);
    for (uint32_t v = 0; v < 4; v++) {
        for (uint32_t k = 0; k < stride; k++) {
            guest[v * stride + k] = uint8_t(v * 32 + k);
        }
    }

    UploadedView view = UploadRestrided(guest.data(), uint32_t(guest.size()), stride, tail);
    assert(view.stride_in_bytes == 36);
    assert(view.size_in_bytes == 4 * 36);

    // Every declared element must fit strictly inside StrideInBytes and SizeInBytes.
    for (const ElementSpec& e : elems) {
        assert(e.offset + e.bytes <= view.stride_in_bytes);
        assert(3 * view.stride_in_bytes + e.offset + e.bytes <= view.size_in_bytes);
    }

    // Vertices 0..2 retain their 28 bytes plus the next vertex's leading 8 bytes.
    for (uint32_t v = 0; v < 3; v++) {
        const uint8_t* rec = view.data.data() + v * 36;
        assert(std::memcmp(rec, guest.data() + v * 28, 28) == 0);
        assert(std::memcmp(rec + 28, guest.data() + (v + 1) * 28, 8) == 0);
    }
    // Last vertex retains its 28 bytes and zero-fills the trailing 8 bytes.
    const uint8_t* last = view.data.data() + 3 * 36;
    assert(std::memcmp(last, guest.data() + 3 * 28, 28) == 0);
    for (uint32_t k = 28; k < 36; k++) {
        assert(last[k] == 0);
    }
}

void TestNormalLayoutUnchanged() {
    const std::vector<ElementSpec> elems = {
        {0, 12},
        {12, 12},
        {24, 8},
    };
    const uint32_t stride = 32;
    const uint32_t tail = ComputeTail(stride, elems);
    assert(tail == 0);

    std::vector<uint8_t> guest(3 * stride, 0xAB);
    UploadedView view = UploadRestrided(guest.data(), uint32_t(guest.size()), stride, tail);
    assert(view.stride_in_bytes == 32);
    assert(view.size_in_bytes == 96);
    assert(view.data == guest);
}

}  // namespace

int main() {
    TestCastlevania2DQuadLayout();
    TestNormalLayoutUnchanged();
    std::puts("vertex_stride_tail_test: PASS");
    return 0;
}
