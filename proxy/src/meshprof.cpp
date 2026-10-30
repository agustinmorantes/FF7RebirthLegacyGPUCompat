// Sampled GPU profiling of emulated mesh calls (MeshPerformanceDiagnostics). A sample records,
// inside the game's own list, four timestamps (start of InjectMesh, before the converted MS
// dispatch, before the indexed draw, after it), a pipeline-statistics query around the converted MS dispatch and a copy of the
// 112-byte scratch header, resolved into one readback slot per pass.
//
// Ownership: the samples of one list generation (Reset to Reset) form a batch. A batch is read
// only after a private per-queue fence, signaled right after the ExecuteCommandLists that
// submitted it, has passed; its slots are recycled only once the list has been reset (or
// recreated) and every submission of that generation has completed. A batch submitted more
// than once is never reported, so a replayed list can't yield stale or duplicate samples. When
// no slot is free the call is simply not sampled: this drops diagnostics, never geometry.
//
// Coverage: every queue the game creates (CreateCommandQueue and CreateCommandQueue1) gets its
// ExecuteCommandLists hook verified before the game sees it. If any hook can't be installed or
// verified, a submission could be invisible, so sampling stops and every existing batch is
// quarantined for the process lifetime (never read, never recycled). A predicated list is never
// sampled: the header copy is predicated but queries and resolves are not; Reset and ClearState
// disable predication (documented). A destroyed list is noticed through a private-data marker
// carrying a unique id; its metadata is retired and its slots are recycled under the same fence
// rules as a Reset. If a marker can't be attached, sampling stops for good (existing batches stay
// tracked): a list whose destruction or predication we can't follow is never sampled.
//
// Pass profiler (PassPerformanceDiagnostics=N): the first list Reset at least N seconds after the
// previous window opens a kPassWindowMs window; every direct list (re)started inside it records a
// timestamp before the first draw or dispatch after each change of bound render targets or between
// compute and graphics work, and one more at Close, where its timestamps are resolved. A span runs
// from its timestamp to the next one in the same list, so it also includes the barriers, clears and
// copies recorded after its last draw. Such a generation is a batch under the rules above, holding
// timestamp chunks instead of slots; predication doesn't apply to timestamps or resolves. Render
// targets are labeled from the views created through the device (copied descriptors show as ?).
// Compute spans also split at each pipeline change and are labeled with the game's CS hash, tagged
// on compute PSOs created or loaded from a pipeline library while the profiler is enabled (others
// show as ?).
//
// PassResourceDiagnostics=1 adds, in one window opened by holding F12: every list's bound render-target
// resources per graphics span, every transition barrier (logged before the span that follows it), and
// a copy of each single-sample 2D texture the size of the last 7-target pass (the G-buffer) taken right
// after a barrier moves it to a shader-resource state, written raw to <proxy dir>\legacygpu_dump\.
#include <d3d12.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common.h"
#include "meshprof.h"

// Readback slot: timestamps at 0, D3D12_QUERY_DATA_PIPELINE_STATISTICS at 32, scratch header at 128.
static constexpr UINT kSlots = 128, kSlotBytes = 256, kStatsOff = 32, kHeaderOff = 128;
static constexpr ULONGLONG kGlobalMs = 125, kPsoMs = 1000;  // sampled mode: <= 8 calls/s, <= 1/s per PSO
// Pass profiler: 128 chunks of 256 timestamps; the last of each chunk is kept for a list's end.
static constexpr UINT kPassChunk = 256, kPassChunks = 128;
static constexpr ULONGLONG kPassWindowMs = 60;  // about two frames at the game's frame rate

struct PassView {
    UINT w, h, fmt, known;
    void* res;
};
struct PassTargets {  // RT0 and the render-target count label a graphics span; the handles identify it
    UINT n, hasDs, single;
    PassView rt, ds;
    SIZE_T rtHandle[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT], dsHandle;
    void* res[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];  // resource diagnostics: each target's resource if known
};
// Resource diagnostics: at most kDumpMax copies totalling kDumpBytes per process.
static constexpr UINT kDumpMax = 64;
static constexpr UINT64 kDumpBytes = 768ull << 20;
struct PassBarrier {
    UINT span;  // logged before this span
    void* res;
    UINT w, h, fmt, sub, before, after;
    int dump;  // index into Batch::dumps, -1: none
};
struct PassDump {
    ID3D12Resource* rb;
    void* src;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 rowBytes;
    UINT id;
};
static bool SameTargets(const PassTargets& a, const PassTargets& b) {
    return a.n == b.n && a.hasDs == b.hasDs && a.single == b.single && a.dsHandle == b.dsHandle &&
           !memcmp(a.rtHandle, b.rtHandle, sizeof(a.rtHandle)) && !memcmp(&a.rt, &b.rt, sizeof(a.rt)) &&
           !memcmp(&a.ds, &b.ds, sizeof(a.ds));
}
struct PassSpan {
    UINT ts;
    bool compute;
    PassTargets t;
    UINT draws, dispatches, indirect, emulated;
    void* pso;  // compute spans: the bound pipeline and the first 8 bytes of its CS hash (0 = unknown)
    UINT64 cs;
};

struct MeshProfPso {
    std::atomic<ULONGLONG> next{0};
    char ms[33];
};
struct SlotInfo {
    const MeshProfPso* pso;
    bool stats, indirect;
    UINT x, y, z;
    UINT pass, passes;
};
struct ProfDevice {
    void* dev;
    ID3D12QueryHeap *ts = nullptr, *stats = nullptr;
    ID3D12Resource* rb = nullptr;
    const unsigned char* map = nullptr;
    std::vector<UINT> free;
    SlotInfo info[kSlots];
    ID3D12QueryHeap* passTs = nullptr;  // pass profiler: kPassChunks * kPassChunk timestamps
    ID3D12Resource* passRb = nullptr;
    const unsigned char* passMap = nullptr;
    std::vector<UINT> passFree;  // free chunks
};
struct ProfQueue {
    void* queue;
    UINT id;
    D3D12_COMMAND_LIST_TYPE type;
    UINT64 freq = 0;
    ID3D12Fence* fence = nullptr;
    UINT64 next = 0;    // last fence value assigned, under submit
    std::mutex submit;  // keeps each ExecuteCommandLists and its Signal in fence-value order
};
struct Wait {
    ProfQueue* q;
    UINT64 value;
};
struct Batch {
    void* list;
    UINT gen;
    ProfDevice* dev;
    std::vector<UINT> slots;
    std::vector<Wait> waits;  // highest fence value per queue the generation was submitted to
    UINT submissions = 0;
    UINT64 lastCall = 0;
    bool ended = false, consumed = false, invalid = false;
    bool untracked = false;  // unhooked queue or quarantine: held for good, never read or recycled
    // Pass batches only: recorded under g_mutex until Close.
    bool pass = false, truncated = false;
    UINT window = 0;
    std::vector<UINT> chunks;
    std::vector<PassSpan> spans;
    UINT next = 0, reserved = 0, endTs = 0;  // next timestamp, the current chunk's kept last one, the end
    PassTargets targets = {};                // bound now; labels the next graphics span
    void* pso = nullptr;                     // bound now; splits and labels compute spans
    std::vector<PassBarrier> barriers;       // resource diagnostics
    std::vector<PassDump> dumps;
};
struct ListProf {
    UINT gen = 0;
    Batch* pass = nullptr;   // pass timestamps of the current generation, if sampled
    bool passRecording = false;  // pass is open until Close
    Batch* open = nullptr;   // samples of the current generation
    int gameStats = 0;       // the game's own pipeline-statistics queries open in this list
    bool predicated = false;  // the game's SetPredication buffer is set
    UINT64 marker = 0;        // id of the attached destruction marker; entries exist only with one
};
struct Dead {
    void* list;
    UINT64 marker;
};

