#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Vtable slot indices, computed from the SDK headers in vtidx.c.
extern const int vt_dev_CheckFeatureSupport;
extern const int vt_dev_CreateGraphicsPipelineState;
extern const int vt_dev_CreateComputePipelineState;
extern const int vt_dev_CreateCommandSignature;
extern const int vt_dev_CreateRootSignature;
extern const int vt_dev_CreatePipelineLibrary;
extern const int vt_lib_LoadGraphicsPipeline;
extern const int vt_lib_LoadComputePipeline;
extern const int vt_lib1_LoadPipeline;
extern const int vt_dev2_CreatePipelineState;
extern const int vt_cl_Reset;
extern const int vt_cl_ClearState;
extern const int vt_cl_DrawInstanced;
extern const int vt_cl_DrawIndexedInstanced;
extern const int vt_cl_Dispatch;
extern const int vt_cl_SetPipelineState;
extern const int vt_cl_ExecuteIndirect;
extern const int vt_cl_RSSetShadingRate;
extern const int vt_cl_RSSetShadingRateImage;
extern const int vt_cl_DispatchMesh;
extern const int vt_dev_CreateCommandList;
extern const int vt_cl_IASetPrimitiveTopology;
extern const int vt_cl_IASetIndexBuffer;
extern const int vt_cl_SetDescriptorHeaps, vt_cl_ExecuteBundle, vt_cl_DispatchRays, vt_cl_SetProtectedResourceSession;
extern const int vt_cl_InitializeMetaCommand, vt_cl_ExecuteMetaCommand;
extern const int vt_alloc_Reset, vt_alloc_Release;
// Queue and query hooks for the sampled mesh profiler (meshprof.cpp).
extern const int vt_dev_CreateCommandQueue, vt_queue_ExecuteCommandLists, vt_cl_BeginQuery, vt_cl_EndQuery;
extern const int vt_cl_SetPredication, vt_dev9_CreateCommandQueue1;
// View creation, for the pass profiler's render-target labels.
extern const int vt_dev_CreateRenderTargetView, vt_dev_CreateDepthStencilView;
// Root-argument setters, tracked for the mesh emulation.
extern const int vt_cl_SetComputeRootSignature, vt_cl_SetGraphicsRootSignature;
extern const int vt_cl_SetComputeRootDescriptorTable, vt_cl_SetGraphicsRootDescriptorTable;
extern const int vt_cl_SetComputeRoot32BitConstant, vt_cl_SetGraphicsRoot32BitConstant;
extern const int vt_cl_SetComputeRoot32BitConstants, vt_cl_SetGraphicsRoot32BitConstants;
extern const int vt_cl_SetComputeRootConstantBufferView, vt_cl_SetGraphicsRootConstantBufferView;
extern const int vt_cl_SetComputeRootShaderResourceView, vt_cl_SetGraphicsRootShaderResourceView;
extern const int vt_cl_SetComputeRootUnorderedAccessView, vt_cl_SetGraphicsRootUnorderedAccessView;
// Pass profiler.
extern const int vt_cl_OMSetRenderTargets, vt_cl_ResourceBarrier;
// Scratch-state tracking at Close; List10's work-graph methods follow List7's Barrier.
extern const int vt_cl7_Barrier, vt_cl_Close;

#ifdef __cplusplus
}

struct Config {
    int emulateMesh;         // run converted mesh shaders as compute + indexed draw
    int meshUsageDiagnostics;  // log scratch usage and emulation PSO details
    int optimizeMeshRootBindings;  // retain emulation compute roots until the game needs its compute state
    int memoryDiagnostics;     // process GPU-memory budgets and retained-object counts
    int meshPerformanceDiagnostics;  // fence-verified GPU samples of emulated calls (1 sampled, 2 every call)
    int passPerformanceDiagnostics;  // seconds between sampled windows of per-pass GPU timestamps (0 off)
    int passResourceDiagnostics;     // 1: F12 opens a pass window that also logs targets and barriers and dumps textures
    UINT meshScratchMinMiB;          // smallest mesh-emulation scratch
    UINT meshScratchMaxMiB;          // geometry capacity cap; retry lists are additional
    UINT meshScratchStepMiB;         // scratch sizes are multiples of this; shrinking goes one step at a time
    UINT meshScratchShrinkSeconds;   // usage window after which an oversized scratch shrinks one step
};
extern Config g_cfg;

void Log(const char* fmt, ...);
HMODULE RealD3D12();
const wchar_t* ProxyDir();  // directory of the proxy DLL, with a trailing backslash
void InstallDeviceHooks(IUnknown* device);
bool RefreshShaders();  // regenerates legacygpu_shaders after a game update; false if that failed
#endif
