// Per-primitive outputs (test_prim.hlsl): runs the converted CS on a hand-made scratch, checks the
// compacted indices and records, then draws them with the generated VS and checks the pixels.
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "prim.h"
#include "prim_ps.h"

int TestPrimitiveOutputs(ID3D12Device* dev) {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s  %s\n", ok ? "PASS" : "FAIL", message);
        failures += !ok;
    };
    constexpr UINT W = 80, H = 16, kIbOff = 256, kVbOff = 1024, kVbBytes = 8192, kGroups = 3;
    constexpr UINT kCap = (kVbOff - kIbOff) / 4, kStride = 20, kVerts = 42, kPrims = 40;
    // Byte off of record v: blocks of 32 records, their first 16 bytes, then the rest (ms2cs.py layout v7).
    auto at = [=](UINT v, UINT off) { return (v >> 5) * 32 * kStride + (off < 16 ? (v & 31) * 16 + off : 512 + (v & 31) * (kStride - 16) + off - 16); };
    ID3D12RootSignature *csRs = nullptr, *gfxRs = nullptr;
    ID3D12PipelineState *csPso = nullptr, *gfxPso = nullptr;
    ID3D12Resource *scratch = nullptr, *upload = nullptr, *readback = nullptr, *rt = nullptr, *rtRb = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12CommandSignature* sig = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    auto cleanup = [&] {
        for (IUnknown* u : std::initializer_list<IUnknown*>{fence, queue, cl, alloc, sig, rtvHeap, rtRb, rt, readback,
                                                           upload, scratch, gfxPso, csPso, gfxRs, csRs})
            if (u) u->Release();
    };
    auto rootSig = [&](D3D12_ROOT_PARAMETER_TYPE type, ID3D12RootSignature** out) {
        D3D12_ROOT_PARAMETER root = {type};
        root.Descriptor.RegisterSpace = 999;
        D3D12_ROOT_SIGNATURE_DESC desc = {1, &root, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* blob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
        if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(out));
        if (blob) blob->Release();
        return hr;
    };
    HRESULT hr = rootSig(D3D12_ROOT_PARAMETER_TYPE_UAV, &csRs);
    if (SUCCEEDED(hr)) hr = rootSig(D3D12_ROOT_PARAMETER_TYPE_SRV, &gfxRs);
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {csRs, {g_primCS, sizeof(g_primCS)}};
    if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&compute, IID_PPV_ARGS(&csPso));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx = {gfxRs, {g_primVS, sizeof(g_primVS)}, {g_primPS, sizeof(g_primPS)}};
    gfx.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gfx.SampleMask = ~0u;
    gfx.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    gfx.RasterizerState.DepthClipEnable = TRUE;
    gfx.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gfx.NumRenderTargets = 1;
    gfx.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
    gfx.SampleDesc.Count = 1;
    if (SUCCEEDED(hr)) hr = dev->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(&gfxPso));
    D3D12_INDIRECT_ARGUMENT_DESC arg = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &arg};
    if (SUCCEEDED(hr)) hr = dev->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&sig));

    D3D12_RESOURCE_DESC buffer = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, kVbOff + kVbBytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&scratch));
    D3D12_RESOURCE_DESC tex = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R32G32_UINT, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const float kClear = 4000;  // integer targets clear to the converted float
    D3D12_CLEAR_VALUE clear = {DXGI_FORMAT_R32G32_UINT, {kClear, kClear, 0, 0}};
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&rt));
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    buffer.Width = 1024 * H;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rtRb));
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    if (SUCCEEDED(hr)) hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    D3D12_COMMAND_QUEUE_DESC q = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (SUCCEEDED(hr)) hr = dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, q.Type, alloc, nullptr, IID_PPV_ARGS(&cl));
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    check(SUCCEEDED(hr), "primitive outputs: GPU resources and converted pair");
    if (FAILED(hr)) { cleanup(); return failures; }

    // Header: DRAW_INDEXED {0, 1, 0, 0, 0}, capacities; the regions start with a recognizable fill.
    UINT* data = nullptr;
    upload->Map(0, nullptr, (void**)&data);
    memset(data, 0xee, kVbOff + kVbBytes);
    memset(data, 0, kIbOff);
    data[1] = 1;
    data[8] = kCap, data[9] = kVbOff, data[10] = kVbBytes, data[28] = kIbOff;
    upload->Unmap(0, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition = {scratch, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_DEST};
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(scratch, 0, upload, 0, kVbOff + kVbBytes);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);
    cl->SetComputeRootSignature(csRs);
    cl->SetComputeRootUnorderedAccessView(0, scratch->GetGPUVirtualAddress());
    cl->SetPipelineState(csPso);
    cl->Dispatch(kGroups, 1, 1);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                              D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    cl->ResourceBarrier(1, &b);
    cl->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)W, (LONG)H};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(gfxRs);
    cl->SetGraphicsRootShaderResourceView(0, scratch->GetGPUVirtualAddress() + kVbOff);
    cl->SetPipelineState(gfxPso);
    D3D12_INDEX_BUFFER_VIEW ibv = {scratch->GetGPUVirtualAddress() + kIbOff, kCap * 4, DXGI_FORMAT_R32_UINT};
    cl->IASetIndexBuffer(&ibv);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->ExecuteIndirect(sig, 1, scratch, 0, nullptr, 0);
    D3D12_RESOURCE_BARRIER bs[2] = {b, {}};
    bs[0].Transition.StateBefore = b.Transition.StateAfter;
    bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bs[1].Transition = {rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(2, bs);
    cl->CopyBufferRegion(readback, 0, scratch, 0, kVbOff + kVbBytes);
    D3D12_TEXTURE_COPY_LOCATION dst = {rtRb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {rt};
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32_UINT, W, H, 1, 1024};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(SUCCEEDED(cl->Close()), "primitive outputs: command list Close");
    queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, event);
    bool completed = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    check(completed && dev->GetDeviceRemovedReason() == S_OK, "primitive outputs: GPU work completes");
    if (completed) {
        auto culled = [](UINT g, UINT t) { return g == 2 || t % 3 == 1 || t == 33; };
        UINT kept = 0;
        for (UINT t = 0; t < kPrims; ++t) kept += !culled(0, t);
        readback->Map(0, nullptr, (void**)&data);
        printf("info  primitive outputs count=%u requested=%u vertices=%u dropped=%u stride=%u\n", data[0], data[7],
               data[5], data[6], data[11]);
        check(data[0] == 2 * kept * 3 && data[7] == 2 * kept * 3, "primitive outputs: only surviving triangles are allocated");
        check(data[5] == kGroups * (kVerts + kPrims) && data[6] == 0 && data[11] == kStride,
              "primitive outputs: vertices plus one record per primitive");
        // Each group's triangles are contiguous and in their original order; the first index names a record.
        const UINT* ib = data + kIbOff / 4;
        const unsigned char* vb = (const unsigned char*)data + kVbOff;
        bool order = true, records = true;
        for (UINT r = 0; r < 2; ++r) {
            UINT g = ~0u, t = 0;
            for (UINT k = 0; k < kept; ++k, ++t) {
                const UINT* tri = ib + (r * kept + k) * 3;
                if (!(tri[0] & 0x80000000u)) { records = false; break; }
                const UINT* rec = (const UINT*)(vb + at(tri[0] & 0x7fffffffu, 0));
                if (g == ~0u) g = rec[1] / 1000;
                while (t < kPrims && culled(g, t)) ++t;
                UINT q = t >> 1, base = rec[0] - ((t & 1) ? 2 * q + 1 : 2 * q);
                UINT expect1 = base + ((t & 1) ? 2 * q + 3 : 2 * q + 1), expect2 = base + 2 * q + 2;
                order &= rec[1] == g * 1000 + t * 7 && tri[1] == expect1 && tri[2] == expect2;
                order &= *(const UINT*)(vb + at(rec[0], 16)) == g + 5;  // TEXCOORD of the provoking vertex
            }
        }
        check(records, "primitive outputs: provoking indices point at records");
        check(order, "primitive outputs: compacted triangles keep their order, vertices and attributes");
        readback->Unmap(0, nullptr);

        const UINT* px = nullptr;
        rtRb->Map(0, nullptr, (void**)&px);
        bool pixels = true;
        for (UINT g = 0; g < 2; ++g)
            for (UINT t = 0; t < kPrims; ++t) {  // centroid of triangle t in band g
                UINT x = (t >> 1) * 4 + ((t & 1) ? 3 : 1), y = g * 8 + ((t & 1) ? 5 : 2);
                const UINT* c = px + y * 256 + x * 2;
                bool ok = culled(g, t) ? c[0] == (UINT)kClear && c[1] == (UINT)kClear : c[0] == g * 1000 + t * 7 && c[1] == g + 5;
                if (!ok && pixels) printf("info  pixel (%u,%u) for triangle %u/%u = %u %u\n", x, y, g, t, c[0], c[1]);
                pixels &= ok;
            }
        check(pixels, "primitive outputs: culled triangles are absent, PRIMITIVEID is per triangle");
        rtRb->Unmap(0, nullptr);

        // Room for two groups' vertices and records only: the third contributes deferred index
        // demand but consumes no drawn indices. Every admitted group's indices must be valid.
        constexpr UINT kSmallVb = 4096, kVcap = kSmallVb / (32 * kStride) * 32;
        upload->Map(0, nullptr, (void**)&data);
        data[10] = kSmallVb;
        upload->Unmap(0, nullptr);
        alloc->Reset();
        cl->Reset(alloc, nullptr);
        bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, bs);
        cl->CopyBufferRegion(scratch, 0, upload, 0, kVbOff + kVbBytes);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(1, &b);
        cl->SetComputeRootSignature(csRs);
        cl->SetComputeRootUnorderedAccessView(0, scratch->GetGPUVirtualAddress());
        cl->SetPipelineState(csPso);
        cl->Dispatch(kGroups, 1, 1);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(readback, 0, scratch, 0, kVbOff);
        check(SUCCEEDED(cl->Close()), "primitive outputs overflow: command list Close");
        queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
        queue->Signal(fence, 2);
        fence->SetEventOnCompletion(2, event);
        completed = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        check(completed && dev->GetDeviceRemovedReason() == S_OK, "primitive outputs overflow: GPU work completes");
        if (completed) {
            readback->Map(0, nullptr, (void**)&data);
            printf("info  primitive outputs overflow count=%u requested=%u vertices=%u dropped=%u\n", data[0], data[7],
                   data[5], data[13]);
            // Atomic allocation need not admit groups in ID order. Of the two admitted groups,
            // either one or both produce visible triangles; the third group is fully culled.
            check((data[0] == kept * 3 || data[0] == 2 * kept * 3) && data[7] == data[0] &&
                      data[6] == kPrims * 3 && data[13] == 1,
                  "primitive outputs overflow: fitted indices, deferred index demand and lost group counted");
            bool valid = true;
            for (UINT i = 0; i < data[0]; i += 3) {
                const UINT* tri = data + kIbOff / 4 + i;
                valid &= (tri[0] & 0x7fffffffu) < kVcap && tri[1] < kVcap && tri[2] < kVcap;
            }
            check(valid, "primitive outputs overflow: drawn indices are in range");
            readback->Unmap(0, nullptr);
        }
        if (completed) {
            // All vertices fit, but only one visible group's compacted indices fit. The fully
            // culled third group must not count as lost even when the index allocator is full.
            upload->Map(0, nullptr, (void**)&data);
            data[8] = kept * 3;
            data[10] = kVbBytes;
            upload->Unmap(0, nullptr);
            alloc->Reset();
            cl->Reset(alloc, nullptr);
            bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cl->ResourceBarrier(1, bs);
            cl->CopyBufferRegion(scratch, 0, upload, 0, kVbOff + kVbBytes);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cl->ResourceBarrier(1, &b);
            cl->SetComputeRootSignature(csRs);
            cl->SetComputeRootUnorderedAccessView(0, scratch->GetGPUVirtualAddress());
            cl->SetPipelineState(csPso);
            cl->Dispatch(kGroups, 1, 1);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b);
            cl->CopyBufferRegion(readback, 0, scratch, 0, kVbOff);
            check(SUCCEEDED(cl->Close()), "primitive index overflow: command list Close");
            queue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
            queue->Signal(fence, 3);
            fence->SetEventOnCompletion(3, event);
            completed = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
            check(completed && dev->GetDeviceRemovedReason() == S_OK, "primitive index overflow: GPU work completes");
            if (completed) {
                readback->Map(0, nullptr, (void**)&data);
                check(data[0] == kept * 3 && data[7] == 2 * kept * 3 && data[6] == 0 && data[13] == 1,
                      "primitive index overflow: only the visible failed group is lost");
                check(data[kIbOff / 4 + kept * 3] == 0xeeeeeeee,
                      "primitive index overflow: sentinel beyond index capacity stays intact");
                readback->Unmap(0, nullptr);
            }
        }
    }
    CloseHandle(event);
    cleanup();
    return failures;
}