// g_mutex is a leaf lock apart from Log and g_deadMutex; a queue's submit mutex is taken before it.
// g_deadMutex is a true leaf: list destruction (any thread, inside the runtime) takes only it.
static std::mutex g_mutex, g_deadMutex;
static std::vector<Dead> g_dead;  // destroyed lists not yet retired, under g_deadMutex
// g_unsafe: coverage lost, sampling stopped and batches quarantined; g_markFailed: sampling stopped.
static std::atomic<bool> g_deadPending, g_unsafe, g_markFailed;
static std::atomic<UINT64> g_markerIds;
static PatchFn g_patch;
static std::unordered_map<void*, ProfDevice*> g_devices;  // null: resources unavailable
static std::unordered_map<void*, ProfQueue*> g_queues;    // replaced entries stay allocated
static std::unordered_map<void*, ListProf> g_lists;
static std::unordered_map<void*, UINT> g_directQueues;    // direct queues created, by device
static std::vector<Batch*> g_batches;
static std::atomic<int> g_live, g_listCount;
static std::atomic<ULONGLONG> g_nextGlobal;
static UINT64 g_callSerial;
static UINT g_queueIds;
static std::atomic<long> s_samples, s_completed, s_dropped, s_invalid, s_replays, s_unsubmitted, s_unsupported,
    s_statsSkipped, s_badTimestamps, s_untracked, s_signalFailed, s_predicated, s_quarantined, s_destroyed,
    s_unmarked;
std::atomic<int> g_passRecording;
static std::atomic<ULONGLONG> g_passNext, g_passEnd;  // next window may open at / current one ends at
static std::atomic<UINT> g_passWindow;
static std::atomic<long> s_passLists, s_passSpans, s_passBad, s_passInvalid, s_passTruncated, s_passNoDevice;
static std::mutex g_viewMutex;  // leaf
static std::unordered_map<SIZE_T, PassView> g_views;  // RTV and DSV descriptors, by CPU handle
static UINT g_gbufW, g_gbufH, g_dumps;  // resource diagnostics, under g_mutex
static UINT64 g_dumpBytes;
static std::atomic<UINT> g_resWindow;  // the window recording resources, 0 until F12 opens it

static bool Coverage() {  // queue, submission and destruction tracking for either profiler
    return g_cfg.meshPerformanceDiagnostics || g_cfg.passPerformanceDiagnostics;
}

using PFN_ECL = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateQueue = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using PFN_CreateQueue1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device9*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, REFIID,
                                                     void**);
using PFN_Query = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
using PFN_SetPredication = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64,
                                                     D3D12_PREDICATION_OP);
static PFN_ECL o_ExecuteCommandLists;
static PFN_CreateQueue o_CreateCommandQueue;
static PFN_CreateQueue1 o_CreateCommandQueue1;
static PFN_Query o_BeginQuery, o_EndQuery;
static PFN_SetPredication o_SetPredication;
static HRESULT(STDMETHODCALLTYPE* o_Close)(ID3D12GraphicsCommandList*);
static void(STDMETHODCALLTYPE* o_OMSetRenderTargets)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                     BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
static void(STDMETHODCALLTYPE* o_ResourceBarrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
static void(STDMETHODCALLTYPE* o_CreateRenderTargetView)(ID3D12Device*, ID3D12Resource*,
                                                         const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE* o_CreateDepthStencilView)(ID3D12Device*, ID3D12Resource*,
                                                         const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static HRESULT STDMETHODCALLTYPE H_Close(ID3D12GraphicsCommandList*);
static void STDMETHODCALLTYPE H_OMSetRenderTargets(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                   BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
static void STDMETHODCALLTYPE H_ExecuteCommandLists(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static void STDMETHODCALLTYPE H_BeginQuery(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
static void STDMETHODCALLTYPE H_EndQuery(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
static void STDMETHODCALLTYPE H_SetPredication(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64,
                                               D3D12_PREDICATION_OP);

static bool Hooked(void* obj, int idx, void* hook) {
    return (*(void***)obj)[idx] == hook;
}

// Owned by the list through private data, so it neither AddRefs nor outlives the list: the
// runtime releases it when the list is destroyed. Only queues (address, id) for retirement; the id
// keeps a late record from retiring a newer list at the same address.
// {c28edb30-5e0e-4f9c-b0bf-96accf601899}, private to this profiler.
static const GUID kMarkerGuid = {0xc28edb30, 0x5e0e, 0x4f9c, {0xb0, 0xbf, 0x96, 0xac, 0xcf, 0x60, 0x18, 0x99}};
struct Marker final : IUnknown {
    std::atomic<ULONG> refs{1};
    void* list;  // null: never attached, nothing to retire
    UINT64 id;
    Marker(void* l, UINT64 i) : list(l), id(i) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = IsEqualIID(riid, IID_IUnknown) ? this : nullptr;
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (!r) {
            if (list) {
                std::lock_guard<std::mutex> lock(g_deadMutex);
                g_dead.push_back({list, id});
                g_deadPending = true;
            }
            delete this;
        }
        return r;
    }
};

// Our own queries bypass the query hooks, which track only the game's.
static void BeginQ(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    o_BeginQuery ? o_BeginQuery(cl, h, t, i) : cl->BeginQuery(h, t, i);
}
static void EndQ(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    o_EndQuery ? o_EndQuery(cl, h, t, i) : cl->EndQuery(h, t, i);
}

static ListProf& List(void* cl) {  // under g_mutex
    auto it = g_lists.find(cl);
    if (it != g_lists.end()) return it->second;
    ++g_listCount;
    return g_lists[cl];
}

static ProfDevice* Device(ID3D12Device* dev) {  // under g_mutex
    auto it = g_devices.find(dev);
    if (it != g_devices.end()) return it->second;
    ProfDevice* d = new ProfDevice();
    d->dev = dev;
    HRESULT hr = S_OK;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, (UINT64)kSlots * kSlotBytes, 1, 1, 1,
                              DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    if (g_cfg.meshPerformanceDiagnostics) {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * 4, 0};
        hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&d->ts));
        qd = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, kSlots, 0};
        if (SUCCEEDED(hr)) hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&d->stats));
        if (SUCCEEDED(hr))
            hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&d->rb));
        if (SUCCEEDED(hr)) hr = d->rb->Map(0, nullptr, (void**)&d->map);
        Log("MESHPROF resources device=%p slots=%u readbackBytes=%u hr=0x%08lx", dev, kSlots, kSlots * kSlotBytes, hr);
    }
    if (SUCCEEDED(hr) && g_cfg.passPerformanceDiagnostics) {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kPassChunks * kPassChunk, 0};
        hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&d->passTs));
        rd.Width = (UINT64)kPassChunks * kPassChunk * 8;
        if (SUCCEEDED(hr))
            hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&d->passRb));
        if (SUCCEEDED(hr)) hr = d->passRb->Map(0, nullptr, (void**)&d->passMap);
        Log("PASSPROF resources device=%p timestamps=%u readbackBytes=%llu hr=0x%08lx", dev, kPassChunks * kPassChunk,
            rd.Width, hr);
    }
    if (FAILED(hr)) {
        for (IUnknown* u : {(IUnknown*)d->ts, (IUnknown*)d->stats, (IUnknown*)d->rb, (IUnknown*)d->passTs,
                            (IUnknown*)d->passRb})
            if (u) u->Release();
        delete d;
        d = nullptr;
    } else {
        for (UINT i = kSlots; i--;) d->free.push_back(i);
        if (d->passTs)
            for (UINT i = kPassChunks; i--;) d->passFree.push_back(i);
    }
    g_devices[dev] = d;
    return d;
}

