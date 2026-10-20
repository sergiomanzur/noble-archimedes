# Xbox D3D8 guest ABI audit

Audited from translated guest instructions on 2026-10-20. Addresses apply to this title binary. `cleanup` below includes the guest return address: a guest `ret N` advances `g_esp` by `N + 4`. Arguments are DWORD stack slots starting at entry ESP+4 unless registers are explicitly listed. Generated `CC` and parameter-count comments are unreliable.

| Entry | Operation / arguments in entry order | cleanup | Instruction evidence |
| --- | --- | ---: | --- |
| 003B6520 | Swap(flags) | 8 | Flags default to 5; calls 003B61E0 to rotate buffers at device+1A18/+1A1C; advances swap counter +2478; tail 003B65D0 returns that counter in EAX. |
| 003B66D0 | Clear(count, rects, flags, color, zBits, stencil) | 28 | Clear body consumes six stack arguments. |
| 003B6FB0 | SetRenderTarget(colorSurface, depthSurface) | 12 | Assigns device+1A04 and +1A08; retains/releases resource references; used during device initialization. This is not Present. |
| 003B6B80 | SetTransform(state, matrix) | 12 | `recomp_stubs_unresolved.c`: copies 16 DWORDs to device+C60+64*state; state 1 computes projection/depth parameters. |
| 003B75F0 | SetViewport(viewport) | 8 | Tails 003B7621/7627 copy X/Y/Width/Height/MinZ/MaxZ into device+EE0..EF4 and reset scissors; terminal ret 4. |
| 003B7820 | SetTexture(stage, texture) | 12 | Resource bindings at device+F88+4*stage; texture push method 1B0C+64*stage. |
| 003B79D0 | SetPalette(stage, palette) | 12 | Resource bindings at device+F98+4*stage; palette method 1B20+64*stage. |
| 003B7A60 | SetIndices(indexBuffer, baseVertex) | 12 | Stores indexBuffer at device+7A0, baseVertex at device+1C, buffer Data at global 003C6AF4; tails 003B7A8A return with ret 8. |
| 003B7B60 | SetTile(index, tileDescriptor) | 12 | Copies six DWORDs into device+1AC0+24*index. Helper 003BC1B2 programs tile base/limit/pitch MMIO at +100240/+400900. This is not SetStreamSource. |
| 003B7C40 | SetScissors(count, exclusive, rectangles) | 16 | Tail 003B7DAF copies count*16 bytes to device+1B80; stores count/exclusive at +1C00/+1C04; ret 12. |
| 003B86E0 | CreateVertexShader(declaration, function, outHandle, usage) | 20 | Allocates shader object and writes output through third argument; ret 16. |
| 003B8820 | SetVertexShaderConstant1: ECX=register, EDX=data | 4 | Copies 16 bytes to global 003C5EE0+16*register and emits constant method. |
| 003B8880 | SetVertexShaderConstant4: ECX=register, EDX=data | 4 | Copies 64 bytes / four float4 constants to the same shadow array. |
| 003B8930 | Constant upload: ECX=register, EDX=data, stack=dwordCount | 8 | Copies dwordCount DWORDs to pushbuffer. Stack count is DWORDs, not float4 registers. |
| 003B8A10 | Constant shadow/upload: ECX=register, EDX=data, stack=dwordCount | 8 | Copies dwordCount DWORDs to shadow array then calls 003B8930. |
| 003B8A50 | SetStreamSource(stream, vertexBuffer, stride) | 16 | Writes globals 003C5BF0+12*stream=vertexBuffer and 003C5BE8+12*stream=stride. Callsite 0003FB9E passes (0,vb,0x24). |
| 003B8AE0 | LoadVertexShader(handle, address) | 12 | Reads native object at handle-1; uploads object+C DWORDs from object+114 using method1E9C; address is the second argument. |
| 003B8B40 | SelectVertexShader(handle, address) | 12 | Nonzero handle updates device+794=handle-1 and +798=handle. Always sets device+79C=address and emits method1EA0. |
| 003B8E30 | SetVertexShader(handle) | 8 | Distinguishes tagged object handle from FVF and updates device shader state; ret 4. |
| 003B8FD0 | Emit simple render method: ECX=method, EDX=value | 4 | Writes two DWORDs directly to pushbuffer. The wrapper 003B9000(state,value) also updates 003C6D00+4*state after this call. |
| 003BA740 | DrawVerticesUP(type, vertexCount, vertexData, stride) | 20 | Reads all four args; type emits method17FC; tail 003BA7D4 copies vertex data using stride; ret 16. |
| 003BA8B0 | DrawIndexedVerticesUP(type, indexCount, indexData, vertexData, stride) | 24 | Tail 003BA937 reads uint16 indices, multiplies by fifth argument stride, adds fourth argument vertexData, copies vertices; ret 20. |
| 003BA9F0 | DrawIndexedVertices(type, indexCount, indexData) | 16 | Copies packed uint16 indices into method1800, handles odd index with method1808; terminal 003BAC41 ret 12. Callsite 0003FBBD uses global003C6AF4 as indexData. |
| 003BAC50 | SetVertexData2f(index, xBits, yBits) | 16 | Writes two float DWORDs using method1880+8*index. |
| 003BAC90 | SetVertexData4f(index, xBits, yBits, zBits, wBits) | 24 | Writes four float DWORDs using method1A00+16*index; index FFFFFFFF selects method1518 and completes a vertex. |
| 003BACF0 | SetVertexDataColor(index, argb) | 12 | Writes method1940+4*index with the red/blue bytes exchanged. |
| 003BAD40 | Begin(type) | 8 | Writes method17FC=type and sets device flag800. |
| 003BAD80 | End() | 4 | Writes method17FC=0 and clears device flags1800. |
| 003BAE20 | SetPixelShaderProgram(program) | 8 | Stores program at device+784 and updates pixel shader state; ret 4. |

