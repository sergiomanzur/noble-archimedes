#ifndef NOBLE_D3D8_HLE_H
#define NOBLE_D3D8_HLE_H

#include <stdint.h>

/* Called at the start of a guest D3D8 entry point.  Returns nonzero when the
 * call was handled; the caller must then return, since g_esp has already been
 * advanced by the original function's clean-up. */
int noble_d3d8_guest_call(uint32_t va, uint32_t cleanup);

/* Load d3d8gl.dll and create its GL device.  Returns nonzero on success. */
int noble_d3d8_init(void);

/* Host-owned main-thread event pump, independent of renderer selection. */
void noble_d3d8_set_host_input_pump(void (*pump)(void));

#endif