static ProfQueue* RegisterQueue(ID3D12CommandQueue* q) {  // under g_mutex
    ProfQueue* p = new ProfQueue();
    p->queue = q;
    p->id = ++g_queueIds;
    p->type = q->GetDesc().Type;
    if (FAILED(q->GetTimestampFrequency(&p->freq))) p->freq = 0;
    g_queues[q] = p;
    Log("MESHPROF queue=%p queueId=%u type=%d freqHz=%llu", q, p->id, p->type, p->freq);
    return p;
}

static bool Complete(const Batch& b, bool* removed) {
    if (b.untracked) return false;
    for (const Wait& w : b.waits) {
        UINT64 v = w.q->fence->GetCompletedValue();
        if (v == UINT64_MAX) *removed = true;  // device removed: the data can't be trusted
        if (v < w.value) return false;
    }
    return true;
}

static void FormatView(char* out, size_t size, const PassView& v) {
    if (v.known) sprintf_s(out, size, "%ux%u:%u", v.w, v.h, v.fmt);
    else strcpy_s(out, size, "?");
}

static void LogBarriers(const Batch& b, size_t span) {
    for (const PassBarrier& x : b.barriers)
        if (x.span == span)
            Log("PASSPROF barrier window=%u list=%p gen=%u beforeSpan=%zu res=%p view=%ux%u:%u sub=0x%x state=0x%x->0x%x "
                "dump=%d",
                b.window, b.list, b.gen, span, x.res, x.w, x.h, x.fmt, x.sub, x.before, x.after,
                x.dump < 0 ? -1 : (int)b.dumps[x.dump].id);
}

// Raw rows, tightly packed; the name carries id, resource, width, height, rows, DXGI format and row bytes.
static void WriteDumps(const Batch& b) {
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%slegacygpu_dump", ProxyDir());
    CreateDirectoryW(dir, nullptr);
    for (const PassDump& d : b.dumps) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%03u_%p_%ux%u_r%u_f%u_b%llu.raw", dir, d.id, d.src, d.fp.Footprint.Width,
                   d.fp.Footprint.Height, d.rows, d.fp.Footprint.Format, d.rowBytes);
        const unsigned char* p = nullptr;
        D3D12_RANGE r = {0, (SIZE_T)d.fp.Footprint.RowPitch * (d.rows - 1) + (SIZE_T)d.rowBytes};  // last row unpadded
        FILE* f = nullptr;
        HRESULT hr = d.rb->Map(0, &r, (void**)&p);
        errno_t e = FAILED(hr) ? 0 : _wfopen_s(&f, path, L"wb");
        if (FAILED(hr) || e || !f) {
            Log("WARN: PASSPROF dump %u could not be written: map=0x%08lx errno=%d path=%ls", d.id, hr, (int)e, path);
            if (p) d.rb->Unmap(0, nullptr);
            continue;
        }
        for (UINT y = 0; y < d.rows; ++y) fwrite(p + (size_t)y * d.fp.Footprint.RowPitch, 1, (size_t)d.rowBytes, f);
        fclose(f);
        D3D12_RANGE none = {0, 0};
        d.rb->Unmap(0, &none);
    }
}

// One line per span in recording order, then one per list. Formats are DXGI_FORMAT values.
static void ConsumePass(const Batch& b) {
    const ProfQueue& q = *b.waits[0].q;
    const UINT64* t = (const UINT64*)b.dev->passMap;
    double us = q.freq ? 1e6 / (double)q.freq : 0;
    long bad = 0;
    for (size_t i = 0; i < b.spans.size(); ++i) {
        const PassSpan& s = b.spans[i];
        LogBarriers(b, i);
        if (b.window == g_resWindow.load() && !s.compute && s.t.n) {
            char res[8 * 20] = "";
            for (UINT k = 0; k < (std::min)(s.t.n, (UINT)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT); ++k)
                sprintf_s(res + strlen(res), sizeof(res) - strlen(res), "%s%p", k ? "," : "", s.t.res[k]);
            Log("PASSPROF targets window=%u list=%p gen=%u idx=%zu rts=%s ds=%p", b.window, b.list, b.gen, i, res,
                s.t.hasDs ? s.t.ds.res : nullptr);
        }
        UINT64 start = t[s.ts], end = t[i + 1 < b.spans.size() ? b.spans[i + 1].ts : b.endTs];
        if (!q.freq || end < start) {
            ++bad;
            continue;
        }
        char rt[48] = "-", ds[40] = "-";
        if (!s.compute && s.t.n) {
            FormatView(rt, sizeof(rt) - 8, s.t.rt);
            sprintf_s(rt + strlen(rt), 8, "*%u", s.t.n);
        }
        if (!s.compute && s.t.hasDs) FormatView(ds, sizeof(ds), s.t.ds);
        char cs[17] = "-";
        if (s.compute && !s.cs) strcpy_s(cs, "?");
        for (int k = 0; s.compute && s.cs && k < 8; ++k) sprintf_s(cs + 2 * k, 3, "%02x", ((const unsigned char*)&s.cs)[k]);
        Log("PASSPROF span window=%u list=%p gen=%u idx=%zu start=%llu ticks=%llu us=%.3f kind=%s rt=%s ds=%s draws=%u "
            "dispatches=%u indirect=%u emulated=%u cs=%s",
            b.window, b.list, b.gen, i, start, end - start, (end - start) * us, s.compute ? "cs" : "gfx", rt, ds, s.draws,
            s.dispatches, s.indirect, s.emulated, cs);
    }
    LogBarriers(b, b.spans.size());
    WriteDumps(b);
    Log("PASSPROF list window=%u list=%p gen=%u queueId=%u freqHz=%llu spans=%zu badSpans=%ld truncated=%d", b.window,
        b.list, b.gen, q.id, q.freq, b.spans.size(), bad, (int)b.truncated);
    s_passSpans += (long)b.spans.size() - bad;
    s_passBad += bad;
}

