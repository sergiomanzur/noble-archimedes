/* Exercise the real backend cache with native D3D11 state objects. */
#include "../src/nv2a_gpu_d3d11.c"

/* Unused texture/guest-memory paths are not part of this state-cache test. */
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    (void)tex;
    (void)argb_out;
    return 0;
}

static int check_state(const Nv2aRenderState *rs, uint32_t guest_mask,
                       UINT8 expected_mask, D3D11_BLEND src, D3D11_BLEND dst,
                       D3D11_BLEND_OP op)
{
    ID3D11BlendState *state = get_blend(rs, guest_mask);
    D3D11_BLEND_DESC desc;
    D3D11_RENDER_TARGET_BLEND_DESC *rt = &desc.RenderTarget[0];
    if (!state) {
        fprintf(stderr, "FAIL: CreateBlendState returned NULL\n");
        return 0;
    }
    state->lpVtbl->GetDesc(state, &desc);
    if (rt->RenderTargetWriteMask != expected_mask ||
        rt->BlendEnable != (rs->blend_enable ? TRUE : FALSE) ||
        /* D3D11 canonicalizes irrelevant factor/op fields with blending off. */
        (rs->blend_enable &&
         (rt->SrcBlend != src || rt->DestBlend != dst || rt->BlendOp != op ||
          rt->SrcBlendAlpha != src || rt->DestBlendAlpha != dst ||
          rt->BlendOpAlpha != op))) {
        fprintf(stderr, "FAIL: guest mask 0x%08X: write mask 0x%X (expected 0x%X), "
                "src/dst/op %u/%u/%u (expected %u/%u/%u)\n",
                guest_mask, rt->RenderTargetWriteMask, expected_mask,
                rt->SrcBlend, rt->DestBlend, rt->BlendOp, src, dst, op);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const uint32_t guest_masks[16] = {
        0x00000000, 0x00010000, 0x00000100, 0x00010100,
        0x00000001, 0x00010001, 0x00000101, 0x00010101,
        0x01000000, 0x01010000, 0x01000100, 0x01010100,
        0x01000001, 0x01010001, 0x01000101, 0x01010101
    };
    /* These factors are valid in both the color and alpha D3D11 fields. */
    static const struct { uint32_t guest; D3D11_BLEND native; } factors[] = {
        {0x0000, D3D11_BLEND_ZERO}, {0x0001, D3D11_BLEND_ONE},
        {0x0302, D3D11_BLEND_SRC_ALPHA}, {0x0303, D3D11_BLEND_INV_SRC_ALPHA},
        {0x0304, D3D11_BLEND_DEST_ALPHA}, {0x0305, D3D11_BLEND_INV_DEST_ALPHA},
        {0x8001, D3D11_BLEND_BLEND_FACTOR}, {0x8002, D3D11_BLEND_INV_BLEND_FACTOR}
    };
    static const struct { uint32_t guest; D3D11_BLEND_OP native; } equations[] = {
        {0x8006, D3D11_BLEND_OP_ADD}, {0x800A, D3D11_BLEND_OP_SUBTRACT},
        {0x800B, D3D11_BLEND_OP_REV_SUBTRACT}, {0x8007, D3D11_BLEND_OP_MIN},
        {0x8008, D3D11_BLEND_OP_MAX}
    };
    Nv2aRenderState rs = {0};
    unsigned enabled, src, dst, eq, mask;
    int passed = 0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &g_gpu.device, NULL,
                                 &g_gpu.context);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: WARP device creation: 0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    /* RGB followed by RGBA must preserve the second draw's destination alpha. */
    rs.blend_enable = 1;
    rs.blend_src = 0x0302;
    rs.blend_dst = 0x0303;
    rs.blend_eq = 0x8006;
    if (!check_state(&rs, 0x00010101, 0x7, D3D11_BLEND_SRC_ALPHA,
                     D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD) ||
        !check_state(&rs, 0x01010101, 0xF, D3D11_BLEND_SRC_ALPHA,
                     D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD)) goto done;

    for (enabled = 0; enabled < 2; enabled++)
    for (src = 0; src < sizeof factors / sizeof factors[0]; src++)
    for (dst = 0; dst < sizeof factors / sizeof factors[0]; dst++)
    for (eq = 0; eq < sizeof equations / sizeof equations[0]; eq++)
    for (mask = 0; mask < 16; mask++) {
        rs.blend_enable = enabled;
        rs.blend_src = factors[src].guest;
        rs.blend_dst = factors[dst].guest;
        rs.blend_eq = equations[eq].guest;
        if (!check_state(&rs, guest_masks[mask], (UINT8)mask,
                         factors[src].native, factors[dst].native,
                         equations[eq].native)) goto done;
        /* Also exercise a cache hit before moving to the next descriptor. */
        if (!check_state(&rs, guest_masks[mask], (UINT8)mask,
                         factors[src].native, factors[dst].native,
                         equations[eq].native)) goto done;
    }
    passed = 1;
    puts("PASS: blend cache preserves RGB/RGBA, all 16 write masks, factors and equations");
done:
    release_all();
    return passed ? 0 : 1;
}
