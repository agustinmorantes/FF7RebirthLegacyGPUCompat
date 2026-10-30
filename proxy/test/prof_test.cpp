// GPU checks for MeshPerformanceDiagnostics (proxy/src/meshprof.cpp), following the ini next to the
// exe: 0 must leave no profiler trace; 1 (sampled) and 2 (every call) must report fence-verified
// samples only, once each, with real timings, statistics and counters.
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <share.h>
#include <string>
#include <vector>
#include "prof_test.h"

namespace {

int g_fail;
void Check(bool ok, const char* what) {
    printf("%s  profile: %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}
int g_mode;
void Expect(bool ok, const char* what) {  // profiler output, only checked when profiling is on
    if (g_mode) Check(ok, what);
}

struct Sample {
    std::string list, queue, cs, dispatch;
    unsigned gen, slot, drawIndices, rawIndices, vertexRaw, overflowGroups;
    unsigned long long freq, computeTicks, drawTicks;
};

std::string Field(const char* line, const char* key) {
    std::string k = std::string(" ") + key + "=";
    const char* p = strstr(line, k.c_str());
    if (!p) return "";
    p += k.size();
    return std::string(p, strcspn(p, " \r\n"));
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

std::vector<Sample> NewSamples() {
    std::vector<Sample> out;
    for (const std::string& l : NewLines("MESHPROF sample")) {
        const char* c = l.c_str();
        Sample s;
        s.list = Field(c, "list"), s.queue = Field(c, "queue"), s.cs = Field(c, "csInvocations"), s.dispatch = Field(c, "dispatch");
        s.gen = atoi(Field(c, "gen").c_str()), s.slot = atoi(Field(c, "slot").c_str());
        s.drawIndices = atoi(Field(c, "drawIndices").c_str()), s.rawIndices = atoi(Field(c, "rawIndices").c_str());
        s.vertexRaw = atoi(Field(c, "vertexRaw").c_str());
        s.overflowGroups = atoi(Field(c, "overflowGroups").c_str());
        s.freq = _strtoui64(Field(c, "freqHz").c_str(), nullptr, 10);
        s.computeTicks = _strtoui64(Field(c, "computeTicks").c_str(), nullptr, 10);
        s.drawTicks = _strtoui64(Field(c, "drawTicks").c_str(), nullptr, 10);
        out.push_back(s);
    }
    return out;
}

std::string Ptr(const void* p) {
    char b[32];
    sprintf_s(b, "%p", p);
    return b;
}

size_t CountFor(const std::vector<Sample>& v, const void* list) {
    std::string p = Ptr(list);
    return std::count_if(v.begin(), v.end(), [&](const Sample& s) { return s.list == p; });
}

}  // namespace

int TestMeshProfiler(const ProfFixture& fx) {
    g_fail = 0;
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible if the GPU stalls
    const int mode = g_mode = GetPrivateProfileIntW(L"legacygpu", L"MeshPerformanceDiagnostics", 0, L".\\legacygpu.ini");
    printf("info  profile: MeshPerformanceDiagnostics=%d\n", mode);
    ID3D12Device* dev = fx.dev;
    ID3D12CommandQueue* queue = fx.queue;
    UINT64 freq = 0;
    queue->GetTimestampFrequency(&freq);

    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;
    auto wait = [&] {
        queue->Signal(fence, ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, ev);
        return WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
    };
    // Lists with their own allocators; index 0 is an empty list submitted to drain the profiler.
    const int kLists = 12;
    ID3D12CommandAllocator* allocs[kLists + 1] = {};
    ID3D12GraphicsCommandList6* lists[kLists] = {};
    for (int i = 0; i < kLists; ++i) {
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[i]));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[i], nullptr, IID_PPV_ARGS(&lists[i]));
        lists[i]->Close();
    }
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[kLists]));
    auto submit = [&](std::initializer_list<ID3D12GraphicsCommandList6*> ls) {
        std::vector<ID3D12CommandList*> v(ls.begin(), ls.end());
        queue->ExecuteCommandLists((UINT)v.size(), v.data());
    };
    auto kick = [&] {  // wait for everything, then let the next submission drain the profiler
        bool ok = wait();
        submit({lists[0]});
        return wait() && ok;
    };
    // Mode 1 samples a PSO at most once a second: wait out the window before each intended sample.
    auto window = [&] {
        if (mode == 1) Sleep(1100);
    };
    // Earlier lists' samples drain on the next submission: let them, then read only from here on.
    kick();
    {
        FILE* f = _wfsopen(L"legacygpu.log", L"rb", _SH_DENYNO);
        if (f) fseek(f, 0, SEEK_END), g_logPos = ftell(f), fclose(f);
    }

    D3D12_VIEWPORT vp = {0, 0, (float)fx.width, (float)fx.height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)fx.width, (LONG)fx.height};
    D3D12_TEXTURE_COPY_LOCATION dst = {fx.rtRb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {fx.rt};
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, fx.width, fx.height, 1, 256};
    auto transition = [&](ID3D12GraphicsCommandList6* l, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br = {};
        br.Transition = {fx.rt, 0, a, b};
        l->ResourceBarrier(1, &br);
    };
    auto prepare = [&](ID3D12GraphicsCommandList6* l, bool clear) {  // a recording list, ready to draw
        transition(l, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float black[4] = {};
        if (clear) l->ClearRenderTargetView(fx.rtv, black, 0, nullptr);
        l->OMSetRenderTargets(1, &fx.rtv, FALSE, nullptr);
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &sc);
        l->SetGraphicsRootSignature(fx.emuRs);
        l->SetGraphicsRootConstantBufferView(0, fx.cbVa + 512);
        l->SetPipelineState(fx.emuPso);
        return l;
    };
    auto begin = [&](int i, bool clear, ID3D12CommandAllocator* a = nullptr) {
        lists[i]->Reset(a ? a : allocs[i], nullptr);
        return prepare(lists[i], clear);
    };
    auto indirect = [&](ID3D12GraphicsCommandList6* l) {  // groups 10 and 11, 2 x 128 threads
        l->SetGraphicsRootConstantBufferView(1, fx.cbVa);
        l->ExecuteIndirect(fx.sig, 1, fx.argBuf, 0, nullptr, 0);
    };
    auto direct = [&](ID3D12GraphicsCommandList6* l) {  // group 20, top right
        l->SetGraphicsRootConstantBufferView(1, fx.cbVa + 256);
        l->DispatchMesh(1, 1, 1);
    };
    auto end = [&](ID3D12GraphicsCommandList6* l) {
        transition(l, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return SUCCEEDED(l->Close());
    };
    auto pixelsOk = [&](bool withDirect) {
        const unsigned char* px = nullptr;
        fx.rtRb->Map(0, nullptr, (void**)&px);
        auto at = [&](UINT x, UINT y, int id) {
            const unsigned char* q = px + y * 256 + x * 4;
            return abs(q[0] - id) <= 1 && abs(q[1] - 102) <= 1 && abs(q[2] - 64) <= 1 && q[3] == 255;
        };
        bool ok = at(8, 4, 10) && at(56, 12, 11) && at(56, 4, withDirect ? 20 : 11);
        fx.rtRb->Unmap(0, nullptr);
        return ok;
    };
    auto pixelsBlank = [&] {  // the cleared target: nothing drawn at any probe
        const unsigned char* px = nullptr;
        fx.rtRb->Map(0, nullptr, (void**)&px);
        bool ok = true;
        for (UINT o : {4u * 256 + 8 * 4, 12u * 256 + 56 * 4, 4u * 256 + 56 * 4})
            ok &= !px[o] && !px[o + 1] && !px[o + 2] && !px[o + 3];
        fx.rtRb->Unmap(0, nullptr);
        return ok;
    };

    // Two lists in one submission: an indirect call, then a direct one that draws over it.
    window();
    ID3D12GraphicsCommandList6* a = begin(1, true);
    indirect(a);
    bool closed = end(a);
    window();
    ID3D12GraphicsCommandList6* b = begin(2, false);
    direct(b);
    closed &= end(b);
    Check(closed, "two lists Close");
    submit({a, b});
    Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "two lists in one submission complete");
    Check(pixelsOk(true), "emulated pixels unchanged with this setting");
    std::vector<Sample> s = NewSamples();

    // Every phase runs its GPU work in all modes; sample expectations apply only with profiling on.
    {
        Expect(s.size() == 2 && CountFor(s, a) == 1 && CountFor(s, b) == 1,
              "one completed sample per list of a two-list submission");
        for (const Sample& x : s) {
            bool isA = x.list == Ptr(a);
            printf("info  profile sample list=%s computeTicks=%llu drawTicks=%llu cs=%s dispatch=%s draw=%u raw=%u "
                   "vertices=%u overflow=%u\n", isA ? "A" : "B", x.computeTicks, x.drawTicks, x.cs.c_str(),
                   x.dispatch.c_str(), x.drawIndices, x.rawIndices, x.vertexRaw, x.overflowGroups);
            Expect(x.freq == freq && x.computeTicks > 0 && x.drawTicks > 0,
                  "positive timings at the submitting queue's timestamp frequency");
            Expect(x.cs == (isA ? "256" : "128"), "CSInvocations = groups x 128 threads");
            Expect(x.dispatch == (isA ? "indirect" : "1,1,1"), "dispatch form recorded");
            Expect(x.drawIndices == (isA ? 12u : 6u) && x.rawIndices == x.drawIndices &&
                      x.vertexRaw == (isA ? 8u : 4u) && x.overflowGroups == 0,
                  "scratch header copied after the compute pass");
        }

        // Mode 1 rate limit: ten calls in one list give at most one sample.
        if (mode == 1) {
            window();
            ID3D12GraphicsCommandList6* l = begin(3, true);
            for (int i = 0; i < 10; ++i) indirect(l);
            end(l);
            submit({l});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "ten calls in one list: GPU work completes");
            Expect(CountFor(NewSamples(), l) == 1, "sampled mode takes one sample of ten back-to-back calls");
        }

        // A closed but unsubmitted list reports nothing and keeps its slot while another list completes.
        window();
        ID3D12GraphicsCommandList6* c = begin(4, true);
        indirect(c);
        end(c);
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "closed, unsubmitted list beside others: GPU work completes");
        Expect(CountFor(NewSamples(), c) == 0, "pending, unsubmitted list produces no sample");
        window();
        ID3D12GraphicsCommandList6* d = begin(5, true);
        indirect(d);
        end(d);
        submit({d});
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "submitted list: GPU work completes");
        std::vector<Sample> sd = NewSamples();
        submit({c});
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "late submission: GPU work completes");
        std::vector<Sample> sc2 = NewSamples();
        Expect(sd.size() == 1 && sc2.size() == 1 && CountFor(sd, d) == 1 && CountFor(sc2, c) == 1,
              "late-submitted list reports once after its own completion");
        Expect(sd.size() == 1 && sc2.size() == 1 && sd[0].slot != sc2[0].slot, "pending list's slot is not reused");

        // Serialized re-execution: the first completion is reported, the replay is not.
        window();
        ID3D12GraphicsCommandList6* e = begin(6, true);
        indirect(e);
        end(e);
        submit({e});
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "first execution: GPU work completes");
        size_t first = CountFor(NewSamples(), e);
        submit({e});
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "serialized re-execution: GPU work completes");
        size_t again = CountFor(NewSamples(), e);
        Expect(first == 1 && again == 0, "re-executed list: one sample, no duplicate");

        // Submitted but still in flight (the queue waits on a CPU-held fence): nothing is reported until the
        // profiler's own fence passes. Executing a list again while an earlier execution is still in flight
        // is invalid D3D12 usage and must never be done here: an earlier stress test that did so hung the
        // queue and left processes stuck in the driver.
        window();
        ID3D12GraphicsCommandList6* f = begin(7, true);
        indirect(f);
        end(f);
        ID3D12Fence* hold = nullptr;
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold));
        queue->Wait(hold, 1);
        submit({f});
        submit({lists[0]});  // drains the profiler while f is still pending
        Sleep(50);
        Expect(CountFor(NewSamples(), f) == 0, "in-flight submission produces no sample before its fence");
        hold->Signal(1);
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "held submission: GPU work completes");
        Expect(CountFor(NewSamples(), f) == 1, "in-flight submission reports once after completion");
        hold->Release();

        // Reset without submission discards that generation; the next one reports.
        window();
        ID3D12GraphicsCommandList6* g = begin(8, true);
        indirect(g);
        g->Close();
        window();
        begin(8, true);
        indirect(g);
        end(g);
        submit({g});
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "reset before submission: GPU work completes");
        Expect(CountFor(NewSamples(), g) == 1, "reset before submission: only the submitted generation reports");

        // Reset while the previous generation is in flight (second allocator): both report, distinct gens.
        window();
        ID3D12GraphicsCommandList6* h = begin(9, true);
        indirect(h);
        end(h);
        submit({h});
        window();
        begin(9, true, allocs[kLists]);
        indirect(h);
        end(h);
        submit({h});
        Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "reset while in flight: GPU work completes");
        std::vector<Sample> sh = NewSamples();
        Expect(CountFor(sh, h) == 2 && sh.size() == 2 && sh[0].gen != sh[1].gen,
              "reset while in flight: each generation reports once");

        // The game's own pipeline-statistics query stays intact; ours is skipped inside it.
        {
            D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1};
            ID3D12QueryHeap* qh = nullptr;
            dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qh));
            D3D12_HEAP_PROPERTIES rbHeap = {D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                      D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
            ID3D12Resource* rb = nullptr;
            dev->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&rb));
            window();
            ID3D12GraphicsCommandList6* l = begin(10, true);
            l->BeginQuery(qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            indirect(l);
            l->EndQuery(qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            l->ResolveQueryData(qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, rb, 0);
            end(l);
            submit({l});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "game statistics query around an emulated call");
            std::vector<Sample> sq = NewSamples();
            D3D12_QUERY_DATA_PIPELINE_STATISTICS* st = nullptr;
            rb->Map(0, nullptr, (void**)&st);
            printf("info  profile: game query CSInvocations=%llu VSInvocations=%llu\n", st->CSInvocations,
                   st->VSInvocations);
            Check(st->CSInvocations == 256 && st->VSInvocations > 0, "game query still counts the emulated work");
            rb->Unmap(0, nullptr);
            Expect(sq.size() == 1 && sq[0].cs == "n/a", "own statistics query skipped inside the game's");
            rb->Release();
            qh->Release();
        }

        // Game predication (D3D12_PREDICATION_OP_EQUAL_ZERO over an upload buffer holding 0 at 0 and 1 at 8):
        // a predicated call is never sampled, whether its predicate skips the work or not, and the game's
        // predicate still decides the pixels. SetPredication(null), ClearState and Reset make it eligible again.
        {
            D3D12_HEAP_PROPERTIES upHeap = {D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                      D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
            ID3D12Resource* pred = nullptr;  // GENERIC_READ includes PREDICATION
            dev->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&pred));
            UINT64* pv = nullptr;
            pred->Map(0, nullptr, (void**)&pv);
            pv[0] = 0, pv[1] = 1;
            pred->Unmap(0, nullptr);
            const UINT64 skip = 0, run = 8;
            auto predicated = [&](int i, UINT64 off) {  // clears first: ClearRenderTargetView is predicated too
                window();
                ID3D12GraphicsCommandList6* l = begin(i, true);
                l->SetPredication(pred, off, D3D12_PREDICATION_OP_EQUAL_ZERO);
                indirect(l);
                l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);  // the readback copy is predicated
                end(l);
                submit({l});
                Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "predicated call: GPU work completes");
                return CountFor(NewSamples(), l);
            };
            Expect(predicated(1, run) == 0, "false predicate (work runs): no sample");
            Check(pixelsOk(false), "false predicate: the call still draws");
            Expect(predicated(2, skip) == 0, "true predicate (work skipped): no sample");
            Check(pixelsBlank(), "true predicate: the call stays skipped");

            window();
            ID3D12GraphicsCommandList6* l = begin(3, true);
            l->SetPredication(pred, run, D3D12_PREDICATION_OP_EQUAL_ZERO);
            indirect(l);
            l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            window();  // the predicated call used this PSO's sampling window in mode 1
            indirect(l);
            end(l);
            submit({l});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "predication cleared: GPU work completes");
            Expect(CountFor(NewSamples(), l) == 1, "SetPredication(null): the next call is sampled");
            Check(pixelsOk(false), "predication cleared: pixels");

            window();
            l = begin(4, true);
            l->SetPredication(pred, run, D3D12_PREDICATION_OP_EQUAL_ZERO);
            indirect(l);
            l->ClearState(fx.emuPso);
            prepare(l, false);  // rebinds what ClearState unbound (the target is still a render target)
            window();
            indirect(l);
            end(l);
            submit({l});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "ClearState after predication: GPU work completes");
            Expect(CountFor(NewSamples(), l) == 1, "ClearState disables predication: the next call is sampled");
            Check(pixelsOk(false), "ClearState after predication: pixels");

            window();
            l = begin(5, true);
            l->SetPredication(pred, run, D3D12_PREDICATION_OP_EQUAL_ZERO);
            indirect(l);
            end(l);  // closed with the predicate still set, never submitted
            window();
            begin(5, true);
            indirect(l);
            end(l);
            submit({l});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "Reset after predication: GPU work completes");
            Expect(CountFor(NewSamples(), l) == 1, "Reset disables predication: the next generation is sampled");
            pred->Release();
        }

        // Released lists (created through the hooked CreateCommandList, destroyed by their last Release): the
        // private marker retires their metadata and their slots return under the fence rules. A list is
        // released only once the GPU is done with it.
        {
            ID3D12CommandAllocator* ta = nullptr;
            ID3D12GraphicsCommandList6* t = nullptr;
            auto create = [&] {
                dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ta));
                dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ta, nullptr, IID_PPV_ARGS(&t));
                prepare(t, true);
                return Ptr(t);
            };
            auto release = [&] {
                t->Release(), ta->Release();
                t = nullptr, ta = nullptr;
            };
            auto countPtr = [](const std::vector<Sample>& v, const std::string& p) {
                return (size_t)std::count_if(v.begin(), v.end(), [&](const Sample& x) { return x.list == p; });
            };

            // Never submitted: nothing reported, its slot returns at destruction.
            window();
            std::string p = create();
            indirect(t);
            end(t);
            release();
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "released unsubmitted list: GPU work completes");
            Expect(countPtr(NewSamples(), p) == 0, "released unsubmitted list: no sample");

            // Completed and already reported, then released: no duplicate, slot returned.
            window();
            p = create();
            indirect(t);
            end(t);
            submit({t});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "released completed list: GPU work completes");
            size_t before = countPtr(NewSamples(), p);
            release();
            Check(kick(), "released completed list: drain");
            Expect(before == 1 && countPtr(NewSamples(), p) == 0, "released completed list: one sample, no duplicate");

            // Still pending for the profiler (queue held on a CPU fence and drained while held), released after
            // the GPU finished but before the profiler looked again: it reports once, from its own fence.
            window();
            p = create();
            indirect(t);
            end(t);
            ID3D12Fence* hold = nullptr;
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold));
            queue->Wait(hold, 1);
            submit({t});
            submit({lists[0]});
            Sleep(50);
            size_t early = countPtr(NewSamples(), p);
            hold->Signal(1);
            bool done = wait();
            release();
            Check(done && kick() && dev->GetDeviceRemovedReason() == S_OK, "released pending list: GPU work completes");
            Expect(early == 0 && countPtr(NewSamples(), p) == 1, "released pending list: one sample after its fence");
            hold->Release();

            // Every-call mode: more sampled transient lists than slots. Without retirement they would hold every
            // slot and later calls would be dropped (the final drop count is exact).
            if (mode == 2) {
                for (int i = 0; i < 140; ++i) {
                    create();
                    indirect(t);
                    end(t);
                    release();
                }
                ID3D12GraphicsCommandList6* l = begin(1, true);
                indirect(l);
                end(l);
                submit({l});
                Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "after 140 released lists: GPU work completes");
                Expect(CountFor(NewSamples(), l) == 1, "after 140 released transient lists: still sampled");
            }
        }

        // A queue created through ID3D12Device9::CreateCommandQueue1 is covered: its submission is tracked by
        // its own fence and reported once, attributed to that queue.
        {
            ID3D12Device9* dev9 = nullptr;
            ID3D12CommandQueue* q1 = nullptr;
            D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
            const GUID creator = {0x5b1c7d0e, 0x2a41, 0x4f6b, {0x9e, 0x37, 0x1d, 0x80, 0x52, 0xc4, 0x6a, 0x13}};
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev9))) &&
                SUCCEEDED(dev9->CreateCommandQueue1(&qd, creator, IID_PPV_ARGS(&q1)))) {
                window();
                ID3D12GraphicsCommandList6* l = begin(6, true);
                indirect(l);
                end(l);
                ID3D12CommandList* one[] = {l};
                q1->ExecuteCommandLists(1, one);
                q1->Signal(fence, ++fenceValue);
                fence->SetEventOnCompletion(fenceValue, ev);
                bool done = WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
                Check(done && kick() && dev->GetDeviceRemovedReason() == S_OK,
                      "CreateCommandQueue1 queue: GPU work completes");
                std::vector<Sample> sq = NewSamples();
                Expect(sq.size() == 1 && CountFor(sq, l) == 1 && sq[0].queue == Ptr(q1),
                       "CreateCommandQueue1 queue: one sample, attributed to that queue");
                Check(pixelsOk(false), "CreateCommandQueue1 queue: pixels");
                q1->Release();
            } else {
                printf("info  profile: ID3D12Device9::CreateCommandQueue1 unavailable, queue-1 check skipped\n");
            }
            if (dev9) dev9->Release();
        }

        // Exhaustion (every-call mode): all slots taken; extra calls lose diagnostics, not geometry.
        for (int i = 1; i < kLists; ++i) {  // end every earlier generation so their slots return
            lists[i]->Reset(allocs[i], nullptr);
            lists[i]->Close();
        }
        if (mode == 2) {
            ID3D12GraphicsCommandList6* l = begin(11, true);
            for (int i = 0; i < 140; ++i) indirect(l);
            direct(l);
            end(l);
            submit({l});
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "141 calls with 128 slots complete");
            Expect(CountFor(NewSamples(), l) == 128, "exactly the 128 available slots are sampled");
            Check(pixelsOk(true), "unsampled calls still draw their geometry");
            lists[11]->Reset(allocs[11], nullptr);
            lists[11]->Close();
        }
    }

    // Keep one recording list's feedback ownership alive while another tracked allocator is
    // destroyed. Its unsubmitted feedback slots return before the first list records again.
    {
        ID3D12CommandAllocator *parentAlloc = nullptr, *discardAlloc = nullptr;
        ID3D12GraphicsCommandList6 *parent = nullptr, *discard = nullptr;
        HRESULT cacheHr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&parentAlloc));
        if (SUCCEEDED(cacheHr)) cacheHr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&discardAlloc));
        if (SUCCEEDED(cacheHr)) cacheHr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            parentAlloc, nullptr, IID_PPV_ARGS(&parent));
        if (SUCCEEDED(cacheHr)) cacheHr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            discardAlloc, nullptr, IID_PPV_ARGS(&discard));
        Check(SUCCEEDED(cacheHr), "feedback ownership: interleaved recording resources");
        if (SUCCEEDED(cacheHr)) {
            prepare(parent, true);
            indirect(parent);
            prepare(discard, true);
            indirect(discard);
            end(discard);
            discard->Release(), discardAlloc->Release();
            discard = nullptr, discardAlloc = nullptr;
            direct(parent);
            end(parent);
            submit({parent});
            bool done = kick();
            Check(done && dev->GetDeviceRemovedReason() == S_OK, "feedback ownership: recording survives another allocator's destruction");
            if (!done) ExitProcess(1);  // prevent caller cleanup/reset while GPU completion is unknown
            Check(pixelsOk(true), "feedback ownership: original indirect and direct group pixels remain correct");
            parentAlloc->Reset();  // completed feedback is read only after the GPU wait
        }
        if (parent) parent->Release();
        if (parentAlloc) parentAlloc->Release();
        if (discard) discard->Release();
        if (discardAlloc) discardAlloc->Release();
        NewSamples();  // this ownership regression is independent of the sampling-rate expectations below
    }

    if (!mode) {
        // Nothing follows these draws but Close, which returns the scratch to COMMON.
        allocs[1]->Reset();  // all earlier submissions have completed through kick()
        a = begin(1, true);
        indirect(a);
        direct(a);
        Check(SUCCEEDED(a->Close()), "Close boundary: drawing list closes");
        b = lists[2];
        b->Reset(allocs[2], nullptr);
        transition(b, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        bool copyClosed = SUCCEEDED(b->Close());
        submit({a, b});
        Check(copyClosed && kick() && dev->GetDeviceRemovedReason() == S_OK,
              "Close boundary: both lists complete");
        Check(pixelsOk(true), "Close boundary: draws keep their arguments and order");
    }

    // Fixed workload, raw GPU time between test-owned timestamps. One submission: 8 warm-up lists (so the GPU is
    // at steady clocks), then M and U, each 128 emulated indirect calls. M is recorded first, so with every-call
    // sampling it takes all 128 slots and U (recorded after) is unsampled: M - U is the sampling cost at equal
    // clocks. Both render the same geometry. 6 runs, the first discarded.
    size_t workloadDropped = 0;
    {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4};
        ID3D12QueryHeap* qh = nullptr;
        dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qh));
        D3D12_HEAP_PROPERTIES rbHeap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        ID3D12Resource* rb = nullptr;
        dev->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&rb));
        std::vector<double> mUs, uUs;
        size_t mSamples = 0, uSamples = 0;
        for (int run = 0; run < 6; ++run) {
            std::vector<ID3D12CommandList*> order;
            for (int i = 1; i <= 10; ++i) {  // 1 = M, 2 = U, 3..10 warm-up; recorded in this order
                ID3D12GraphicsCommandList6* l = begin(i, true);
                if (i <= 2) l->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, (i - 1) * 2);
                for (int c = 0; c < 128; ++c) indirect(l);
                if (i <= 2) l->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, (i - 1) * 2 + 1);
                if (i == 2) l->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, rb, 0);
                end(l);
                if (i > 2) order.push_back(l);
            }
            order.push_back(lists[1]);
            order.push_back(lists[2]);
            queue->ExecuteCommandLists((UINT)order.size(), order.data());
            Check(kick() && dev->GetDeviceRemovedReason() == S_OK, "fixed workload: GPU work completes");
            std::vector<Sample> sw = NewSamples();
            mSamples += CountFor(sw, lists[1]), uSamples += CountFor(sw, lists[2]);
            if (mode == 2) workloadDropped += 9 * 128;
            UINT64* t = nullptr;
            rb->Map(0, nullptr, (void**)&t);
            if (run) mUs.push_back((t[1] - t[0]) * 1e6 / (double)freq), uUs.push_back((t[3] - t[2]) * 1e6 / (double)freq);
            rb->Unmap(0, nullptr);
        }
        for (int i = 1; i <= 10; ++i) lists[i]->Reset(allocs[i], nullptr), lists[i]->Close();
        std::sort(mUs.begin(), mUs.end());
        std::sort(uUs.begin(), uUs.end());
        double m = mUs[mUs.size() / 2], u = uUs[uUs.size() / 2];
        printf("info  fixed workload (%s): mode=%d 128 emulated calls per list, %zu runs, GPU us M median=%.1f "
               "(min %.1f) U median=%.1f (min %.1f) M-U=%.1f us (%.3f us/call) samples M=%zu U=%zu\n",
               mode ? "profiling overhead" : "emulation", mode,
               mUs.size(), m, mUs.front(), u, uUs.front(), m - u, (m - u) / 128, mSamples, uSamples);
        Check(pixelsOk(false), "fixed workload pixels");
        if (mode == 2) Check(mSamples == 6 * 128 && uSamples == 0, "every-call mode: M fully sampled, U dropped");
        rb->Release();
        qh->Release();
    }

    // Counters, through the 5 s statistics flush on SetPipelineState.
    Sleep(5100);
    lists[1]->Reset(allocs[1], nullptr);
    lists[1]->SetPipelineState(fx.emuPso);
    lists[1]->Close();
    g_logPos = 0;  // a stats line may have been flushed (and skipped by NewSamples) earlier
    std::vector<std::string> stats = NewLines("MESHPROF");
    std::string last;
    for (const std::string& l : stats)
        if (l.find("MESHPROF stats") != std::string::npos) last = l;
    if (mode == 0) {
        // The pass profiler shares the queue and fence tracking, which logs under MESHPROF too.
        if (GetPrivateProfileIntW(L"legacygpu", L"PassPerformanceDiagnostics", 0, L".\\legacygpu.ini"))
            stats.erase(std::remove_if(stats.begin(), stats.end(), [](const std::string& l) {
                            return l.find("MESHPROF queue=") != std::string::npos ||
                                   l.find("MESHPROF coverage ") != std::string::npos ||
                                   l.find("MESHPROF fence ") != std::string::npos;
                        }), stats.end());
        Check(stats.empty(), "disabled: no profiler resources, fences, samples or counters logged");
    } else {
        printf("info  %s", last.c_str());
        const char* l = last.c_str();
        Check(!last.empty() && atoi(Field(l, "replayedSubmissions").c_str()) >= 1 &&
                  atoi(Field(l, "unsubmittedDiscarded").c_str()) >= 1 &&
                  atoi(Field(l, "statsSkippedGameQuery").c_str()) == 1 && atoi(Field(l, "badTimestamps").c_str()) == 0 &&
                  atoi(Field(l, "slotsHeld").c_str()) == 0,
              "counters: replays, discarded, skipped stats; no bad timestamps; all slots returned");
        Check(atoi(Field(l, "coverageLost").c_str()) == 0 && atoi(Field(l, "unmarkedLists").c_str()) == 0 &&
                  atoi(Field(l, "predicatedSkipped").c_str()) >= 5 &&
                  atoi(Field(l, "listsDestroyed").c_str()) >= (mode == 2 ? 143 : 3) &&
                  atoi(Field(l, "lists").c_str()) <= kLists,
              "counters: coverage kept, predicated calls skipped, released lists retired, list metadata bounded");
        if (mode == 2) Check(atoi(Field(l, "profileSamplesDropped").c_str()) == 13 + (long)workloadDropped,
                             "dropped = 13 exhaustion calls + the workload lists recorded after M");
    }

    for (int i = 0; i < kLists; ++i) lists[i]->Release();
    for (auto* al : allocs) al->Release();
    fence->Release();
    CloseHandle(ev);
    return g_fail;
}