static void Consume(Batch& b) {
    b.consumed = true;
    if (b.invalid || b.submissions != 1 || b.waits.size() != 1) {
        s_invalid += (long)b.slots.size();
        s_passInvalid += (long)b.spans.size();
        return;
    }
    if (b.pass) return ConsumePass(b);
    const ProfQueue& q = *b.waits[0].q;
    for (UINT slot : b.slots) {
        const SlotInfo& info = b.dev->info[slot];
        const unsigned char* base = b.dev->map + (size_t)slot * kSlotBytes;
        const UINT64* t = (const UINT64*)base;
        const auto* st = (const D3D12_QUERY_DATA_PIPELINE_STATISTICS*)(base + kStatsOff);
        const UINT* h = (const UINT*)(base + kHeaderOff);
        if (!q.freq || t[0] > t[1] || t[1] > t[2] || t[2] > t[3]) {
            ++s_badTimestamps;
            continue;
        }
        char cs[24] = "n/a", dims[40] = "indirect";
        if (info.stats) sprintf_s(cs, "%llu", st->CSInvocations);
        if (!info.indirect) sprintf_s(dims, "%u,%u,%u", info.x, info.y, info.z);
        double us = 1e6 / (double)q.freq;
        Log("MESHPROF sample ms=%s dev=%p queue=%p queueId=%u list=%p gen=%u slot=%u fence=%llu freqHz=%llu "
            "computeTicks=%llu drawTicks=%llu computeUs=%.3f drawUs=%.3f setupUs=%.3f dispatchUs=%.3f "
            "csInvocations=%s dispatch=%s "
            "drawIndices=%u rawIndices=%llu vertexRaw=%u overflowGroups=%u activeGroups=unknown retryPass=%u/%u",
            info.pso->ms, b.dev->dev, q.queue, q.id, b.list, b.gen, slot, b.waits[0].value, q.freq, t[2] - t[0],
            t[3] - t[2], (t[2] - t[0]) * us, (t[3] - t[2]) * us, (t[1] - t[0]) * us, (t[2] - t[1]) * us, cs,
            dims, h[0],
            (UINT64)h[7] + (info.pass ? 0 : h[6]), h[5], h[13], info.pass + 1, info.passes);
        ++s_completed;
    }
}

// Reads a batch once all its submissions completed and frees it once its generation ended too.
static bool Visit(Batch* b) {  // under g_mutex; true = freed
    if (b->untracked) return false;  // even with no observed submission: one may be invisible
    bool removed = false;
    bool done = b->submissions && Complete(*b, &removed);
    if (removed) b->invalid = true;
    if (done && !b->consumed) Consume(*b);
    if (!b->ended || (b->submissions && !done)) return false;
    if (!b->submissions) s_unsubmitted += (long)b->slots.size();  // never executed, so never written
    for (UINT slot : b->slots) b->dev->free.push_back(slot);
    for (UINT chunk : b->chunks) b->dev->passFree.push_back(chunk);
    for (const PassDump& d : b->dumps) d.rb->Release();
    delete b;
    return true;
}

static void DrainLocked() {
    for (size_t i = 0; i < g_batches.size();) {
        if (Visit(g_batches[i])) {
            g_batches[i] = g_batches.back();
            g_batches.pop_back();
            --g_live;
        } else {
            ++i;
        }
    }
}

// A destroyed list's generation ends like a Reset: Visit frees its slots only after every known
// submission's fence (at once if it was never submitted), and the address can be reused fresh.
static void EndPassLocked(ListProf& lp) {
    if (lp.passRecording) lp.passRecording = false, --g_passRecording;
    if (lp.pass) lp.pass->ended = true, lp.pass = nullptr;
}

static void RetireLocked(std::unordered_map<void*, ListProf>::iterator it) {
    if (it->second.open) it->second.open->ended = true;
    EndPassLocked(it->second);
    g_lists.erase(it);
    --g_listCount;
    ++s_destroyed;
}

static void ReapLocked() {
    if (!g_deadPending.load()) return;
    std::vector<Dead> dead;
    {
        std::lock_guard<std::mutex> lock(g_deadMutex);
        dead.swap(g_dead);
        g_deadPending = false;
    }
    for (const Dead& x : dead) {
        auto it = g_lists.find(x.list);
        if (it != g_lists.end() && it->second.marker == x.marker) RetireLocked(it);
    }
    DrainLocked();
}

// *id = 0: no marker of ours on this object. False: the lookup itself failed, nothing is known.
static bool MarkerId(ID3D12GraphicsCommandList* cl, UINT64* id) {
    IUnknown* u = nullptr;
    UINT size = sizeof(u);
    HRESULT hr = cl->GetPrivateData(kMarkerGuid, &size, &u);
    *id = 0;
    if (hr == DXGI_ERROR_NOT_FOUND) return true;
    if (FAILED(hr) || !u) return false;
    *id = static_cast<Marker*>(u)->id;  // only our markers are stored under this GUID
    u->Release();
    return true;
}

static void MarkFailed(HRESULT hr) {
    ++s_unmarked;
    if (!g_markFailed.exchange(true))
        Log("WARN: MESHPROF destruction marker unavailable (hr=0x%08lx); sampling stops", hr);
}

// Makes sure the object carries a marker matching its metadata, outside g_mutex: private-data calls
// may release a marker, and the runtime releases it on whichever thread destroys the list. An entry
// whose marker isn't on the object belongs to a destroyed list at the same address: it is retired
// here even if that list's destruction record hasn't been reaped yet (the record is then ignored).
static bool EnsureMarked(ID3D12GraphicsCommandList* cl) {
    if (g_markFailed) return false;
    UINT64 have;
    if (!MarkerId(cl, &have)) {  // never retire metadata on an unknown answer
        MarkFailed(E_FAIL);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReapLocked();
        auto it = g_lists.find(cl);
        if (it != g_lists.end() && it->second.marker != have) {
            RetireLocked(it);
            DrainLocked();
        }
        if (have) {
            List(cl).marker = have;
            return true;
        }
    }
    Marker* m = new Marker(cl, ++g_markerIds);
    HRESULT hr = cl->SetPrivateDataInterface(kMarkerGuid, m);
    if (FAILED(hr)) m->list = nullptr;
    UINT64 id = m->id;
    m->Release();  // on success the list holds the only reference
    if (FAILED(hr)) {
        MarkFailed(hr);
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ReapLocked();
    List(cl).marker = id;
    return true;
}

// A submission may have bypassed ExecuteCommandLists: no batch can be proven complete any more.
static void Quarantine(const char* why) {
    if (g_unsafe.exchange(true)) return;
    Log("WARN: MESHPROF %s; sampling stops and every existing sample is quarantined for good", why);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Batch* b : g_batches)
        if (!b->untracked) b->untracked = true, s_quarantined += (long)b->slots.size();
}

MeshProfPso* MeshProfNewPso(const char* msHash) {
    if (!g_cfg.meshPerformanceDiagnostics) return nullptr;
    MeshProfPso* p = new MeshProfPso();
    strcpy_s(p->ms, msHash);
    return p;
}

