/* Flat C ABI exported by d3d8gl.dll to the recompiled game. */
#include "platform.h"

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type,
                                     void *unused, DWORD behavior_flags,
                                     D3DPRESENT_PARAMETERS *presentation,
                                     D3DDevice **returned_device);
void WINAPI D3DDevice_SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture);
void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette);
void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix);
void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, const void *data, DWORD count);
void WINAPI D3DDevice_SetStreamSource(UINT stream, D3DVertexBuffer *data, UINT stride);
void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *data, UINT base_vertex);
void WINAPI D3DDevice_SetVertexShader(DWORD handle);
HRESULT WINAPI D3DDevice_CreateVertexShader(const DWORD *declaration, const DWORD *function,
                                            DWORD *handle, DWORD usage);
void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition);
void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive, UINT start, UINT count);
void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive, UINT count,
                                          const WORD *indices);
void WINAPI D3DDevice_Clear(DWORD count, const D3DRECT *rectangles, DWORD flags,
                            D3DCOLOR color, float z, DWORD stencil);
void WINAPI D3DDevice_Present(const RECT *source, const RECT *destination,
                              void *unused, void *unused2);
void WINAPI D3DDevice_BeginScene(void);
void WINAPI D3DDevice_EndScene(void);
void WINAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *viewport);
void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil);
void WINAPI D3DDevice_BlockUntilVerticalBlank(void);
void noble_d3d8_set_state_arrays(const DWORD *render_state,
                                 const DWORD (*texture_state)[D3DTSS_MAX]);
void noble_d3d8_sync_state(void);

__declspec(dllexport) int noble_d3d8_init(void)
{
    D3DPRESENT_PARAMETERS presentation;
    D3DDevice *device = NULL;

    memset(&presentation, 0, sizeof(presentation));
    presentation.BackBufferWidth = 640;
    presentation.BackBufferHeight = 480;
    presentation.BackBufferFormat = D3DFMT_LIN_A8R8G8B8;
    presentation.EnableAutoDepthStencil = TRUE;
    presentation.AutoDepthStencilFormat = D3DFMT_LIN_D24S8;
    if (Direct3D_CreateDevice(0, 0, NULL, 0, &presentation, &device) < 0)
        return 0;
    /* The title's D3D8 library keeps these arrays at fixed guest addresses. */
    noble_d3d8_set_state_arrays((const DWORD *)(uintptr_t)0x003C6D00u,
                                (const DWORD (*)[D3DTSS_MAX])(uintptr_t)0x003C6B00u);
    return 1;
}

__declspec(dllexport) void noble_d3d8_set_state_arrays_guest(void)
{
    noble_d3d8_set_state_arrays((const DWORD *)(uintptr_t)0x003C6D00u,
                                (const DWORD (*)[D3DTSS_MAX])(uintptr_t)0x003C6B00u);
}

__declspec(dllexport) void noble_d3d8_state(unsigned state, unsigned value)
{
    D3DDevice_SetRenderState((D3DRENDERSTATETYPE)state, value);
}
__declspec(dllexport) void noble_d3d8_texture(unsigned stage, unsigned texture)
{
    D3DDevice_SetTexture(stage, (D3DBaseTexture *)(uintptr_t)texture);
}
__declspec(dllexport) void noble_d3d8_palette(unsigned stage, unsigned palette)
{
    D3DDevice_SetPalette(stage, (D3DPalette *)(uintptr_t)palette);
}
__declspec(dllexport) void noble_d3d8_transform(unsigned state, unsigned matrix)
{
    D3DDevice_SetTransform((D3DTRANSFORMSTATETYPE)state,
                           (const D3DMATRIX *)(uintptr_t)matrix);
}
__declspec(dllexport) void noble_d3d8_vertex_constants(int reg, unsigned data, unsigned count)
{
    D3DDevice_SetVertexShaderConstant(reg, (const void *)(uintptr_t)data, count);
}
__declspec(dllexport) void noble_d3d8_stream(unsigned stream, unsigned buffer, unsigned stride)
{
    D3DDevice_SetStreamSource(stream, (D3DVertexBuffer *)(uintptr_t)buffer, stride);
}
__declspec(dllexport) void noble_d3d8_indices(unsigned buffer, unsigned base)
{
    D3DDevice_SetIndices((D3DIndexBuffer *)(uintptr_t)buffer, base);
}
__declspec(dllexport) void noble_d3d8_vertex_shader(unsigned handle)
{
    D3DDevice_SetVertexShader(handle);
}
__declspec(dllexport) unsigned noble_d3d8_create_vertex_shader(unsigned declaration,
                                                               unsigned function,
                                                               unsigned usage)
{
    DWORD handle = 0;
    if (D3DDevice_CreateVertexShader((const DWORD *)(uintptr_t)declaration,
                                     (const DWORD *)(uintptr_t)function,
                                     &handle, usage) < 0)
        return 0;
    return handle;
}
__declspec(dllexport) void noble_d3d8_pixel_shader(unsigned definition)
{
    D3DDevice_SetPixelShaderProgram((D3DPIXELSHADERDEF *)(uintptr_t)definition);
}
__declspec(dllexport) void noble_d3d8_draw(unsigned primitive, unsigned start, unsigned count)
{
    noble_d3d8_sync_state();
    D3DDevice_DrawVertices((D3DPRIMITIVETYPE)primitive, start, count);
}
__declspec(dllexport) void noble_d3d8_draw_indexed(unsigned primitive, unsigned count,
                                                    unsigned indices)
{
    noble_d3d8_sync_state();
    D3DDevice_DrawIndexedVertices((D3DPRIMITIVETYPE)primitive, count,
                                  (const WORD *)(uintptr_t)indices);
}
__declspec(dllexport) void noble_d3d8_clear(unsigned count, unsigned rectangles, unsigned flags,
                                            unsigned color, float z, unsigned stencil)
{
    noble_d3d8_sync_state();
    D3DDevice_Clear(count, (const D3DRECT *)(uintptr_t)rectangles, flags, color, z, stencil);
}
__declspec(dllexport) void noble_d3d8_present(void)
{
    D3DDevice_Present(NULL, NULL, NULL, NULL);
}
__declspec(dllexport) void noble_d3d8_begin_scene(void) { D3DDevice_BeginScene(); }
__declspec(dllexport) void noble_d3d8_end_scene(void) { D3DDevice_EndScene(); }
__declspec(dllexport) void noble_d3d8_viewport(unsigned viewport)
{
    D3DDevice_SetViewport((const D3DVIEWPORT8 *)(uintptr_t)viewport);
}
__declspec(dllexport) void noble_d3d8_render_target(unsigned target, unsigned depth)
{
    D3DDevice_SetRenderTarget((D3DSurface *)(uintptr_t)target,
                              (D3DSurface *)(uintptr_t)depth);
}
__declspec(dllexport) void noble_d3d8_block_vblank(void)
{
    D3DDevice_BlockUntilVerticalBlank();
}

