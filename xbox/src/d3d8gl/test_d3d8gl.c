#include "platform.h"

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type,
                                     void *unused, DWORD behavior_flags,
                                     D3DPRESENT_PARAMETERS *presentation,
                                     D3DDevice **returned_device);
void WINAPI D3DDevice_Clear(DWORD count, const D3DRECT *rectangles, DWORD flags,
                            D3DCOLOR color, float z, DWORD stencil);
void WINAPI D3DDevice_Present(const RECT *source, const RECT *destination,
                              void *unused, void *unused2);

int main(void)
{
    D3DPRESENT_PARAMETERS presentation;
    D3DDevice *device = 0;
    int frame;
    memset(&presentation, 0, sizeof(presentation));
    presentation.BackBufferWidth = 640;
    presentation.BackBufferHeight = 480;
    presentation.BackBufferFormat = D3DFMT_LIN_A8R8G8B8;
    presentation.EnableAutoDepthStencil = TRUE;
    presentation.AutoDepthStencilFormat = D3DFMT_LIN_D24S8;
    if ((Direct3D_CreateDevice(0, 0, 0, 0, &presentation, &device)) < 0) {
        platform_log("test: Direct3D_CreateDevice failed");
        return 1;
    }
    for (frame = 0; frame < 180; frame++) {
        D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                        ((D3DCOLOR)((20 + frame) << 16 | (40 << 8) | 80)), 1.0f, 0);
        D3DDevice_Present(NULL, NULL, NULL, NULL);
    }
    platform_log("test: 180 frames presented");
    return 0;
}
