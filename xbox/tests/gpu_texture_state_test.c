/* Exercise the actual backend texture cache and read back its WARP resources. */
#include "../src/nv2a_gpu_d3d11.c"

ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }

/* Fixed colors model two palettes for identical P8 index bytes. The cache,
 * GPU texture creation, SRV and staging readback are the production paths. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    uint32_t color, i;
    if (tex->palette == 0x2000u) color = 0xFFCC2211u;
    else if (tex->palette == 0x3000u) color = 0xFF1144DDu;
    else return 0;
    for (i = 0; i < tex->width * tex->height; i++) argb_out[i] = color;
    return 1;
}

static int check_pixels(ID3D11ShaderResourceView *srv, uint32_t expected)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Resource *source = NULL;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr;
    unsigned x, y;
    int passed = 0;
    if (!srv) {
        fprintf(stderr, "FAIL: texture cache returned NULL\n");
        return 0;
    }
    srv->lpVtbl->GetResource(srv, &source);
    memset(&td, 0, sizeof td);
    td.Width = 2; td.Height = 2; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td, NULL, &staging);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: staging creation: 0x%08lX\n", (unsigned long)hr);
        goto done;
    }
    g_gpu.context->lpVtbl->CopyResource(g_gpu.context, (ID3D11Resource *)staging, source);
    hr = g_gpu.context->lpVtbl->Map(g_gpu.context, (ID3D11Resource *)staging,
                                   0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: staging map: 0x%08lX\n", (unsigned long)hr);
        goto done;
    }
    passed = 1;
    for (y = 0; y < 2; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)mapped.pData + y * mapped.RowPitch);
        for (x = 0; x < 2; x++) {
            if (row[x] != expected) {
                fprintf(stderr, "FAIL: cached GPU pixel (%u,%u) = %08X; expected %08X\n",
                        x, y, row[x], expected);
                passed = 0;
            }
        }
    }
    g_gpu.context->lpVtbl->Unmap(g_gpu.context, (ID3D11Resource *)staging, 0);
done:
    if (staging) staging->lpVtbl->Release(staging);
    if (source) source->lpVtbl->Release(source);
    return passed;
}

int main(void)
{
    Nv2aTexture tex = {0};
    ID3D11ShaderResourceView *a, *b;
    int passed = 0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &g_gpu.device, NULL, &g_gpu.context);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: WARP device creation: 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    tex.offset = 0x1000; tex.width = 2; tex.height = 2;
    tex.color = 0x0B; tex.levels = 1; tex.palette = 0x2000;
    a = get_texture(&tex, 40);
    if (!check_pixels(a, 0xFFCC2211u)) goto done;
    tex.palette = 0x3000;
    b = get_texture(&tex, 40);
    if (!check_pixels(b, 0xFF1144DDu)) goto done;
    tex.palette = 0x2000;
    if (get_texture(&tex, 40) != a || !check_pixels(a, 0xFFCC2211u)) {
        fprintf(stderr, "FAIL: first palette did not retain its cached resource\n");
        goto done;
    }
    tex.palette = 0x3000;
    if (get_texture(&tex, 40) != b || !check_pixels(b, 0xFF1144DDu)) {
        fprintf(stderr, "FAIL: second palette did not retain its cached resource\n");
        goto done;
    }
    passed = 1;
    puts("PASS: identical P8 indices with different palettes retain distinct GPU pixels");
done:
    release_all();
    return passed ? 0 : 1;
}
