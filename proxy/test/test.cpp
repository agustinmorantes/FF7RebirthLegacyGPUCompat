// Loads the proxy the way the game does (d3d12.dll next to the exe) and checks the spoof and
// skip paths on hardware without mesh shaders. Prints PASS/FAIL per check; exit code = failures.
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <share.h>
#include "test_ms.h"
#include "test_ps.h"
#include "test_half.h"
#include "test_emu_ms.h"
#include "test_emu_ps.h"
#include "test_emu_cs.h"
#include "prof_test.h"

// Same Agility SDK request as ff7rebirth_.exe. Without a .\D3D12\ folder the OS core is used.
extern "C" __declspec(dllexport) extern const UINT D3D12SDKVersion = 614;
extern "C" __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";

static int g_failures;
int TestDrawBounds(ID3D12Device* dev);
int TestRetryPasses(ID3D12Device* dev);
int TestPrimitiveOutputs(ID3D12Device* dev);
static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value;
};

int main() {
    ID3D12Device2* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&dev));
    Check(SUCCEEDED(hr), "D3D12CreateDevice at FL 12_2");
    if (!dev) return 1;

    wchar_t path[MAX_PATH] = L"(not loaded)";
    if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll")) GetModuleFileNameW(core, path, MAX_PATH);
    printf("info  D3D12Core: %ls\n", path);

    D3D_FEATURE_LEVEL req = D3D_FEATURE_LEVEL_12_2;
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {1, &req};
    hr = dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    Check(SUCCEEDED(hr) && fl.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_12_2, "FEATURE_LEVELS {12_2} -> 12_2");

    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
    hr = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    Check(SUCCEEDED(hr) && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6, "SHADER_MODEL >= 6.6");

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    printf("info  reported MeshShaderTier=%d\n", o7.MeshShaderTier);
    Check(SUCCEEDED(hr), "OPTIONS7 query");

    // Empty root signature for the MS PSO.
    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    ID3DBlob* blob = nullptr;
    ID3D12RootSignature* rs = nullptr;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    blob->Release();
    Check(SUCCEEDED(hr), "CreateRootSignature");

    // Root signature with mesh/amplification visibility sharing registers with the pixel stage.
    {
        D3D12_ROOT_PARAMETER1 p[3] = {};
        for (auto& q : p) q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
        p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
        D3D12_STATIC_SAMPLER_DESC s = {};
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {D3D_ROOT_SIGNATURE_VERSION_1_1};
        v.Desc_1_1 = {3, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ID3DBlob* msBlob = nullptr;
        ID3D12RootSignature* msRs = nullptr;
        hr = D3D12SerializeVersionedRootSignature(&v, &msBlob, nullptr);
        Check(SUCCEEDED(hr), "serialize mesh-visible root signature");
        hr = dev->CreateRootSignature(0, msBlob->GetBufferPointer(), msBlob->GetBufferSize(), IID_PPV_ARGS(&msRs));
        msBlob->Release();
        Check(SUCCEEDED(hr) && msRs, "CreateRootSignature with MS/AS visibility");
        if (msRs) msRs->Release();
    }

    struct {
        Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rs;
        Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
        Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
        Sub<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
        Sub<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> mask;
        Sub<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
        Sub<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rtv;
        Sub<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
    } stream = {};
    stream.rs.value = rs;
    stream.ms.value = {g_testMS, sizeof(g_testMS)};
    stream.ps.value = {g_testPS, sizeof(g_testPS)};
    stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.mask.value = UINT_MAX;
    stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
    stream.rtv.value.NumRenderTargets = 1;
    stream.rtv.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    stream.sample.value.Count = 1;
    D3D12_PIPELINE_STATE_STREAM_DESC sd = {sizeof(stream), &stream};
    ID3D12PipelineState* msPso = nullptr;
    hr = dev->CreatePipelineState(&sd, IID_PPV_ARGS(&msPso));
    Check(SUCCEEDED(hr) && msPso, "MS PSO creation returns an object (skip token on no-MS hardware)");

    D3D12_INDIRECT_ARGUMENT_DESC arg = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
    D3D12_COMMAND_SIGNATURE_DESC csd = {sizeof(D3D12_DISPATCH_MESH_ARGUMENTS), 1, &arg, 0};
    ID3D12CommandSignature* sig = nullptr;
    hr = dev->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&sig));
    Check(SUCCEEDED(hr) && sig, "DISPATCH_MESH command signature returns an object");
    if (!msPso || !sig) return g_failures;

    // Indirect args buffer.
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ID3D12Resource* argBuf = nullptr;
    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                 IID_PPV_ARGS(&argBuf));
    // Mesh dispatch args (2,3,1), copied in each frame so the proxy's args probe has data to read.
    D3D12_HEAP_PROPERTIES upHeap = {D3D12_HEAP_TYPE_UPLOAD};
    ID3D12Resource* argUp = nullptr;
    dev->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                 IID_PPV_ARGS(&argUp));
    UINT* argData = nullptr;
    argUp->Map(0, nullptr, (void**)&argData);
    argData[0] = 2, argData[1] = 3, argData[2] = 1;
    argUp->Unmap(0, nullptr);

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList6* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (UINT64 frame = 1; frame <= 2; ++frame) {
        if (frame > 1) {
            alloc->Reset();
            cl->Reset(alloc, msPso);  // token as the initial PSO
        }
        D3D12_RESOURCE_BARRIER b = {};
        b.Transition = {argBuf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_COPY_DEST};
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(argBuf, 0, argUp, 0, 12);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        cl->ResourceBarrier(1, &b);
        cl->SetGraphicsRootSignature(rs);
        cl->SetPipelineState(msPso);
        cl->DispatchMesh(1, 1, 1);
        cl->ExecuteIndirect(sig, 1, argBuf, 0, nullptr, 0);
        cl->DrawInstanced(3, 1, 0, 0);
        hr = cl->Close();
        Check(SUCCEEDED(hr), "command list Close");
        queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
        queue->Signal(fence, frame);
        fence->SetEventOnCompletion(frame, ev);
        Check(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0, "GPU work completes");
    }
    Check(dev->GetDeviceRemovedReason() == S_OK, "device not removed");

    // 16-bit CS through a PSO stream: the proxy swaps in the converted shader, which must read
    // the R16_FLOAT buffer correctly. Without the swap the PSO is a skip token and Out stays 0.
    {
        const unsigned short halfs[4] = {0x3C00, 0xC100, 0x3800, 0x7BFF};  // 1, -2.5, 0.5, 65504
        const float expected[4] = {2.0f, -5.0f, 1.0f, 131008.0f};
        D3D12_HEAP_PROPERTIES upload = {D3D12_HEAP_TYPE_UPLOAD}, readback = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd = bd;
        rd.Width = sizeof(halfs);
        ID3D12Resource *in = nullptr, *out = nullptr, *rb = nullptr;
        dev->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&in));
        void* p = nullptr;
        in->Map(0, nullptr, &p);
        memcpy(p, halfs, sizeof(halfs));
        in->Unmap(0, nullptr);
        rd.Width = sizeof(expected);
        dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&rb));
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&out));

        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        ID3D12DescriptorHeap* dh = nullptr;
        dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dh));
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = dh->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {DXGI_FORMAT_R16_FLOAT, D3D12_SRV_DIMENSION_BUFFER,
                                               D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        srv.Buffer.NumElements = 4;
        dev->CreateShaderResourceView(in, &srv, cpu);
        cpu.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {DXGI_FORMAT_R32_FLOAT, D3D12_UAV_DIMENSION_BUFFER};
        uav.Buffer.NumElements = 4;
        dev->CreateUnorderedAccessView(out, nullptr, &uav, cpu);

        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
        D3D12_ROOT_PARAMETER param = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
        param.DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC csRsDesc = {1, &param};
        ID3DBlob* csBlob = nullptr;
        ID3D12RootSignature* csRs = nullptr;
        D3D12SerializeRootSignature(&csRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &csBlob, nullptr);
        dev->CreateRootSignature(0, csBlob->GetBufferPointer(), csBlob->GetBufferSize(), IID_PPV_ARGS(&csRs));
        csBlob->Release();

        struct {
            Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rs;
            Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS> cs;
        } csStream = {};
        csStream.rs.value = csRs;
        csStream.cs.value = {g_testHalfCS, sizeof(g_testHalfCS)};
        D3D12_PIPELINE_STATE_STREAM_DESC csd2 = {sizeof(csStream), &csStream};
        ID3D12PipelineState* csPso = nullptr;
        hr = dev->CreatePipelineState(&csd2, IID_PPV_ARGS(&csPso));
        Check(SUCCEEDED(hr) && csPso, "16-bit CS PSO creation");

        alloc->Reset();
        cl->Reset(alloc, nullptr);
        cl->SetDescriptorHeaps(1, &dh);
        cl->SetComputeRootSignature(csRs);
        cl->SetComputeRootDescriptorTable(0, dh->GetGPUDescriptorHandleForHeapStart());
        cl->SetPipelineState(csPso);
        D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cl->ResourceBarrier(1, &b);
        cl->Dispatch(1, 1, 1);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        cl->CopyResource(rb, out);
        cl->Close();
        queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
        queue->Signal(fence, 3);
        fence->SetEventOnCompletion(3, ev);
        Check(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0, "16-bit CS dispatch completes");

        float got[4] = {};
        rb->Map(0, nullptr, &p);
        memcpy(got, p, sizeof(got));
        rb->Unmap(0, nullptr);
        printf("info  16-bit CS output %g %g %g %g\n", got[0], got[1], got[2], got[3]);
        Check(memcmp(got, expected, sizeof(got)) == 0, "16-bit CS reads R16_FLOAT values correctly");

        if (csPso) csPso->Release();
        csRs->Release();
        dh->Release();
        out->Release();
        rb->Release();
        in->Release();
    }

    // Mesh-shader emulation through the converted pair in legacygpu_shaders: an ExecuteIndirect of
    // (2,1,1) groups fills the target (ids 10, 11), then a DispatchMesh(1,1,1) with x offset 1 and
    // id base 20 covers the top-right quarter. A compute root signature, constant and UAV set
    // before must survive both, so a dispatch after them writes the constant.
    {
        // b0 visible to the pixel and the mesh stage: creation needs the proxy's retry.
        D3D12_ROOT_PARAMETER1 p[2] = {};
        for (auto& q : p) q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {D3D_ROOT_SIGNATURE_VERSION_1_1};
        v.Desc_1_1 = {2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS};
        ID3DBlob* b = nullptr;
        ID3D12RootSignature* emuRs = nullptr;
        D3D12SerializeVersionedRootSignature(&v, &b, nullptr);
        hr = dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&emuRs));
        b->Release();
        Check(SUCCEEDED(hr) && emuRs, "emulation: mesh root signature");

        D3D12_ROOT_PARAMETER cp[2] = {};
        cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cp[0].Constants.Num32BitValues = 1;
        cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC cd = {2, cp};
        ID3D12RootSignature* csRs = nullptr;
        D3D12SerializeRootSignature(&cd, D3D_ROOT_SIGNATURE_VERSION_1, &b, nullptr);
        dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&csRs));
        b->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {csRs, {g_emuCS, sizeof(g_emuCS)}};
        ID3D12PipelineState* csPso = nullptr;
        dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&csPso));

        stream.rs.value = emuRs;
        stream.ms.value = {g_emuMS, sizeof(g_emuMS)};
        stream.ps.value = {g_emuPS, sizeof(g_emuPS)};
        ID3D12PipelineState* emuPso = nullptr;
        hr = dev->CreatePipelineState(&sd, IID_PPV_ARGS(&emuPso));
        Check(SUCCEEDED(hr) && emuPso, "emulation: MS PSO");

        const UINT W = 64, H = 16;
        D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ID3D12Resource* rt = nullptr;
        dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                     IID_PPV_ARGS(&rt));
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        ID3D12DescriptorHeap* rtvHeap = nullptr;
        dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(rt, nullptr, rtv);

        // Constants: MS for the indirect draw, MS for the direct one, PS. 256-byte slots.
        D3D12_HEAP_PROPERTIES upload = {D3D12_HEAP_TYPE_UPLOAD}, readback = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd = bd;
        rd.Width = 768;
        ID3D12Resource *cb = nullptr, *out = nullptr, *outRb = nullptr, *rtRb = nullptr;
        dev->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&cb));
        float* c = nullptr;
        cb->Map(0, nullptr, (void**)&c);
        const float msInd[4] = {0, 10, 0.4f, -1}, msDir[4] = {1, 20, 0.4f, 0}, ps[4] = {0.25f};
        memcpy(c, msInd, 16), memcpy(c + 64, msDir, 16), memcpy(c + 128, ps, 16);
        cb->Unmap(0, nullptr);
        UINT* a = nullptr;
        argUp->Map(0, nullptr, (void**)&a);
        a[4] = 2, a[5] = 1, a[6] = 1;
        argUp->Unmap(0, nullptr);
        rd.Width = 256;
        dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&outRb));
        rd.Width = 256 * H;
        dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&rtRb));
        rd.Width = 256;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&out));

        if (emuPso && emuRs && csPso) {
            alloc->Reset();
            cl->Reset(alloc, nullptr);
            D3D12_RESOURCE_BARRIER br = {};
            br.Transition = {argBuf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_COPY_DEST};
            cl->ResourceBarrier(1, &br);
            cl->CopyBufferRegion(argBuf, 0, argUp, 16, 12);
            br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            br.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            cl->ResourceBarrier(1, &br);
            br.Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            cl->ResourceBarrier(1, &br);

            cl->SetComputeRootSignature(csRs);
            cl->SetComputeRoot32BitConstant(0, 0x1234, 0);
            cl->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());

            const float black[4] = {};
            cl->ClearRenderTargetView(rtv, black, 0, nullptr);
            cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
            D3D12_RECT sc = {0, 0, (LONG)W, (LONG)H};
            cl->RSSetViewports(1, &vp);
            cl->RSSetScissorRects(1, &sc);
            cl->SetGraphicsRootSignature(emuRs);
            D3D12_GPU_VIRTUAL_ADDRESS cbVa = cb->GetGPUVirtualAddress();
            cl->SetGraphicsRootConstantBufferView(0, cbVa + 512);
            cl->SetGraphicsRootConstantBufferView(1, cbVa);
            cl->SetPipelineState(emuPso);
            cl->ExecuteIndirect(sig, 1, argBuf, 0, nullptr, 0);
            cl->SetGraphicsRootConstantBufferView(1, cbVa + 256);
            cl->DispatchMesh(1, 1, 1);

            cl->SetPipelineState(csPso);
            cl->Dispatch(1, 1, 1);
            br.Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE};
            cl->ResourceBarrier(1, &br);
            cl->CopyBufferRegion(outRb, 0, out, 0, 4);
            br.Transition = {rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
            cl->ResourceBarrier(1, &br);
            D3D12_TEXTURE_COPY_LOCATION dst = {rtRb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {rt};
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256};
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            hr = cl->Close();
            Check(SUCCEEDED(hr), "emulation: command list Close");
            queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
            queue->Signal(fence, 4);
            fence->SetEventOnCompletion(4, ev);
            Check(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0, "emulation: GPU work completes");
            Check(dev->GetDeviceRemovedReason() == S_OK, "emulation: device not removed");

            const unsigned char* px = nullptr;
            rtRb->Map(0, nullptr, (void**)&px);
            auto pixel = [&](UINT x, UINT y) { return px + y * 256 + x * 4; };
            auto matches = [](const unsigned char* q, int r, int g, int b) {
                return abs(q[0] - r) <= 1 && abs(q[1] - g) <= 1 && abs(q[2] - b) <= 1 && q[3] == 255;
            };
            struct { UINT x, y; int id; const char* what; } probes[] = {
                {8, 4, 10, "emulation: indirect group 0 drawn (left)"},
                {56, 12, 11, "emulation: indirect group 1 drawn (bottom right)"},
                {56, 4, 20, "emulation: DispatchMesh drawn over it (top right)"}};
            for (auto& q : probes) {
                const unsigned char* c4 = pixel(q.x, q.y);
                printf("info  pixel (%u,%u) = %u %u %u %u\n", q.x, q.y, c4[0], c4[1], c4[2], c4[3]);
                Check(matches(c4, q.id, 102, 64), q.what);
            }
            rtRb->Unmap(0, nullptr);
            UINT* o = nullptr;
            outRb->Map(0, nullptr, (void**)&o);
            printf("info  compute after emulation wrote 0x%x\n", *o);
            Check(*o == 0x1234, "emulation: compute root signature and arguments restored");
            outRb->Unmap(0, nullptr);

            // Scratch does not decay between lists in a single submission. Repeat the two
            // emulated passes across two lists, after the previous submission has decayed.
            ID3D12CommandAllocator* secondAlloc = nullptr;
            ID3D12GraphicsCommandList6* second = nullptr;
            hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&secondAlloc));
            Check(SUCCEEDED(hr), "emulation two lists: second allocator");
            if (secondAlloc)
                hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, secondAlloc, nullptr,
                                            IID_PPV_ARGS(&second));
            Check(SUCCEEDED(hr) && second, "emulation two lists: second list");
            if (second) {
                alloc->Reset();
                cl->Reset(alloc, nullptr);
                br.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
                cl->ResourceBarrier(1, &br);
                cl->ClearRenderTargetView(rtv, black, 0, nullptr);
                for (ID3D12GraphicsCommandList6* list : {cl, second}) {
                    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    list->RSSetViewports(1, &vp);
                    list->RSSetScissorRects(1, &sc);
                    list->SetGraphicsRootSignature(emuRs);
                    list->SetGraphicsRootConstantBufferView(0, cbVa + 512);
                    list->SetGraphicsRootConstantBufferView(1, cbVa + (list == cl ? 0 : 256));
                    list->SetPipelineState(emuPso);
                }
                cl->ExecuteIndirect(sig, 1, argBuf, 0, nullptr, 0);
                Check(SUCCEEDED(cl->Close()), "emulation two lists: first list Close");
                second->DispatchMesh(1, 1, 1);
                br.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                second->ResourceBarrier(1, &br);
                second->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Check(SUCCEEDED(second->Close()), "emulation two lists: second list Close");
                ID3D12CommandList* lists[] = {cl, second};
                queue->ExecuteCommandLists(2, lists);
                queue->Signal(fence, 5);
                fence->SetEventOnCompletion(5, ev);
                Check(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0, "emulation two lists: GPU work completes");
                Check(dev->GetDeviceRemovedReason() == S_OK, "emulation two lists: device not removed");
                rtRb->Map(0, nullptr, (void**)&px);
                for (auto& q : probes)
                    Check(matches(pixel(q.x, q.y), q.id, 102, 64), "emulation two lists: pixel readback matches");
                rtRb->Unmap(0, nullptr);
                second->Release();
            }
            if (secondAlloc) secondAlloc->Release();

            cl->Reset(alloc, nullptr);  // ends cl's recorded generation, as a game's next frame would
            cl->Close();
            ProfFixture fx = {dev, queue, emuRs, emuPso, sig, argBuf, cbVa, rtv, rt, rtRb, W, H, csRs, csPso, {g_emuCS, sizeof(g_emuCS)}};
            if (second) g_failures += TestMeshProfiler(fx);
            if (second) g_failures += TestPassProfiler(fx);

            // Many calls: 528 single-pass calls in one list, each resetting the shared header.
            // Call (k, s) covers rows 0..k and columns s..s+31; k falls and s rises, so each call
            // stays visible on some pixel and a header that wasn't reset (cumulative indices redrawing
            // earlier quads) changes the image. Variant 1 records calls 200..259 under a passing predicate.
            {
                constexpr UINT kRingCalls = 16 * 33;
                ID3D12Resource* ringCb = nullptr;
                rd.Flags = D3D12_RESOURCE_FLAG_NONE;
                rd.Width = (kRingCalls + 1) * 256;
                hr = dev->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&ringCb));
                Check(SUCCEEDED(hr), "many calls: constants");
                unsigned char expected[16][64] = {}, expectedBlue[16][64] = {};
                if (ringCb) {
                    float* rc = nullptr;
                    ringCb->Map(0, nullptr, (void**)&rc);
                    memset(rc, 0, (kRingCalls + 1) * 256);  // the last slot stays zero: the predicate
                    for (UINT i = 0; i < kRingCalls; ++i) {
                        UINT k = 15 - i / 33, s = i % 33, id = i * 7 % 250 + 3;
                        const float v[4] = {s / 32.0f, (float)id, 0.4f, 1 - (k + 1) / 8.0f};
                        memcpy(rc + i * 64, v, 16);
                        for (UINT y = 0; y <= k; ++y) for (UINT x = s; x < s + 32; ++x) {
                            expected[y][x] = (unsigned char)id;
                            expectedBlue[y][x] = (unsigned char)(s * 255 / 32);
                        }
                    }
                    ringCb->Unmap(0, nullptr);
                }
                D3D12_GPU_VIRTUAL_ADDRESS ringVa = ringCb ? ringCb->GetGPUVirtualAddress() : 0;
                for (UINT variant = 0; ringCb && variant < 2; ++variant) {
                    alloc->Reset();
                    cl->Reset(alloc, nullptr);
                    br.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
                    cl->ResourceBarrier(1, &br);
                    cl->ClearRenderTargetView(rtv, black, 0, nullptr);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    cl->RSSetViewports(1, &vp);
                    cl->RSSetScissorRects(1, &sc);
                    cl->SetGraphicsRootSignature(emuRs);
                    cl->SetGraphicsRootConstantBufferView(0, cbVa + 512);
                    cl->SetPipelineState(emuPso);
                    for (UINT i = 0; i < kRingCalls; ++i) {
                        if (variant == 1 && i == 200)
                            cl->SetPredication(ringCb, kRingCalls * 256, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
                        if (variant == 1 && i == 260) cl->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                        cl->SetGraphicsRootConstantBufferView(0, ringVa + i * 256);
                        cl->SetGraphicsRootConstantBufferView(1, ringVa + i * 256);
                        cl->DispatchMesh(1, 1, 1);
                    }
                    br.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    cl->ResourceBarrier(1, &br);
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    Check(SUCCEEDED(cl->Close()), "many calls: list Close");
                    queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
                    UINT64 v = fence->GetCompletedValue() + 1;
                    queue->Signal(fence, v);
                    fence->SetEventOnCompletion(v, ev);
                    bool done = WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
                    Check(done && dev->GetDeviceRemovedReason() == S_OK, "many calls: GPU work completes");
                    if (!done) return 1;
                    rtRb->Map(0, nullptr, (void**)&px);
                    UINT wrong = 0;
                    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x)
                        wrong += !(expected[y][x] ? matches(pixel(x, y), expected[y][x], 102, expectedBlue[y][x])
                                                  : !pixel(x, y)[0] && !pixel(x, y)[3]);
                    rtRb->Unmap(0, nullptr);
                    printf("info  many calls variant=%u wrong pixels=%u\n", variant, wrong);
                    Check(!wrong, variant ? "many calls: predicated calls in between draw every call in order"
                                          : "many calls: 528 calls in one list draw every call in order");
                }
                if (ringCb) ringCb->Release();
            }

            // Native compute must observe its roots after emulation, including partial setters,
            // indirect dispatch, same/reordered heaps, a heap switch and opaque bundle boundaries.
            ID3D12Fence* stateFence = nullptr;
            ID3D12CommandSignature* nativeSig = nullptr;
            ID3D12Resource* nativeArgs = nullptr;
            ID3D12DescriptorHeap *heaps[2] = {}, *sampler = nullptr;
            ID3D12RootSignature* tableRs = nullptr;
            ID3D12PipelineState* tablePso = nullptr;
            ID3D12CommandAllocator* bundleAlloc = nullptr;
            ID3D12GraphicsCommandList* bundle = nullptr;
            HRESULT stateHr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stateFence));
            D3D12_INDIRECT_ARGUMENT_DESC nativeArg = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
            D3D12_COMMAND_SIGNATURE_DESC nativeDesc = {12, 1, &nativeArg};
            if (SUCCEEDED(stateHr)) stateHr = dev->CreateCommandSignature(&nativeDesc, nullptr, IID_PPV_ARGS(&nativeSig));
            rd.Flags = D3D12_RESOURCE_FLAG_NONE;
            rd.Width = 256;
            if (SUCCEEDED(stateHr)) stateHr = dev->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&nativeArgs));
            if (SUCCEEDED(stateHr)) {
                UINT* data = nullptr;
                nativeArgs->Map(0, nullptr, (void**)&data);
                data[0] = data[1] = data[2] = 1;
                nativeArgs->Unmap(0, nullptr);
            }
            D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1};
            D3D12_ROOT_PARAMETER tableParams[2] = {cp[0], {}};
            tableParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            tableParams[1].DescriptorTable = {1, &range};
            D3D12_ROOT_SIGNATURE_DESC tableDesc = {2, tableParams};
            if (SUCCEEDED(stateHr)) stateHr = D3D12SerializeRootSignature(&tableDesc, D3D_ROOT_SIGNATURE_VERSION_1, &b, nullptr);
            if (SUCCEEDED(stateHr)) {
                stateHr = dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&tableRs));
                b->Release();
            }
            cpd.pRootSignature = tableRs;
            if (SUCCEEDED(stateHr)) stateHr = dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&tablePso));
            D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            for (auto& h : heaps) if (SUCCEEDED(stateHr)) stateHr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h));
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            if (SUCCEEDED(stateHr)) stateHr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sampler));
            if (SUCCEEDED(stateHr)) stateHr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAlloc));
            if (SUCCEEDED(stateHr)) stateHr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAlloc,
                emuPso, IID_PPV_ARGS(&bundle));
            if (SUCCEEDED(stateHr)) stateHr = bundle->Close();
            Check(SUCCEEDED(stateHr), "root cache: native compute, descriptor tables and bundle fixtures");
            if (SUCCEEDED(stateHr)) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
                uav.Format = DXGI_FORMAT_R32_TYPELESS;
                uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                uav.Buffer.NumElements = 64;
                uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                for (auto h : heaps) dev->CreateUnorderedAccessView(out, nullptr, &uav, h->GetCPUDescriptorHandleForHeapStart());
                for (UINT scenario = 0; scenario < 15; ++scenario) {
                    alloc->Reset();
                    cl->Reset(alloc, nullptr);
                    bool table = scenario >= 6 && scenario <= 9;
                    ID3D12DescriptorHeap* bound[] = {heaps[0], sampler};
                    cl->SetDescriptorHeaps(2, bound);
                    cl->SetComputeRootSignature(table ? tableRs : csRs);
                    UINT value = 0x1000 + scenario;
                    cl->SetComputeRoot32BitConstant(0, value, 0);
                    if (table) cl->SetComputeRootDescriptorTable(1, heaps[0]->GetGPUDescriptorHandleForHeapStart());
                    else cl->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
                    D3D12_RESOURCE_BARRIER states[2] = {};
                    states[0].Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
                    states[1].Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
                    cl->ResourceBarrier(2, states);
                    cl->ClearRenderTargetView(rtv, black, 0, nullptr);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    cl->RSSetViewports(1, &vp);
                    cl->RSSetScissorRects(1, &sc);
                    cl->SetGraphicsRootSignature(emuRs);
                    cl->SetGraphicsRootConstantBufferView(0, cbVa + 512);
                    cl->SetGraphicsRootConstantBufferView(1, cbVa);
                    cl->SetPipelineState(emuPso);
                    cl->DispatchMesh(1, 1, 1);
                    if (scenario == 1) cl->SetComputeRoot32BitConstant(0, ++value, 0);
                    if (scenario == 2) { ++value; cl->SetComputeRoot32BitConstants(0, 1, &value, 0); }
                    if (scenario == 3) cl->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
                    if (scenario == 4) cl->SetComputeRootSignature(csRs);
                    if (scenario == 7) cl->SetDescriptorHeaps(2, bound);
                    if (scenario == 8) {
                        bound[0] = heaps[1];
                        cl->SetDescriptorHeaps(2, bound);
                        cl->SetComputeRootDescriptorTable(1, heaps[1]->GetGPUDescriptorHandleForHeapStart());
                    }
                    if (scenario == 9) { ID3D12DescriptorHeap* reverse[] = {sampler, heaps[0]}; cl->SetDescriptorHeaps(2, reverse); }
                    if (scenario == 10) { cl->ExecuteBundle(bundle); cl->DispatchMesh(1, 1, 1); }
                    if (scenario == 11 || scenario == 12) {
                        if (scenario == 11) cl->ClearState(nullptr);
                        else cl->SetProtectedResourceSession(nullptr);
                        cl->SetComputeRootSignature(csRs);
                        cl->SetComputeRoot32BitConstant(0, value, 0);
                        cl->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
                    }
                    if (scenario == 13 || scenario == 14) {
                        if (scenario == 13) cl->SetGraphicsRootSignature(emuRs);
                        cl->SetGraphicsRootConstantBufferView(1, cbVa + 256);
                        cl->DispatchMesh(1, 1, 1);
                    }
                    cl->SetPipelineState(table ? tablePso : csPso);
                    if (scenario == 5) cl->ExecuteIndirect(nativeSig, 1, nativeArgs, 0, nullptr, 0);
                    else cl->Dispatch(1, 1, 1);
                    for (auto& state : states) std::swap(state.Transition.StateBefore, state.Transition.StateAfter);
                    cl->ResourceBarrier(2, states);
                    cl->CopyBufferRegion(outRb, 0, out, 0, 4);
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    bool closed = SUCCEEDED(cl->Close());
                    Check(closed, "root cache: state-change command list Close");
                    if (!closed) break;
                    queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
                    stateFence->SetEventOnCompletion(scenario + 1, ev);
                    queue->Signal(stateFence, scenario + 1);
                    bool done = WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
                    Check(done && dev->GetDeviceRemovedReason() == S_OK, "root cache: state-change GPU work completes");
                    if (!done) return 1;  // do not reset or release work that may still be in flight
                    const UINT* read = nullptr;
                    outRb->Map(0, nullptr, (void**)&read);
                    printf("info  root state scenario=%u expected=%u actual=%u\n", scenario, value, read[0]);
                    Check(read[0] == value, "root cache: native compute keeps untouched roots and applies partial changes");
                    outRb->Unmap(0, nullptr);
                    if (scenario >= 13) {
                        rtRb->Map(0, nullptr, (void**)&px);
                        Check(matches(pixel(56, 4), 20, 102, 64), "root cache: changed graphics argument reaches the next mesh draw");
                        rtRb->Unmap(0, nullptr);
                    }
                }
            }
            for (IUnknown* object : {static_cast<IUnknown*>(bundle), static_cast<IUnknown*>(bundleAlloc),
                    static_cast<IUnknown*>(tablePso), static_cast<IUnknown*>(tableRs), static_cast<IUnknown*>(sampler),
                    static_cast<IUnknown*>(heaps[0]), static_cast<IUnknown*>(heaps[1]), static_cast<IUnknown*>(nativeArgs),
                    static_cast<IUnknown*>(nativeSig), static_cast<IUnknown*>(stateFence)}) if (object) object->Release();

            // Scratch sizing: (65535,16,1) groups need 224 MiB of vertices, more than the 256 MiB
            // floor's vertex region. The first call loses what doesn't fit; its allocator's reset
            // reads the need and increases retry passes, so the second fits without exceeding
            // the geometry cap. A small third call exercises the lower-demand observation path.
            for (UINT pass = 0; second && pass < 3; ++pass) {
                alloc->Reset();
                cl->Reset(alloc, nullptr);
                br.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
                cl->ResourceBarrier(1, &br);
                cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                cl->RSSetViewports(1, &vp);
                cl->RSSetScissorRects(1, &sc);
                cl->SetGraphicsRootSignature(emuRs);
                cl->SetGraphicsRootConstantBufferView(0, cbVa + 512);
                cl->SetGraphicsRootConstantBufferView(1, cbVa + 256);
                cl->SetPipelineState(emuPso);
                if (pass < 2) cl->DispatchMesh(65535, 16, 1);
                else cl->DispatchMesh(1, 1, 1);
                br.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cl->ResourceBarrier(1, &br);
                Check(SUCCEEDED(cl->Close()), "scratch sizing: list Close");
                queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
                UINT64 v = fence->GetCompletedValue() + 1;
                queue->Signal(fence, v);
                fence->SetEventOnCompletion(v, ev);
                bool completed = WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
                Check(completed, "scratch sizing: GPU work completes");
                if (!completed) return 1;
                Check(dev->GetDeviceRemovedReason() == S_OK, "scratch sizing: device not removed");
            }
            alloc->Reset();  // reads the last pass
        }
        for (IUnknown* u : std::initializer_list<IUnknown*>{emuPso, csPso, csRs, emuRs, rt, rtvHeap, cb, out, outRb, rtRb})
            if (u) u->Release();
    }

    g_failures += TestDrawBounds(dev);
    g_failures += TestRetryPasses(dev);
    g_failures += TestPrimitiveOutputs(dev);

    // The proxy flushes its counters on SetPipelineState at most every 5 s.
    Sleep(5100);
    alloc->Reset();
    cl->Reset(alloc, nullptr);
    cl->SetPipelineState(msPso);
    cl->Close();
    {
        FILE* f = _wfsopen(L"legacygpu.log", L"r", _SH_DENYNO);
        char line[512];
        bool probed = false;
        bool emulated = false, grew = false, shrank = false, adapted = false;
        unsigned overflowCalls = 0;
        unsigned long long budget = 0, scratch = 0;
        while (f && fgets(line, sizeof(line), f)) {
            probed |= strstr(line, "groups=6 first=(2,3,1)") != nullptr;
            emulated |= strstr(line, "mesh emulation MS=") && strstr(line, ": PSO ");
            adapted |= strstr(line, "EMU retry MS=") && strstr(line, "passes=1 -> 2");
            grew |= strstr(line, "EMU scratch grow: 256 -> 384 MiB") != nullptr;
            shrank |= strstr(line, "EMU scratch shrink: 384 -> 256 MiB") != nullptr;
            if (const char* o = strstr(line, "EMU overflow: ")) {
                unsigned n = 0;
                sscanf_s(o, "EMU overflow: %u", &n);
                overflowCalls += n;
            }
            if (strstr(line, "MEMORY device=")) {  // the last one counts
                const char* b = strstr(line, "localBudget=");
                const char* s = strstr(line, "scratchBytes=");
                if (b) sscanf_s(b, "localBudget=%llu", &budget);
                if (s) sscanf_s(s, "scratchBytes=%llu", &scratch);
            }
        }
        if (f) fclose(f);
        Check(probed, "mesh ExecuteIndirect args probe reads (2,3,1)");
        char shaderPath[128], hash[33];
        for (int i = 0; i < 16; ++i) sprintf_s(hash + i * 2, 33 - i * 2, "%02x", g_emuMS[4 + i]);
        sprintf_s(shaderPath, "legacygpu_shaders\\%s.emucs7.cso", hash);
        bool pair = GetFileAttributesA(shaderPath) != INVALID_FILE_ATTRIBUTES;
        sprintf_s(shaderPath, "legacygpu_shaders\\%s.emuvs7.cso", hash);
        pair &= GetFileAttributesA(shaderPath) != INVALID_FILE_ATTRIBUTES;
        Check(pair && emulated, "emulation: PSO built from the .emucs7/.emuvs7 pair");
        printf("info  scratch grew=%d shrank=%d overflowCalls=%u final scratchBytes=%llu\n", grew, shrank, overflowCalls,
               scratch);
        Check(!grew && adapted, "scratch sizing: overflowing call increases retries without growing beyond cap");
        Check(overflowCalls == 1u, "scratch sizing: an overflowing call loses groups only before adaptation");
        Check(!shrank, "scratch sizing: the geometry capacity remains at its 256 MiB floor/cap");
        bool memory = budget > 0 && scratch == 320ull << 20;  // geometry plus legal-size retry lists
        int memoryEnabled = GetPrivateProfileIntW(L"legacygpu", L"MemoryDiagnostics", 0, L".\\legacygpu.ini");
        Check(memory == (memoryEnabled != 0), "process memory telemetry follows ini and reports budget/scratch");
    }

    CloseHandle(ev);
    fence->Release();
    cl->Release();
    alloc->Release();
    queue->Release();
    argBuf->Release();
    argUp->Release();
    sig->Release();
    msPso->Release();
    rs->Release();
    dev->Release();
    printf("%s (%d failures). See legacygpu.log next to the exe.\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
