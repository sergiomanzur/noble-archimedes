/* Flat C ABI exported by d3d8gl.dll to the recompiled game. */
#include "platform.h"

ptrdiff_t noble_guest_memory_offset;
__declspec(dllexport) void noble_d3d8_set_guest_offset(ptrdiff_t offset)
{
    noble_guest_memory_offset = offset;
}

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
    noble_d3d8_set_state_arrays((const DWORD *)noble_guest_pointer(0x003C6D00u),
                                (const DWORD (*)[D3DTSS_MAX])noble_guest_pointer(0x003C6B00u));
    return 1;
}

__declspec(dllexport) void noble_d3d8_set_state_arrays_guest(void)
{
    noble_d3d8_set_state_arrays((const DWORD *)noble_guest_pointer(0x003C6D00u),
                                (const DWORD (*)[D3DTSS_MAX])noble_guest_pointer(0x003C6B00u));
}

__declspec(dllexport) void noble_d3d8_state(unsigned state, unsigned value)
{
    D3DDevice_SetRenderState((D3DRENDERSTATETYPE)state, value);
}
__declspec(dllexport) void noble_d3d8_texture(unsigned stage, unsigned texture)
{
    D3DDevice_SetTexture(stage, (D3DBaseTexture *)noble_guest_pointer(texture));
}
__declspec(dllexport) void noble_d3d8_palette(unsigned stage, unsigned palette)
{
    D3DDevice_SetPalette(stage, (D3DPalette *)noble_guest_pointer(palette));
}
__declspec(dllexport) void noble_d3d8_transform(unsigned state, unsigned matrix)
{
    D3DDevice_SetTransform((D3DTRANSFORMSTATETYPE)state,
                           (const D3DMATRIX *)noble_guest_pointer(matrix));
}
__declspec(dllexport) void noble_d3d8_vertex_constants(int reg, unsigned data, unsigned count)
{
    D3DDevice_SetVertexShaderConstant(reg, (const void *)noble_guest_pointer(data), count);
}
__declspec(dllexport) void noble_d3d8_stream(unsigned stream, unsigned buffer, unsigned stride)
{
    D3DDevice_SetStreamSource(stream, (D3DVertexBuffer *)noble_guest_pointer(buffer), stride);
}
__declspec(dllexport) void noble_d3d8_indices(unsigned buffer, unsigned base)
{
    D3DDevice_SetIndices((D3DIndexBuffer *)noble_guest_pointer(buffer), base);
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
    if (D3DDevice_CreateVertexShader((const DWORD *)noble_guest_pointer(declaration),
                                     (const DWORD *)noble_guest_pointer(function),
                                     &handle, usage) < 0)
        return 0;
    return handle;
}
__declspec(dllexport) void noble_d3d8_pixel_shader(unsigned definition)
{
    D3DDevice_SetPixelShaderProgram((D3DPIXELSHADERDEF *)noble_guest_pointer(definition));
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
                                  (const WORD *)noble_guest_pointer(indices));
}
__declspec(dllexport) void noble_d3d8_clear(unsigned count, unsigned rectangles, unsigned flags,
                                            unsigned color, float z, unsigned stencil)
{
    noble_d3d8_sync_state();
    D3DDevice_Clear(count, (const D3DRECT *)noble_guest_pointer(rectangles), flags, color, z, stencil);
}
void noble_d3d8_present_host(void);
__declspec(dllexport) void noble_d3d8_present(void)
{
    noble_d3d8_present_host();
}
__declspec(dllexport) void noble_d3d8_begin_scene(void) { D3DDevice_BeginScene(); }
__declspec(dllexport) void noble_d3d8_end_scene(void) { D3DDevice_EndScene(); }
__declspec(dllexport) void noble_d3d8_viewport(unsigned viewport)
{
    D3DDevice_SetViewport((const D3DVIEWPORT8 *)noble_guest_pointer(viewport));
}
__declspec(dllexport) void noble_d3d8_render_target(unsigned target, unsigned depth)
{
    D3DDevice_SetRenderTarget((D3DSurface *)noble_guest_pointer(target),
                              (D3DSurface *)noble_guest_pointer(depth));
}
__declspec(dllexport) void noble_d3d8_block_vblank(void)
{
    D3DDevice_BlockUntilVerticalBlank();
}


void noble_d3d8_draw_up_host(unsigned primitive, unsigned count, const void *vertices, unsigned stride);
void noble_d3d8_draw_indexed_up_host(unsigned primitive, unsigned count, const WORD *indices,
                                    const void *vertices, unsigned stride);
int noble_d3d8_readback_host(D3DSurface *surface);
__declspec(dllexport) void noble_d3d8_draw_up(unsigned primitive, unsigned count,
                                             unsigned data, unsigned stride)
{
    noble_d3d8_sync_state();
    noble_d3d8_draw_up_host(primitive, count, noble_guest_pointer(data), stride);
}
__declspec(dllexport) void noble_d3d8_draw_indexed_up(unsigned primitive, unsigned count,
                                unsigned indices, unsigned data, unsigned stride)
{
    noble_d3d8_sync_state();
    noble_d3d8_draw_indexed_up_host(primitive, count, (const WORD *)noble_guest_pointer(indices),
                                    noble_guest_pointer(data), stride);
}
__declspec(dllexport) int noble_d3d8_readback(unsigned surface)
{
    return noble_d3d8_readback_host((D3DSurface *)noble_guest_pointer(surface));
}

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE type);
void WINAPI D3DDevice_End(void);
void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d);
void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b);
void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color);
void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address);
void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address);
static float guest_float_bits(unsigned bits)
{
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}
__declspec(dllexport) void noble_d3d8_begin(unsigned type)
{
    D3DDevice_Begin((D3DPRIMITIVETYPE)type);
}
__declspec(dllexport) void noble_d3d8_end(void)
{
    noble_d3d8_sync_state();
    D3DDevice_End();
}
__declspec(dllexport) void noble_d3d8_vertex4f(unsigned index, unsigned x, unsigned y, unsigned z, unsigned w)
{
    D3DDevice_SetVertexData4f((INT)index, guest_float_bits(x), guest_float_bits(y),
                            guest_float_bits(z), guest_float_bits(w));
}
__declspec(dllexport) void noble_d3d8_vertex2f(unsigned index, unsigned x, unsigned y)
{
    D3DDevice_SetVertexData2f((INT)index, guest_float_bits(x), guest_float_bits(y));
}
__declspec(dllexport) void noble_d3d8_vertex_color(unsigned index, unsigned argb)
{
    D3DDevice_SetVertexDataColor((INT)index, argb);
}
__declspec(dllexport) void noble_d3d8_load_vertex_shader(unsigned handle, unsigned address)
{
    D3DDevice_LoadVertexShader(handle, address);
}
__declspec(dllexport) void noble_d3d8_select_vertex_shader(unsigned handle, unsigned address)
{
    D3DDevice_SelectVertexShader(handle, address);
}
