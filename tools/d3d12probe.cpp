// d3d12probe: checks the ID3D12Device5::CreateStateObject vtable slot the add-on hooks,
// on this machine's D3D12Core.dll.
//
//   d3d12probe [--hook-test]
//
// Prints the module and the slot's function RVA. --hook-test patches the slot the way
// the add-on does, calls CreateStateObject through the C++ interface with an empty
// collection description, and passes if the call went through the hook and returned
// the same result as without it.
//
// The slot comes from MSVC's C++ vtable (offset 0x1f0 = slot 62 for CreateStateObject),
// not from the C ID3D12Device5Vtbl struct in d3d12.h, whose order differs: counting
// that struct gave 66, which is ID3D12Device7::AddToStateObject in the C++ layout.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "version.lib")

constexpr size_t kCreateStateObjectSlot = 62;
using CreateStateObjectFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID,
                                                         void**);
CreateStateObjectFn g_original = nullptr;
int g_hookCalls = 0;

HRESULT STDMETHODCALLTYPE Hook(ID3D12Device5* self, const D3D12_STATE_OBJECT_DESC* desc, REFIID riid, void** out) {
    ++g_hookCalls;
    return g_original(self, desc, riid, out);
}

void PatchSlot(void** slot, void* value) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

int HookTest(ID3D12Device5* device5) {
    D3D12_STATE_OBJECT_DESC empty = {D3D12_STATE_OBJECT_TYPE_COLLECTION, 0, nullptr};
    ID3D12StateObject* so = nullptr;
    HRESULT before = device5->CreateStateObject(&empty, IID_PPV_ARGS(&so));
    if (so) so->Release(), so = nullptr;

    void** slot = *reinterpret_cast<void***>(device5) + kCreateStateObjectSlot;
    g_original = reinterpret_cast<CreateStateObjectFn>(*slot);
    PatchSlot(slot, reinterpret_cast<void*>(&Hook));
    HRESULT during = device5->CreateStateObject(&empty, IID_PPV_ARGS(&so));
    if (so) so->Release(), so = nullptr;
    PatchSlot(slot, reinterpret_cast<void*>(g_original));
    HRESULT after = device5->CreateStateObject(&empty, IID_PPV_ARGS(&so));
    if (so) so->Release();

    bool ok = g_hookCalls == 1 && during == before && after == before;
    printf("hook test: %s (hook calls %d; result 0x%08lx before, 0x%08lx hooked, 0x%08lx restored)\n",
           ok ? "PASS" : "FAIL", g_hookCalls, static_cast<unsigned long>(before), static_cast<unsigned long>(during),
           static_cast<unsigned long>(after));
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        printf("D3D12CreateDevice failed\n");
        return 1;
    }
    ID3D12Device5* device5 = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) {
        printf("no ID3D12Device5\n");
        return 1;
    }
    void** vtable = *reinterpret_cast<void***>(device5);
    const uint8_t* fn = static_cast<const uint8_t*>(vtable[kCreateStateObjectSlot]);

    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(fn), &module);
    GetModuleFileNameW(module, path, MAX_PATH);
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path, &handle);
    char version[64] = "?";
    if (size) {
        void* data = malloc(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&info), &len))
            snprintf(version, sizeof(version), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                     HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        free(data);
    }
    printf("module %ls %s\n", path, version);
    printf("vtable rva 0x%llx, slot %zu (CreateStateObject) rva 0x%llx\n",
           static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(vtable) - reinterpret_cast<uint8_t*>(module)),
           kCreateStateObjectSlot, static_cast<unsigned long long>(fn - reinterpret_cast<uint8_t*>(module)));

    int rc = argc > 1 && strcmp(argv[1], "--hook-test") == 0 ? HookTest(device5) : 0;
    device5->Release();
    device->Release();
    return rc;
}