`recomp_0074.c` owns most entry bodies; continuations and SetTransform are in `recomp_stubs_unresolved.c`. No standalone non-UP DrawVertices entry was established by this audit. All observed method17FC writes in those sources belong to UP/indexed draws or immediate Begin/End. Do not assign a guessed address.

Immediate drawing is used by actual game code: `recomp_0070.c` calls Begin(8) at 00393010, End at 0039304E, color register 3 at 003931A1, and vertex register FFFFFFFF at 003932C7/003932DF/00393301/00393323. Hooking only the three bulk draw entries does not cover this route. The host already implements D3DDevice_Begin/End/SetVertexData2f/4f/Color and LoadVertexShader/SelectVertexShader in `d3d8_gl.c`.

## Native FVF declaration

Helper 003B81E0(outObject, fvf), cleanup12, generates the native declaration. SetVertexShader(FVF) uses the static object at 003C5DC0, then stores its address at device+794. Each input register has a 16-byte descriptor: data byte offset at object+18+16*register and packed format at object+1C+16*register. All packed formats initialize to 2 (no components). Format high nibble is component count; low nibble 2 means float, low nibble 0 means color.

| FVF input | Register / format / offset rule |
| --- | --- |
| FVF&0xE == 4 | Position r0, format42 (float4), object flags+4 bit2. Total position bytes16. |
| Other nonzero position | Position r0, format32 (float3). Position bytes12. |
| Blend position FVF&0xE >= 6 | Weights r1 at offset12, count=((FVF&0xE)-6)/2+1; format=(count<<4)|2. |
| FVF bit10 | Normal r2, format32, 12 bytes after position/weights. |
| FVF bit40 | Diffuse r3, format40, 4 bytes; object flags+4 bit400. |
| FVF bit80 | Specular r4, format40, 4 bytes; object flags+4 bit800. |
| Texture coordinates | Registers9 onward; count=(FVF>>8)&F. Packed two-bit size codes from FVF>>16 map to component counts (2,3,4,1). Original XBE DWORD table at003C4870 was read to confirm this order. Each coordinate adds 4*componentCount bytes. |

