// DarkPicturesRT: a ReShade add-on for the game's ray tracing console variables.
// Phase 1 test build: the census and snapshots of phase 0, plus overlay toggles that set
// cvars through IConsoleVariable::Set (see "overrides" and docs/FINDINGS.md).
//
// What it records, next to the game executable:
//   DarkPicturesRT.log     every r.RayTracing* (and related) cvar the game has: how its
//                          value is stored, the object and vtable addresses, the value;
//                          later snapshots log what changed (e.g. after toggling the
//                          in-game RT reflections option); every override and release
//   DarkPicturesRT_text_<pe timestamp>.bin
//                          the decrypted .text section, once per build: the exe on disk
//                          is SteamStub-encrypted, so vtable entries can only be
//                          disassembled from memory
//
// Two kinds of cvar:
//   TAutoConsoleVariable<int/float>  AsVariableInt/Float return TConsoleVariableData
//                                    (game- and render-thread copies at +0x48)
//   FAutoConsoleVariableRef          both return null; +0x48 points at the global the
//                                    renderer reads, +0x50 is the main value
// Set (+0x80) handles both, so overrides don't need to know which kind a cvar is.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DPRT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Ray tracing settings for " DPRT_GAMES " (DirectX 12). Test build: logs the ray tracing cvars; RT shadows test toggles.";

namespace {

using namespace reshade::api;

// ---------------------------------------------------------------- logging

std::mutex g_mutex;  // present (RHI thread) and overlay (same thread, but be safe)
FILE* g_log = nullptr;
std::wstring g_dir;  // game executable folder, with trailing slash

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_dir.assign(path, n);
    g_dir.resize(g_dir.find_last_of(L"\\/") + 1);
    std::wstring log = g_dir + L"" DPRT_PRODUCT ".log";
    std::wstring prev = g_dir + L"" DPRT_PRODUCT ".prev.log";
    // ReShade loads and unloads the add-on for each device the game creates at
    // startup, so only the first load in a process starts a new log.
    if (GetEnvironmentVariableW(L"DPRT_LOG_OPEN", nullptr, 0) == 0) {
        SetEnvironmentVariableW(L"DPRT_LOG_OPEN", L"1");
        MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(log.c_str(), L"a");
}

std::string Narrow(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- executable image

struct GameInfo {
    const wchar_t* exe;
    const char* name;
};
constexpr GameInfo kGames[] = {
    {L"ManOfMedan-Win64-Shipping.exe", "Man of Medan"},      // SMG019
    {L"LittleHope-Win64-Shipping.exe", "Little Hope"},       // SMG024
    {L"HouseOfAshes-Win64-Shipping.exe", "House of Ashes"},  // SMG025, UE 4.26
    {L"TheDevilInMe-Win64-Shipping.exe", "The Devil in Me"},  // SMG028, UE 4.26
};

const uint8_t* g_base = nullptr;
uint32_t g_imageSize = 0;
uint32_t g_timestamp = 0;
cvars::Section g_text, g_rdata, g_data;

void LogGameInfo() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    g_timestamp = nt->FileHeader.TimeDateStamp;
    cvars::FindSection(g_base, ".text", g_text);
    cvars::FindSection(g_base, ".rdata", g_rdata);
    cvars::FindSection(g_base, ".data", g_data);

    Log("%s %s", DPRT_PRODUCT, DPRT_VERSION);
    Log("exe: %s", Narrow(path).c_str());
    const wchar_t* file = wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    const char* game = nullptr;
    for (const GameInfo& g : kGames)
        if (_wcsicmp(file, g.exe) == 0) game = g.name;
    Log("game: %s", game ? game : "not one this add-on is made for (UE 4.26 games may still work)");
    Log("exe: PE timestamp 0x%08x, image size 0x%x, base %p", g_timestamp, g_imageSize, g_base);
}

bool InSection(const cvars::Section& s, const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    return s.begin && b >= s.begin && b < s.begin + s.size;
}

// "exe+0x1234 .data", or "" for addresses outside the image.
std::string Where(const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    if (!g_base || b < g_base || b >= g_base + g_imageSize) return "";
    const char* sec = InSection(g_text, p) ? " .text" : InSection(g_rdata, p) ? " .rdata" : InSection(g_data, p) ? " .data" : "";
    char buf[48];
    snprintf(buf, sizeof(buf), "exe+0x%llx%s", static_cast<unsigned long long>(b - g_base), sec);
    return buf;
}

// ---------------------------------------------------------------- .text dump

bool DumpText() {
    if (!g_text.begin) return false;
    wchar_t name[64];
    swprintf(name, 64, L"" DPRT_PRODUCT "_text_%08x.bin", g_timestamp);
    std::wstring path = g_dir + name;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Log("text dump: %s already exists", Narrow(name).c_str());
        return true;
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        Log("text dump: cannot create %s", Narrow(name).c_str());
        return false;
    }
    // Header: magic, image base at run time, .text RVA, .text size; then the bytes.
    struct {
        char magic[8] = {'D', 'P', 'R', 'T', 'T', 'E', 'X', 'T'};
        uint64_t base;
        uint32_t rva;
        uint32_t size;
    } header;
    header.base = reinterpret_cast<uint64_t>(g_base);
    header.rva = static_cast<uint32_t>(g_text.begin - g_base);
    header.size = static_cast<uint32_t>(g_text.size);
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1 && fwrite(g_text.begin, 1, g_text.size, f) == g_text.size;
    fclose(f);
    Log("text dump: %s, .text at exe+0x%x, 0x%x bytes%s", Narrow(name).c_str(), header.rva, header.size,
        ok ? "" : " (write failed)");
    return ok;
}