bool MeshProfBegin(ID3D12GraphicsCommandList* cl, MeshProfPso* pso, bool indirect, UINT x, UINT y, UINT z,
                   UINT passes, MeshProfSample* out) {
    if (!pso || !o_ExecuteCommandLists || g_unsafe || g_markFailed) return false;  // a submission we can't see would free slots early
    if (g_cfg.meshPerformanceDiagnostics == 1) {
        ULONGLONG now = GetTickCount64();
        if (now < pso->next.load(std::memory_order_relaxed)) return false;
        ULONGLONG g = g_nextGlobal.load(std::memory_order_relaxed);
        if (now < g || !g_nextGlobal.compare_exchange_strong(g, now + kGlobalMs)) return false;
        pso->next.store(now + kPsoMs, std::memory_order_relaxed);
    }
    // Bundles can't hold queries; a list class without our predication and query hooks can't be judged.
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !Hooked(cl, vt_cl_SetPredication, (void*)H_SetPredication) ||
        !Hooked(cl, vt_cl_BeginQuery, (void*)H_BeginQuery) || !Hooked(cl, vt_cl_EndQuery, (void*)H_EndQuery)) {
        ++s_unsupported;
        return false;
    }
    if (!EnsureMarked(cl)) return false;  // its destruction would go unseen
    ID3D12Device* dev = nullptr;
    if (FAILED(cl->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    ReapLocked();
    ProfDevice* d = g_unsafe ? nullptr : Device(dev);  // rechecked here: no allocation after a quarantine sweep
    dev->Release();  // the list keeps the device alive
    ListProf& lp = List(cl);
    // Predication skips the header copy but not the queries or resolves: the slot would mix fresh
    // timings with a stale header.
    if (lp.predicated) {
        ++s_predicated;
        return false;
    }
    // A submitted generation recording again means a Reset was missed: don't mix generations.
    if (!d || d->free.size() < passes || (lp.open && lp.open->submissions)) {
        ++s_dropped;
        return false;
    }
    if (!lp.open) {
        lp.open = new Batch{cl, lp.gen, d};
        g_batches.push_back(lp.open);
        ++g_live;
    }
    bool stats = lp.gameStats == 0;  // never nest inside the game's own statistics query
    *out = {d, 0, stats, {}};
    for (UINT pass = 0; pass < passes; ++pass) {
        UINT slot = d->free.back();
        d->free.pop_back();
        lp.open->slots.push_back(slot);
        out->slots.push_back(slot);
        d->info[slot] = {pso, stats, indirect || pass != 0, x, y, z, pass, passes};
        s_statsSkipped += !stats;
        ++s_samples;
    }
    out->slot = out->slots[0];
    EndQ(cl, d->ts, D3D12_QUERY_TYPE_TIMESTAMP, out->slot * 4);
    return true;
}

void MeshProfStartPass(ID3D12GraphicsCommandList* cl, MeshProfSample& s, UINT pass) {
    s.slot = s.slots[pass];
    EndQ(cl, ((ProfDevice*)s.dev)->ts, D3D12_QUERY_TYPE_TIMESTAMP, s.slot * 4);
}

// Splits compute into setup (header reset, barriers, waits for earlier work) and the dispatch itself.
void MeshProfDispatch(ID3D12GraphicsCommandList* cl, const MeshProfSample& s, bool begin) {
    if (begin) EndQ(cl, ((ProfDevice*)s.dev)->ts, D3D12_QUERY_TYPE_TIMESTAMP, s.slot * 4 + 1);
    if (!s.stats) return;
    ID3D12QueryHeap* h = ((ProfDevice*)s.dev)->stats;
    (begin ? BeginQ : EndQ)(cl, h, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, s.slot);
}

void MeshProfPreDraw(ID3D12GraphicsCommandList* cl, const MeshProfSample& s) {
    EndQ(cl, ((ProfDevice*)s.dev)->ts, D3D12_QUERY_TYPE_TIMESTAMP, s.slot * 4 + 2);
}

// The scratch is in its read state here, which includes COPY_SOURCE; the readback buffer stays
// in COPY_DEST for its whole life.
void MeshProfEnd(ID3D12GraphicsCommandList* cl, const MeshProfSample& s, ID3D12Resource* scratch) {
    const ProfDevice& d = *(const ProfDevice*)s.dev;
    UINT64 base = (UINT64)s.slot * kSlotBytes;
    EndQ(cl, d.ts, D3D12_QUERY_TYPE_TIMESTAMP, s.slot * 4 + 3);
    cl->CopyBufferRegion(d.rb, base + kHeaderOff, scratch, 0, 112);
    cl->ResolveQueryData(d.ts, D3D12_QUERY_TYPE_TIMESTAMP, s.slot * 4, 4, d.rb, base);
    if (s.stats) cl->ResolveQueryData(d.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, s.slot, 1, d.rb, base + kStatsOff);
}

// True while a pass window is open; the first call after the interval opens the next one. With
// resource diagnostics, holding F12 (polled every 50 ms) opens the one resource window at once.
static bool PassWindow() {
    ULONGLONG now = GetTickCount64();
    if (now < g_passEnd.load(std::memory_order_relaxed)) return true;
    if (g_cfg.passResourceDiagnostics && !g_resWindow.load(std::memory_order_relaxed)) {
        static std::atomic<ULONGLONG> poll;
        ULONGLONG p = poll.load(std::memory_order_relaxed);
        if (now >= p && poll.compare_exchange_strong(p, now + 50) && (GetAsyncKeyState(VK_F12) & 0x8000)) {
            UINT w = ++g_passWindow;
            g_passEnd.store(now + kPassWindowMs);
            g_resWindow.store(w);
            Log("PASSPROF window=%u opens for %llu ms with resource diagnostics", w, kPassWindowMs);
            return true;
        }
    }
    ULONGLONG next = g_passNext.load(std::memory_order_relaxed);
    if (now < next || !g_passNext.compare_exchange_strong(next, now + 1000ull * g_cfg.passPerformanceDiagnostics))
        return false;
    g_passEnd.store(now + kPassWindowMs);
    Log("PASSPROF window=%u opens for %llu ms", ++g_passWindow, kPassWindowMs);
    return true;
}

// A list whose end, submissions or destruction we can't see is never sampled.
static bool PassEligible(ID3D12GraphicsCommandList* cl) {
    return o_ExecuteCommandLists && !g_unsafe && !g_markFailed && cl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
           Hooked(cl, vt_cl_Close, (void*)H_Close) &&
           Hooked(cl, vt_cl_OMSetRenderTargets, (void*)H_OMSetRenderTargets) &&
           EnsureMarked(cl);
}

void MeshProfListReset(void* cl) {
    auto gl = (ID3D12GraphicsCommandList*)cl;
    bool sample = g_cfg.passPerformanceDiagnostics && PassWindow() && PassEligible(gl);
    if (!sample && g_listCount.load(std::memory_order_relaxed) == 0) return;
    ID3D12Device* dev = nullptr;
    if (sample && FAILED(gl->GetDevice(IID_PPV_ARGS(&dev)))) sample = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    ReapLocked();
    auto it = g_lists.find(cl);
    if (it != g_lists.end()) {
        ListProf& lp = it->second;
        ++lp.gen;
        lp.gameStats = 0;
        lp.predicated = false;
        bool ended = lp.open || lp.pass;
        if (lp.open) lp.open->ended = true, lp.open = nullptr;
        EndPassLocked(lp);
        if (ended) DrainLocked();
        // EnsureMarked created the entry; a list destroyed since then is simply not sampled.
        ProfDevice* d = sample && !g_unsafe ? Device(dev) : nullptr;
        if (sample && (!d || !d->passTs)) ++s_passNoDevice;
        if (d && d->passTs) {
            lp.pass = new Batch{cl, lp.gen, d};
            lp.pass->pass = true;
            lp.pass->window = g_passWindow.load();
            g_batches.push_back(lp.pass);
            ++g_live;
            lp.passRecording = true;
            ++g_passRecording;
            ++s_passLists;
        }
    }
    if (dev) dev->Release();  // the list keeps the device alive
}

// ClearState disables predication (documented) and unbinds render targets; the generation and
// open queries are unaffected.
void MeshProfClearState(void* cl) {
    if (g_listCount.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ReapLocked();
    auto it = g_lists.find(cl);
    if (it == g_lists.end()) return;
    it->second.predicated = false;
    if (it->second.passRecording) it->second.pass->targets = {}, it->second.pass->pso = nullptr;
}

static const GUID kCsHashGuid = {0x5d0e3c71, 0x2b4a, 0x4e8f, {0x9a, 0x61, 0x0c, 0x7d, 0x3e, 0x22, 0xb4, 0x90}};

void PassProfTagPso(void* pso, const D3D12_SHADER_BYTECODE& cs) {
    if (!g_cfg.passPerformanceDiagnostics || !pso || !cs.pShaderBytecode || cs.BytecodeLength < 20) return;
    ID3D12Object* o = nullptr;
    if (FAILED(((IUnknown*)pso)->QueryInterface(IID_PPV_ARGS(&o)))) return;
    o->SetPrivateData(kCsHashGuid, 8, (const char*)cs.pShaderBytecode + 4);  // as logged by ShaderHash
    o->Release();
}

static UINT64 CsLabel(void* pso) {
    UINT64 id = 0;
    UINT size = sizeof(id);
    if (!pso || FAILED(((ID3D12PipelineState*)pso)->GetPrivateData(kCsHashGuid, &size, &id)) || size != sizeof(id))
        return 0;
    return id;
}

void PassProfPsoSlow(void* cl, void* pso) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_lists.find(cl);
    if (it != g_lists.end() && it->second.passRecording) it->second.pass->pso = pso;
}

// Takes the generation's next timestamp. The last one of each chunk is kept for the end, written
// here when no chunk is left: the batch is then truncated and records nothing more.
static bool NextTs(ID3D12GraphicsCommandList* cl, Batch& b, UINT* ts) {  // under g_mutex
    if (b.truncated) return false;
    if (b.next == b.reserved) {
        if (b.dev->passFree.empty()) {
            if (!b.chunks.empty()) {
                b.endTs = b.next++;
                EndQ(cl, b.dev->passTs, D3D12_QUERY_TYPE_TIMESTAMP, b.endTs);
            }
            b.truncated = true;
            ++s_passTruncated;
            return false;
        }
        UINT chunk = b.dev->passFree.back();
        b.dev->passFree.pop_back();
        b.chunks.push_back(chunk);
        b.next = chunk * kPassChunk, b.reserved = b.next + kPassChunk - 1;
    }
    *ts = b.next++;
    return true;
}

// Starts a span at the first work after a target change or a compute/graphics switch. Indirect
// calls (signature not tracked) and emulated calls stay in the current span's kind or graphics.
void PassProfWorkSlow(void* cl, PassWork w) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_lists.find(cl);
    if (it == g_lists.end() || !it->second.passRecording) return;
    Batch& b = *it->second.pass;
    if (b.truncated) return;
    bool compute = w == kPassDispatch || (w == kPassIndirect && !b.spans.empty() && b.spans.back().compute);
    PassSpan* s = b.spans.empty() ? nullptr : &b.spans.back();
    if (!s || s->compute != compute || (compute ? s->pso != b.pso : !SameTargets(s->t, b.targets))) {
        UINT ts;
        if (!NextTs((ID3D12GraphicsCommandList*)cl, b, &ts)) return;
        EndQ((ID3D12GraphicsCommandList*)cl, b.dev->passTs, D3D12_QUERY_TYPE_TIMESTAMP, ts);
        b.spans.push_back({ts, compute, compute ? PassTargets{} : b.targets});
        s = &b.spans.back();
        if (compute) s->pso = b.pso, s->cs = CsLabel(b.pso);
    }
    switch (w) {
    case kPassDraw: ++s->draws; break;
    case kPassDispatch: ++s->dispatches; break;
    case kPassIndirect: ++s->indirect; break;
    case kPassEmulated: ++s->emulated; break;
    }
}

