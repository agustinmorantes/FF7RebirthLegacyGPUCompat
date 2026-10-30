; Pass-through export stubs. Each stub resolves its target in System32\d3d12.dll on demand,
; preserving the integer and float argument registers, then tail-jumps to it.

EXTERN ResolveExport:PROC   ; void* ResolveExport(int index)

STUB MACRO name, idx
name PROC
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 68h            ; 32 shadow + 4 xmm saves (64) + 8 align = 68h, keeps rsp 16-aligned
    movdqu [rsp+20h], xmm0
    movdqu [rsp+30h], xmm1
    movdqu [rsp+40h], xmm2
    movdqu [rsp+50h], xmm3
    mov ecx, idx
    call ResolveExport
    movdqu xmm0, [rsp+20h]
    movdqu xmm1, [rsp+30h]
    movdqu xmm2, [rsp+40h]
    movdqu xmm3, [rsp+50h]
    add rsp, 68h
    pop r9
    pop r8
    pop rdx
    pop rcx
    jmp rax
name ENDP
ENDM

.code
; Indices must match kExports[] in proxy.cpp.
STUB Stub_Ordinal99, 0
STUB Stub_SetAppCompatStringPointer, 1
STUB Stub_D3D12GetDebugInterface, 2
STUB Stub_D3D12CoreCreateLayeredDevice, 3
STUB Stub_D3D12CoreGetLayeredDeviceSize, 4
STUB Stub_D3D12CoreRegisterLayers, 5
STUB Stub_D3D12CreateRootSignatureDeserializer, 6
STUB Stub_D3D12CreateVersionedRootSignatureDeserializer, 7
STUB Stub_D3D12EnableExperimentalFeatures, 9
STUB Stub_D3D12GetInterface, 10
STUB Stub_D3D12PIXEventsReplaceBlock, 11
STUB Stub_D3D12PIXGetThreadInfo, 12
STUB Stub_D3D12PIXNotifyWakeFromFenceSignal, 13
STUB Stub_D3D12PIXReportCounter, 14
STUB Stub_D3D12SerializeRootSignature, 15
STUB Stub_D3D12SerializeVersionedRootSignature, 16
STUB Stub_GetBehaviorValue, 17
END