// ---------------------------------------------------------------- cvar census

// Names that start with these are looked up; the full names come from the exe's own
// strings, so nothing is hard-coded and a new build's cvars are found too.
const wchar_t* const kPrefixes[] = {
    L"r.RayTracing",       L"r.D3D12.RayTracing", L"r.D3D12.DXR",          L"r.Shadow.Denoiser",
    L"r.AmbientOcclusion.Denoiser", L"r.Reflections.Denoiser", L"r.GlobalIllumination.Denoiser",
    L"r.DiffuseIndirect.Denoiser", L"r.DynamicGIType", L"r.VPLGI.",            L"r.VSGL.",
    L"r.SurfelsAndProbes.", L"r.SkinCache.",      L"r.Water.SingleLayer.RTR", L"r.ScreenPercentage",
    L"r.TemporalAA.Upsampling", L"r.SSR.",       L"r.SSGI.",
};

std::vector<std::wstring> CollectNames() {
    std::set<std::wstring> names;
    const uint8_t* p = g_rdata.begin;
    const size_t n = g_rdata.size & ~size_t(1);
    for (size_t i = 2; i + 4 <= n; i += 2) {
        if (p[i] != 'r' || p[i + 1] != 0 || p[i + 2] != '.' || p[i + 3] != 0) continue;
        if (p[i - 2] != 0 || p[i - 1] != 0) continue;  // must start a string
        std::wstring s;
        size_t j = i;
        bool ok = true;
        for (; j + 1 < n; j += 2) {
            wchar_t c = static_cast<wchar_t>(p[j] | (p[j + 1] << 8));
            if (c == 0) break;
            if (!(iswalnum(c) || c == L'.' || c == L'_') || s.size() > 96) {
                ok = false;
                break;
            }
            s.push_back(c);
        }
        if (!ok) continue;
        for (const wchar_t* prefix : kPrefixes)
            if (s.compare(0, wcslen(prefix), prefix) == 0) {
                names.insert(s);
                break;
            }
    }
    return {names.begin(), names.end()};
}

constexpr size_t kObjectBytes = 0x60;  // FConsoleVariableBase + Ref/Data fields fit in this

struct Cvar {
    std::wstring name;
    void* var = nullptr;
    void* vtable = nullptr;
    int32_t* intData = nullptr;  // TConsoleVariableData<int32>::ShadowedValue[2]
    float* floatData = nullptr;  // TConsoleVariableData<float>::ShadowedValue[2]
    uint8_t object[kObjectBytes] = {};
    uint32_t pointee[kObjectBytes / 8] = {};  // 4 bytes at each qword that points into .data
    uint32_t value[2] = {};                   // intData / floatData copies, game and render thread
};

void* g_manager = nullptr;
std::vector<Cvar> g_cvars;  // last snapshot
int g_snapshots = 0;
std::string g_status = "waiting for the game";

void ReadState(Cvar& c) {
    cvars::SafeCopy(c.object, c.var, kObjectBytes);
    if (c.intData || c.floatData) cvars::SafeCopy(c.value, c.intData ? static_cast<void*>(c.intData) : c.floatData, 8);
    for (size_t q = 0; q < kObjectBytes / 8; ++q) {
        void* ptr;
        memcpy(&ptr, c.object + q * 8, 8);
        c.pointee[q] = 0;
        if (InSection(g_data, ptr)) cvars::SafeCopy(&c.pointee[q], ptr, 4);
    }
}

