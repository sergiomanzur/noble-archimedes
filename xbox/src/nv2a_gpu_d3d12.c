/*
 * D3D12 queue rasteriser.
 *
 * Same seam as the D3D11 backend: the executor already produces clipped,
 * surface-space triangles with texture stages and combiner constants.  This
 * file records them into a D3D12 command list, batches consecutive equal-state
 * triangles, and reads the finished target back to guest memory once per
 * queue.  The architecture follows BLACK-XBERecomp's d3d12.cpp/render_thread
 * split without its game-specific D3D8 HLE.
 */
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kernel/nv2a_gpu.h"
#include "kernel/nv2a_backend.h"
#include "nv2a_gpu_shaders.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

#define D12_W 640
#define D12_H 480
#define D12_BATCH_MAX 8190
#define D12_SRV_MAX 8192
#define D12_SMP_MAX 2048
#define D12_TEX_CACHE 128
#define D12_FRAME_TEX 256
#define D12_PSO_MAX 64

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse;
    float uv[4][2];
} D12Vertex;

typedef struct {
    uint32_t offset, color, width, height, pitch, palette, palette_hash, levels, filter, generation;
    ID3D12Resource *tex;
    ID3D12Resource *up;
} D12TexCache;

typedef struct {
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    HANDLE event;
    UINT64 fence_value;
    ID3D12RootSignature *root;
    ID3DBlob *vs_blob, *ps_blob;
    ID3D12PipelineState *pso[D12_PSO_MAX];
    uint32_t pso_key[D12_PSO_MAX];
    ID3D12Resource *rt, *ds, *readback, *sync_up, *vb_up, *cb_up, *white;
    ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *srv_heap, *smp_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, smp_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, smp_gpu;
    UINT srv_inc, smp_inc;
    uint32_t srv_cursor, smp_cursor;
    uint8_t *sync_cpu, *vb_cpu, *cb_cpu;
    uint32_t vb_off, cb_off;
    uint32_t init_ok, init_tried;
    uint32_t in_frame, list_open;
    D3D12_RESOURCE_STATES rt_state;
    Nv2aSurface last_surface;
    uint32_t has_last_surface;
    D12TexCache tex_cache[D12_TEX_CACHE];
    uint32_t tex_n;
    ID3D12Resource *frame_tex[D12_FRAME_TEX];
    ID3D12Resource *frame_up[D12_FRAME_TEX];
    uint32_t frame_n;
    uint32_t debug;
} D12State;

typedef struct {
    Nv2aSurface surface;
    Nv2aRenderState state;
    uint32_t plan;
    Nv2aTexture tex[4];
    uint32_t has_tex[4];
    Nv2aRcPlan rc_plan;
    uint32_t has_plan;
    float depth_min, depth_max;
    uint32_t alpha_test, alpha_func, alpha_ref;
    float tex_width[4], tex_height[4];
    uint32_t frame;
} D12BatchKey;

static D12State g;
static D12BatchKey s_key;
static D12Vertex s_verts[D12_BATCH_MAX];
static uint32_t s_count;

/* ------------------------------------------------------------------------- */
/* small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    if (before == after)
        return;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
}

static void wait_gpu(void)
{
    if (!g.queue || !g.fence)
        return;
    g.queue->lpVtbl->Signal(g.queue, g.fence, ++g.fence_value);
    if (g.fence->lpVtbl->GetCompletedValue(g.fence) < g.fence_value) {
        g.fence->lpVtbl->SetEventOnCompletion(g.fence, g.fence_value, g.event);
        WaitForSingleObject(g.event, INFINITE);
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *h)
{
    D3D12_CPU_DESCRIPTOR_HANDLE r;
    memset(&r, 0, sizeof r);
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &r);
    return r;
}

static D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap *h)
{
    D3D12_GPU_DESCRIPTOR_HANDLE r;
    memset(&r, 0, sizeof r);
    h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &r);
    return r;
}

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES h;
    memset(&h, 0, sizeof h);
    h.Type = type;
    h.CreationNodeMask = 1;
    h.VisibleNodeMask = 1;
    return h;
}

static ID3D12Resource *create_buffer(size_t bytes, D3D12_HEAP_TYPE heap,
                                     D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES hp = heap_props(heap);
    ID3D12Resource *res = NULL;
    memset(&desc, 0, sizeof desc);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (UINT64)bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    {
        HRESULT h = g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp,
            D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&res);
        if (FAILED(h)) {
            fprintf(stderr, "[D3D12] buffer %llu heap %d state %d: 0x%08lX\n",
                    (unsigned long long)bytes, (int)heap, (int)state, (unsigned long)h);
            return NULL;
        }
    }
    return res;
}

static ID3D12Resource *create_tex2d(uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                                    D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_STATES state,
                                    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES hp = heap_props(heap);
    ID3D12Resource *res = NULL;
    memset(&desc, 0, sizeof desc);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    {
        HRESULT hr = g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp,
            D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
            &IID_ID3D12Resource, (void **)&res);
        if (FAILED(hr)) {
            fprintf(stderr, "[D3D12] tex %ux%u fmt %d heap %d state %d flags %d: 0x%08lX\n",
                    w, h, (int)fmt, (int)heap, (int)state, (int)flags, (unsigned long)hr);
            return NULL;
        }
    }
    return res;
}

/* ------------------------------------------------------------------------- */
/* shader state                                                               */
/* ------------------------------------------------------------------------- */