// Ends the last span and resolves every timestamp written, chunk by chunk, into the readback at
// the same index; the kept last timestamp of a full chunk is written only as an end.
static HRESULT STDMETHODCALLTYPE H_Close(ID3D12GraphicsCommandList* cl) {
    if (g_passRecording.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_lists.find(cl);
        if (it != g_lists.end() && it->second.passRecording) {
            it->second.passRecording = false;
            --g_passRecording;
            Batch& b = *it->second.pass;
            if (!b.spans.empty() && !b.truncated) {
                b.endTs = b.next++;
                EndQ(cl, b.dev->passTs, D3D12_QUERY_TYPE_TIMESTAMP, b.endTs);
            }
            for (size_t i = 0; i < b.chunks.size(); ++i) {
                UINT first = b.chunks[i] * kPassChunk;
                UINT count = i + 1 < b.chunks.size() ? kPassChunk - 1 : b.next - first;
                cl->ResolveQueryData(b.dev->passTs, D3D12_QUERY_TYPE_TIMESTAMP, first, count, b.dev->passRb,
                                     (UINT64)first * 8);
            }
        }
    }
    return o_Close(cl);
}

static void STDMETHODCALLTYPE H_OMSetRenderTargets(ID3D12GraphicsCommandList* cl, UINT n,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (g_passRecording.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_lists.find(cl);
        if (it != g_lists.end() && it->second.passRecording) {
            PassTargets t = {n, ds != nullptr, (UINT)!!single};
            // A single range is identified by its base handle; the count tells the ranges apart.
            for (UINT i = 0; rts && i < (single ? 1u : (std::min)(n, (UINT)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)); ++i)
                t.rtHandle[i] = rts[i].ptr;
            if (ds) t.dsHandle = ds->ptr;
            std::lock_guard<std::mutex> views(g_viewMutex);
            if (n && rts) {
                auto v = g_views.find(rts[0].ptr);  // the first target either way, with or without single
                if (v != g_views.end()) t.rt = v->second;
            }
            if (g_cfg.passResourceDiagnostics) {  // a single range's later handles are not resolved
                for (UINT i = 0; rts && i < (single ? 1u : (std::min)(n, (UINT)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)); ++i) {
                    auto v = g_views.find(rts[i].ptr);
                    if (v != g_views.end()) t.res[i] = v->second.res;
                }
                if (n == 7 && t.rt.known) g_gbufW = t.rt.w, g_gbufH = t.rt.h;
            }
            if (ds) {
                auto v = g_views.find(ds->ptr);
                if (v != g_views.end()) t.ds = v->second;
            }
            it->second.pass->targets = t;
        }
    }
    o_OMSetRenderTargets(cl, n, rts, single, ds);
}

