/*
 * D3D11 rasteriser for the deferred NV2A queue.
 *
 * The executor still transforms and clips vertices, so this backend only has
 * to do what the software rasteriser's pixel half does: rasterise
 * surface-space triangles, sample up to four texture stages, apply the exact
 * register-combiner plans, depth/blend/alpha test, and copy the finished
 * 640x480 surface back to guest memory for the title's own presentation path.
 *
 * The software path remains the fallback for other surfaces, the generic
 * combiner interpreter and any device failure.
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kernel/nv2a_gpu.h"
#include "kernel/nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define GPU_W 640
#define GPU_H 480
#define GPU_TEX_CACHE 8192
#define GPU_BATCH_VERTS 8192
#define GPU_WHITE 0xFFFFFFFFu

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
    float uv[4][2];
} GpuVertex;

typedef struct {
    uint32_t offset, color, width, height, pitch, palette, palette_hash, levels;
    ID3D11ShaderResourceView *srv;
    uint32_t tex_w, tex_h;
    uint64_t last_used;
    uint32_t generation;
} GpuTextureEntry;

typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *color;
    ID3D11RenderTargetView *rtv;
    ID3D11Texture2D *depth;
    ID3D11DepthStencilView *dsv;
    ID3D11Texture2D *staging;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vs_cb;
    ID3D11Buffer *ps_cb;
    ID3D11Buffer *vb;
    ID3D11RasterizerState *raster;
    ID3D11SamplerState *samplers[4][2];   /* [wrap/clamp][linear/point] */
    ID3D11BlendState *blend[64];
    uint32_t blend_key[64];
    ID3D11DepthStencilState *depth_state[16];
    uint32_t depth_key[16];
    GpuTextureEntry textures[GPU_TEX_CACHE];
    Nv2aGpuDraw batch_key;
    Nv2aTexture batch_tex[4];
    Nv2aCombiner batch_combiner;
    Nv2aRcPlan batch_rc;
    Nv2aGpuVertex batch[GPU_BATCH_VERTS];
    uint32_t batch_count;
    int batch_active;
    ID3D11ShaderResourceView *white;
    uint32_t init_tried, init_ok;
    uint64_t serial;
    Nv2aSurface last_surface;
    uint32_t has_last_surface, dirty;
    uint32_t draws;
    uint32_t batch_flushes;
    uint64_t batch_vertices;
    LARGE_INTEGER perf_freq;
    LARGE_INTEGER perf_draw_ticks;
    LARGE_INTEGER perf_readback_ticks;
    LARGE_INTEGER perf_tex_ticks;
    LARGE_INTEGER perf_gpu_ticks;
    uint32_t perf_draws;
    uint32_t tex_hits;
    uint32_t tex_misses;
    uint32_t debug;
    uint32_t bg_frame;
    uint32_t log_frame;
    uint32_t log_count;
    uint32_t last_frame;
    uint32_t dumped_frame;
    uint32_t title_frame;
} GpuState;

static GpuState g_gpu;

static void gpu_flush_batch(void);

static const char *k_vs =
    "cbuffer VP : register(b0) { float2 invViewport; float depthScale; float depthBias; };\n"
    "struct VSIn { float3 pos : POSITION; float rhw : TEXCOORD0;"
    " float4 color : COLOR;"
    " float2 uv0 : TEXCOORD1; float2 uv1 : TEXCOORD2;"
    " float2 uv2 : TEXCOORD3; float2 uv3 : TEXCOORD4; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 color : COLOR;"
    " float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1;"
    " float2 uv2 : TEXCOORD2; float2 uv3 : TEXCOORD3; };\n"
    "VSOut main(VSIn i) {\n"
    "  VSOut o;\n"
    "  float w = i.rhw != 0.0 ? 1.0 / i.rhw : 1.0;\n"
    "  float2 ndc = float2(i.pos.x * invViewport.x - 1.0,"
    " 1.0 - i.pos.y * invViewport.y);\n"
    "  float z = saturate(i.pos.z * depthScale + depthBias);\n"
    "  o.pos = float4(ndc.x * w, ndc.y * w, z * w, w);\n"
    "  o.color = i.color;\n"
    "  o.uv0 = i.uv0; o.uv1 = i.uv1; o.uv2 = i.uv2; o.uv3 = i.uv3;\n"
    "  return o;\n"
    "}\n";

static const char *k_ps =
    "Texture2D t0 : register(t0);\n"
    "Texture2D t1 : register(t1);\n"
    "Texture2D t2 : register(t2);\n"
    "Texture2D t3 : register(t3);\n"
    "SamplerState s0 : register(s0);\n"
    "SamplerState s1 : register(s1);\n"
    "SamplerState s2 : register(s2);\n"
    "SamplerState s3 : register(s3);\n"
    "cbuffer PP : register(b1) {\n"
    "  float2 texSize0; float2 texSize1; float2 texSize2; float2 texSize3;\n"
    "  float plan; float alphaTest; float alphaFunc; float alphaRef;\n"
    "  float4 c0; float c1a; float3 pad;\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; float4 color : COLOR;"
    " float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1;"
    " float2 uv2 : TEXCOORD2; float2 uv3 : TEXCOORD3; };\n"
    "float4 main(VSOut i) : SV_Target {\n"
    "  float4 v0 = i.color;\n"
    "  float4 T0 = t0.Sample(s0, i.uv0 / texSize0);\n"
    "  float4 T1 = t1.Sample(s1, i.uv1 / texSize1);\n"
    "  float4 T2 = t2.Sample(s2, i.uv2 / texSize2);\n"
    "  float4 o;\n"
    "  if (plan < 1.5) {\n"
    "    o = float4(saturate(v0.rgb * T0.rgb * 2.0), saturate(v0.a * T0.a));\n"
    "  } else if (plan < 2.5) {\n"
    "    o = float4(saturate(v0.rgb * T0.rgb), saturate(v0.a));\n"
    "  } else if (plan < 3.5) {\n"
    "    o = saturate(v0);\n"
    "  } else if (plan < 4.5) {\n"
    "    float w0 = clamp((T1.a * c0.a) * 2.0, -1.0, 1.0);\n"
    "    float w1 = clamp((T1.a * c1a) * 2.0, -1.0, 1.0);\n"
    "    float3 a = saturate(T2.rgb * v0.rgb * 2.0);\n"
    "    float3 b = saturate(T0.rgb * c0.rgb * 2.0);\n"
    "    float3 d = clamp(b - a, -1.0, 1.0);\n"
    "    float3 val = clamp(d * w0 + a, -1.0, 1.0);\n"
    "    val = clamp(d * w1 + val, -1.0, 1.0);\n"
    "    o = float4(saturate(val), saturate(T2.a * v0.a));\n"
    "  } else if (plan < 5.5) {\n"
    "    float w0 = clamp((T1.a * c0.a) * 2.0, -1.0, 1.0);\n"
    "    float w1 = clamp((T1.a * c1a) * 2.0, -1.0, 1.0);\n"
    "    float3 a = saturate(T0.rgb * c0.rgb * 2.0);\n"
    "    float3 b = saturate(T2.rgb * v0.rgb * 2.0);\n"
    "    float3 val = clamp(a * w0 + b, -1.0, 1.0);\n"
    "    val = clamp(a * w1 + val, -1.0, 1.0);\n"
    "    o = float4(saturate(val), saturate(T2.a * v0.a));\n"
    "  } else if (plan < 6.5) {\n"
    "    o = float4(saturate(T0.rgb * c0.rgb), saturate(T1.a * c0.a));\n"
    "  } else if (plan < 7.5) {\n"
    "    o = float4(saturate(v0.rgb * T0.rgb), saturate(v0.a * T0.a));\n"
    "  } else if (plan < 8.5) {\n"
    "    o = float4(saturate(saturate(v0.rgb) * saturate(T0.rgb) + saturate(v0.a)),"
    " saturate(T0.a));\n"
    "  } else {\n"
    "    o = saturate(c0);\n"
    "  }\n"
    "  if (alphaTest > 0.5) {\n"
    "    float a = o.a * 255.0;\n"
    "    bool ok = true;\n"
    "    if (alphaFunc < 0.5) ok = false;\n"
    "    else if (alphaFunc < 1.5) ok = a < alphaRef;\n"
    "    else if (alphaFunc < 2.5) ok = a == alphaRef;\n"
    "    else if (alphaFunc < 3.5) ok = a <= alphaRef;\n"
    "    else if (alphaFunc < 4.5) ok = a > alphaRef;\n"
    "    else if (alphaFunc < 5.5) ok = a != alphaRef;\n"
    "    else if (alphaFunc < 6.5) ok = a >= alphaRef;\n"
    "    if (!ok) discard;\n"
    "  }\n"
    "  return o;\n"
    "}\n";