static D3D12_BLEND blend_of(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return D3D12_BLEND_SRC_COLOR;
    case 0x0301: return D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D12_BLEND_DEST_COLOR;
    case 0x0307: return D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

static D3D12_COMPARISON_FUNC compare(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D12_COMPARISON_FUNC_NEVER;
    case 0x201: return D3D12_COMPARISON_FUNC_LESS;
    case 0x202: return D3D12_COMPARISON_FUNC_EQUAL;
    case 0x203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 0x204: return D3D12_COMPARISON_FUNC_GREATER;
    case 0x205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 0x206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default:    return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

static ID3D12PipelineState *get_pso(const Nv2aRenderState *rs)
{
    uint32_t key = (rs->blend_enable ? 1u : 0u)
                 | ((uint32_t)blend_of(rs->blend_src) << 1)
                 | ((uint32_t)blend_of(rs->blend_dst) << 6)
                 | ((uint32_t)blend_op(rs->blend_eq) << 11)
                 | ((rs->depth_test_enable ? 1u : 0u) << 16)
                 | ((rs->depth_write ? 1u : 0u) << 17)
                 | ((rs->depth_func & 0xFu) << 18)
                 ^ ((rs->color_mask & 0x01010101u) * 2654435761u);
    uint32_t i;
    for (i = 0; i < D12_PSO_MAX; i++)
        if (g.pso[i] && g.pso_key[i] == key)
            return g.pso[i];
    for (i = 0; i < D12_PSO_MAX; i++)
        if (!g.pso[i]) break;
    if (i == D12_PSO_MAX) i = 0;
    if (g.pso[i]) {
        g.pso[i]->lpVtbl->Release(g.pso[i]);
        g.pso[i] = NULL;
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso;
        D3D12_INPUT_ELEMENT_DESC elem[7];
        D3D12_RENDER_TARGET_BLEND_DESC *b;
        D3D12_DEPTH_STENCIL_DESC *d;
        memset(&pso, 0, sizeof pso);
        pso.pRootSignature = g.root;
        pso.VS.pShaderBytecode = g.vs_blob->lpVtbl->GetBufferPointer(g.vs_blob);
        pso.VS.BytecodeLength = g.vs_blob->lpVtbl->GetBufferSize(g.vs_blob);
        pso.PS.pShaderBytecode = g.ps_blob->lpVtbl->GetBufferPointer(g.ps_blob);
        pso.PS.BytecodeLength = g.ps_blob->lpVtbl->GetBufferSize(g.ps_blob);
        memset(elem, 0, sizeof elem);
        elem[0].SemanticName = "POSITION"; elem[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; elem[0].InputSlot = 0; elem[0].AlignedByteOffset = 0; elem[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        elem[1].SemanticName = "TEXCOORD"; elem[1].SemanticIndex = 0; elem[1].Format = DXGI_FORMAT_R32_FLOAT; elem[1].InputSlot = 0; elem[1].AlignedByteOffset = 12; elem[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        elem[2].SemanticName = "COLOR"; elem[2].Format = DXGI_FORMAT_B8G8R8A8_UNORM; elem[2].InputSlot = 0; elem[2].AlignedByteOffset = 16; elem[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        for (i = 0; i < 4; i++) {
            elem[3 + i].SemanticName = "TEXCOORD";
            elem[3 + i].SemanticIndex = 1 + i;
            elem[3 + i].Format = DXGI_FORMAT_R32G32_FLOAT;
            elem[3 + i].InputSlot = 0;
            elem[3 + i].AlignedByteOffset = 20 + i * 8;
            elem[3 + i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        }
        pso.InputLayout.pInputElementDescs = elem;
        pso.InputLayout.NumElements = 7;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pso.SampleDesc.Count = 1;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.FrontCounterClockwise = FALSE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        b = &pso.BlendState.RenderTarget[0];
        b->BlendEnable = rs->blend_enable ? TRUE : FALSE;
        b->LogicOpEnable = FALSE;
        b->SrcBlend = blend_of(rs->blend_src);
        b->DestBlend = blend_of(rs->blend_dst);
        b->BlendOp = blend_op(rs->blend_eq);
        b->SrcBlendAlpha = b->SrcBlend;
        b->DestBlendAlpha = b->DestBlend;
        b->BlendOpAlpha = b->BlendOp;
        b->RenderTargetWriteMask = 0;
        if (rs->color_mask & 0x00010000u) b->RenderTargetWriteMask |= D3D12_COLOR_WRITE_ENABLE_RED;
        if (rs->color_mask & 0x00000100u) b->RenderTargetWriteMask |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        if (rs->color_mask & 0x00000001u) b->RenderTargetWriteMask |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        if (rs->color_mask & 0x01000000u) b->RenderTargetWriteMask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        d = &pso.DepthStencilState;
        d->DepthEnable = rs->depth_test_enable ? TRUE : FALSE;
        d->DepthWriteMask = rs->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d->DepthFunc = compare(rs->depth_func);
        d->StencilEnable = FALSE;
        if (FAILED(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pso,
                &IID_ID3D12PipelineState, (void **)&g.pso[i])))
            return NULL;
        g.pso_key[i] = key;
    }
    return g.pso[i];
}

/* ------------------------------------------------------------------------- */
/* textures                                                                   */
/* ------------------------------------------------------------------------- */

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

static ID3D12Resource *get_texture(const Nv2aTexture *t, uint32_t frame)
{
    uint32_t i, generation = (t && t->color == 0x0Bu) ? frame : frame / 8;
    uint32_t *argb;
    uint32_t palette_hash = texture_palette_hash(t);
    ID3D12Resource *tex = NULL;
    ID3D12Resource *up = NULL;
    void *p = NULL;
    size_t bytes, row;
    D12TexCache *e = NULL;
    if (!t || !t->offset || !t->width || !t->height)
        return g.white;
    for (i = 0; i < g.tex_n; i++) {
        D12TexCache *c = &g.tex_cache[i];
        if (c->offset == t->offset && c->color == t->color
            && c->width == t->width && c->height == t->height
            && c->pitch == t->pitch && c->levels == t->levels
            && c->filter == t->filter && c->palette == t->palette
            && (c->palette_hash == palette_hash
                || (t->color == 0x0Bu && c->generation == generation))) {
            e = c;
            break;
        }
    }
    if (e && e->generation == generation)
        return e->tex;
    argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!argb || !nv2a_backend_decode_texture(t, argb)) {
        free(argb);
        return e ? e->tex : g.white;
    }
    bytes = (size_t)t->width * t->height * 4;
    row = (size_t)t->width * 4;
    if (e) {
        tex = e->tex;
        up = e->up;
        {
            D3D12_RANGE range = {0, bytes};
            if (FAILED(up->lpVtbl->Map(up, 0, &range, &p))) {
                free(argb);
                return tex;
            }
            memcpy(p, argb, bytes);
            up->lpVtbl->Unmap(up, 0, NULL);
        }
        barrier(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
    } else {
        if (g.tex_n >= D12_TEX_CACHE) {
            free(argb);
            return g.white;
        }
        tex = create_tex2d(t->width, t->height, DXGI_FORMAT_B8G8R8A8_UNORM,
                           D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_FLAG_NONE);
        up = create_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!tex || !up) {
            if (tex) tex->lpVtbl->Release(tex);
            if (up) up->lpVtbl->Release(up);
            free(argb);
            return g.white;
        }
        {
            D3D12_RANGE range = {0, bytes};
            if (FAILED(up->lpVtbl->Map(up, 0, &range, &p))) {
                tex->lpVtbl->Release(tex);
                up->lpVtbl->Release(up);
                free(argb);
                return g.white;
            }
            memcpy(p, argb, bytes);
            up->lpVtbl->Unmap(up, 0, NULL);
        }
        e = &g.tex_cache[g.tex_n++];
        e->offset = t->offset;
        e->color = t->color;
        e->width = t->width;
        e->height = t->height;
        e->pitch = t->pitch;
        e->palette = t->palette;
        e->palette_hash = palette_hash;
        e->levels = t->levels;
        e->filter = t->filter;
        e->tex = tex;
        e->up = up;
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        memset(&dst, 0, sizeof dst);
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        memset(&src, 0, sizeof src);
        src.pResource = up;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = t->width;
        src.PlacedFootprint.Footprint.Height = t->height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = (UINT)row;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
    }
    barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    e->generation = generation;
    free(argb);
    return tex;
}

/* ------------------------------------------------------------------------- */
/* batching                                                                   */
/* ------------------------------------------------------------------------- */

static void key_from_draw(const Nv2aGpuDraw *d, D12BatchKey *k)
{
    uint32_t st;
    memset(k, 0, sizeof *k);
    k->surface = d->surface;
    k->state = d->state;
    k->plan = d->plan;
    k->depth_min = d->depth_min;
    k->depth_max = d->depth_max;
    k->alpha_test = d->alpha_test;
    k->alpha_func = d->alpha_func;
    k->alpha_ref = d->alpha_ref;
    k->frame = d->frame;
    for (st = 0; st < 4; st++) {
        k->tex_width[st] = d->tex_width[st];
        k->tex_height[st] = d->tex_height[st];
        if (d->textures[st]) {
            k->has_tex[st] = 1;
            k->tex[st] = *d->textures[st];
        }
    }
    if (d->rc_plan) {
        k->has_plan = 1;
        k->rc_plan.kind = d->rc_plan->kind;
        k->rc_plan.fog_final = d->rc_plan->fog_final;
        k->rc_plan.uses_v1 = d->rc_plan->uses_v1;
        k->rc_plan.uses_fog = d->rc_plan->uses_fog;
        memcpy(k->rc_plan.six_c0, d->rc_plan->six_c0, sizeof k->rc_plan.six_c0);
        k->rc_plan.six_c1_alpha = d->rc_plan->six_c1_alpha;
    }
}

static void begin_frame(const Nv2aSurface *s)
{
    D3D12_VIEWPORT vp;
    D3D12_RECT scissor;
    ID3D12DescriptorHeap *heaps[2];
    if (g.in_frame)
        return;
    g.alloc->lpVtbl->Reset(g.alloc);
    g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    g.list_open = 1;
    g.in_frame = 1;
    g.srv_cursor = 0;
    g.smp_cursor = 0;
    g.vb_off = 0;
    g.cb_off = 0;
    if (s)
        g.last_surface = *s;
    heaps[0] = g.srv_heap;
    heaps[1] = g.smp_heap;
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.root);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = D12_W; vp.Height = D12_H;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    scissor.left = 0; scissor.top = 0; scissor.right = D12_W; scissor.bottom = D12_H;
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &scissor);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_h =
            cpu_handle(g.rtv_heap);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_h =
            cpu_handle(g.dsv_heap);
        g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv_h, FALSE, &dsv_h);
    }
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

static void flush_batch(void)
{
    D3D12_GPU_DESCRIPTOR_HANDLE srv_h, smp_h;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_c, smp_c;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    ID3D12Resource *texs[4];
    ID3D12PipelineState *pso;
    uint32_t i, st, srv0, smp0;
    float vscb[4], pscb[24];
    if (!s_count)
        return;
    if (!g.in_frame)
        begin_frame(&s_key.surface);
    srv0 = g.srv_cursor;
    smp0 = g.smp_cursor;
    if (srv0 + 4 > D12_SRV_MAX || smp0 + 4 > D12_SMP_MAX)
        return;
    srv_c = g.srv_cpu;
    srv_c.ptr += (SIZE_T)srv0 * g.srv_inc;
    smp_c = g.smp_cpu;
    smp_c.ptr += (SIZE_T)smp0 * g.smp_inc;
    srv_h = g.srv_gpu;
    srv_h.ptr += (UINT64)srv0 * g.srv_inc;
    smp_h = g.smp_gpu;
    smp_h.ptr += (UINT64)smp0 * g.smp_inc;
    for (st = 0; st < 4; st++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv;
        D3D12_SAMPLER_DESC smp;
        const Nv2aTexture *t = s_key.has_tex[st] ? &s_key.tex[st] : NULL;
        texs[st] = get_texture(t, s_key.frame);
        memset(&srv, 0, sizeof srv);
        srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srv_c;
            h.ptr += (SIZE_T)st * g.srv_inc;
            g.dev->lpVtbl->CreateShaderResourceView(g.dev, texs[st], &srv, h);
        }
        memset(&smp, 0, sizeof smp);
        smp.Filter = (t && ((t->filter >> 24) & 0xF) != 2
                      && ((t->filter >> 16) & 0xFF) != 2
                      && ((t->filter >> 16) & 0xFF) != 4
                      && ((t->filter >> 16) & 0xFF) != 6)
                    ? D3D12_FILTER_MIN_MAG_MIP_POINT
                    : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = (t && t->addr_u == 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.AddressV = (t && t->addr_v == 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = smp_c;
            h.ptr += (SIZE_T)st * g.smp_inc;
            g.dev->lpVtbl->CreateSampler(g.dev, &smp, h);
        }
    }
    g.srv_cursor += 4;
    g.smp_cursor += 4;
    /* constants: VS at cb_off, PS at cb_off + 16 */
    vscb[0] = 2.0f / (float)D12_W;
    vscb[1] = 2.0f / (float)D12_H;
    if (s_key.depth_max > s_key.depth_min) {
        vscb[2] = 1.0f / (s_key.depth_max - s_key.depth_min);
        vscb[3] = -s_key.depth_min / (s_key.depth_max - s_key.depth_min);
    } else {
        vscb[2] = 1.0f; vscb[3] = 0.0f;
    }
    memset(pscb, 0, sizeof pscb);
    for (st = 0; st < 4; st++) {
        pscb[st * 2] = s_key.tex_width[st] > 0.0f ? s_key.tex_width[st] : 1.0f;
        pscb[st * 2 + 1] = s_key.tex_height[st] > 0.0f ? s_key.tex_height[st] : 1.0f;
    }
    pscb[8] = (float)s_key.plan;
    pscb[9] = s_key.alpha_test ? 1.0f : 0.0f;
    pscb[10] = (float)((s_key.alpha_func >= 0x200 && s_key.alpha_func <= 0x207)
                       ? s_key.alpha_func - 0x200 : 7);
    pscb[11] = (float)(s_key.alpha_ref & 0xFF);
    if (s_key.has_plan) {
        /* Shader colors are RGBA; the resource formats handle BGRA storage. */
        pscb[12] = s_key.rc_plan.six_c0[0];
        pscb[13] = s_key.rc_plan.six_c0[1];
        pscb[14] = s_key.rc_plan.six_c0[2];
        pscb[15] = s_key.rc_plan.six_c0[3];
        pscb[16] = s_key.rc_plan.six_c1_alpha;
    }
    {
        size_t off = g.cb_off;
        memcpy(g.cb_cpu + off, vscb, sizeof vscb);
        memcpy(g.cb_cpu + off + 16, pscb, sizeof pscb);
        g.list->lpVtbl->SetGraphicsRootConstantBufferView(g.list, 0,
            g.cb_up->lpVtbl->GetGPUVirtualAddress(g.cb_up) + off);
        g.list->lpVtbl->SetGraphicsRootConstantBufferView(g.list, 1,
            g.cb_up->lpVtbl->GetGPUVirtualAddress(g.cb_up) + off + 16);
        g.cb_off += 256;
    }
    {
        size_t off = g.vb_off;
        memcpy(g.vb_cpu + off, s_verts, (size_t)s_count * sizeof(D12Vertex));
        vbv.BufferLocation = g.vb_up->lpVtbl->GetGPUVirtualAddress(g.vb_up) + off;
        vbv.SizeInBytes = s_count * sizeof(D12Vertex);
        vbv.StrideInBytes = sizeof(D12Vertex);
        g.vb_off += s_count * sizeof(D12Vertex);
        g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    }
    pso = get_pso(&s_key.state);
    if (pso)
        g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 2, srv_h);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 3, smp_h);
    g.list->lpVtbl->DrawInstanced(g.list, s_count, 1, 0, 0);
    s_count = 0;
    (void)i;
}