Object+10 stores texture component counts in consecutive byte lanes. Native shader handles are object-address+1; they are not interchangeable with independently allocated host shader handles. A host mapping must retain the association if guest CreateVertexShader is allowed to write its own output handle after a mirrored host creation.

## Shader initialization and constant synchronization

The game creates its device at 00387457, swaps at 00387497, then calls shader initialization 003910A0 at 0038749C. That helper creates 219 shaders through 003B86E0 (loops of 8, 141, 4, 20, 20, 20, one standalone shader, and 5). A 512-entry map covers this initial set. Device initialization 003C1050 initializes the static FVF object 003C5DC0 and calls SetVertexShader(4), then SetVertexShader(2); no programmable CreateVertexShader call was found inside initialization. No separate programmable allocation path was established.

SetVertexShader's nested LoadVertexShader/SelectVertexShader calls occur on its tagged programmable-handle branch and receive that same handle with address0. Its even FVF branch builds the static declaration directly. Device+798 is the guest current handle, +794 the native declaration object, and +79C the selected program address.

Constant ECX values are raw hardware slots. Subtracting96 when invoking the host API is correct because the host adds its96 bias. UI callsites use ECX=BC/BE/BF. The shadow at003C5EE0 contains192 float4 constants. SetVertexShaderConstant1/4 and the 003B8A10 wrapper update that shadow. Native LoadVertexShader additionally calls003B8320; continuations003B836C/003B83B9 copy embedded methodB80 constant bundles directly into the same shadow without calling any intercepted constant setter. The host declaration parser currently skips CONSTMEM payloads. Consequently setter mirroring alone misses these embedded constants; synchronize the shadow after native shader loading or before draws.

When synchronizing the full shadow, preserve or recompute reserved host constants c[-38]/c[-37] (raw slots58/59). Host viewport_update_constants derives these from viewport/depth state; blindly replacing them with potentially stale shadow values can invalidate screen mapping. The host constants_store function compares each float4 and updates serials only when data changed, so repeated shadow synchronization need not force unchanged GPU uniform uploads.

## Lifecycle and state pitfalls

- A deterministic HLE transition can use the first Clear with a valid initialized guest device and successful host initialization. A ten-second wall-clock gate is unnecessary and makes runs machine-dependent. SetRenderTarget occurs during initialization and must not be used as an activation boundary.
- Guest state must remain coherent when suppressing translated setters: preserve viewport, transform, stream, indices, texture/palette and shader shadow data used by later guest code. Alternatively forward to host while retaining the guest implementation until the first rendering boundary, then explicitly synchronize host state from the guest shadow data.
- Intercepting Swap bypasses guest tail 003B65F9, where the current frame-pacing hook lives. Pace exactly once on the selected host path; return the swap counter rather than assuming HRESULT semantics.
- Draw counts are vertex/index counts, not primitive counts. Xbox primitive enum values appear directly in pushbuffer method17FC (callsites use 5 for triangle lists); do not interpret these as desktop D3D enum values without conversion.
- DrawIndexedVertices receives a pointer to uint16 index data. It does not receive a desktop DrawIndexedPrimitive start-index parameter. SetIndices supplies a separate base-vertex value.
- User-pointer draws receive raw vertex data, not a resource object. Reading vertexData as a vertex-buffer header corrupts interpretation.
- Device+1AC0 is tile state; stream source objects and strides live in global arrays at 003C5BF0 and 003C5BE8 with 12-byte entries.
- SetTile manipulates MMIO tiling configuration and should remain on the guest path until equivalent host tiling behavior is deliberately implemented.

This audit did not launch the game and makes no visual-output or frame-rate claim.