// The value as of the snapshot that read it.
std::string Value(const Cvar& c) {
    char buf[64];
    if (c.intData) {
        int32_t v[2];
        memcpy(v, c.value, sizeof(v));
        snprintf(buf, sizeof(buf), "int %d / %d", v[0], v[1]);
    } else if (c.floatData) {
        float v[2];
        memcpy(v, c.value, sizeof(v));
        snprintf(buf, sizeof(buf), "float %g / %g", v[0], v[1]);
    } else {
        snprintf(buf, sizeof(buf), "no data (Ref/bool/string)");
    }
    return buf;
}

void LogObject(const Cvar& c) {
    for (size_t q = 0; q < kObjectBytes / 8; ++q) {
        uint64_t v;
        memcpy(&v, c.object + q * 8, 8);
        std::string where = Where(reinterpret_cast<void*>(v));
        char extra[96] = "";
        if (InSection(g_data, reinterpret_cast<void*>(v))) {
            float f;
            memcpy(&f, &c.pointee[q], 4);
            snprintf(extra, sizeof(extra), " -> int %d / float %g", static_cast<int32_t>(c.pointee[q]), f);
        }
        Log("      +0x%02zx %016llx %s%s", q * 8, static_cast<unsigned long long>(v), where.c_str(), extra);
    }
}

void LogVtable(void* vtable) {
    Log("  vtable %s:", Where(vtable).c_str());
    for (int i = 0; i < 32; ++i) {
        void* fn = nullptr;
        if (!cvars::SafeCopy(&fn, static_cast<uint8_t*>(vtable) + i * 8, 8) || !InSection(g_text, fn)) {
            Log("    +0x%02x end", i * 8);
            break;
        }
        Log("    +0x%02x %s", i * 8, Where(fn).c_str());
    }
}

// First snapshot: the full census. Later ones: what changed since the previous one.
void Snapshot() {
    if (!g_manager) {
        void** singleton = cvars::FindConsoleManagerSingleton(g_base);
        if (!singleton) {
            g_status = "IConsoleManager::Singleton not found";
            Log("cvars: %s (no lea of L\"r.ScreenPercentage\" after mov rcx,[rip+x])", g_status.c_str());
            return;
        }
        if (!*singleton) {
            g_status = "console manager not created yet";
            return;
        }
        g_manager = *singleton;
        Log("cvars: IConsoleManager::Singleton at %s, manager %p, vtable %s", Where(singleton).c_str(), g_manager,
            Where(*static_cast<void**>(g_manager)).c_str());
    }

    std::vector<Cvar> now;
    for (const std::wstring& name : CollectNames()) {
        Cvar c;
        c.name = name;
        c.var = cvars::FindVariable(g_manager, name.c_str());
        if (!c.var) continue;
        cvars::SafeCopy(&c.vtable, c.var, 8);
        c.intData = static_cast<int32_t*>(cvars::AsData(c.var, false));
        if (!c.intData) c.floatData = static_cast<float*>(cvars::AsData(c.var, true));
        ReadState(c);
        now.push_back(std::move(c));
    }
    ++g_snapshots;

    if (g_snapshots == 1) {
        std::map<void*, int> vtables;  // vtable -> number of cvars using it
        int ints = 0, floats = 0, others = 0;
        Log("cvars: %zu found (name | object | vtable | value game / render thread)", now.size());
        for (const Cvar& c : now) {
            ++vtables[c.vtable];
            (c.intData ? ints : c.floatData ? floats : others)++;
            std::string obj = Where(c.var);
            Log("  %-60s | %s | %s | %s", Narrow(c.name.c_str()).c_str(), obj.empty() ? "heap" : obj.c_str(),
                Where(c.vtable).c_str(), Value(c).c_str());
            if (!c.intData && !c.floatData) LogObject(c);
        }
        Log("cvars: %d int data, %d float data, %d without data; %zu distinct vtables", ints, floats, others,
            vtables.size());
        for (auto& [vt, count] : vtables) {
            Log("vtable used by %d cvars", count);
            LogVtable(vt);
        }
    } else {
        std::map<std::wstring, const Cvar*> before;
        for (const Cvar& c : g_cvars) before[c.name] = &c;
        int changes = 0;
        Log("snapshot %d: changes since snapshot %d", g_snapshots, g_snapshots - 1);
        for (const Cvar& c : now) {
            auto it = before.find(c.name);
            if (it == before.end()) {
                Log("  new %s | %s", Narrow(c.name.c_str()).c_str(), Value(c).c_str());
                ++changes;
                continue;
            }
            const Cvar& b = *it->second;
            if (memcmp(b.object, c.object, kObjectBytes) == 0 && memcmp(b.pointee, c.pointee, sizeof(c.pointee)) == 0 &&
                memcmp(b.value, c.value, sizeof(c.value)) == 0)
                continue;
            ++changes;
            Log("  %s: %s", Narrow(c.name.c_str()).c_str(), Value(c).c_str());
            for (size_t q = 0; q < kObjectBytes / 8; ++q) {
                uint64_t x, y;
                memcpy(&x, b.object + q * 8, 8);
                memcpy(&y, c.object + q * 8, 8);
                if (x != y)
                    Log("      +0x%02zx %016llx -> %016llx", q * 8, static_cast<unsigned long long>(x),
                        static_cast<unsigned long long>(y));
                if (b.pointee[q] != c.pointee[q])
                    Log("      +0x%02zx -> %s: int %d -> %d", q * 8, Where(reinterpret_cast<void*>(y)).c_str(),
                        static_cast<int32_t>(b.pointee[q]), static_cast<int32_t>(c.pointee[q]));
            }
        }
        Log("snapshot %d: %d changed", g_snapshots, changes);
    }
    g_cvars = std::move(now);
    char buf[96];
    snprintf(buf, sizeof(buf), "%zu cvars, %d snapshot%s", g_cvars.size(), g_snapshots, g_snapshots == 1 ? "" : "s");
    g_status = buf;
}