/* ------------------------------------------------------------------------- */
/* backend entry points                                                       */
/* ------------------------------------------------------------------------- */

static int d12_init(void);

static int d12_draw(const Nv2aGpuDraw *d)
{
    D12BatchKey key;
    uint32_t i;
    if (!d || !d->vertices || d->count != 3)
        return 0;
    if (d->plan < 1 || d->plan > 9)
        return 0;
    if (!g.init_ok && !d12_init())
        return 0;
    key_from_draw(d, &key);
    if (s_count && memcmp(&key, &s_key, sizeof key) != 0)
        flush_batch();
    if (!s_count)
        s_key = key;
    if (s_count + 3 > D12_BATCH_MAX) {
        flush_batch();
        s_key = key;
    }
    for (i = 0; i < 3; i++) {
        const Nv2aGpuVertex *v = &d->vertices[i];
        D12Vertex *o = &s_verts[s_count + i];
        uint32_t st;
        o->x = v->x; o->y = v->y; o->z = v->z;
        o->rhw = v->rhw; o->diffuse = v->diffuse;
        for (st = 0; st < 4; st++) {
            o->uv[st][0] = v->uv[st][0];
            o->uv[st][1] = v->uv[st][1];
        }
    }
    s_count += 3;
    return 1;
}

static void d12_sync(const Nv2aSurface *s)
{
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    const uint8_t *guest;
    uint32_t y, row;
    if (!s || s->width != D12_W || s->height != D12_H
        || s->bytes_per_pixel != 4 || !s->pitch)
        return;
    if (!g.init_ok && !d12_init())
        return;
    if (!g.in_frame)
        begin_frame(s);
    if (g.rt_state != D3D12_RESOURCE_STATE_COPY_DEST)
        barrier(g.rt, g.rt_state, D3D12_RESOURCE_STATE_COPY_DEST);
    guest = (const uint8_t *)xbox_GetMemoryOffset() + s->color_va;
    row = D12_W * 4;
    for (y = 0; y < D12_H; y++)
        memcpy(g.sync_cpu + (size_t)y * row, guest + (size_t)y * s->pitch, row);
    memset(&dst, 0, sizeof dst);
    dst.pResource = g.rt;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    memset(&src, 0, sizeof src);
    src.pResource = g.sync_up;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = D12_W;
    src.PlacedFootprint.Footprint.Height = D12_H;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = row;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
    barrier(g.rt, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.last_surface = *s;
    g.has_last_surface = 1;
}

static void d12_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    if (!s || s->width != D12_W || s->height != D12_H)
        return;
    if (!g.init_ok && !d12_init())
        return;
    if (!g.in_frame)
        begin_frame(s);
    if (g.rt_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
        barrier(g.rt, g.rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (flags & 0xF0) {
        float c[4];
        c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (float)(argb & 0xFF) / 255.0f;
        c[3] = (float)((argb >> 24) & 0xFF) / 255.0f;
        g.list->lpVtbl->ClearRenderTargetView(g.list,
            cpu_handle(g.rtv_heap), c, 0, NULL);
    }
    if (flags & 0x1) {
        float z = (float)((zstencil >> 8) & 0xFFFFFFu) / 16777215.0f;
        g.list->lpVtbl->ClearDepthStencilView(g.list,
            cpu_handle(g.dsv_heap),
            D3D12_CLEAR_FLAG_DEPTH, z, 0, 0, NULL);
    }
    g.last_surface = *s;
    g.has_last_surface = 1;
    (void)rs;
}

static void release_frame(void)
{
    /* Textures persist across frames; the cache is released at shutdown. */
}

static void d12_readback(void)
{
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_RANGE range;
    void *p = NULL;
    uint8_t *out;
    const uint8_t *in;
    uint32_t y, row;
    if (!g.in_frame || !g.has_last_surface)
        return;
    flush_batch();
    if (g.rt_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        barrier(g.rt, g.rt_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    memset(&dst, 0, sizeof dst);
    dst.pResource = g.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = D12_W;
    dst.PlacedFootprint.Footprint.Height = D12_H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = D12_W * 4;
    memset(&src, 0, sizeof src);
    src.pResource = g.rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
    g.list->lpVtbl->Close(g.list);
    g.list_open = 0;
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1,
        (ID3D12CommandList *const *)&g.list);
    wait_gpu();
    g.rt_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.in_frame = 0;
    range.Begin = 0; range.End = D12_W * D12_H * 4;
    if (FAILED(g.readback->lpVtbl->Map(g.readback, 0, &range, &p))) {
        release_frame();
        return;
    }
    out = (uint8_t *)xbox_GetMemoryOffset() + g.last_surface.color_va;
    in = (const uint8_t *)p;
    row = D12_W * 4;
    if (g.last_surface.pitch < row) row = g.last_surface.pitch;
    for (y = 0; y < D12_H; y++)
        memcpy(out + (size_t)y * g.last_surface.pitch,
               in + (size_t)y * row, row);
    g.readback->lpVtbl->Unmap(g.readback, 0, NULL);
    release_frame();
}

static void d12_readback_to(uint32_t color_va, uint32_t pitch)
{
    if (!g.has_last_surface)
        return;
    g.last_surface.color_va = color_va;
    if (pitch)
        g.last_surface.pitch = pitch;
    d12_readback();
}

static void d12_shutdown(void)
{
    uint32_t ti;
    wait_gpu();
    release_frame();
    for (ti = 0; ti < g.tex_n; ti++) {
        if (g.tex_cache[ti].tex) g.tex_cache[ti].tex->lpVtbl->Release(g.tex_cache[ti].tex);
        if (g.tex_cache[ti].up) g.tex_cache[ti].up->lpVtbl->Release(g.tex_cache[ti].up);
    }
    g.tex_n = 0;
    if (g.white) g.white->lpVtbl->Release(g.white);
    if (g.sync_up) g.sync_up->lpVtbl->Release(g.sync_up);
    if (g.vb_up) g.vb_up->lpVtbl->Release(g.vb_up);
    if (g.cb_up) g.cb_up->lpVtbl->Release(g.cb_up);
    if (g.readback) g.readback->lpVtbl->Release(g.readback);
    if (g.ds) g.ds->lpVtbl->Release(g.ds);
    if (g.rt) g.rt->lpVtbl->Release(g.rt);
    if (g.srv_heap) g.srv_heap->lpVtbl->Release(g.srv_heap);
    if (g.smp_heap) g.smp_heap->lpVtbl->Release(g.smp_heap);
    if (g.rtv_heap) g.rtv_heap->lpVtbl->Release(g.rtv_heap);
    if (g.dsv_heap) g.dsv_heap->lpVtbl->Release(g.dsv_heap);
    if (g.root) g.root->lpVtbl->Release(g.root);
    if (g.vs_blob) g.vs_blob->lpVtbl->Release(g.vs_blob);
    if (g.ps_blob) g.ps_blob->lpVtbl->Release(g.ps_blob);
    for (uint32_t i = 0; i < D12_PSO_MAX; i++)
        if (g.pso[i]) g.pso[i]->lpVtbl->Release(g.pso[i]);
    if (g.list) g.list->lpVtbl->Release(g.list);
    if (g.alloc) g.alloc->lpVtbl->Release(g.alloc);
    if (g.fence) g.fence->lpVtbl->Release(g.fence);
    if (g.queue) g.queue->lpVtbl->Release(g.queue);
    if (g.dev) g.dev->lpVtbl->Release(g.dev);
    if (g.event) CloseHandle(g.event);
    memset(&g, 0, sizeof g);
}

/* ------------------------------------------------------------------------- */
/* init                                                                       */
/* ------------------------------------------------------------------------- */

static int compile_shader(const char *src, const char *entry, const char *target,
                          ID3DBlob **blob)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D12] shader %s: %s\n", target,
                err ? (const char *)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        return 0;
    }
    if (err) err->lpVtbl->Release(err);
    return 1;
}