static void release_all(void)
{
    uint32_t i;
    int j;
    if (g_gpu.white) { g_gpu.white->lpVtbl->Release(g_gpu.white); g_gpu.white = NULL; }
    for (i = 0; i < GPU_TEX_CACHE; i++)
        if (g_gpu.textures[i].srv) {
            g_gpu.textures[i].srv->lpVtbl->Release(g_gpu.textures[i].srv);
            g_gpu.textures[i].srv = NULL;
        }
    for (i = 0; i < 64; i++)
        if (g_gpu.blend[i]) { g_gpu.blend[i]->lpVtbl->Release(g_gpu.blend[i]); g_gpu.blend[i] = NULL; }
    for (i = 0; i < 16; i++)
        if (g_gpu.depth_state[i]) { g_gpu.depth_state[i]->lpVtbl->Release(g_gpu.depth_state[i]); g_gpu.depth_state[i] = NULL; }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 2; j++)
            if (g_gpu.samplers[i][j]) { g_gpu.samplers[i][j]->lpVtbl->Release(g_gpu.samplers[i][j]); g_gpu.samplers[i][j] = NULL; }
    if (g_gpu.raster) { g_gpu.raster->lpVtbl->Release(g_gpu.raster); g_gpu.raster = NULL; }
    if (g_gpu.vb) { g_gpu.vb->lpVtbl->Release(g_gpu.vb); g_gpu.vb = NULL; }
    if (g_gpu.vs_cb) { g_gpu.vs_cb->lpVtbl->Release(g_gpu.vs_cb); g_gpu.vs_cb = NULL; }
    if (g_gpu.ps_cb) { g_gpu.ps_cb->lpVtbl->Release(g_gpu.ps_cb); g_gpu.ps_cb = NULL; }
    if (g_gpu.layout) { g_gpu.layout->lpVtbl->Release(g_gpu.layout); g_gpu.layout = NULL; }
    if (g_gpu.ps) { g_gpu.ps->lpVtbl->Release(g_gpu.ps); g_gpu.ps = NULL; }
    if (g_gpu.vs) { g_gpu.vs->lpVtbl->Release(g_gpu.vs); g_gpu.vs = NULL; }
    if (g_gpu.staging) { g_gpu.staging->lpVtbl->Release(g_gpu.staging); g_gpu.staging = NULL; }
    if (g_gpu.dsv) { g_gpu.dsv->lpVtbl->Release(g_gpu.dsv); g_gpu.dsv = NULL; }
    if (g_gpu.depth) { g_gpu.depth->lpVtbl->Release(g_gpu.depth); g_gpu.depth = NULL; }
    if (g_gpu.rtv) { g_gpu.rtv->lpVtbl->Release(g_gpu.rtv); g_gpu.rtv = NULL; }
    if (g_gpu.color) { g_gpu.color->lpVtbl->Release(g_gpu.color); g_gpu.color = NULL; }
    if (g_gpu.context) { g_gpu.context->lpVtbl->Release(g_gpu.context); g_gpu.context = NULL; }
    if (g_gpu.device) { g_gpu.device->lpVtbl->Release(g_gpu.device); g_gpu.device = NULL; }
    memset(&g_gpu, 0, sizeof g_gpu);
}

static int compile_shader(const char *src, const char *entry, const char *target,
                          ID3DBlob **blob)
{
    ID3DBlob *error = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &error);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] shader %s failed: %s\n", target,
                error ? (const char *)error->lpVtbl->GetBufferPointer(error)
                      : "unknown");
        if (error) error->lpVtbl->Release(error);
        return 0;
    }
    if (error) error->lpVtbl->Release(error);
    return 1;
}

