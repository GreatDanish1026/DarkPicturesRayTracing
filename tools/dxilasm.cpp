// dxilasm: assembles DXIL IR text (as written by `dxc -dumpbin`) into a signed DXIL
// container, with the Windows SDK's DXC: dxcompiler.dll (IDxcAssembler) and dxil.dll
// (IDxcValidator, which signs the container when it validates).
//
//   dxilasm <sdk bin\x64 folder> <in.ll> <out.dxil>
//
// Exit code 0 on success; assembler or validator messages go to stderr.

#include <windows.h>  // not lean: dxcapi.h needs the COM declarations (IUnknown, IStream)

#include <dxcapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

template <typename T>
struct Ptr {  // minimal COM pointer
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool ReadFile(const wchar_t* path, std::vector<char>& out) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

DxcCreateInstanceProc Load(const std::wstring& dir, const wchar_t* dll) {
    HMODULE m = LoadLibraryExW((dir + L"\\" + dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        fwprintf(stderr, L"cannot load %s from %s\n", dll, dir.c_str());
        return nullptr;
    }
    return reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance"));
}

// Prints an operation's error buffer, if any.
void PrintErrors(IDxcOperationResult* result, const char* what) {
    Ptr<IDxcBlobEncoding> errors;
    if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors && errors->GetBufferSize())
        fprintf(stderr, "%s:\n%.*s\n", what, static_cast<int>(errors->GetBufferSize()),
                static_cast<const char*>(errors->GetBufferPointer()));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        fwprintf(stderr, L"usage: dxilasm <sdk bin\\x64 folder> <in.ll> <out.dxil>\n");
        return 2;
    }
    DxcCreateInstanceProc compiler = Load(argv[1], L"dxcompiler.dll");
    DxcCreateInstanceProc validatorDll = Load(argv[1], L"dxil.dll");
    if (!compiler || !validatorDll) return 1;

    std::vector<char> text;
    if (!ReadFile(argv[2], text)) {
        fwprintf(stderr, L"cannot read %s\n", argv[2]);
        return 1;
    }

    Ptr<IDxcLibrary> library;
    Ptr<IDxcAssembler> assembler;
    Ptr<IDxcValidator> validator;
    if (FAILED(compiler(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
        FAILED(compiler(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) ||
        FAILED(validatorDll(CLSID_DxcValidator, IID_PPV_ARGS(&validator)))) {
        fprintf(stderr, "cannot create the DXC library, assembler or validator\n");
        return 1;
    }

    Ptr<IDxcBlobEncoding> source;
    if (FAILED(library->CreateBlobWithEncodingFromPinned(text.data(), static_cast<UINT32>(text.size()), CP_UTF8,
                                                          &source))) {
        fprintf(stderr, "cannot create the source blob\n");
        return 1;
    }

    Ptr<IDxcOperationResult> assembled;
    HRESULT status = E_FAIL;
    if (FAILED(assembler->AssembleToContainer(source.p, &assembled)) || FAILED(assembled->GetStatus(&status)) ||
        FAILED(status)) {
        if (assembled) PrintErrors(assembled.p, "assembler");
        fprintf(stderr, "assembly failed (0x%08lx)\n", status);
        return 1;
    }
    Ptr<IDxcBlob> container;
    assembled->GetResult(&container);

    // Validation signs the container in place (the HASH part D3D12 checks).
    Ptr<IDxcOperationResult> validated;
    status = E_FAIL;
    if (FAILED(validator->Validate(container.p, DxcValidatorFlags_InPlaceEdit, &validated)) ||
        FAILED(validated->GetStatus(&status)) || FAILED(status)) {
        if (validated) PrintErrors(validated.p, "validator");
        fprintf(stderr, "validation failed (0x%08lx)\n", status);
        return 1;
    }

    FILE* f = _wfopen(argv[3], L"wb");
    if (!f || fwrite(container->GetBufferPointer(), 1, container->GetBufferSize(), f) != container->GetBufferSize()) {
        fwprintf(stderr, L"cannot write %s\n", argv[3]);
        if (f) fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}
