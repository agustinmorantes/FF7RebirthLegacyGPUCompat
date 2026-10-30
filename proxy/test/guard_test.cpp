#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "guard_cs.h"

int TestDrawBounds(ID3D12Device* dev) {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s  %s\n", ok ? "PASS" : "FAIL", message);
        failures += !ok;
    };
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource *scratch = nullptr, *upload = nullptr, *readback = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    auto cleanup = [&] {
        for (IUnknown* u : std::initializer_list<IUnknown*>{fence, queue, cl, alloc, readback, upload, scratch, pso, rs})
            if (u) u->Release();
    };
    D3D12_ROOT_PARAMETER root[2] = {};
    root[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root[1].Descriptor.RegisterSpace = 999;
    D3D12_ROOT_SIGNATURE_DESC desc = {2, root};
    ID3DBlob* blob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    if (blob) blob->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {rs, {g_guardCS, sizeof(g_guardCS)}};
    if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pso));
    constexpr UINT64 ibOff = 256, vbOff = 1024;  // the header below leaves room for one quad's indices
    D3D12_RESOURCE_DESC buffer = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, vbOff + 4096, 1, 1, 1,
                                DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratch));
    buffer.Width = 512;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    D3D12_COMMAND_QUEUE_DESC q = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (SUCCEEDED(hr)) hr = dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, q.Type, alloc, nullptr, IID_PPV_ARGS(&cl));
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    check(SUCCEEDED(hr), "draw bounds: GPU resources and converted shader");
    if (FAILED(hr)) { cleanup(); return failures; }
    UINT* data = nullptr;
    upload->Map(0, nullptr, (void**)&data);
    memset(data, 0, 512);
    data[1] = 1;
    data[8] = 6, data[9] = (UINT)vbOff, data[10] = 4096;  // index capacity, vertex region offset and size
    data[28] = (UINT)ibOff, data[29] = 0xaabbccdd;  // index region offset, then a sentinel
    const float constants[] = {0, 10, 0.4f, -1};
    memcpy(data + 64, constants, sizeof(constants));
    upload->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition = {scratch, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_DEST};
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(scratch, 0, upload, 0, 116);
    cl->CopyBufferRegion(scratch, ibOff + 24, upload, 116, 4);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);
    cl->SetComputeRootSignature(rs);
    cl->SetComputeRootConstantBufferView(0, upload->GetGPUVirtualAddress() + 256);
    cl->SetComputeRootUnorderedAccessView(1, scratch->GetGPUVirtualAddress());
    cl->SetPipelineState(pso);
    cl->Dispatch(3, 1, 1);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(readback, 0, scratch, 0, 112);
    cl->CopyBufferRegion(readback, 112, scratch, ibOff + 24, 4);
    check(SUCCEEDED(cl->Close()), "draw bounds: command list Close");
    queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, event);
    bool completed = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    check(completed && dev->GetDeviceRemovedReason() == S_OK, "draw bounds: GPU work completes");
    if (completed) {
        readback->Map(0, nullptr, (void**)&data);
        printf("info  draw bounds count=%u requested=%u dropped=%u stride=%u sentinel=0x%x\n", data[0], data[7], data[13],
               data[11], data[28]);
        check(data[0] == 6, "draw bounds: draw count stays within one meshlet capacity");
        check(data[7] == 18 && data[13] == 2, "draw bounds: excess work is counted explicitly");
        check(data[28] == 0xaabbccdd, "draw bounds: out-of-capacity index stores leave sentinel intact");
        check(data[11] != 0, "draw bounds: the shader reports its vertex stride");
        readback->Unmap(0, nullptr);
    }
    CloseHandle(event);
    cleanup();
    return failures;
}