// ---------------------------------------------------------------- overrides

// A cvar the add-on sets through IConsoleVariable::Set at ECVF_SetByConsole, which
// outranks the game's menu (GameSetting, 0x02) and ini defaults (ProjectSetting, 0x03),
// so the game can't change it back. Set runs on ReShade's present thread, where the
// engine skips the copy the renderer reads, so MirrorToRenderer does that part.
// Releasing restores the original flags (priority) and value, so the game is in charge
// again.
struct Override {
    const wchar_t* name;
    const char* label;
    const wchar_t* forced;  // value while on
    bool want = false;      // overlay checkbox
    bool active = false;    // forced right now
    void* var = nullptr;
    uint32_t originalFlags = 0;
    int32_t original = 0;
    uint32_t repairs = 0;  // times CheckOverrides put the value back
};
// Test toggles. The first turns ray-traced shadows on; the rest find what blocks the
// light when it is on (0.1.1 test: the key spotlight went dark everywhere). Light-type
// toggles put that type back on shadow maps; geometry toggles drop that geometry from
// the ray tracing scene (all effects, not only shadows).
Override g_overrides[] = {
    {L"r.RayTracing.Shadows", "Ray traced shadows (r.RayTracing.Shadows 1)", L"1"},
    {L"r.RayTracing.Shadows.Lights.Spot", "  Spot lights on shadow maps (...Lights.Spot 0)", L"0"},
    {L"r.RayTracing.Shadows.Lights.Point", "  Point lights on shadow maps (...Lights.Point 0)", L"0"},
    {L"r.RayTracing.Shadows.Lights.Rect", "  Rect lights on shadow maps (...Lights.Rect 0)", L"0"},
    {L"r.RayTracing.Shadows.Lights.Directional", "  Directional on shadow maps (...Lights.Directional 0)", L"0"},
    {L"r.RayTracing.Shadows.EnableTwoSidedGeometry", "  Single-sided geometry (...EnableTwoSidedGeometry 0)", L"0"},
    {L"r.RayTracing.Shadows.EnableMaterials", "  Masked materials as opaque (...EnableMaterials 0)", L"0"},
    {L"r.RayTracing.Geometry.SkeletalMeshes", "  No skeletal meshes in RT scene (Geometry.SkeletalMeshes 0)", L"0"},
    {L"r.RayTracing.Geometry.StaticMeshes", "  No static meshes in RT scene (Geometry.StaticMeshes 0)", L"0"},
    {L"r.RayTracing.Geometry.NiagaraSprites", "  No Niagara sprites in RT scene (Geometry.NiagaraSprites 0)", L"0"},
    {L"r.RayTracing.Geometry.NiagaraMeshes", "  No Niagara meshes in RT scene (Geometry.NiagaraMeshes 0)", L"0"},
};