static int gpu_init(void)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC raster;
    D3D11_INPUT_ELEMENT_DESC elems[7];
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr;
    int i, j;

    if (g_gpu.init_tried)
        return g_gpu.init_ok;
    g_gpu.init_tried = 1;
    g_gpu.debug = getenv("RECOMP_GPU_DEBUG") != NULL;
    QueryPerformanceFrequency(&g_gpu.perf_freq);

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, 0,
                           D3D11_SDK_VERSION, &g_gpu.device, &level,
                           &g_gpu.context);
    if (SUCCEEDED(hr)) {
        IDXGIDevice *dxdev = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(g_gpu.device->lpVtbl->QueryInterface(g_gpu.device, &IID_IDXGIDevice, (void **)&dxdev)) && dxdev) {
            if (SUCCEEDED(dxdev->lpVtbl->GetAdapter(dxdev, &adapter)) && adapter) {
                if (SUCCEEDED(adapter->lpVtbl->GetDesc(adapter, &desc)))
                    fprintf(stderr, "[GPU] adapter: %ls (vendor 0x%04X device 0x%04X)\n", desc.Description, desc.VendorId, desc.DeviceId);
                adapter->lpVtbl->Release(adapter);
            }
            dxdev->lpVtbl->Release(dxdev);
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] D3D11CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
        release_all();
        return 0;
    }

    memset(&td, 0, sizeof td);
    td.Width = GPU_W; td.Height = GPU_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td, NULL, &g_gpu.color)))
        goto fail;

    memset(&rd, 0, sizeof rd);
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_gpu.device->lpVtbl->CreateRenderTargetView(g_gpu.device,
            (ID3D11Resource *)g_gpu.color, &rd, &g_gpu.rtv)))
        goto fail;

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td, NULL, &g_gpu.depth)))
        goto fail;
    memset(&dd, 0, sizeof dd);
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(g_gpu.device->lpVtbl->CreateDepthStencilView(g_gpu.device,
            (ID3D11Resource *)g_gpu.depth, &dd, &g_gpu.dsv)))
        goto fail;

    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td, NULL, &g_gpu.staging)))
        goto fail;

    if (!compile_shader(k_vs, "main", "vs_5_0", &vsb) ||
        !compile_shader(k_ps, "main", "ps_5_0", &psb))
        goto fail;
    if (FAILED(g_gpu.device->lpVtbl->CreateVertexShader(g_gpu.device,
            vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb),
            NULL, &g_gpu.vs)))
        goto fail;
    if (FAILED(g_gpu.device->lpVtbl->CreatePixelShader(g_gpu.device,
            psb->lpVtbl->GetBufferPointer(psb), psb->lpVtbl->GetBufferSize(psb),
            NULL, &g_gpu.ps)))
        goto fail;

    elems[0].SemanticName = "POSITION"; elems[0].SemanticIndex = 0;
    elems[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; elems[0].InputSlot = 0;
    elems[0].AlignedByteOffset = 0; elems[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elems[0].InstanceDataStepRate = 0;
    elems[1].SemanticName = "TEXCOORD"; elems[1].SemanticIndex = 0;
    elems[1].Format = DXGI_FORMAT_R32_FLOAT; elems[1].InputSlot = 0;
    elems[1].AlignedByteOffset = 12; elems[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elems[1].InstanceDataStepRate = 0;
    elems[2].SemanticName = "COLOR"; elems[2].SemanticIndex = 0;
    elems[2].Format = DXGI_FORMAT_B8G8R8A8_UNORM; elems[2].InputSlot = 0;
    elems[2].AlignedByteOffset = 16; elems[2].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elems[2].InstanceDataStepRate = 0;
    for (i = 0; i < 4; i++) {
        elems[3 + i].SemanticName = "TEXCOORD";
        elems[3 + i].SemanticIndex = 1 + i;
        elems[3 + i].Format = DXGI_FORMAT_R32G32_FLOAT; elems[3 + i].InputSlot = 0;
        elems[3 + i].AlignedByteOffset = 20 + i * 8;
        elems[3 + i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[3 + i].InstanceDataStepRate = 0;
    }
    if (FAILED(g_gpu.device->lpVtbl->CreateInputLayout(g_gpu.device, elems, 7,
            vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb),
            &g_gpu.layout)))
        goto fail;

    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_gpu.device->lpVtbl->CreateBuffer(g_gpu.device, &bd, NULL, &g_gpu.vs_cb)))
        goto fail;
    bd.ByteWidth = 96;
    if (FAILED(g_gpu.device->lpVtbl->CreateBuffer(g_gpu.device, &bd, NULL, &g_gpu.ps_cb)))
        goto fail;

    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(GpuVertex) * GPU_BATCH_VERTS;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_gpu.device->lpVtbl->CreateBuffer(g_gpu.device, &bd, NULL, &g_gpu.vb)))
        goto fail;

    memset(&raster, 0, sizeof raster);
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.FrontCounterClockwise = FALSE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(g_gpu.device->lpVtbl->CreateRasterizerState(g_gpu.device, &raster, &g_gpu.raster)))
        goto fail;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            memset(&sd, 0, sizeof sd);
            sd.Filter = j ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = i ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            sd.AddressV = sd.AddressU;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(g_gpu.device->lpVtbl->CreateSamplerState(g_gpu.device, &sd, &g_gpu.samplers[i][j])))
                goto fail;
        }
    }

    {
        D3D11_SUBRESOURCE_DATA data;
        D3D11_TEXTURE2D_DESC wd;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv;
        ID3D11Texture2D *wt = NULL;
        uint32_t white = GPU_WHITE;
        memset(&wd, 0, sizeof wd);
        wd.Width = 1; wd.Height = 1; wd.MipLevels = 1; wd.ArraySize = 1;
        wd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        wd.SampleDesc.Count = 1;
        wd.Usage = D3D11_USAGE_IMMUTABLE;
        wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        memset(&data, 0, sizeof data);
        data.pSysMem = &white; data.SysMemPitch = 4;
        if (SUCCEEDED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &wd, &data, &wt))) {
            memset(&srv, 0, sizeof srv);
            srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            g_gpu.device->lpVtbl->CreateShaderResourceView(g_gpu.device,
                (ID3D11Resource *)wt, &srv, &g_gpu.white);
            wt->lpVtbl->Release(wt);
        }
    }

    if (vsb) vsb->lpVtbl->Release(vsb);
    if (psb) psb->lpVtbl->Release(psb);
    g_gpu.init_ok = 1;
    fprintf(stderr, "[GPU] D3D11 rasteriser initialised (feature level 0x%X)\n", level);
    return 1;

fail:
    fprintf(stderr, "[GPU] D3D11 rasteriser init failed: 0x%08lX\n", (unsigned long)hr);
    if (vsb) vsb->lpVtbl->Release(vsb);
    if (psb) psb->lpVtbl->Release(psb);
    release_all();
    return 0;
}

