/* Render with the production pipeline, selecting WARP instead of hardware. */
#include <windows.h>
#include <d3d11.h>

static HRESULT WINAPI create_warp_device(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count,
    UINT sdk, ID3D11Device **device, D3D_FEATURE_LEVEL *level,
    ID3D11DeviceContext **context)
{
    (void)adapter; (void)type; (void)software;
    return D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
                              levels, count, sdk, device, level, context);
}

#define D3D11CreateDevice create_warp_device
#include "../src/nv2a_gpu_d3d11.c"
#undef D3D11CreateDevice

ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    (void)tex; (void)argb_out;
    return 0;
}

static int draw_color(unsigned kind, uint32_t diffuse, const float constant[4], uint32_t expected)
{
    Nv2aGpuVertex vertices[3] = {0};
    Nv2aGpuDraw draw = {0};
    Nv2aRcPlan plan = {0};
    D3D11_MAPPED_SUBRESOURCE mapped;
    const float clear[4] = {0, 0, 0, 0};
    uint32_t actual;
    unsigned i;
    HRESULT hr;
    vertices[0].x = 10; vertices[0].y = 10;
    vertices[1].x = 200; vertices[1].y = 10;
    vertices[2].x = 10; vertices[2].y = 200;
    for (i = 0; i < 3; i++) {
        vertices[i].rhw = 1.0f;
        vertices[i].diffuse = diffuse;
    }
    draw.surface.width = GPU_W; draw.surface.height = GPU_H;
    draw.surface.pitch = GPU_W * 4; draw.surface.bytes_per_pixel = 4;
    draw.surface.color_va = 0x1000;
    draw.state.color_mask = 0x01010101u;
    draw.state.depth_func = 0x207;
    draw.vertices = vertices; draw.count = 3; draw.plan = kind;
    plan.kind = kind;
    if (constant) memcpy(plan.six_c0, constant, sizeof plan.six_c0);
    draw.rc_plan = &plan;
    g_gpu.context->lpVtbl->ClearRenderTargetView(g_gpu.context, g_gpu.rtv, clear);
    gpu_draw_one(&draw);
    g_gpu.context->lpVtbl->CopyResource(g_gpu.context, (ID3D11Resource *)g_gpu.staging,
                                       (ID3D11Resource *)g_gpu.color);
    hr = g_gpu.context->lpVtbl->Map(g_gpu.context, (ID3D11Resource *)g_gpu.staging,
                                   0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: plan %u staging map: 0x%08lX\n", kind, (unsigned long)hr);
        return 0;
    }
    actual = ((const uint32_t *)((const uint8_t *)mapped.pData + 20 * mapped.RowPitch))[20];
    g_gpu.context->lpVtbl->Unmap(g_gpu.context, (ID3D11Resource *)g_gpu.staging, 0);
    if (actual != expected) {
        fprintf(stderr, "FAIL: plan %u diffuse %08X: rendered %08X; expected %08X\n",
                kind, diffuse, actual, expected);
        return 0;
    }
    return 1;
}

int main(void)
{
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
    const float mixed[4] = {18.0f / 255.0f, 52.0f / 255.0f, 86.0f / 255.0f, 127.0f / 255.0f};
    int passed = 1;
    if (!gpu_init()) {
        fprintf(stderr, "FAIL: WARP backend initialization\n");
        return 1;
    }
    passed &= draw_color(3, 0xFFFF0000u, NULL, 0xFFFF0000u);
    passed &= draw_color(3, 0xFF00FF00u, NULL, 0xFF00FF00u);
    passed &= draw_color(3, 0xFF0000FFu, NULL, 0xFF0000FFu);
    passed &= draw_color(3, 0x7F123456u, NULL, 0x7F123456u);
    passed &= draw_color(9, 0xFFFFFFFFu, red, 0xFFFF0000u);
    passed &= draw_color(9, 0xFFFFFFFFu, green, 0xFF00FF00u);
    passed &= draw_color(9, 0xFFFFFFFFu, blue, 0xFF0000FFu);
    passed &= draw_color(9, 0xFFFFFFFFu, mixed, 0x7F123456u);
    release_all();
    if (passed) puts("PASS: WARP solid vertices and combiner constants preserve ARGB channels and alpha");
    return passed ? 0 : 1;
}