// Copies subresource 0 of r, now in state, into a new readback buffer; false when not dumpable.
static bool Dump(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, const D3D12_RESOURCE_DESC& d, UINT sub,
                 D3D12_RESOURCE_STATES state, Batch& b) {  // under g_mutex
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1 ||
        (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || d.Width != g_gbufW || d.Height != g_gbufH ||
        (sub != 0 && sub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) || g_dumps >= kDumpMax)
        return false;
    ID3D12Device* dev = nullptr;
    if (FAILED(cl->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    PassDump p = {nullptr, r};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &p.fp, &p.rows, &p.rowBytes, &total);
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    HRESULT hr = total && g_dumpBytes + total <= kDumpBytes
                     ? dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&p.rb))
                     : E_OUTOFMEMORY;
    dev->Release();
    if (FAILED(hr)) return false;
    D3D12_RESOURCE_BARRIER t = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    t.Transition = {r, sub, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    o_ResourceBarrier(cl, 1, &t);
    D3D12_TEXTURE_COPY_LOCATION dst = {p.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {r};
    dst.PlacedFootprint = p.fp;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    o_ResourceBarrier(cl, 1, &t);
    p.id = g_dumps++;
    g_dumpBytes += total;
    b.dumps.push_back(p);
    return true;
}

// Records the game's transitions after issuing them; a G-buffer-sized texture entering a
// shader-resource state is copied right there.
static void STDMETHODCALLTYPE H_ResourceBarrier(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* bs) {
    o_ResourceBarrier(cl, n, bs);
    if (!g_passRecording.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_lists.find(cl);
    if (it == g_lists.end() || !it->second.passRecording || it->second.pass->window != g_resWindow.load()) return;
    Batch& b = *it->second.pass;
    constexpr UINT read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr UINT write = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                           D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST;
    for (UINT i = 0; i < n; ++i) {
        if (bs[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !bs[i].Transition.pResource) continue;
        const D3D12_RESOURCE_TRANSITION_BARRIER& x = bs[i].Transition;
        D3D12_RESOURCE_DESC d = x.pResource->GetDesc();
        PassBarrier rec = {(UINT)b.spans.size(), x.pResource, (UINT)d.Width, d.Height, (UINT)d.Format, x.Subresource,
                           (UINT)x.StateBefore, (UINT)x.StateAfter, -1};
        if (bs[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE && (x.StateAfter & read) && !(x.StateAfter & write) &&
            Dump(cl, x.pResource, d, x.Subresource, x.StateAfter, b))
            rec.dump = (int)b.dumps.size() - 1;
        b.barriers.push_back(rec);
    }
}

static void RecordView(SIZE_T handle, ID3D12Resource* r, DXGI_FORMAT format, UINT mip) {
    PassView v = {0, 0, (UINT)format, 1, r};
    if (r) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        v.w = (std::max)(1u, (UINT)(d.Width >> mip)), v.h = (std::max)(1u, d.Height >> mip);
        if (format == DXGI_FORMAT_UNKNOWN) v.fmt = d.Format;
    }
    std::lock_guard<std::mutex> lock(g_viewMutex);
    g_views[handle] = v;
}

static void STDMETHODCALLTYPE H_CreateRenderTargetView(ID3D12Device* dev, ID3D12Resource* r,
                                                       const D3D12_RENDER_TARGET_VIEW_DESC* d,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRenderTargetView(dev, r, d, h);
    UINT mip = !d ? 0
               : d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D      ? d->Texture2D.MipSlice
               : d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY ? d->Texture2DArray.MipSlice
                                                                        : 0;
    RecordView(h.ptr, r, d ? d->Format : DXGI_FORMAT_UNKNOWN, mip);
}

static void STDMETHODCALLTYPE H_CreateDepthStencilView(ID3D12Device* dev, ID3D12Resource* r,
                                                       const D3D12_DEPTH_STENCIL_VIEW_DESC* d,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateDepthStencilView(dev, r, d, h);
    UINT mip = !d ? 0
               : d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D      ? d->Texture2D.MipSlice
               : d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY ? d->Texture2DArray.MipSlice
                                                                        : 0;
    RecordView(h.ptr, r, d ? d->Format : DXGI_FORMAT_UNKNOWN, mip);
}

static void STDMETHODCALLTYPE H_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    // After a quarantine no batch changes state again, so nothing needs tracking.
    if (g_live.load(std::memory_order_relaxed) == 0 || g_unsafe) return o_ExecuteCommandLists(q, n, lists);
    ProfQueue* pq;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_queues.find(q);
        pq = it != g_queues.end() ? it->second : RegisterQueue(q);
    }
    std::lock_guard<std::mutex> submit(pq->submit);
    UINT64 value = 0;
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReapLocked();
        UINT64 call = ++g_callSerial;
        std::vector<Batch*> submitted;
        for (UINT i = 0; i < n; ++i) {
            auto it = g_lists.find(lists[i]);
            if (it == g_lists.end()) continue;
            for (Batch* b : {it->second.open, it->second.pass}) {
                if (!b) continue;
                if (b->lastCall == call) {  // the same list twice in one call
                    b->invalid = true;
                    ++s_replays;
                    continue;
                }
                b->lastCall = call;
                if (++b->submissions > 1) b->invalid = true, ++s_replays;
                submitted.push_back(b);
            }
        }
        any = !submitted.empty();
        if (any && !pq->fence) {
            ID3D12Device* dev = nullptr;
            HRESULT hr = q->GetDevice(IID_PPV_ARGS(&dev));
            if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pq->fence));
            if (dev) dev->Release();
            Log("MESHPROF fence queue=%p queueId=%u hr=0x%08lx", q, pq->id, hr);
        }
        if (any && pq->fence) value = ++pq->next;
        for (Batch* b : submitted) {
            if (!pq->fence || pq->type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                b->untracked = true;  // held for good: never read or recycled
                ++s_untracked;
                continue;
            }
            bool found = false;
            for (Wait& w : b->waits)
                if (w.q == pq) w.value = value, found = true;
            if (!found) b->waits.push_back({pq, value});
        }
    }
    o_ExecuteCommandLists(q, n, lists);
    if (any && pq->fence && FAILED(q->Signal(pq->fence, value))) ++s_signalFailed;
    std::lock_guard<std::mutex> lock(g_mutex);
    DrainLocked();
}

static bool IsStatsQuery(D3D12_QUERY_TYPE t) {
    return t == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || t == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
}

static void STDMETHODCALLTYPE H_BeginQuery(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t,
                                           UINT i) {
    if (IsStatsQuery(t) && EnsureMarked(cl)) {  // no marker: sampling has stopped, nothing to track
        std::lock_guard<std::mutex> lock(g_mutex);
        ReapLocked();
        ++List(cl).gameStats;
    }
    o_BeginQuery(cl, h, t, i);
}

static void STDMETHODCALLTYPE H_EndQuery(ID3D12GraphicsCommandList* cl, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t,
                                         UINT i) {
    if (IsStatsQuery(t) && EnsureMarked(cl)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReapLocked();
        --List(cl).gameStats;
    }
    o_EndQuery(cl, h, t, i);
}

// The game's predicate is forwarded unchanged; only whether one is set is tracked.
static void STDMETHODCALLTYPE H_SetPredication(ID3D12GraphicsCommandList* cl, ID3D12Resource* buf, UINT64 off,
                                               D3D12_PREDICATION_OP op) {
    if (buf && EnsureMarked(cl)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReapLocked();
        List(cl).predicated = true;
    } else if (!buf && g_listCount.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReapLocked();
        auto it = g_lists.find(cl);
        if (it != g_lists.end()) it->second.predicated = false;
    }
    o_SetPredication(cl, buf, off, op);
}

// Before the game sees a new queue: hook and verify its ExecuteCommandLists outside g_mutex (the
// patch takes its own lock and may log), then register it fresh, as its address may be reused.
static void OnQueueCreated(void* obj) {
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(((IUnknown*)obj)->QueryInterface(IID_PPV_ARGS(&q))))
        return Quarantine("a created queue has no ID3D12CommandQueue interface");
    g_patch(q, vt_queue_ExecuteCommandLists, (void*)H_ExecuteCommandLists, (void**)&o_ExecuteCommandLists);
    if (!Hooked(q, vt_queue_ExecuteCommandLists, (void*)H_ExecuteCommandLists))
        Quarantine("ExecuteCommandLists of a created queue could not be hooked");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RegisterQueue(q);
    }
    q->Release();
}

// Logs every queue, and warns about a second direct queue: the emulation scratch is per device,
// so emulated lists executing concurrently on two direct queues would share it.
static void QueueCreated(const char* api, void* dev, const D3D12_COMMAND_QUEUE_DESC* d, void* out) {
    if (Coverage()) OnQueueCreated(out);
    UINT direct = 0;
    if (d->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard<std::mutex> lock(g_mutex);
        direct = ++g_directQueues[dev];
    }
    Log("%s device=%p type=%d priority=%d -> %p", api, dev, d->Type, d->Priority, out);
    if (direct >= 2)
        Log("WARN: device %p created direct queue #%u; the mesh-emulation scratch is per device, so emulated lists "
            "executing concurrently on different direct queues would race on it", dev, direct);
}