static uint32_t gl_blend(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return D3D11_BLEND_SRC_COLOR;
    case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D11_BLEND_DEST_COLOR;
    case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x8001: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *rs, uint32_t color_mask)
{
    D3D11_BLEND_DESC bd;
    uint32_t key, i;
    memset(&bd, 0, sizeof bd);
    bd.AlphaToCoverageEnable = FALSE;
    bd.IndependentBlendEnable = FALSE;
    bd.RenderTarget[0].BlendEnable = rs->blend_enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = gl_blend(rs->blend_src);
    bd.RenderTarget[0].DestBlend = gl_blend(rs->blend_dst);
    bd.RenderTarget[0].BlendOp = rs->blend_eq == 0x800A ? D3D11_BLEND_OP_SUBTRACT
                                 : rs->blend_eq == 0x800B ? D3D11_BLEND_OP_REV_SUBTRACT
                                 : rs->blend_eq == 0x8007 ? D3D11_BLEND_OP_MIN
                                 : rs->blend_eq == 0x8008 ? D3D11_BLEND_OP_MAX
                                 : D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].SrcBlend;
    bd.RenderTarget[0].DestBlendAlpha = bd.RenderTarget[0].DestBlend;
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    if (color_mask & 0x00010000u) bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (color_mask & 0x00000100u) bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (color_mask & 0x00000001u) bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (color_mask & 0x01000000u) bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

    /* Pack the native descriptor fields, not the sparse guest masks/enums.
     * enable: 1 bit; each factor: 5 bits; op: 3 bits; write mask: 4 bits.
     * Alpha factors/operation match color; the other descriptor fields are fixed. */
    key = (uint32_t)bd.RenderTarget[0].BlendEnable
        | ((uint32_t)bd.RenderTarget[0].SrcBlend << 1)
        | ((uint32_t)bd.RenderTarget[0].DestBlend << 6)
        | ((uint32_t)bd.RenderTarget[0].BlendOp << 11)
        | ((uint32_t)bd.RenderTarget[0].RenderTargetWriteMask << 14);
    for (i = 0; i < 64; i++)
        if (g_gpu.blend[i] && g_gpu.blend_key[i] == key)
            return g_gpu.blend[i];
    for (i = 0; i < 64; i++)
        if (!g_gpu.blend[i]) break;
    if (i == 64) i = 0;
    if (g_gpu.blend[i]) { g_gpu.blend[i]->lpVtbl->Release(g_gpu.blend[i]); g_gpu.blend[i] = NULL; }
    if (FAILED(g_gpu.device->lpVtbl->CreateBlendState(g_gpu.device, &bd, &g_gpu.blend[i])))
        return NULL;
    g_gpu.blend_key[i] = key;
    return g_gpu.blend[i];
}

static D3D11_COMPARISON_FUNC d3d_compare(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP d3d_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *get_depth(const Nv2aRenderState *rs)
{
    uint32_t key = (rs->depth_test_enable ? 1u : 0u) | (rs->depth_write ? 2u : 0u)
                 | ((rs->depth_func & 0xF) << 4);
#define MIX_STENCIL(v) key = (key ^ (uint32_t)(v)) * 16777619u
    MIX_STENCIL(rs->stencil_enable);
    MIX_STENCIL(rs->stencil_func);
    MIX_STENCIL(rs->stencil_ref);
    MIX_STENCIL(rs->stencil_func_mask);
    MIX_STENCIL(rs->stencil_write_mask);
    MIX_STENCIL(rs->stencil_fail);
    MIX_STENCIL(rs->stencil_zfail);
    MIX_STENCIL(rs->stencil_zpass);
#undef MIX_STENCIL
    uint32_t i;
    for (i = 0; i < 16; i++)
        if (g_gpu.depth_state[i] && g_gpu.depth_key[i] == key)
            return g_gpu.depth_state[i];
    for (i = 0; i < 16; i++)
        if (!g_gpu.depth_state[i]) break;
    if (i == 16) i = 0;
    if (g_gpu.depth_state[i]) { g_gpu.depth_state[i]->lpVtbl->Release(g_gpu.depth_state[i]); g_gpu.depth_state[i] = NULL; }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = rs->depth_test_enable ? TRUE : FALSE;
        dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = d3d_compare(rs->depth_func);
        dd.StencilEnable = rs->stencil_enable ? TRUE : FALSE;
        dd.StencilReadMask = (UINT8)(rs->stencil_func_mask & 0xFF);
        dd.StencilWriteMask = (UINT8)(rs->stencil_write_mask & 0xFF);
        dd.FrontFace.StencilFailOp = d3d_stencil_op(rs->stencil_fail);
        dd.FrontFace.StencilDepthFailOp = d3d_stencil_op(rs->stencil_zfail);
        dd.FrontFace.StencilPassOp = d3d_stencil_op(rs->stencil_zpass);
        dd.FrontFace.StencilFunc = d3d_compare(rs->stencil_func);
        dd.BackFace = dd.FrontFace;
        if (FAILED(g_gpu.device->lpVtbl->CreateDepthStencilState(g_gpu.device, &dd, &g_gpu.depth_state[i])))
            return NULL;
        g_gpu.depth_key[i] = key;
    }
    return g_gpu.depth_state[i];
}