static int d12_init(void)
{
    D3D12_COMMAND_QUEUE_DESC qd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    D3D12_ROOT_SIGNATURE_DESC rd;
    D3D12_ROOT_PARAMETER rp[4];
    D3D12_DESCRIPTOR_RANGE srv_r, smp_r;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
    D3D12_RANGE range;
    ID3DBlob *sig = NULL, *err = NULL;
    void *p = NULL;
    if (g.init_tried)
        return g.init_ok;
    g.init_tried = 1;
    g.debug = getenv("RECOMP_D3D12_DEBUG") != NULL;
    fprintf(stderr, "[D3D12] stage 1 device\n");
    {
        HRESULT hrdev = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
            &IID_ID3D12Device, (void **)&g.dev);
        if (FAILED(hrdev)) {
            fprintf(stderr, "[D3D12] device creation failed: 0x%08lX\n",
                    (unsigned long)hrdev);
            return 0;
        }
    }
    fprintf(stderr, "[D3D12] stage 2 commands\n");
    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.dev->lpVtbl->CreateCommandQueue(g.dev, &qd,
            &IID_ID3D12CommandQueue, (void **)&g.queue)) ||
        FAILED(g.dev->lpVtbl->CreateCommandAllocator(g.dev, qd.Type,
            &IID_ID3D12CommandAllocator, (void **)&g.alloc)) ||
        FAILED(g.dev->lpVtbl->CreateCommandList(g.dev, 0, qd.Type, g.alloc,
            NULL, &IID_ID3D12GraphicsCommandList, (void **)&g.list)) ||
        FAILED(g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&g.fence)))
        goto fail;
    g.list->lpVtbl->Close(g.list);
    g.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g.event) goto fail;
    fprintf(stderr, "[D3D12] stage 3 shaders\n");
    if (!compile_shader(k_vs, "main", "vs_5_0", &g.vs_blob) ||
        !compile_shader(k_ps, "main", "ps_5_0", &g.ps_blob))
        goto fail;
    fprintf(stderr, "[D3D12] stage 4 root signature\n");
    memset(&srv_r, 0, sizeof srv_r);
    srv_r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_r.NumDescriptors = 4;
    srv_r.BaseShaderRegister = 0;
    srv_r.RegisterSpace = 0;
    srv_r.OffsetInDescriptorsFromTableStart = 0;
    memset(&smp_r, 0, sizeof smp_r);
    smp_r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    smp_r.NumDescriptors = 4;
    smp_r.BaseShaderRegister = 0;
    smp_r.RegisterSpace = 0;
    smp_r.OffsetInDescriptorsFromTableStart = 0;
    memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1;
    rp[2].DescriptorTable.pDescriptorRanges = &srv_r;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 1;
    rp[3].DescriptorTable.pDescriptorRanges = &smp_r;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&rd, 0, sizeof rd);
    rd.NumParameters = 4;
    rd.pParameters = rp;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
            &sig, &err))) {
        if (err) {
            fprintf(stderr, "[D3D12] root signature: %s\n",
                    (const char *)err->lpVtbl->GetBufferPointer(err));
            err->lpVtbl->Release(err);
        }
        goto fail;
    }
    if (FAILED(g.dev->lpVtbl->CreateRootSignature(g.dev, 0,
            sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
            &IID_ID3D12RootSignature, (void **)&g.root))) {
        sig->lpVtbl->Release(sig);
        goto fail;
    }
    sig->lpVtbl->Release(sig);
    fprintf(stderr, "[D3D12] stage 5 heaps\n");
    /* heaps */
    memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = D12_SRV_MAX;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    {
        HRESULT h;
        h = g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&g.srv_heap);
        if (FAILED(h)) { fprintf(stderr, "[D3D12] srv heap 0x%08lX\n", (unsigned long)h); goto fail; }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = D12_SMP_MAX;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        h = g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd,
            &IID_ID3D12DescriptorHeap, (void **)&g.smp_heap);
        if (FAILED(h)) { fprintf(stderr, "[D3D12] smp heap 0x%08lX\n", (unsigned long)h); goto fail; }
        hd.NumDescriptors = 1; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        h = g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&g.rtv_heap);
        if (FAILED(h)) { fprintf(stderr, "[D3D12] rtv heap 0x%08lX\n", (unsigned long)h); goto fail; }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        h = g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&g.dsv_heap);
        if (FAILED(h)) { fprintf(stderr, "[D3D12] dsv heap 0x%08lX\n", (unsigned long)h); goto fail; }
    }
    g.srv_inc = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.smp_inc = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    g.srv_cpu = cpu_handle(g.srv_heap);
    g.srv_gpu = gpu_handle(g.srv_heap);
    g.smp_cpu = cpu_handle(g.smp_heap);
    g.smp_gpu = gpu_handle(g.smp_heap);
    fprintf(stderr, "[D3D12] stage 6 resources\n");
    /* target and depth */
    g.rt = create_tex2d(D12_W, D12_H, DXGI_FORMAT_B8G8R8A8_UNORM,
                        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    g.ds = create_tex2d(D12_W, D12_H, DXGI_FORMAT_D24_UNORM_S8_UINT,
                        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    g.readback = create_buffer((size_t)D12_W * D12_H * 4,
                               D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST);
    g.sync_up = create_buffer((size_t)D12_W * D12_H * 4, D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
    g.vb_up = create_buffer(16u << 20, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
    g.cb_up = create_buffer(1u << 20, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
    g.white = create_tex2d(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                           D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_FLAG_NONE);
    if (!g.rt || !g.ds || !g.readback || !g.sync_up || !g.vb_up || !g.cb_up || !g.white)
        goto fail;
    fprintf(stderr, "[D3D12] stage 7 white\n");
    {
        ID3D12Resource *wup = create_buffer(4, D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RANGE wr = {0, 0};
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        void *wp = NULL;
        if (!wup)
            goto fail;
        if (FAILED(wup->lpVtbl->Map(wup, 0, &wr, &wp))) {
            wup->lpVtbl->Release(wup);
            goto fail;
        }
        *(uint32_t *)wp = 0xFFFFFFFFu;
        wup->lpVtbl->Unmap(wup, 0, NULL);
        g.alloc->lpVtbl->Reset(g.alloc);
        g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
        memset(&dst, 0, sizeof dst);
        dst.pResource = g.white;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        memset(&src, 0, sizeof src);
        src.pResource = wup;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = 1;
        src.PlacedFootprint.Footprint.Height = 1;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = 4;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        barrier(g.white, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g.list->lpVtbl->Close(g.list);
        g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1,
            (ID3D12CommandList *const *)&g.list);
        wait_gpu();
        wup->lpVtbl->Release(wup);
    }
    fprintf(stderr, "[D3D12] stage 8 views\n");
    g.rt_state = D3D12_RESOURCE_STATE_COPY_DEST;
    range.Begin = 0; range.End = 0;
    memset(&rtv, 0, sizeof rtv);
    rtv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, g.rt, &rtv,
        cpu_handle(g.rtv_heap));
    memset(&dsv, 0, sizeof dsv);
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateDepthStencilView(g.dev, g.ds, &dsv,
        cpu_handle(g.dsv_heap));
    fprintf(stderr, "[D3D12] stage 9 maps\n");
    if (FAILED(g.sync_up->lpVtbl->Map(g.sync_up, 0, &range, (void **)&g.sync_cpu)) ||
        FAILED(g.vb_up->lpVtbl->Map(g.vb_up, 0, &range, (void **)&g.vb_cpu)) ||
        FAILED(g.cb_up->lpVtbl->Map(g.cb_up, 0, &range, (void **)&g.cb_cpu)))
        goto fail;
    g.init_ok = 1;
    fprintf(stderr, "[D3D12] rasteriser initialised\n");
    return 1;
fail:
    fprintf(stderr, "[D3D12] initialization failed at stage\n");
    d12_shutdown();
    g.init_tried = 1;
    return 0;
}

static const Nv2aGpuBackend k_d12_backend = {
    d12_draw,
    d12_sync,
    d12_clear,
    d12_readback,
    d12_readback_to,
    d12_shutdown
};

const Nv2aGpuBackend *nv2a_gpu_d3d12_backend(void)
{
    return &k_d12_backend;
}
