// d3d12.dll proxy: forwards everything to System32\d3d12.dll (which still honours the exe's
// Agility SDK exports), spoofs the 12_2 device request and installs the logging hooks.
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include "common.h"

Config g_cfg;

static wchar_t g_dir[MAX_PATH];
static FILE* g_log;
static std::mutex g_logMutex;

// Placeholder for the real module's exported DRED data block (a data export can't be stubbed).
extern "C" __declspec(align(16)) unsigned char g_dredData[64] = {};

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%slegacygpu.log", g_dir);
        g_log = _wfsopen(path, L"w", _SH_DENYWR);
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);  // keep the log intact if the game crashes
}

const wchar_t* ProxyDir() { return g_dir; }

HMODULE RealD3D12() {
    static HMODULE real = [] {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        wcscpy_s(path + n, MAX_PATH - n, L"\\d3d12.dll");
        HMODULE m = LoadLibraryW(path);
        Log("loaded %ls -> %p", path, m);
        return m;
    }();
    return real;
}

// Order must match the stub indices in exports.asm. Index 8 (DRED data) is not stubbed.
static const char* const kExports[] = {
    MAKEINTRESOURCEA(99),
    "SetAppCompatStringPointer",
    "D3D12GetDebugInterface",
    "D3D12CoreCreateLayeredDevice",
    "D3D12CoreGetLayeredDeviceSize",
    "D3D12CoreRegisterLayers",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12DeviceRemovedExtendedData",
    "D3D12EnableExperimentalFeatures",
    "D3D12GetInterface",
    "D3D12PIXEventsReplaceBlock",
    "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal",
    "D3D12PIXReportCounter",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "GetBehaviorValue",
};

extern "C" void* ResolveExport(int index) {
    static void* cache[ARRAYSIZE(kExports)];
    void* p = cache[index];
    if (!p) {
        p = (void*)GetProcAddress(RealD3D12(), kExports[index]);
        if (!p) {
            Log("FATAL: export %d missing in system d3d12.dll", index);
            ExitProcess(0xD3D12);
        }
        cache[index] = p;
    }
    return p;
}

extern "C" HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minLevel,
                                                  REFIID riid, void** device) {
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    static Fn real = (Fn)GetProcAddress(RealD3D12(), "D3D12CreateDevice");
    D3D_FEATURE_LEVEL level = minLevel;
    if (level > D3D_FEATURE_LEVEL_12_1) level = D3D_FEATURE_LEVEL_12_1;
    HRESULT hr = real(adapter, level, riid, device);
    Log("D3D12CreateDevice(adapter=%p, fl=0x%x -> 0x%x, out=%p) = 0x%08lx", adapter, minLevel, level,
        device, hr);
    if (SUCCEEDED(hr) && device && *device) InstallDeviceHooks((IUnknown*)*device);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        DWORD n = GetModuleFileNameW(inst, g_dir, MAX_PATH);
        while (n && g_dir[n - 1] != L'\\') --n;
        g_dir[n] = 0;
        wchar_t ini[MAX_PATH];
        swprintf_s(ini, L"%slegacygpu.ini", g_dir);
        g_cfg.emulateMesh = GetPrivateProfileIntW(L"legacygpu", L"EmulateMeshShaders", 1, ini);
        g_cfg.meshUsageDiagnostics = GetPrivateProfileIntW(L"legacygpu", L"MeshUsageDiagnostics", 0, ini);
        g_cfg.optimizeMeshRootBindings = GetPrivateProfileIntW(L"legacygpu", L"OptimizeMeshRootBindings", 1, ini);
        g_cfg.memoryDiagnostics = GetPrivateProfileIntW(L"legacygpu", L"MemoryDiagnostics", 0, ini);
        g_cfg.meshPerformanceDiagnostics = GetPrivateProfileIntW(L"legacygpu", L"MeshPerformanceDiagnostics", 0, ini);
        g_cfg.passPerformanceDiagnostics = GetPrivateProfileIntW(L"legacygpu", L"PassPerformanceDiagnostics", 0, ini);
        g_cfg.passResourceDiagnostics = GetPrivateProfileIntW(L"legacygpu", L"PassResourceDiagnostics", 0, ini);
        g_cfg.meshScratchMinMiB = GetPrivateProfileIntW(L"legacygpu", L"MeshScratchMinMiB", 384, ini);
        g_cfg.meshScratchMaxMiB = GetPrivateProfileIntW(L"legacygpu", L"MeshScratchMaxMiB", 512, ini);
        g_cfg.meshScratchStepMiB = GetPrivateProfileIntW(L"legacygpu", L"MeshScratchStepMiB", 128, ini);
        if (!g_cfg.meshScratchMinMiB) g_cfg.meshScratchMinMiB = 1;
        if (g_cfg.meshScratchMaxMiB < g_cfg.meshScratchMinMiB)
            g_cfg.meshScratchMaxMiB = g_cfg.meshScratchMinMiB;
        if (!g_cfg.meshScratchStepMiB) g_cfg.meshScratchStepMiB = 1;
        g_cfg.meshScratchShrinkSeconds = GetPrivateProfileIntW(L"legacygpu", L"MeshScratchShrinkSeconds", 30, ini);
    }
    return TRUE;
}