static uint32_t texture_palette_hash(const Nv2aTexture *t)
{
    const uint32_t *p;
    uint32_t h = 2166136261u, i;
    if (!t || t->color != 0x0Bu || !t->palette || !xbox_GetMemoryOffset())
        return 0;
    p = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + t->palette);
    for (i = 0; i < 256; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static ID3D11ShaderResourceView *get_texture(const Nv2aTexture *t, uint32_t frame)
{
    uint64_t key;
    uint32_t i, slot = 0;
    uint32_t *argb;
    uint64_t oldest = ~0ull;
    static int static_tex = -1, static_p8 = -1;
    uint32_t generation;
    if (static_tex < 0)
        static_tex = getenv("RECOMP_GPU_TEX_STATIC") != NULL;
    if (static_p8 < 0)
        static_p8 = getenv("RECOMP_GPU_P8_STATIC") != NULL;
    generation = (t && t->color == 0x0Bu)
               ? (static_p8 ? 0u : frame)
               : (static_tex ? 0u : frame / 8);
    uint32_t palette_hash = texture_palette_hash(t);

    if (!t || !t->offset || !t->width || !t->height)
        return g_gpu.white;
    key = ((uint64_t)t->offset << 32) ^ t->color ^ ((uint64_t)t->width << 8)
        ^ ((uint64_t)t->height << 20) ^ ((uint64_t)t->pitch << 4)
        ^ ((uint64_t)t->levels << 40) ^ ((uint64_t)t->filter << 48);
    (void)key;
    for (i = 0; i < GPU_TEX_CACHE; i++) {
        GpuTextureEntry *e = &g_gpu.textures[i];
        if (e->srv && e->generation == generation
            && e->offset == t->offset && e->color == t->color
            && e->width == t->width && e->height == t->height
            && e->pitch == t->pitch && e->levels == t->levels
            && e->palette == t->palette
            && (e->palette_hash == palette_hash
                || (t->color == 0x0Bu && e->generation == generation))) {
            g_gpu.tex_hits++;
            return e->srv;
        }
        if (!e->srv) { slot = i; break; }
        if (e->last_used < oldest) { oldest = e->last_used; slot = i; }
    }
    if (g_gpu.textures[slot].srv) {
        g_gpu.textures[slot].srv->lpVtbl->Release(g_gpu.textures[slot].srv);
        memset(&g_gpu.textures[slot], 0, sizeof g_gpu.textures[slot]);
    }
    g_gpu.tex_misses++;
    argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!argb)
        return g_gpu.white;
    if (!nv2a_backend_decode_texture(t, argb)) {
        free(argb);
        return g_gpu.white;
    }
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA data;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv;
        ID3D11Texture2D *texture = NULL;
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = 1;
        td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        memset(&data, 0, sizeof data);
        data.pSysMem = argb; data.SysMemPitch = t->width * 4;
        if (FAILED(g_gpu.device->lpVtbl->CreateTexture2D(g_gpu.device, &td, &data, &texture))) {
            free(argb);
            return g_gpu.white;
        }
        memset(&srv, 0, sizeof srv);
        srv.Format = td.Format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(g_gpu.device->lpVtbl->CreateShaderResourceView(g_gpu.device,
                (ID3D11Resource *)texture, &srv, &g_gpu.textures[slot].srv))) {
            texture->lpVtbl->Release(texture);
            free(argb);
            return g_gpu.white;
        }
        texture->lpVtbl->Release(texture);
    }
    if (getenv("RECOMP_GPU_DUMP_TEX")) {
        char path[160];
        FILE *fp;
        uint32_t y, x;
        sprintf(path, "output/gputex_%08X.ppm", t->offset);
        fp = fopen(path, "wb");
        if (fp) {
            fprintf(fp, "P6\n%u %u\n255\n", t->width, t->height);
            for (y = 0; y < t->height; y++)
                for (x = 0; x < t->width; x++) {
                    uint32_t c = argb[(size_t)y * t->width + x];
                    fputc((c >> 16) & 0xFF, fp);
                    fputc((c >> 8) & 0xFF, fp);
                    fputc(c & 0xFF, fp);
                }
            fclose(fp);
            sprintf(path, "output/gputex_%08X_alpha.pgm", t->offset);
            fp = fopen(path, "wb");
            if (fp) {
                fprintf(fp, "P5\n%u %u\n255\n", t->width, t->height);
                for (y = 0; y < t->height; y++)
                    for (x = 0; x < t->width; x++)
                        fputc((argb[(size_t)y * t->width + x] >> 24) & 0xFF, fp);
                fclose(fp);
            }
        }
    }
    free(argb);
    g_gpu.textures[slot].offset = t->offset;
    g_gpu.textures[slot].color = t->color;
    g_gpu.textures[slot].width = t->width;
    g_gpu.textures[slot].height = t->height;
    g_gpu.textures[slot].pitch = t->pitch;
    g_gpu.textures[slot].levels = t->levels;
    g_gpu.textures[slot].palette = t->palette;
    g_gpu.textures[slot].palette_hash = palette_hash;
    g_gpu.textures[slot].tex_w = t->width;
    g_gpu.textures[slot].tex_h = t->height;
    g_gpu.textures[slot].generation = generation;
    g_gpu.textures[slot].last_used = ++g_gpu.serial;
    return g_gpu.textures[slot].srv;
}

static uint32_t alpha_func_index(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207) return f - 0x200;
    return 7;
}

static void bind_sampler(uint32_t slot, const Nv2aTexture *t)
{
    uint32_t wrap = (t && t->addr_u == 1) ? 0 : 1;
    uint32_t linear = 0;
    if (t) {
        uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
        linear = (mag == 2 || min == 2 || min == 4 || min == 6);
    }
    g_gpu.context->lpVtbl->PSSetSamplers(g_gpu.context, slot, 1,
        &g_gpu.samplers[wrap][linear ? 0 : 1]);
}

