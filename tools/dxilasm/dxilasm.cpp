// Assembles DXIL LLVM IR text (as printed by `dxc -dumpbin`) into a validated, signed DXIL
// container, using dxcompiler.dll and dxil.dll from the DXC release next to this tool.
// usage: dxilasm in.ll out.cso
#include <windows.h>
#include <cstdio>
#include <vector>
#include "dxcapi.h"

static IDxcBlob* Fail(const char* what, HRESULT hr, IDxcOperationResult* r) {
    fprintf(stderr, "%s failed hr=0x%08lx\n", what, hr);
    IDxcBlobEncoding* err = nullptr;
    if (r && SUCCEEDED(r->GetErrorBuffer(&err)) && err && err->GetBufferSize())
        fprintf(stderr, "%.*s\n", (int)err->GetBufferSize(), (const char*)err->GetBufferPointer());
    return nullptr;
}

static IDxcBlob* Result(const char* what, HRESULT hr, IDxcOperationResult* r) {
    if (FAILED(hr)) return Fail(what, hr, nullptr);
    HRESULT status;
    r->GetStatus(&status);
    if (FAILED(status)) return Fail(what, status, r);
    IDxcBlob* out = nullptr;
    r->GetResult(&out);
    return out;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: dxilasm in.ll out.cso\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<char> text;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.insert(text.end(), buf, buf + n);
    fclose(f);

    auto compilerCreate = (DxcCreateInstanceProc)GetProcAddress(LoadLibraryA("dxcompiler.dll"), "DxcCreateInstance");
    auto dxilCreate = (DxcCreateInstanceProc)GetProcAddress(LoadLibraryA("dxil.dll"), "DxcCreateInstance");
    if (!compilerCreate || !dxilCreate) {
        fprintf(stderr, "dxcompiler.dll / dxil.dll not found\n");
        return 1;
    }
    IDxcUtils* utils = nullptr;
    IDxcAssembler* asmb = nullptr;
    IDxcValidator* val = nullptr;
    compilerCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    compilerCreate(CLSID_DxcAssembler, IID_PPV_ARGS(&asmb));
    dxilCreate(CLSID_DxcValidator, IID_PPV_ARGS(&val));

    IDxcBlobEncoding* src = nullptr;
    utils->CreateBlob(text.data(), (UINT32)text.size(), DXC_CP_UTF8, &src);
    // Each call runs before Result reads its output pointer: argument evaluation order is unspecified.
    IDxcOperationResult* r = nullptr;
    HRESULT hr = asmb->AssembleToContainer(src, &r);
    IDxcBlob* container = Result("assemble", hr, r);
    if (!container) return 1;
    IDxcOperationResult* v = nullptr;
    hr = val->Validate(container, DxcValidatorFlags_InPlaceEdit, &v);
    if (!Result("validate", hr, v) && v) {
        HRESULT status;
        v->GetStatus(&status);
        if (FAILED(status)) return 1;
    }

    f = fopen(argv[2], "wb");
    fwrite(container->GetBufferPointer(), 1, container->GetBufferSize(), f);
    fclose(f);
    printf("%s: %zu bytes\n", argv[2], container->GetBufferSize());
    return 0;
}
