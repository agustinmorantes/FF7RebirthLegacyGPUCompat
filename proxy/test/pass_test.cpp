// GPU checks for PassPerformanceDiagnostics (proxy/src/meshprof.cpp), following the ini next to the
// exe: 0 must leave no trace; N must report, once per submitted generation started inside a window,
// one span per render-target or compute/graphics change with real timings and labels.
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>
#include "prof_test.h"

namespace {

int g_fail;
void Check(bool ok, const char* what) {
    printf("%s  passprof: %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

std::string Field(const std::string& line, const char* key) {
    std::string k = std::string(" ") + key + "=";
    size_t p = line.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    return line.substr(p, line.find_first_of(" \r\n", p) - p);
}

long g_logPos;
std::vector<std::string> NewLines(const char* tag) {
    std::vector<std::string> out;
    FILE* f = _wfsopen(L"legacygpu.log", L"rb", _SH_DENYNO);
    if (!f) return out;
    fseek(f, g_logPos, SEEK_SET);
    char line[2048];
    while (fgets(line, sizeof(line), f))
        if (strstr(line, tag)) out.push_back(line);
    g_logPos = ftell(f);
    fclose(f);
    return out;
}

std::string Ptr(const void* p) {
    char b[32];
    sprintf_s(b, "%p", p);
    return b;
}

std::vector<std::string> For(const std::vector<std::string>& v, const void* list) {
    std::vector<std::string> out;
    for (const std::string& l : v)
        if (Field(l, "list") == Ptr(list)) out.push_back(l);
    return out;
}

}  // namespace

int TestPassProfiler(const ProfFixture& fx) {
    g_fail = 0;
    const int interval = GetPrivateProfileIntW(L"legacygpu", L"PassPerformanceDiagnostics", 0, L".\\legacygpu.ini");
    printf("info  passprof: PassPerformanceDiagnostics=%d\n", interval);
    ID3D12Device* dev = fx.dev;
    ID3D12CommandQueue* queue = fx.queue;
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;
    auto wait = [&] {
        queue->Signal(fence, ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, ev);
        return WaitForSingleObject(ev, 20000) == WAIT_OBJECT_0;
    };
    // Lists with their own allocators; the last one is empty and drains the profiler when submitted.
    const int kLists = 22, kDrain = kLists - 1;
    ID3D12CommandAllocator* allocs[kLists] = {};
    ID3D12GraphicsCommandList6* lists[kLists] = {};
    for (int i = 0; i < kLists; ++i) {
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[i]));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[i], nullptr, IID_PPV_ARGS(&lists[i]));
        lists[i]->Close();
    }
    auto submit = [&](ID3D12GraphicsCommandList6* l) {
        ID3D12CommandList* c = l;
        queue->ExecuteCommandLists(1, &c);
    };
    auto kick = [&] {
        bool ok = wait();
        submit(lists[kDrain]);
        return wait() && ok && dev->GetDeviceRemovedReason() == S_OK;
    };
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ID3D12Resource* uav = nullptr;  // promoted from COMMON on first use in each submission
    dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                 IID_PPV_ARGS(&uav));
    {
        FILE* f = _wfsopen(L"legacygpu.log", L"rb", _SH_DENYNO);
        if (f) fseek(f, 0, SEEK_END), g_logPos = ftell(f), fclose(f);
    }

    D3D12_VIEWPORT vp = {0, 0, (float)fx.width, (float)fx.height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)fx.width, (LONG)fx.height};
    auto transition = [&](ID3D12GraphicsCommandList6* l, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br = {};
        br.Transition = {fx.rt, 0, a, b};
        l->ResourceBarrier(1, &br);
    };
    auto begin = [&](int i) {  // Reset decides whether the generation is sampled
        lists[i]->Reset(allocs[i], nullptr);
        return lists[i];
    };
    auto graphics = [&](ID3D12GraphicsCommandList6* l) {
        l->OMSetRenderTargets(1, &fx.rtv, FALSE, nullptr);
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &sc);
        l->SetGraphicsRootSignature(fx.emuRs);
        l->SetGraphicsRootConstantBufferView(0, fx.cbVa + 512);
        l->SetPipelineState(fx.emuPso);
    };
    auto emulated = [&](ID3D12GraphicsCommandList6* l) {  // one direct emulated mesh call
        l->SetGraphicsRootConstantBufferView(1, fx.cbVa + 256);
        l->DispatchMesh(1, 1, 1);
    };
    auto compute = [&](ID3D12GraphicsCommandList6* l) {
        l->SetComputeRootSignature(fx.csRs);
        l->SetComputeRoot32BitConstant(0, 7, 0);
        l->SetComputeRootUnorderedAccessView(1, uav->GetGPUVirtualAddress());
        l->SetPipelineState(fx.csPso);
        l->Dispatch(1, 1, 1);
    };
    auto start = [&](int i) {
        ID3D12GraphicsCommandList6* l = begin(i);
        transition(l, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        return l;
    };
    auto end = [&](ID3D12GraphicsCommandList6* l) {
        transition(l, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        return SUCCEEDED(l->Close());
    };

    // Three more views of the same render target: equal labels, distinct bindings.
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE views[3];
    for (UINT i = 0; i < 3; ++i) {
        views[i].ptr = rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                       i * dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dev->CreateRenderTargetView(fx.rt, nullptr, views[i]);
    }

    // Two more pipelines with the same shader, stored by description and by stream and loaded back
    // from a library rebuilt from the serialized one, so they are new objects: new compute spans
    // with the same label.
    ID3D12Device2* dev2 = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&dev2));
    struct {
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE rsType;
        ID3D12RootSignature* rs;
        alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE csType;
        D3D12_SHADER_BYTECODE cs;
    } stream = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, fx.csRs, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,
                fx.csCode};
    D3D12_PIPELINE_STATE_STREAM_DESC sd = {sizeof(stream), &stream};
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {fx.csRs, fx.csCode};
    ID3D12PipelineState *streamPso = nullptr, *csPso2 = nullptr, *csPso3 = nullptr;
    ID3D12PipelineLibrary1 *store = nullptr, *lib = nullptr;
    dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&streamPso));
    dev2->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&store));
    store->StorePipeline(L"desc", fx.csPso);
    store->StorePipeline(L"stream", streamPso);
    std::vector<char> blob(store->GetSerializedSize());
    store->Serialize(blob.data(), blob.size());
    dev2->CreatePipelineLibrary(blob.data(), blob.size(), IID_PPV_ARGS(&lib));
    Check(lib && SUCCEEDED(lib->LoadComputePipeline(L"desc", &cpd, IID_PPV_ARGS(&csPso2))) &&
              SUCCEEDED(lib->LoadPipeline(L"stream", &sd, IID_PPV_ARGS(&csPso3))),
          "compute pipelines load from a library, by description and by stream");
    char csLabel[17];
    for (int k = 0; k < 8; ++k) sprintf_s(csLabel + 2 * k, 3, "%02x", ((const unsigned char*)fx.csCode.pShaderBytecode)[4 + k]);

    // A window opens at the first Reset after the interval: eight spans, gfx x4 / cs x3 / gfx.
    if (interval) Sleep(interval * 1000 + 100);
    ID3D12GraphicsCommandList6* a = start(0);
    graphics(a);
    emulated(a);
    emulated(a);
    a->OMSetRenderTargets(1, &fx.rtv, FALSE, nullptr);  // same targets: no new span
    emulated(a);
    a->OMSetRenderTargets(1, &views[0], FALSE, nullptr);  // equal description, another view: new span
    emulated(a);
    D3D12_CPU_DESCRIPTOR_HANDLE pair[2] = {views[0], views[1]};
    a->OMSetRenderTargets(2, pair, FALSE, nullptr);  // a second target: new span
    emulated(a);
    pair[1] = views[2];
    a->OMSetRenderTargets(2, pair, FALSE, nullptr);  // only RT1 changes: new span
    emulated(a);
    a->OMSetRenderTargets(1, &fx.rtv, FALSE, nullptr);
    compute(a);
    a->Dispatch(1, 1, 1);
    a->SetPipelineState(csPso2);  // another pipeline: new span
    a->Dispatch(1, 1, 1);
    a->SetPipelineState(csPso3);
    a->Dispatch(1, 1, 1);
    a->SetPipelineState(fx.emuPso);  // graphics roots are still bound
    emulated(a);
    Check(end(a), "sampled list closes");
    submit(a);
    Check(kick(), "sampled list: GPU work completes");
    std::vector<std::string> spans = For(NewLines("PASSPROF span"), a);
    g_logPos = 0;
    std::vector<std::string> listLines = For(NewLines("PASSPROF list"), a);
    if (!interval) {
        g_logPos = 0;
        Check(NewLines("PASSPROF").empty(), "disabled: no pass-profiler resources, windows or spans logged");
    } else {
        for (const std::string& s : spans) printf("info  %s", s.c_str() + s.find("PASSPROF"));
        bool shape = spans.size() == 8;
        for (size_t i = 0; shape && i < 8; ++i) shape = Field(spans[i], "kind") == (i >= 4 && i <= 6 ? "cs" : "gfx");
        Check(shape, "eight spans: four graphics (one per distinct binding), three compute (one per pipeline), graphics");
        if (shape) {
            Check(Field(spans[0], "emulated") == "3" && Field(spans[1], "emulated") == "1" &&
                      Field(spans[2], "emulated") == "1" && Field(spans[3], "emulated") == "1" &&
                      Field(spans[4], "dispatches") == "2" && Field(spans[5], "dispatches") == "1" &&
                      Field(spans[6], "dispatches") == "1" && Field(spans[7], "emulated") == "1" &&
                      Field(spans[0], "draws") == "0",
                  "work counted per span; repeating the same targets keeps the span");
            Check(Field(spans[4], "cs") == csLabel && Field(spans[0], "cs") == "-" && Field(spans[7], "cs") == "-",
                  "compute spans labeled with the CS hash, graphics spans not");
            Check(Field(spans[5], "cs") == csLabel && Field(spans[6], "cs") == csLabel,
                  "pipelines loaded from a library labeled too");
            char rt[32], rt2[32];
            sprintf_s(rt, "%ux%u:%d*1", fx.width, fx.height, (int)DXGI_FORMAT_R8G8B8A8_UNORM);
            sprintf_s(rt2, "%ux%u:%d*2", fx.width, fx.height, (int)DXGI_FORMAT_R8G8B8A8_UNORM);
            Check(Field(spans[0], "rt") == rt && Field(spans[1], "rt") == rt && Field(spans[2], "rt") == rt2 &&
                      Field(spans[3], "rt") == rt2 && Field(spans[7], "rt") == rt && Field(spans[4], "rt") == "-" &&
                      Field(spans[5], "rt") == "-" && Field(spans[6], "rt") == "-" &&
                      Field(spans[0], "ds") == "-",
                  "graphics spans labeled with RT0 size, format and count; compute spans unlabeled");
            bool timed = true;
            unsigned long long prev = 0;
            for (const std::string& s : spans) {
                unsigned long long st = _strtoui64(Field(s, "start").c_str(), nullptr, 10);
                timed &= _strtoui64(Field(s, "ticks").c_str(), nullptr, 10) > 0 && st >= prev;
                prev = st;
            }
            Check(timed, "positive durations, starts in recording order");
        }
        Check(listLines.size() == 1 && Field(listLines[0], "spans") == "8" && Field(listLines[0], "truncated") == "0" &&
                  Field(listLines[0], "badSpans") == "0",
              "one list summary for the generation");

        // Re-executing the same generation reports nothing more.
        submit(a);
        Check(kick(), "re-execution: GPU work completes");
        g_logPos = 0;
        Check(For(NewLines("PASSPROF span"), a).size() == 8, "re-executed list: no duplicate spans");
    }

    // A list started after the window closed is not sampled.
    Sleep(200);
    {
        FILE* f = _wfsopen(L"legacygpu.log", L"rb", _SH_DENYNO);
        if (f) fseek(f, 0, SEEK_END), g_logPos = ftell(f), fclose(f);
    }
    ID3D12GraphicsCommandList6* b = start(1);
    graphics(b);
    emulated(b);
    compute(b);
    end(b);
    submit(b);
    Check(kick(), "unsampled list: GPU work completes");
    Check(For(NewLines("PASSPROF"), b).empty(), "list started outside a window: no spans");

    // Twenty lists started in one window, each alternating 800 times: 1600 spans need 7 chunks, so
    // the 128 chunks run out and later lists are truncated; every chunk returns afterwards.
    if (interval) Sleep(interval * 1000 + 100);
    const int kBig = 20, kAlternations = 800;
    std::vector<ID3D12GraphicsCommandList6*> big;
    for (int i = 0; i < kBig; ++i) big.push_back(start(i));  // all inside the window
    bool closed = true;
    for (ID3D12GraphicsCommandList6* l : big) {
        for (int k = 0; k < kAlternations; ++k) {
            graphics(l);
            emulated(l);
            compute(l);
        }
        closed &= end(l);
    }
    Check(closed, "long lists close");
    for (ID3D12GraphicsCommandList6* l : big) submit(l);
    Check(kick(), "long lists: GPU work completes");
    if (interval) {
        g_logPos = 0;
        std::vector<std::string> all = NewLines("PASSPROF list");
        size_t full = 0, truncated = 0, spanTotal = 0;
        bool bounded = true;
        for (ID3D12GraphicsCommandList6* l : big) {
            std::vector<std::string> ls = For(all, l);
            if (ls.empty()) continue;
            const std::string& x = ls.back();
            size_t n = (size_t)atoi(Field(x, "spans").c_str());
            spanTotal += n;
            if (Field(x, "truncated") == "1") ++truncated, bounded &= n < 2 * kAlternations;
            else full += n == 2 * kAlternations, bounded &= n == 2 * kAlternations;
            bounded &= Field(x, "badSpans") == "0";
        }
        printf("info  passprof: long lists full=%zu truncated=%zu spans=%zu\n", full, truncated, spanTotal);
        Check(full == 18 && truncated == 2 && bounded && spanTotal <= 128 * 255,
              "chunk exhaustion: 18 complete lists, the rest truncated, no bad spans");

        // A generation's chunks return once it has completed and its list is reset, here outside
        // a window; the next window gets them again.
        for (int i = 0; i < kBig; ++i) begin(i)->Close();
        Sleep(interval * 1000 + 100);
        {
            FILE* f = _wfsopen(L"legacygpu.log", L"rb", _SH_DENYNO);
            if (f) fseek(f, 0, SEEK_END), g_logPos = ftell(f), fclose(f);
        }
        ID3D12GraphicsCommandList6* c = start(2);
        graphics(c);
        emulated(c);
        compute(c);
        end(c);
        submit(c);
        Check(kick(), "after exhaustion: GPU work completes");
        std::vector<std::string> cs = For(NewLines("PASSPROF span"), c);
        Check(cs.size() == 2, "chunks returned: the next window records again");

        // Counters, through the 5 s statistics flush on SetPipelineState. That Reset opens a new
        // window, so its own list is the one still recording.
        Sleep(5100);
        lists[3]->Reset(allocs[3], nullptr);
        lists[3]->SetPipelineState(fx.emuPso);
        lists[3]->Close();
        g_logPos = 0;
        std::string last;
        for (const std::string& l : NewLines("PASSPROF stats")) last = l;
        printf("info  %s", last.empty() ? "no stats\n" : last.c_str() + last.find("PASSPROF"));
        Check(!last.empty() && atoi(Field(last, "truncatedLists").c_str()) >= 2 &&
                  atoi(Field(last, "badSpans").c_str()) == 0 && atoi(Field(last, "coverageLost").c_str()) == 0 &&
                  Field(last, "recording") == "1",
              "counters: truncation seen, no bad spans, coverage kept, only the open list recording");
    }

    for (auto* l : lists) l->Release();
    for (auto* al : allocs) al->Release();
    uav->Release();
    rtvHeap->Release();
    for (IUnknown* o : {(IUnknown*)csPso2, (IUnknown*)csPso3, (IUnknown*)streamPso, (IUnknown*)lib, (IUnknown*)store,
                        (IUnknown*)dev2})
        if (o) o->Release();
    fence->Release();
    CloseHandle(ev);
    return g_fail;
}
