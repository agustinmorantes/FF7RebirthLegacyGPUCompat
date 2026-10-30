#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "retry_fixture.h"

int TestRetryPasses(ID3D12Device* dev) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s  retries: %s\n", ok ? "PASS" : "FAIL", what);
        failures += !ok;
    };
    // Vertex regions hold whole blocks of 32 records (ms2cs.py), so vb..listBase fits one block of stride <= 64.
    constexpr UINT ib = 256, vb = 512, listBase = 2560, W = 120, H = 8, rtOffset = 4096;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState *cs = nullptr, *gfx = nullptr;
    ID3D12PipelineState* rawCs[2] = {};
    ID3D12Resource *scratch = nullptr, *upload = nullptr, *rb = nullptr, *args = nullptr, *rt = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandSignature *dispatch = nullptr, *draw = nullptr;
    ID3D12Fence* fence = nullptr;
    auto cleanup = [&] {
        for (IUnknown* u : std::initializer_list<IUnknown*>{fence, draw, dispatch, queue, cl, alloc, heap, rt,
                                                            args, rb, upload, scratch, rawCs[0], rawCs[1],
                                                            gfx, cs, rs})
            if (u) u->Release();
    };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.RegisterSpace = params[1].Descriptor.RegisterSpace = 999;
    D3D12_ROOT_SIGNATURE_DESC root = {2, params};
    ID3DBlob* blob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    if (blob) blob->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp = {rs, {g_retryCS, sizeof(g_retryCS)}};
    if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&cs));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gp = {};
    gp.pRootSignature = rs;
    gp.VS = {g_retryVS, sizeof(g_retryVS)};
    gp.PS = {g_retryPS, sizeof(g_retryPS)};
    gp.SampleMask = UINT_MAX;
    gp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gp.RasterizerState.DepthClipEnable = TRUE;
    gp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    gp.SampleDesc.Count = 1;
    if (SUCCEEDED(hr)) hr = dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&gfx));
    cp.CS = {g_retryRawScalarCS, sizeof(g_retryRawScalarCS)};
    if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&rawCs[0]));
    cp.CS = {g_retryRawCS, sizeof(g_retryRawCS)};
    if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&rawCs[1]));
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratch));
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    bd.Width = rtOffset + 512 * H;
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    bd.Width = 12;
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&args));
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    if (SUCCEEDED(hr)) hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    if (SUCCEEDED(hr)) hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (SUCCEEDED(hr)) hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, qd.Type, alloc, nullptr, IID_PPV_ARGS(&cl));
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    D3D12_INDIRECT_ARGUMENT_DESC ad = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    D3D12_COMMAND_SIGNATURE_DESC sd = {12, 1, &ad};
    if (SUCCEEDED(hr)) hr = dev->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&dispatch));
    ad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    sd.ByteStride = 20;
    if (SUCCEEDED(hr)) hr = dev->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&draw));
    check(SUCCEEDED(hr), "fixture resources and real converted shaders");
    if (FAILED(hr)) { cleanup(); return failures; }
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);
    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        cl->ResourceBarrier(1, &b);
    };
    const auto read = D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER |
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 value = 0;
    // Enough passes, explicit exhaustion, extra empty passes, and a deliberately undersized queue. The
    // 6-index capacity admits one group per pass; the vertex region is one block (32 records of stride 24).
    const UINT passCounts[] = {12, 2, 14, 12}, capacities[] = {12, 12, 12, 1}, retained[] = {12, 2, 12, 2};
    for (UINT test = 0; test < 4; ++test) {
        if (test) {
            alloc->Reset();
            cl->Reset(alloc, nullptr);
            transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        UINT* data = nullptr;
        upload->Map(0, nullptr, (void**)&data);
        memset(data, 0, 1280);
        for (UINT mode = 0; mode < 8; ++mode) {
            UINT h[29] = {0, 1, 0, 0, 0, 0, 0, 0, 6, vb, 32 * 24, 0, mode, 0, listBase, capacities[test]};
            h[23] = h[27] = 1;
            h[28] = ib;
            memcpy(data + mode * 32, h, sizeof(h));
        }
        data[256] = 0xaabbccdd;
        upload->Unmap(0, nullptr);
        const float clear[4] = {};
        cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
        D3D12_RECT rect = {0, 0, W, H};
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &rect);
        for (UINT pass = 0; pass < passCounts[test]; ++pass) {
            if (pass) {
                transition(args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyBufferRegion(args, 0, scratch, 84 + ((pass - 1) & 1) * 16, 12);
                transition(args, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            }
            transition(scratch, pass || test ? read : D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            UINT mode = (pass ? 1 : 0) | (pass + 1 < passCounts[test] ? 2 : 0) | ((pass & 1) << 2);
            if (!pass) {
                cl->CopyBufferRegion(scratch, 0, upload, mode * 128, 116);
                cl->CopyBufferRegion(scratch, ib + 24, upload, 1024, 4);
                cl->CopyBufferRegion(scratch, listBase + capacities[test] * 16, upload, 1024, 4);
            } else {
                cl->CopyBufferRegion(scratch, 0, upload, mode * 128, 48);
                cl->CopyBufferRegion(scratch, 48, upload, mode * 128 + 48, 4);
                UINT desc = 80 + (pass & 1) * 16;
                cl->CopyBufferRegion(scratch, desc, upload, mode * 128 + desc, 16);
            }
            transition(scratch, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->SetComputeRootSignature(rs);
            cl->SetComputeRootUnorderedAccessView(0, scratch->GetGPUVirtualAddress());
            cl->SetPipelineState(cs);
            if (!pass) cl->Dispatch(2, 2, 3);
            else cl->ExecuteIndirect(dispatch, 1, args, 0, nullptr, 0);
            transition(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
            cl->SetGraphicsRootSignature(rs);
            cl->SetGraphicsRootShaderResourceView(1, scratch->GetGPUVirtualAddress() + vb);
            cl->SetPipelineState(gfx);
            D3D12_INDEX_BUFFER_VIEW iv = {scratch->GetGPUVirtualAddress() + ib, 24, DXGI_FORMAT_R32_UINT};
            cl->IASetIndexBuffer(&iv);
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->ExecuteIndirect(draw, 1, scratch, 0, nullptr, 0);
            cl->CopyBufferRegion(rb, pass * 128, scratch, 0, 112);
        }
        cl->CopyBufferRegion(rb, 2048, scratch, ib + 24, 4);
        cl->CopyBufferRegion(rb, 2052, scratch, listBase + capacities[test] * 16, 4);
        transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = rt;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {rtOffset, {DXGI_FORMAT_R32_UINT, W, H, 1, 512}};
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        check(SUCCEEDED(cl->Close()), "valid command-list Close");
        ID3D12CommandList* one[] = {cl};
        queue->ExecuteCommandLists(1, one);
        queue->Signal(fence, ++value);
        fence->SetEventOnCompletion(value, event);
        bool complete = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
        check(complete && dev->GetDeviceRemovedReason() == S_OK, "GPU completes without device removal");
        if (!complete) return failures + 1;  // do not destroy resources that may still be in flight
        rb->Map(0, nullptr, (void**)&data);
        UINT indices = 0;
        bool bounded = true, pixels = true;
        for (UINT pass = 0; pass < passCounts[test]; ++pass) {
            indices += data[pass * 32];
            bounded &= data[pass * 32] <= 6;
        }
        UINT visible = 0;
        for (UINT tile = 0; tile < 12; ++tile) {
            UINT id = data[rtOffset / 4 + (H / 2) * 128 + tile * 10 + 5];
            visible += id != 0;
            pixels &= id == 0 || id == tile + 1;
        }
        UINT lost = data[(passCounts[test] - 1) * 32 + 13];
        printf("info  retries case=%u passes=%u capacity=%u drawnIndices=%u visible=%u lost=%u\n",
               test, passCounts[test], capacities[test], indices, visible, lost);
        check(bounded && indices == retained[test] * 6, "all admitted groups drawn exactly once within capacity");
        check(pixels && visible == retained[test], "pixels preserve original XYZ group IDs across retries");
        check(lost == 12 - retained[test], "exhausted passes or queue report every unprocessed group");
        check(data[512] == 0xaabbccdd && data[513] == 0xaabbccdd, "index and retry-list sentinels intact");
        rb->Unmap(0, nullptr);
    }
    // One group has stable allocation offsets. Compare raw bits, including f32 signed zero,
    // subnormal, NaN payload, infinity and arbitrary uint values; also force geometry overflow
    // (one byte short of a block of 32 records of stride 48).
    UINT rawOutput[2][48] = {};
    const UINT expected[12] = {0, 0, 0x3f000000, 0x3f800000, 0x80000000, 1, 0x7fc12345, 0xff800000,
                              0, 0xffffffff, 0x80000000, 0x12345678};
    const auto rawRead = D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                         D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE;
    for (UINT overflow = 0; overflow < 2; ++overflow) for (UINT variant = 0; variant < 2; ++variant) {
        UINT* data = nullptr;
        alloc->Reset();
        cl->Reset(alloc, nullptr);
        upload->Map(0, nullptr, (void**)&data);
        UINT header[29] = {0, 1, 0, 0, 0, 0, 0, 0, 6, vb, overflow ? 32u * 48 - 1 : 32u * 48};
        header[28] = ib;
        memcpy(data, header, sizeof(header));
        upload->Unmap(0, nullptr);
        transition(scratch, rawRead, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyBufferRegion(scratch, 0, upload, 0, sizeof(header));
        cl->CopyBufferRegion(scratch, vb + 32 * 48, upload, 1024, 4);
        cl->CopyBufferRegion(scratch, listBase, upload, 1024, 4);
        transition(scratch, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->SetComputeRootSignature(rs);
        cl->SetComputeRootUnorderedAccessView(0, scratch->GetGPUVirtualAddress());
        cl->SetPipelineState(rawCs[variant]);
        cl->Dispatch(1, 1, 1);
        transition(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, rawRead);
        cl->CopyBufferRegion(rb, 0, scratch, 0, listBase + 4);
        check(SUCCEEDED(cl->Close()), "vector raw-bit command list closes");
        ID3D12CommandList* one[] = {cl};
        queue->ExecuteCommandLists(1, one);
        queue->Signal(fence, ++value);
        fence->SetEventOnCompletion(value, event);
        bool complete = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
        check(complete && dev->GetDeviceRemovedReason() == S_OK, "vector raw-bit GPU work completes");
        if (!complete) return failures + 1;
        rb->Map(0, nullptr, (void**)&data);
        check(data[0] == (overflow ? 0u : 6u) && data[13] == overflow &&
              data[(vb + 32 * 48) / 4] == 0xaabbccdd && data[listBase / 4] == 0xaabbccdd,
              "vector stores preserve admitted counts, overflow loss and region sentinels");
        if (!overflow) {
            // Record v's first 16 bytes at v * 16 in the block, its other 32 at 512 + v * 32.
            for (UINT v = 0; v < 4; ++v)
                for (UINT k = 0; k < 12; ++k)
                    rawOutput[variant][v * 12 + k] = data[(vb + (k < 4 ? v * 16 + k * 4 : 512 + v * 32 + (k - 4) * 4)) / 4];
            bool bits = true;
            for (UINT vertex = 0; vertex < 4; ++vertex)
                bits &= !memcmp(rawOutput[variant] + vertex * 12, expected, sizeof(expected));
            check(bits, "vector stores preserve exact f32 and uint bit patterns");
        }
        rb->Unmap(0, nullptr);
    }
    check(!memcmp(rawOutput[0], rawOutput[1], sizeof(rawOutput[0])), "scalar and vector raw vertex records are identical");
    CloseHandle(event);
    cleanup();
    return failures;
}
