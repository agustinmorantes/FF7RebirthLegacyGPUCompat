#pragma once
// Sampled, fence-verified GPU profiling of emulated mesh calls (MeshPerformanceDiagnostics).
#include <d3d12.h>
#include <atomic>
#include <vector>

struct MeshProfPso;  // per emulated PSO sampling state, kept for the process lifetime
struct MeshProfSample {
    void* dev;
    UINT slot;
    bool stats;  // pipeline-statistics query recorded
    std::vector<UINT> slots;  // reserve every pass together, or do not sample the call
};
using PatchFn = void (*)(void* obj, int idx, void* hook, void** orig);

// Null when profiling is disabled; InjectMesh then records nothing extra.
MeshProfPso* MeshProfNewPso(const char* msHash);
// Recording points inside InjectMesh. Begin decides whether this call is sampled and, if so,
// writes the start timestamp; the others are only called for a sampled call.
bool MeshProfBegin(ID3D12GraphicsCommandList* cl, MeshProfPso* pso, bool indirect, UINT x, UINT y, UINT z,
                   UINT passes, MeshProfSample* out);
void MeshProfStartPass(ID3D12GraphicsCommandList* cl, MeshProfSample& s, UINT pass);
void MeshProfDispatch(ID3D12GraphicsCommandList* cl, const MeshProfSample& s, bool begin);
void MeshProfPreDraw(ID3D12GraphicsCommandList* cl, const MeshProfSample& s);
void MeshProfEnd(ID3D12GraphicsCommandList* cl, const MeshProfSample& s, ID3D12Resource* scratch);
// A list starts a new recording (Reset, or a new list possibly at a released list's address).
void MeshProfListReset(void* cl);
void MeshProfClearState(void* cl);  // after the real ClearState, which disables predication
void MeshProfFlush();  // reads completed samples and logs counters; called from the 5 s stats flush
void MeshProfInstall(ID3D12Device* dev, PatchFn patch);  // device-level hooks, once per vtable
void MeshProfHookList(void* cl, PatchFn patch);           // query and pass hooks, once per list class

// Pass profiler (PassPerformanceDiagnostics): direct lists that start during a short window every
// N seconds get a timestamp at each render-target or compute/graphics change, read back under the
// same fence and generation rules as the mesh samples. The draw and dispatch hooks report work here.
enum PassWork { kPassDraw, kPassDispatch, kPassIndirect, kPassEmulated };
extern std::atomic<int> g_passRecording;  // sampled lists currently recording
void PassProfWorkSlow(void* cl, PassWork w);
inline void PassProfWork(void* cl, PassWork w) {
    if (g_passRecording.load(std::memory_order_relaxed)) PassProfWorkSlow(cl, w);
}
// Compute spans also split at each pipeline change and carry the game's CS hash, tagged on the PSO
// at creation (only while the pass profiler is enabled).
void PassProfPsoSlow(void* cl, void* pso);
inline void PassProfPso(void* cl, void* pso) {
    if (g_passRecording.load(std::memory_order_relaxed)) PassProfPsoSlow(cl, pso);
}
void PassProfTagPso(void* pso, const D3D12_SHADER_BYTECODE& cs);