// Main value (what Set stored) and the value the renderer reads, with the priority.
std::string Live(void* var) {
    cvars::Storage st;
    int32_t main = 0, render = 0;
    if (!cvars::GetStorage(var, st) || !cvars::SafeCopy(&main, st.main, st.size) || !cvars::RenderInt(var, render))
        return "unreadable";
    char buf[80];
    snprintf(buf, sizeof(buf), "main %d / renderer %d, priority 0x%02x", main, render, cvars::Flags(var) >> 24);
    return buf;
}

void ApplyOverrides() {
    if (!g_manager) return;
    for (Override& o : g_overrides) {
        if (o.want == o.active) continue;
        if (!o.var) o.var = cvars::FindVariable(g_manager, o.name);
        if (!o.var) {
            Log("override: %ls not found", o.name);
            o.want = false;
            continue;
        }
        std::string before = Live(o.var);
        if (o.want) {
            o.originalFlags = cvars::Flags(o.var);
            cvars::RenderInt(o.var, o.original);  // what the renderer used before us
            bool ok = cvars::Set(o.var, o.forced, cvars::kSetByConsole) && cvars::MirrorToRenderer(o.var);
            Log("override: %ls = %ls%s: %s -> %s", o.name, o.forced, ok ? "" : " (Set or mirror failed)", before.c_str(),
                Live(o.var).c_str());
            o.active = ok;
            if (!ok) o.want = false;
        } else {
            // Back to the game's priority first, or Set would refuse the lower one.
            cvars::Flags(o.var) = o.originalFlags;
            wchar_t value[16];
            swprintf(value, 16, L"%d", o.original);
            bool ok = cvars::Set(o.var, value, o.originalFlags & 0xff000000) && cvars::MirrorToRenderer(o.var);
            Log("override: %ls released to %ls%s: %s -> %s", o.name, value, ok ? "" : " (Set or mirror failed)", before.c_str(),
                Live(o.var).c_str());
            o.active = false;
        }
    }
}

// Puts a forced value back if something changed it (it shouldn't, given the priority).
// Logs the first few times per override.
void CheckOverrides() {
    for (Override& o : g_overrides) {
        if (!o.active) continue;
        int32_t render = 0;
        if (!cvars::RenderInt(o.var, render) || render == _wtoi(o.forced)) continue;
        if (++o.repairs <= 5)
            Log("override: %ls changed behind our back: %s; setting it again%s", o.name, Live(o.var).c_str(),
                o.repairs == 5 ? " (not logging further repairs)" : "");
        cvars::Set(o.var, o.forced, cvars::kSetByConsole);
        cvars::MirrorToRenderer(o.var);
    }
}

// ---------------------------------------------------------------- events

constexpr uint64_t kCensusFrame = 600;  // ~10 s in: plugins have registered their cvars by then
uint64_t g_frames = 0;
bool g_textDumped = false;
bool g_snapshotRequested = false;

void OnInitDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    Log("device created: %s", dev->get_api() == device_api::d3d12 ? "Direct3D 12"
                              : dev->get_api() == device_api::d3d11 ? "Direct3D 11 (ray tracing needs -dx12)"
                                                                    : "other API");
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
    if (g_frames == kCensusFrame || (g_snapshotRequested && g_frames > kCensusFrame)) {
        g_snapshotRequested = false;
        Snapshot();
        if (!g_textDumped) g_textDumped = DumpText();
    }
    ApplyOverrides();
    if (g_frames % 60 == 0) CheckOverrides();
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextUnformatted(DPRT_PRODUCT " " DPRT_VERSION " - phase 1 test");
    ImGui::Text("Status: %s", g_frames < kCensusFrame ? "first census after 10 s" : g_status.c_str());

    ImGui::Separator();
    if (!g_manager) ImGui::BeginDisabled();
    for (Override& o : g_overrides) {
        ImGui::Checkbox(o.label, &o.want);
        if (o.var) ImGui::Text("    now: %s", Live(o.var).c_str());
    }
    if (!g_manager) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button("Snapshot cvars (logs what changed)")) g_snapshotRequested = true;
    ImGui::TextWrapped(
        "Change a graphics option in the game's menu (e.g. ray traced reflections), then take a snapshot: "
        "the log lists the cvars it changed.");
}

void Install() {
    OpenLog();
    LogGameInfo();
    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_overlay(DPRT_PRODUCT, &DrawOverlay);
}

void Uninstall() {
    reshade::unregister_overlay(DPRT_PRODUCT, &DrawOverlay);
    if (g_log) {
        Log("unloading");
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            Install();
            break;
        case DLL_PROCESS_DETACH:
            Uninstall();
            reshade::unregister_addon(module);  // also removes the event callbacks
            break;
    }
    return TRUE;
}
