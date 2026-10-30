#pragma once
#include <d3d12.h>

// The emulation fixture of test.cpp, reused by the MeshPerformanceDiagnostics checks.
struct ProfFixture {
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    ID3D12RootSignature* emuRs;
    ID3D12PipelineState* emuPso;
    ID3D12CommandSignature* sig;  // DISPATCH_MESH
    ID3D12Resource* argBuf;       // (2,1,1) at offset 0
    D3D12_GPU_VIRTUAL_ADDRESS cbVa;  // MS indirect constants, MS direct at +256, PS at +512
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12Resource* rt;  // in COPY_SOURCE on entry and exit
    ID3D12Resource* rtRb;
    UINT width, height;
    ID3D12RootSignature* csRs;  // b0 constant, u0 root UAV
    ID3D12PipelineState* csPso;
    D3D12_SHADER_BYTECODE csCode;  // csPso's shader
};
int TestMeshProfiler(const ProfFixture& f);
int TestPassProfiler(const ProfFixture& f);  // pass_test.cpp