static HRESULT STDMETHODCALLTYPE H_CreateCommandQueue(ID3D12Device* dev, const D3D12_COMMAND_QUEUE_DESC* d, REFIID riid,
                                                      void** out) {
    HRESULT hr = o_CreateCommandQueue(dev, d, riid, out);
    if (SUCCEEDED(hr) && out && *out && d) QueueCreated("CreateCommandQueue", dev, d, *out);
    return hr;
}

// Patched only with profiling on; ID3D12Device9 shares the device's vtable.
static HRESULT STDMETHODCALLTYPE H_CreateCommandQueue1(ID3D12Device9* dev, const D3D12_COMMAND_QUEUE_DESC* d,
                                                       REFIID creator, REFIID riid, void** out) {
    HRESULT hr = o_CreateCommandQueue1(dev, d, creator, riid, out);
    if (SUCCEEDED(hr) && out && *out && d) QueueCreated("CreateCommandQueue1", dev, d, *out);
    return hr;
}

void MeshProfFlush() {
    if (!Coverage()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ReapLocked();
    DrainLocked();
    if (g_cfg.passPerformanceDiagnostics) {
        static long lastPass = -1;
        long passSum = s_passLists + s_passSpans + s_passBad + s_passInvalid + s_passTruncated + s_passNoDevice +
                       s_replays + s_untracked + (long)g_unsafe.load() + s_unmarked;
        if (passSum != lastPass) {
            lastPass = passSum;
            Log("PASSPROF stats intervalS=%d windows=%u lists=%ld spans=%ld badSpans=%ld invalidatedSpans=%ld "
                "truncatedLists=%ld noResources=%ld replayedSubmissions=%ld untrackedSubmissions=%ld coverageLost=%d "
                "unmarkedLists=%ld recording=%d liveBatches=%d",
                g_cfg.passPerformanceDiagnostics, g_passWindow.load(), s_passLists.load(), s_passSpans.load(),
                s_passBad.load(), s_passInvalid.load(), s_passTruncated.load(), s_passNoDevice.load(), s_replays.load(),
                s_untracked.load(), (int)g_unsafe.load(), s_unmarked.load(), g_passRecording.load(), g_live.load());
        }
    }
    if (!g_cfg.meshPerformanceDiagnostics) return;
    UINT held = 0;
    for (auto& d : g_devices)
        if (d.second) held += kSlots - (UINT)d.second->free.size();
    static long last = -1;
    long sum = s_samples + s_completed + s_dropped + s_invalid + s_unsubmitted + s_unsupported + s_badTimestamps +
               s_untracked + s_predicated + s_quarantined + s_destroyed + s_unmarked + (long)held +
               (long)g_lists.size();
    if (sum == last) return;
    last = sum;
    Log("MESHPROF stats mode=%d samples=%ld completed=%ld profileSamplesDropped=%ld invalidated=%ld "
        "replayedSubmissions=%ld unsubmittedDiscarded=%ld unsupportedList=%ld statsSkippedGameQuery=%ld "
        "badTimestamps=%ld untrackedSubmissions=%ld signalFailed=%ld predicatedSkipped=%ld coverageLost=%d "
        "quarantinedSlots=%ld listsDestroyed=%ld unmarkedLists=%ld lists=%zu slotsHeld=%u liveBatches=%d queues=%zu",
        g_cfg.meshPerformanceDiagnostics, s_samples.load(), s_completed.load(), s_dropped.load(), s_invalid.load(),
        s_replays.load(), s_unsubmitted.load(), s_unsupported.load(), s_statsSkipped.load(), s_badTimestamps.load(),
        s_untracked.load(), s_signalFailed.load(), s_predicated.load(), (int)g_unsafe.load(), s_quarantined.load(),
        s_destroyed.load(), s_unmarked.load(), g_lists.size(), held, g_live.load(), g_queues.size());
}

void MeshProfInstall(ID3D12Device* dev, PatchFn patch) {
    g_patch = patch;
    if (Coverage()) {
        // Patch ExecuteCommandLists through a throwaway queue before the game can create one.
        D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
            patch(q, vt_queue_ExecuteCommandLists, (void*)H_ExecuteCommandLists, (void**)&o_ExecuteCommandLists);
            if (!Hooked(q, vt_queue_ExecuteCommandLists, (void*)H_ExecuteCommandLists))
                Quarantine("ExecuteCommandLists of this device's queues could not be hooked");
            q->Release();
        }
        if (!o_ExecuteCommandLists) Log("WARN: MESHPROF could not hook ExecuteCommandLists; profiling stays off");
    }
    patch(dev, vt_dev_CreateCommandQueue, (void*)H_CreateCommandQueue, (void**)&o_CreateCommandQueue);
    if (!Coverage()) return;
    if (g_cfg.passPerformanceDiagnostics) {
        patch(dev, vt_dev_CreateRenderTargetView, (void*)H_CreateRenderTargetView, (void**)&o_CreateRenderTargetView);
        patch(dev, vt_dev_CreateDepthStencilView, (void*)H_CreateDepthStencilView, (void**)&o_CreateDepthStencilView);
        g_passNext = GetTickCount64() + 1000ull * g_cfg.passPerformanceDiagnostics;
    }
    // A queue created through an unhooked API would be invisible: lose coverage rather than guess.
    if (!Hooked(dev, vt_dev_CreateCommandQueue, (void*)H_CreateCommandQueue))
        Quarantine("CreateCommandQueue could not be hooked");
    ID3D12Device9* dev9 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev9)))) {  // older runtimes have no CreateCommandQueue1
        patch(dev9, vt_dev9_CreateCommandQueue1, (void*)H_CreateCommandQueue1, (void**)&o_CreateCommandQueue1);
        if (!Hooked(dev9, vt_dev9_CreateCommandQueue1, (void*)H_CreateCommandQueue1))
            Quarantine("CreateCommandQueue1 could not be hooked");
        dev9->Release();
    }
    Log("MESHPROF coverage device=%p createCommandQueue1=%s coverageLost=%d", dev, dev9 ? "hooked" : "unsupported",
        (int)g_unsafe.load());
}

void MeshProfHookList(void* cl, PatchFn patch) {
    if (g_cfg.meshPerformanceDiagnostics) {
        patch(cl, vt_cl_BeginQuery, (void*)H_BeginQuery, (void**)&o_BeginQuery);
        patch(cl, vt_cl_EndQuery, (void*)H_EndQuery, (void**)&o_EndQuery);
        patch(cl, vt_cl_SetPredication, (void*)H_SetPredication, (void**)&o_SetPredication);
    }
    if (g_cfg.passPerformanceDiagnostics) {  // only for the direct-list class, the first one hooked
        patch(cl, vt_cl_Close, (void*)H_Close, (void**)&o_Close);
        patch(cl, vt_cl_OMSetRenderTargets, (void*)H_OMSetRenderTargets, (void**)&o_OMSetRenderTargets);
        if (g_cfg.passResourceDiagnostics)
            patch(cl, vt_cl_ResourceBarrier, (void*)H_ResourceBarrier, (void**)&o_ResourceBarrier);
    }
}
