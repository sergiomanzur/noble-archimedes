// Compile the renderer's actual shader variants without creating a window.
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include "gpu/shaders.h"

using Microsoft::WRL::ComPtr;

static std::string Disassemble(const gpu::PixelKey& key) {
    const auto source = gpu::PixelHlsl(key);
    ComPtr<ID3DBlob> bytecode, diagnostics, assembly;
    if (FAILED(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &bytecode, &diagnostics))) {
        if (diagnostics) std::fputs(static_cast<const char*>(diagnostics->GetBufferPointer()), stderr);
        std::exit(1);
    }
    if (FAILED(D3DDisassemble(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), 0, nullptr, &assembly)))
        std::exit(1);
    return std::string(static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize());
}

int main() {
    for (bool programmable : {false, true}) {
        gpu::PixelKey key{};
        key.quad = true;
        key.programmable = programmable;
        key.tex_modes = 1;      // projected 2D texture
        key.final_abcd = 8;     // texture 0 as final combiner D
        key.ff[0][0] = 2;      // select arg 1
        key.ff[0][1] = 2;      // texture
        key.tex[0] = {gpu::kTex2D, 1, 0, gpu::kTexTarget};
        const auto ordinary = Disassemble(key);
        if (ordinary.find("deriv_") == std::string::npos || ordinary.find("sample_indexable") == std::string::npos) {
            std::fputs("ordinary texture sampling did not exercise the derivative path\n", stderr);
            return 1;
        }
        key.tex[0].flags |= gpu::kTexFrameCopy;  // screen-aligned frame copy variant
        const auto copy = Disassemble(key);
        if (copy.find("deriv_") != std::string::npos || copy.find("sample_l") == std::string::npos) {
            std::fputs("frame copy did not use derivative-free base-level sampling\n", stderr);
            return 1;
        }
    }
    std::puts("programmable and fixed-function frame copies compile without texture derivatives");
}
