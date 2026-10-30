// Computes COM vtable slot indices from the C declarations in d3d12.h, so nothing is hardcoded.
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d12.h>

#define DEV(m)  (int)(offsetof(ID3D12Device2Vtbl, m) / sizeof(void*))
#define CL(m)   (int)(offsetof(ID3D12GraphicsCommandList6Vtbl, m) / sizeof(void*))

const int vt_dev_CheckFeatureSupport = DEV(CheckFeatureSupport);
const int vt_dev_CreateGraphicsPipelineState = DEV(CreateGraphicsPipelineState);
const int vt_dev_CreateComputePipelineState = DEV(CreateComputePipelineState);
const int vt_dev_CreateCommandSignature = DEV(CreateCommandSignature);
const int vt_dev_CreateRootSignature = DEV(CreateRootSignature);
const int vt_dev_CreatePipelineLibrary = DEV(CreatePipelineLibrary);
const int vt_lib_LoadGraphicsPipeline = (int)(offsetof(ID3D12PipelineLibrary1Vtbl, LoadGraphicsPipeline) / sizeof(void*));
const int vt_lib_LoadComputePipeline = (int)(offsetof(ID3D12PipelineLibrary1Vtbl, LoadComputePipeline) / sizeof(void*));
const int vt_lib1_LoadPipeline = (int)(offsetof(ID3D12PipelineLibrary1Vtbl, LoadPipeline) / sizeof(void*));
const int vt_dev2_CreatePipelineState = DEV(CreatePipelineState);
const int vt_cl_Reset = CL(Reset);
const int vt_cl_ClearState = CL(ClearState);
const int vt_cl_DrawInstanced = CL(DrawInstanced);
const int vt_cl_DrawIndexedInstanced = CL(DrawIndexedInstanced);
const int vt_cl_Dispatch = CL(Dispatch);
const int vt_cl_SetPipelineState = CL(SetPipelineState);
const int vt_cl_ExecuteIndirect = CL(ExecuteIndirect);
const int vt_cl_RSSetShadingRate = CL(RSSetShadingRate);
const int vt_cl_RSSetShadingRateImage = CL(RSSetShadingRateImage);
const int vt_cl_DispatchMesh = CL(DispatchMesh);
const int vt_dev_CreateCommandList = DEV(CreateCommandList);
const int vt_cl_IASetPrimitiveTopology = CL(IASetPrimitiveTopology);
const int vt_cl_IASetIndexBuffer = CL(IASetIndexBuffer);
const int vt_cl_SetDescriptorHeaps = CL(SetDescriptorHeaps);
const int vt_cl_ExecuteBundle = CL(ExecuteBundle);
const int vt_cl_DispatchRays = CL(DispatchRays);
const int vt_cl_SetProtectedResourceSession = CL(SetProtectedResourceSession);
const int vt_cl_InitializeMetaCommand = CL(InitializeMetaCommand);
const int vt_cl_ExecuteMetaCommand = CL(ExecuteMetaCommand);
const int vt_alloc_Reset = (int)(offsetof(ID3D12CommandAllocatorVtbl, Reset) / sizeof(void*));
const int vt_alloc_Release = (int)(offsetof(ID3D12CommandAllocatorVtbl, Release) / sizeof(void*));
const int vt_dev_CreateCommandQueue = DEV(CreateCommandQueue);
const int vt_queue_ExecuteCommandLists = (int)(offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists) / sizeof(void*));
const int vt_cl_BeginQuery = CL(BeginQuery);
const int vt_cl_EndQuery = CL(EndQuery);
const int vt_cl_SetPredication = CL(SetPredication);
const int vt_dev_CreateRenderTargetView = DEV(CreateRenderTargetView);
const int vt_dev_CreateDepthStencilView = DEV(CreateDepthStencilView);
const int vt_dev9_CreateCommandQueue1 =(int)(offsetof(ID3D12Device9Vtbl, CreateCommandQueue1) / sizeof(void*));
const int vt_cl_SetComputeRootSignature = CL(SetComputeRootSignature);
const int vt_cl_SetGraphicsRootSignature = CL(SetGraphicsRootSignature);
const int vt_cl_SetComputeRootDescriptorTable = CL(SetComputeRootDescriptorTable);
const int vt_cl_SetGraphicsRootDescriptorTable = CL(SetGraphicsRootDescriptorTable);
const int vt_cl_SetComputeRoot32BitConstant = CL(SetComputeRoot32BitConstant);
const int vt_cl_SetGraphicsRoot32BitConstant = CL(SetGraphicsRoot32BitConstant);
const int vt_cl_SetComputeRoot32BitConstants = CL(SetComputeRoot32BitConstants);
const int vt_cl_SetGraphicsRoot32BitConstants = CL(SetGraphicsRoot32BitConstants);
const int vt_cl_SetComputeRootConstantBufferView = CL(SetComputeRootConstantBufferView);
const int vt_cl_SetGraphicsRootConstantBufferView = CL(SetGraphicsRootConstantBufferView);
const int vt_cl_SetComputeRootShaderResourceView = CL(SetComputeRootShaderResourceView);
const int vt_cl_SetGraphicsRootShaderResourceView = CL(SetGraphicsRootShaderResourceView);
const int vt_cl_SetComputeRootUnorderedAccessView = CL(SetComputeRootUnorderedAccessView);
const int vt_cl_SetGraphicsRootUnorderedAccessView = CL(SetGraphicsRootUnorderedAccessView);
const int vt_cl_OMSetRenderTargets = CL(OMSetRenderTargets);
const int vt_cl_ResourceBarrier = CL(ResourceBarrier);
const int vt_cl7_Barrier = (int)(offsetof(ID3D12GraphicsCommandList7Vtbl, Barrier) / sizeof(void*));
const int vt_cl_Close = CL(Close);