static void gpu_draw_one_inner(const Nv2aGpuDraw *d)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float vscb[4];
    float pscb[24];
    const GpuVertex *verts = (const GpuVertex *)d->vertices;
    memset(pscb, 0, sizeof pscb);
    ID3D11ShaderResourceView *srvs[4];
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *ds;
    ID3D11Buffer *cbs[1];
    UINT stride = sizeof(GpuVertex), offset = 0;
    uint32_t i, st;
    float f[4];

    if (!g_gpu.init_ok && !gpu_init())
        return;
    if (d->surface.width != GPU_W || d->surface.height != GPU_H
        || d->surface.bytes_per_pixel != 4)
        return;

    g_gpu.context->lpVtbl->OMSetRenderTargets(g_gpu.context, 1, &g_gpu.rtv, g_gpu.dsv);
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = GPU_W; vp.Height = GPU_H;
        vp.MinDepth = 0; vp.MaxDepth = 1;
        g_gpu.context->lpVtbl->RSSetViewports(g_gpu.context, 1, &vp);
    }
    g_gpu.context->lpVtbl->RSSetState(g_gpu.context, g_gpu.raster);

    if (FAILED(g_gpu.context->lpVtbl->Map(g_gpu.context, (ID3D11Resource *)g_gpu.vb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    memcpy(map.pData, verts, (size_t)d->count * sizeof(GpuVertex));
    g_gpu.context->lpVtbl->Unmap(g_gpu.context, (ID3D11Resource *)g_gpu.vb, 0);

    vscb[0] = 2.0f / (float)GPU_W;
    vscb[1] = 2.0f / (float)GPU_H;
    if (d->depth_max > d->depth_min) {
        vscb[2] = 1.0f / (d->depth_max - d->depth_min);
        vscb[3] = -d->depth_min / (d->depth_max - d->depth_min);
    } else {
        vscb[2] = 1.0f;
        vscb[3] = 0.0f;
    }
    if (FAILED(g_gpu.context->lpVtbl->Map(g_gpu.context, (ID3D11Resource *)g_gpu.vs_cb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    memcpy(map.pData, vscb, sizeof vscb);
    g_gpu.context->lpVtbl->Unmap(g_gpu.context, (ID3D11Resource *)g_gpu.vs_cb, 0);

    for (st = 0; st < 4; st++) {
        pscb[st * 2] = d->tex_width[st] > 0.0f ? d->tex_width[st] : 1.0f;
        pscb[st * 2 + 1] = d->tex_height[st] > 0.0f ? d->tex_height[st] : 1.0f;
    }
    pscb[8] = (float)d->plan;
    pscb[9] = d->alpha_test ? 1.0f : 0.0f;
    pscb[10] = (float)alpha_func_index(d->alpha_func);
    pscb[11] = (float)(d->alpha_ref & 0xFF);
    if (d->rc_plan) {
        /* Shader colors are RGBA; the resource formats handle BGRA storage. */
        pscb[12] = d->rc_plan->six_c0[0];
        pscb[13] = d->rc_plan->six_c0[1];
        pscb[14] = d->rc_plan->six_c0[2];
        pscb[15] = d->rc_plan->six_c0[3];
        pscb[16] = d->rc_plan->six_c1_alpha;
    } else {
        pscb[12] = pscb[13] = pscb[14] = pscb[15] = 0.0f;
        pscb[16] = 0.0f;
    }
    pscb[17] = pscb[18] = pscb[19] = 0.0f;
    if (FAILED(g_gpu.context->lpVtbl->Map(g_gpu.context, (ID3D11Resource *)g_gpu.ps_cb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    memcpy(map.pData, pscb, sizeof pscb);
    g_gpu.context->lpVtbl->Unmap(g_gpu.context, (ID3D11Resource *)g_gpu.ps_cb, 0);

    {
        LARGE_INTEGER ta, tb;
        QueryPerformanceCounter(&ta);
        for (st = 0; st < 4; st++) {
            srvs[st] = get_texture(d->textures[st], d->frame);
            bind_sampler(st, d->textures[st]);
        }
        QueryPerformanceCounter(&tb);
        if (g_gpu.perf_freq.QuadPart)
            g_gpu.perf_tex_ticks.QuadPart += tb.QuadPart - ta.QuadPart;
    }
    g_gpu.context->lpVtbl->PSSetShaderResources(g_gpu.context, 0, 4, srvs);

    blend = get_blend(&d->state, d->state.color_mask);
    ds = get_depth(&d->state);
    if (g_gpu.debug && d->plan == 1 && d->state.blend_enable
        && d->state.blend_src == 0x0305 && d->state.blend_dst == 0x0304)
        fprintf(stderr, "[GPU] BG f=%u draw=%u tex=%08X bbox=%.0f,%.0f-%.0f,%.0f "
                "uv=%.2f,%.2f z=%u/%u %04X\n",
                d->frame, g_gpu.draws,
                d->textures[0] ? d->textures[0]->offset : 0,
                verts[0].x, verts[0].y, verts[2].x, verts[2].y,
                verts[0].uv[0][0], verts[0].uv[0][1],
                d->state.depth_test_enable, d->state.depth_write,
                d->state.depth_func);
    if (getenv("RECOMP_GPU_BG_TEST") && d->plan == 1
        && d->state.blend_src == 0x0305 && d->state.blend_dst == 0x0304) {
        blend = NULL;
        ds = NULL;
    }
    f[0] = (float)((d->state.blend_color >> 16) & 0xFF) / 255.0f;
    f[1] = (float)((d->state.blend_color >> 8) & 0xFF) / 255.0f;
    f[2] = (float)(d->state.blend_color & 0xFF) / 255.0f;
    f[3] = (float)((d->state.blend_color >> 24) & 0xFF) / 255.0f;
    g_gpu.context->lpVtbl->OMSetBlendState(g_gpu.context, blend, f, 0xFFFFFFFFu);
    g_gpu.context->lpVtbl->OMSetDepthStencilState(g_gpu.context, ds,
        (UINT)(d->state.stencil_ref & 0xFF));

    g_gpu.context->lpVtbl->IASetInputLayout(g_gpu.context, g_gpu.layout);
    g_gpu.context->lpVtbl->IASetVertexBuffers(g_gpu.context, 0, 1, &g_gpu.vb, &stride, &offset);
    g_gpu.context->lpVtbl->IASetPrimitiveTopology(g_gpu.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_gpu.context->lpVtbl->VSSetShader(g_gpu.context, g_gpu.vs, NULL, 0);
    cbs[0] = g_gpu.vs_cb;
    g_gpu.context->lpVtbl->VSSetConstantBuffers(g_gpu.context, 0, 1, cbs);
    g_gpu.context->lpVtbl->PSSetShader(g_gpu.context, g_gpu.ps, NULL, 0);
    cbs[0] = g_gpu.ps_cb;
    /* The pixel shader declares its constants at b1; the vertex shader's b0
     * is a different stage, so both may use slot 0 in principle, but the
     * shader explicitly names b1 and an unbound b1 reads as zero -- which
     * silently turned every plan into the first branch. */
    g_gpu.context->lpVtbl->PSSetConstantBuffers(g_gpu.context, 1, 1, cbs);
    {
        LARGE_INTEGER ga, gb;
        QueryPerformanceCounter(&ga);
        g_gpu.context->lpVtbl->Draw(g_gpu.context, d->count, 0);
        QueryPerformanceCounter(&gb);
        if (g_gpu.perf_freq.QuadPart)
            g_gpu.perf_gpu_ticks.QuadPart += gb.QuadPart - ga.QuadPart;
    }

    if (d->plan == 5 && g_gpu.title_frame == 0)
        g_gpu.title_frame = d->frame;
    g_gpu.last_surface = d->surface;
    g_gpu.last_frame = d->frame;
    g_gpu.has_last_surface = 1;
    g_gpu.dirty = 1;
    g_gpu.draws++;
    if (g_gpu.debug && (g_gpu.draws % 100u) == 0u)
        fprintf(stderr, "[GPU] heartbeat draw=%u frame=%u plan=%u surf=%08X\n",
                g_gpu.draws, d->frame, d->plan, d->surface.color_va);
    if (g_gpu.debug && d->plan == 5 && g_gpu.log_frame == 0)
        g_gpu.log_frame = d->frame;
    if (g_gpu.debug && g_gpu.log_frame && d->frame == g_gpu.log_frame
        && g_gpu.log_count < 400) {
        g_gpu.log_count++;
        float minx = verts[0].x, maxx = verts[0].x;
        float miny = verts[0].y, maxy = verts[0].y;
        for (i = 1; i < 3; i++) {
            if (verts[i].x < minx) minx = verts[i].x;
            if (verts[i].x > maxx) maxx = verts[i].x;
            if (verts[i].y < miny) miny = verts[i].y;
            if (verts[i].y > maxy) maxy = verts[i].y;
        }
        fprintf(stderr, "[GPU] f=%u draw=%u plan=%u surf=%08X tex0=%08X "
                "tex1=%08X tex2=%08X bbox=%.0f,%.0f-%.0f,%.0f "
                "uv0=%.2f,%.2f uv1=%.2f,%.2f uv2=%.2f,%.2f "
                "sz2=%.0fx%.0f c0=%.3f,%.3f,%.3f,%.3f d=%08X "
                "bl=%u %04X/%04X eq=%04X z=%u/%u %04X cm=%08X\n",
                d->frame, g_gpu.draws, d->plan, d->surface.color_va,
                d->textures[0] ? d->textures[0]->offset : 0,
                d->textures[1] ? d->textures[1]->offset : 0,
                d->textures[2] ? d->textures[2]->offset : 0,
                minx, miny, maxx, maxy,
                verts[0].uv[0][0], verts[0].uv[0][1],
                verts[0].uv[1][0], verts[0].uv[1][1],
                verts[0].uv[2][0], verts[0].uv[2][1],
                d->tex_width[2], d->tex_height[2],
                d->rc_plan ? d->rc_plan->six_c0[0] : 0.0f,
                d->rc_plan ? d->rc_plan->six_c0[1] : 0.0f,
                d->rc_plan ? d->rc_plan->six_c0[2] : 0.0f,
                d->rc_plan ? d->rc_plan->six_c0[3] : 0.0f,
                verts[0].diffuse,
                d->state.blend_enable, d->state.blend_src, d->state.blend_dst,
                d->state.blend_eq, d->state.depth_test_enable,
                d->state.depth_write, d->state.depth_func,
                d->state.color_mask);
    }
}

static void gpu_sync(const Nv2aSurface *s)
{
    const uint8_t *src;
    if (!g_gpu.init_ok && !gpu_init())
        return;
    gpu_flush_batch();
    if (!s || s->width != GPU_W || s->height != GPU_H
        || s->bytes_per_pixel != 4 || !s->pitch)
        return;
    src = (const uint8_t *)xbox_GetMemoryOffset() + s->color_va;
    if (s->pitch == GPU_W * 4) {
        g_gpu.context->lpVtbl->UpdateSubresource(g_gpu.context,
            (ID3D11Resource *)g_gpu.color, 0, NULL, src, s->pitch, 0);
    } else {
        uint8_t *tmp = (uint8_t *)malloc((size_t)GPU_W * GPU_H * 4);
        uint32_t y;
        if (!tmp)
            return;
        for (y = 0; y < GPU_H; y++)
            memcpy(tmp + (size_t)y * GPU_W * 4, src + (size_t)y * s->pitch,
                   GPU_W * 4);
        g_gpu.context->lpVtbl->UpdateSubresource(g_gpu.context,
            (ID3D11Resource *)g_gpu.color, 0, NULL, tmp, GPU_W * 4, 0);
        free(tmp);
    }
    g_gpu.last_surface = *s;
    g_gpu.has_last_surface = 1;
    g_gpu.dirty = 0;
}

static void gpu_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    if (!g_gpu.init_ok && !gpu_init())
        return;
    gpu_flush_batch();
    if (!s || s->width != GPU_W || s->height != GPU_H || s->bytes_per_pixel != 4)
        return;
    if (flags & 0xF0) {
        float c[4];
        c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (float)(argb & 0xFF) / 255.0f;
        c[3] = (float)((argb >> 24) & 0xFF) / 255.0f;
        g_gpu.context->lpVtbl->ClearRenderTargetView(g_gpu.context, g_gpu.rtv, c);
    }
    if (flags & 0x3) {
        float z = (float)((zstencil >> 8) & 0xFFFFFFu) / 16777215.0f;
        UINT8 s = (UINT8)(zstencil & 0xFF);
        UINT clear = 0;
        if (flags & 0x1) clear |= D3D11_CLEAR_DEPTH;
        if (flags & 0x2) clear |= D3D11_CLEAR_STENCIL;
        g_gpu.context->lpVtbl->ClearDepthStencilView(g_gpu.context, g_gpu.dsv,
            clear, z, s);
    }
    (void)rs;
}

static void gpu_readback(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    static DWORD last_report;
    LARGE_INTEGER rb_a, rb_b;
    QueryPerformanceCounter(&rb_a);
    gpu_flush_batch();
    if (getenv("RECOMP_GPU_BATCH_STATS") && g_gpu.dirty) {
        DWORD now = GetTickCount();
        if (!last_report || now - last_report >= 1000) {
            fprintf(stderr, "[BATCH] flushes=%u vertices=%llu draws=%u\n",
                    g_gpu.batch_flushes,
                    (unsigned long long)g_gpu.batch_vertices,
                    g_gpu.draws);
            last_report = now;
        }
    }
    uint8_t *dst;
    const uint8_t *src;
    uint32_t y, row;
    if (!g_gpu.dirty || !g_gpu.has_last_surface || !g_gpu.init_ok)
        return;
    g_gpu.context->lpVtbl->CopyResource(g_gpu.context,
        (ID3D11Resource *)g_gpu.staging, (ID3D11Resource *)g_gpu.color);
    if (FAILED(g_gpu.context->lpVtbl->Map(g_gpu.context, (ID3D11Resource *)g_gpu.staging,
            0, D3D11_MAP_READ, 0, &map)))
        return;
    dst = (uint8_t *)xbox_GetMemoryOffset() + g_gpu.last_surface.color_va;
    src = (const uint8_t *)map.pData;
    row = g_gpu.last_surface.width * 4;
    if (g_gpu.last_surface.pitch < row) row = g_gpu.last_surface.pitch;
    for (y = 0; y < g_gpu.last_surface.height; y++)
        memcpy(dst + (size_t)y * g_gpu.last_surface.pitch,
               src + (size_t)y * map.RowPitch, row);
    {
        const char *want = getenv("RECOMP_GPU_DUMP_FRAME");
        int dump_title = getenv("RECOMP_GPU_DUMP_TITLE") != NULL;
        if (g_gpu.dumped_frame != g_gpu.last_frame
            && ((want && (uint32_t)atoi(want) == g_gpu.last_frame)
                || (dump_title && g_gpu.title_frame
                    && g_gpu.last_frame == g_gpu.title_frame))) {
            char path[160];
            FILE *fp;
            uint32_t x;
            sprintf(path, "output/gpu_frame_%u.ppm", g_gpu.last_frame);
            fp = fopen(path, "wb");
            if (fp) {
                fprintf(fp, "P6\n%u %u\n255\n",
                        g_gpu.last_surface.width, g_gpu.last_surface.height);
                for (y = 0; y < g_gpu.last_surface.height; y++) {
                    const uint8_t *p = src + (size_t)y * map.RowPitch;
                    for (x = 0; x < g_gpu.last_surface.width; x++) {
                        fputc(p[x * 4 + 2], fp);
                        fputc(p[x * 4 + 1], fp);
                        fputc(p[x * 4 + 0], fp);
                    }
                }
                fclose(fp);
            }
            g_gpu.dumped_frame = g_gpu.last_frame;
        }
    }
    g_gpu.context->lpVtbl->Unmap(g_gpu.context, (ID3D11Resource *)g_gpu.staging, 0);
    QueryPerformanceCounter(&rb_b);
    if (g_gpu.perf_freq.QuadPart)
        g_gpu.perf_readback_ticks.QuadPart += rb_b.QuadPart - rb_a.QuadPart;
    g_gpu.dirty = 0;
    if (getenv("RECOMP_GPU_DEBUG_PERF") && g_gpu.perf_freq.QuadPart) {
        DWORD now = GetTickCount();
        static DWORD report_at;
        if (!report_at || now - report_at >= 1000) {
            double f = (double)g_gpu.perf_freq.QuadPart;
            fprintf(stderr, "[D3DPERF] draw_ms=%.2f readback_ms=%.2f tex_ms=%.2f gpu_ms=%.2f draws=%u flushes=%u texhit=%u texmiss=%u\n",
                    (double)g_gpu.perf_draw_ticks.QuadPart * 1000.0 / f,
                    (double)g_gpu.perf_readback_ticks.QuadPart * 1000.0 / f,
                    (double)g_gpu.perf_tex_ticks.QuadPart * 1000.0 / f,
                    (double)g_gpu.perf_gpu_ticks.QuadPart * 1000.0 / f,
                    g_gpu.perf_draws, g_gpu.batch_flushes,
                    g_gpu.tex_hits, g_gpu.tex_misses);
            g_gpu.tex_hits = 0;
            g_gpu.tex_misses = 0;
            g_gpu.perf_draw_ticks.QuadPart = 0;
            g_gpu.perf_readback_ticks.QuadPart = 0;
            g_gpu.perf_tex_ticks.QuadPart = 0;
            g_gpu.perf_gpu_ticks.QuadPart = 0;
            g_gpu.perf_draws = 0;
            report_at = now;
        }
    }
}

static int batch_matches(const Nv2aGpuDraw *a, const Nv2aGpuDraw *b)
{
    uint32_t st;
    if (a->plan != b->plan || a->frame != b->frame)
        return 0;
    if (memcmp(&a->surface, &b->surface, sizeof a->surface) ||
        memcmp(&a->state, &b->state, sizeof a->state))
        return 0;
    if ((a->combiner == NULL) != (b->combiner == NULL))
        return 0;
    if (a->combiner && memcmp(&g_gpu.batch_combiner, b->combiner, sizeof *b->combiner))
        return 0;
    if ((a->rc_plan == NULL) != (b->rc_plan == NULL))
        return 0;
    if (a->rc_plan) {
        Nv2aRcPlan other = *b->rc_plan;
        Nv2aRcPlan ours = g_gpu.batch_rc;
        other.rc = NULL;
        ours.rc = NULL;
        if (memcmp(&ours, &other, sizeof ours))
            return 0;
    }
    if (a->depth_min != b->depth_min || a->depth_max != b->depth_max ||
        a->alpha_test != b->alpha_test || a->alpha_func != b->alpha_func ||
        a->alpha_ref != b->alpha_ref)
        return 0;
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *ta = g_gpu.batch_key.textures[st];
        const Nv2aTexture *tb = b->textures[st];
        if ((ta == NULL) != (tb == NULL))
            return 0;
        if (ta && (ta->offset != tb->offset || ta->color != tb->color ||
                   ta->width != tb->width || ta->height != tb->height ||
                   ta->pitch != tb->pitch || ta->palette != tb->palette ||
                   ta->filter != tb->filter))
            return 0;
        if (a->tex_width[st] != b->tex_width[st] ||
            a->tex_height[st] != b->tex_height[st])
            return 0;
    }
    return 1;
}

static void gpu_draw_one(const Nv2aGpuDraw *d)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    gpu_draw_one_inner(d);
    QueryPerformanceCounter(&b);
    if (g_gpu.perf_freq.QuadPart)
        g_gpu.perf_draw_ticks.QuadPart += b.QuadPart - a.QuadPart;
    g_gpu.perf_draws++;
}

static void gpu_flush_batch(void)
{
    Nv2aGpuDraw d;
    if (!g_gpu.batch_active)
        return;
    d = g_gpu.batch_key;
    d.vertices = g_gpu.batch;
    d.count = g_gpu.batch_count;
    g_gpu.batch_active = 0;
    g_gpu.batch_count = 0;
    g_gpu.batch_flushes++;
    g_gpu.batch_vertices += d.count;
    gpu_draw_one(&d);
}

static int gpu_draw(const Nv2aGpuDraw *d)
{
    uint32_t i;
    if (!d || !d->vertices || d->count < 3 || (d->count % 3) != 0)
        return 0;
    if (d->plan < 1 || d->plan > 9)
        return 0;
    if (!g_gpu.init_ok && !gpu_init())
        return 0;
    if (d->surface.width != GPU_W || d->surface.height != GPU_H
        || d->surface.bytes_per_pixel != 4) {
        gpu_flush_batch();
        return 0;
    }
    if (g_gpu.batch_active && !batch_matches(&g_gpu.batch_key, d))
        gpu_flush_batch();
    if (g_gpu.batch_active &&
        g_gpu.batch_count + d->count > GPU_BATCH_VERTS)
        gpu_flush_batch();
    if (!g_gpu.batch_active) {
        uint32_t st;
        g_gpu.batch_key = *d;
        g_gpu.batch_key.vertices = NULL;
        g_gpu.batch_key.count = 0;
        for (st = 0; st < 4; st++) {
            if (d->textures[st]) {
                g_gpu.batch_tex[st] = *d->textures[st];
                g_gpu.batch_key.textures[st] = &g_gpu.batch_tex[st];
            } else {
                g_gpu.batch_key.textures[st] = NULL;
            }
        }
        if (d->combiner) {
            g_gpu.batch_combiner = *d->combiner;
            g_gpu.batch_key.combiner = &g_gpu.batch_combiner;
        }
        if (d->rc_plan) {
            g_gpu.batch_rc = *d->rc_plan;
            g_gpu.batch_rc.rc = NULL;
            g_gpu.batch_key.rc_plan = &g_gpu.batch_rc;
        }
        g_gpu.batch_count = 0;
        g_gpu.batch_active = 1;
    }
    for (i = 0; i < d->count; i++)
        g_gpu.batch[g_gpu.batch_count++] = d->vertices[i];
    return 1;
}

static void gpu_readback_to(uint32_t color_va, uint32_t pitch)
{
    if (!g_gpu.has_last_surface || !g_gpu.init_ok)
        return;
    g_gpu.last_surface.color_va = color_va;
    if (pitch)
        g_gpu.last_surface.pitch = pitch;
    gpu_readback();
}

static void gpu_shutdown(void)
{
    release_all();
}

static const Nv2aGpuBackend k_gpu_backend = {
    gpu_draw,
    gpu_sync,
    gpu_clear,
    gpu_readback,
    gpu_readback_to,
    gpu_shutdown
};

const Nv2aGpuBackend *nv2a_gpu_d3d11_backend(void)
{
    return &k_gpu_backend;
}
