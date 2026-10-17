// DarkPicturesRT: a ReShade add-on that tunes the game's ray tracing through its console
// variables, from an overlay tab with presets ("settings"), saved in ReShade.ini.
// It also carries the investigation tools: the cvar census and snapshots, the ray tracing
// pipeline log and DXIL dumps, and shader replacement (see "shader replacement" and
// docs/FINDINGS.md).
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
//   DarkPicturesRT_shaders\<hash>.dxil (read, not written)
//                          replacement ray tracing libraries, matched by the original's
//                          hash (see "shader replacement")
//   DarkPicturesRT_dxil\<hash>.dxil, index.txt
//                          every ray tracing DXIL library the game creates, once, with
//                          its export names (see "ray tracing pipelines")
//
// Two kinds of cvar:
//   TAutoConsoleVariable<int/float>  AsVariableInt/Float return TConsoleVariableData
//                                    (game- and render-thread copies at +0x48)
//   FAutoConsoleVariableRef          both return null; +0x48 points at the global the
//                                    renderer reads, +0x50 is the main value
// Set (+0x80) handles both, so overrides don't need to know which kind a cvar is.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cmath>
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
    "Ray traced reflections, AO and shadows settings with presets for " DPRT_GAMES " (DirectX 12).";

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
// Finds the console manager: the singleton's address once (a scan of .text), then waits
// for the engine to create the manager. Called every frame until it succeeds.
bool FindManager() {
    static void** singleton = nullptr;
    static bool searched = false;
    if (g_manager) return true;
    if (!searched) {
        searched = true;
        singleton = cvars::FindConsoleManagerSingleton(g_base);
        if (!singleton) {
            g_status = "IConsoleManager::Singleton not found";
            Log("cvars: %s (no lea of L\"r.ScreenPercentage\" after mov rcx,[rip+x])", g_status.c_str());
        }
    }
    if (!singleton || !*singleton) return false;
    g_manager = *singleton;
    Log("cvars: IConsoleManager::Singleton at %s, manager %p, vtable %s", Where(singleton).c_str(), g_manager,
        Where(*static_cast<void**>(g_manager)).c_str());
    return true;
}

void Snapshot() {
    if (!FindManager()) return;

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

// ---------------------------------------------------------------- settings

// Every control in the overlay is a Setting on one console variable. While a setting is
// overridden, the add-on sets the cvar through IConsoleVariable::Set at
// ECVF_SetByConsole, which outranks the game's menu (GameSetting, 0x02) and ini defaults
// (ProjectSetting, 0x03), so the game can't change it back. Set runs on ReShade's present
// thread, where the engine skips the copy the renderer reads, so MirrorToRenderer does
// that part. Turning an override off restores the original flags (priority) and value,
// so the game is in charge again. Overrides persist in ReShade.ini, section
// [DarkPicturesRT], one key per cvar.

enum class Kind { Toggle, Choice, Int, Float };

struct Choice {
    int value;
    const char* label;
};

struct Setting {
    const char* group;
    const wchar_t* cvar;
    const char* label;
    const char* help;
    Kind kind;
    float min = 0, max = 1;  // Int / Float range; Toggle: the value forced while ticked
    std::vector<Choice> choices = {};
    bool persist = true;  // diagnostics are session-only

    bool overridden = false;  // the user wants this cvar forced
    float value = 0;          // to this value
    bool active = false;      // forced right now
    float applied = 0;        // the value forced
    void* var = nullptr;
    bool missing = false;  // not a cvar in this build
    std::wstring original;  // renderer's value before we forced it, as text
    uint32_t originalFlags = 0;
    uint32_t repairs = 0;  // times CheckSettings put the value back
};

std::vector<Setting> g_settings;
bool g_settingsDirty = false;  // save to ReShade.ini on the next present

void DefineSettings() {
    const std::vector<Choice> gameOffOn = {{-1, "Post-process volume"}, {0, "Off"}, {1, "On"}};
    const std::vector<Choice> denoiser = {{0, "Off"}, {1, "Engine default"}, {2, "Screen-space denoiser"}};
    g_settings = {
        // Reflections. The game runs UE's "experimental deferred" path (fast, subset of
        // features); samples, bounces and shadows only apply to the standard path, which
        // needs a denoiser (the game turns it off for the deferred path).
        {"Reflections", L"r.RayTracing.Reflections", "Ray traced reflections",
         "The game's menu sets this (RT Reflections). Overriding locks it.", Kind::Choice, 0, 0, gameOffOn},
        {"Reflections", L"r.RayTracing.Reflections.ExperimentalDeferred", "Algorithm",
         "Fast deferred is the game's choice, with its own spatial and temporal resolve. Standard supports samples, "
         "bounces and shadows in reflections, but with UE 4.26's denoiser it is very noisy in this game's dark "
         "scenes, and glass smears (tested).",
         Kind::Choice, 0, 0, {{1, "Fast deferred"}, {0, "Standard (noisy)"}}},
        {"Reflections", L"r.RayTracing.Reflections.ScreenPercentage", "Resolution (%)",
         "Percentage of the render resolution reflections are traced at. The game uses 25.", Kind::Float, 25, 100},
        {"Reflections", L"r.RayTracing.Reflections.MaxRoughness", "Max roughness",
         "Surfaces rougher than this use the fallback instead of traced reflections.", Kind::Float, 0, 1},
        {"Reflections", L"r.Reflections.Denoiser", "Denoiser", "Needed by the standard algorithm.", Kind::Choice, 0, 0,
         denoiser},
        {"Reflections", L"r.RayTracing.Reflections.SamplesPerPixel", "Samples per pixel (standard)", nullptr, Kind::Int,
         1, 4},
        {"Reflections", L"r.RayTracing.Reflections.MaxBounces", "Bounces (standard)", nullptr, Kind::Int, 1, 4},
        {"Reflections", L"r.RayTracing.Reflections.Shadows", "Shadows in reflections (standard)", nullptr, Kind::Choice,
         0, 0, {{-1, "Post-process volume"}, {0, "Off"}, {1, "Hard"}, {2, "Soft (area)"}}},
        {"Reflections", L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.NumSamples",
         "Resolve samples (deferred)", "Samples of the deferred algorithm's spatial resolve. The game uses 8.",
         Kind::Int, 4, 32},
        {"Reflections", L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.MaxRadius",
         "Resolve radius (deferred)", "Maximum resolve radius in pixels (rougher surfaces use more). The game uses 8.",
         Kind::Float, 2, 16},
        {"Reflections", L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.TemporalQuality",
         "Temporal accumulation (deferred)", nullptr, Kind::Choice, 0, 0,
         {{0, "Off"}, {1, "Tiles (low)"}, {2, "Randomized tiles (medium)"}}},
        {"Reflections", L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.TemporalWeight",
         "History weight (deferred)", "Weight of the previous frames. Higher is smoother but smears in motion. "
         "The game uses 0.95.", Kind::Float, 0.5f, 0.99f},
        {"Reflections", L"r.Reflections.Denoiser.ReconstructionSamples", "Denoiser samples (standard)",
         "Reconstruction samples of UE's reflection denoiser. Default 8.", Kind::Int, 4, 32},

        // Ambient occlusion: the game already enables RT AO through its settings.
        {"Ambient occlusion", L"r.RayTracing.AmbientOcclusion", "Ray traced AO", nullptr, Kind::Choice, 0, 0, gameOffOn},
        {"Ambient occlusion", L"r.RayTracing.AmbientOcclusion.SamplesPerPixel", "Samples per pixel",
         "More samples, less noise, more cost.", Kind::Int, 1, 8},
        {"Ambient occlusion", L"r.AmbientOcclusion.Denoiser", "Denoiser", nullptr, Kind::Choice, 0, 0, denoiser},
        {"Ambient occlusion", L"r.RayTracing.AmbientOcclusion.EnableTwoSidedGeometry", "Two-sided geometry", nullptr,
         Kind::Toggle, 1},

        // Shadows: RT for point, rect and directional lights; spot lights stay on shadow maps
        // (docs/FINDINGS.md). They carry the game's key lighting and lose prop shadows
        // (the mannequin's hat) under RT. Tested to look good this way.
        {"Shadows", L"r.RayTracing.Shadows", "Ray traced shadows",
         "Ray traced shadows for the game's lights. Turning this on also puts spot lights on shadow maps.",
         Kind::Toggle, 1},
        {"Shadows", L"r.RayTracing.Shadows.Lights.Spot", "Spot lights on shadow maps",
         "Recommended. The game's key lights are spot lights; with RT shadows some props (such as hats) lose "
         "their shadows from them.",
         Kind::Toggle, 0},

        // Translucency: tested; it fixes glass that raster translucency blows out to white
        // (the pharmacy display cases). High and Ultra turn it on.
        {"Translucency", L"r.RayTracing.Translucency", "Ray traced translucency",
         "Proper reflections and refraction on glass; fixes display cases that render blown out. Replaces raster "
         "translucency, so watch particles and fog.",
         Kind::Choice, 0, 0, {{-1, "Post-process volume"}, {0, "Off"}, {1, "On"}}},
        {"Translucency", L"r.RayTracing.Translucency.Refraction", "Refraction", nullptr, Kind::Choice, 0, 0,
         {{0, "Off"}, {1, "On"}}},
        {"Translucency", L"r.RayTracing.Translucency.MaxRoughness", "Max roughness", nullptr, Kind::Float, 0, 1},

        // Experimental: effects the game was cooked with but never turns on. Presets leave
        // them alone. Tested: final gather GI is noisy as is; sky light is noisy indoors
        // and looks good outdoors.
        {"Experimental", L"r.RayTracing.GlobalIllumination", "Ray traced global illumination",
         "Bounce light. Expensive; final gather is the cheaper method. The game's lighting is largely baked, "
         "so it may double up.",
         Kind::Choice, 0, 0, {{-1, "Post-process volume"}, {0, "Off"}, {1, "Brute force"}, {2, "Final gather"}}},
        {"Experimental", L"r.RayTracing.GlobalIllumination.SamplesPerPixel", "GI: samples per pixel", nullptr,
         Kind::Int, 1, 8},
        {"Experimental", L"r.RayTracing.GlobalIllumination.MaxBounces", "GI: bounces", nullptr, Kind::Int, 1, 4},
        {"Experimental", L"r.RayTracing.GlobalIllumination.ScreenPercentage", "GI: resolution (%)",
         "UE's default is 50.", Kind::Float, 25, 100},
        {"Experimental", L"r.RayTracing.GlobalIllumination.FireflySuppression", "GI: firefly suppression",
         "Tonemaps samples to suppress bright speckles.", Kind::Toggle, 1},
        {"Experimental", L"r.RayTracing.GlobalIllumination.FinalGather.FilterWidth", "GI: filter width (final gather)",
         "Neighbourhood for sample stealing between pixels. Default 0; higher is smoother and blurrier.", Kind::Int, 0,
         8},
        {"Experimental", L"r.RayTracing.GlobalIllumination.FinalGather.Iterations", "GI: iterations (final gather)",
         "Iterations of gather point creation. More is less noisy and slower.", Kind::Int, 1, 8},
        {"Experimental", L"r.RayTracing.GlobalIllumination.Denoiser", "GI: denoiser", nullptr, Kind::Choice, 0, 0,
         {{0, "Off"}, {1, "On"}}},
        {"Experimental", L"r.RayTracing.SkyLight", "Ray traced sky light",
         "Occlusion of sky lighting. For outdoor scenes: indoors it is very noisy (tested).", Kind::Toggle, 1},
        {"Experimental", L"r.RayTracing.SkyLight.Denoiser", "Sky light: denoiser", nullptr, Kind::Choice, 0, 0,
         {{0, "Off"}, {1, "On"}}},
        {"Experimental", L"r.RayTracing.SkyLight.SamplesPerPixel", "Sky light: samples per pixel", nullptr, Kind::Int, 1,
         8},

        // Diagnostics: session-only test switches from the shadow investigation.
        {"Diagnostics", L"r.RayTracing.Shadows.Lights.Point", "Point lights on shadow maps", nullptr, Kind::Toggle, 0, 1,
         {}, false},
        {"Diagnostics", L"r.RayTracing.Shadows.Lights.Rect", "Rect lights on shadow maps", nullptr, Kind::Toggle, 0, 1,
         {}, false},
        {"Diagnostics", L"r.RayTracing.Shadows.Lights.Directional", "Directional on shadow maps", nullptr, Kind::Toggle,
         0, 1, {}, false},
        {"Diagnostics", L"r.RayTracing.Shadows.EnableTwoSidedGeometry", "Shadows: single-sided geometry", nullptr,
         Kind::Toggle, 0, 1, {}, false},
        {"Diagnostics", L"r.RayTracing.Shadows.EnableMaterials", "Shadows: masked materials as opaque", nullptr,
         Kind::Toggle, 0, 1, {}, false},
        {"Diagnostics", L"r.RayTracing.Geometry.SkeletalMeshes", "No skeletal meshes in RT scene", nullptr,
         Kind::Toggle, 0, 1, {}, false},
        {"Diagnostics", L"r.RayTracing.Geometry.StaticMeshes", "No static meshes in RT scene", nullptr, Kind::Toggle, 0,
         1, {}, false},
        {"Diagnostics", L"r.SkinCache.SceneMemoryLimitInMB", "Skin cache budget 1024 MB", nullptr, Kind::Toggle, 1024,
         1, {}, false},
    };
}

// Presets set the non-diagnostic settings listed and clear the rest.
struct Preset {
    const char* name;
    const char* help;
    std::vector<std::pair<const wchar_t*, double>> values;
};
const Preset kPresets[] = {
    {"Game default", "Everything back to the game's own values.", {}},
    {"Sharper reflections", "The game's fast algorithm at 50% resolution (the game uses 25%).",
     {{L"r.RayTracing.Reflections.ScreenPercentage", 50}}},
    // At full resolution each output pixel gets 16x the game's rays, so the resolve needs
    // less history: the game's 0.95 history weight smears in motion there (tested).
    {"High", "Reflections at full resolution with less history (0.9) to cut smearing; ray traced translucency.",
     {{L"r.RayTracing.Reflections.ScreenPercentage", 100},
      {L"r.RayTracing.Translucency", 1},
      {L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.TemporalWeight", 0.9}}},
    {"Ultra",
     "Reflections at full resolution with 16 resolve samples (the game uses 8) and history 0.85; ray traced "
     "translucency; AO at 2 samples.",
     {{L"r.RayTracing.Reflections.ScreenPercentage", 100},
      {L"r.RayTracing.Translucency", 1},
      {L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.NumSamples", 16},
      {L"r.RayTracing.Reflections.ExperimentalDeferred.SpatialResolve.TemporalWeight", 0.85},
      {L"r.RayTracing.AmbientOcclusion.SamplesPerPixel", 2}}},
};

bool IsFloat(void* var) { return cvars::IsFloatVar(var); }

// The value the renderer reads, as text in the cvar's own type (for Set and the log).
std::wstring RenderText(void* var) {
    cvars::Storage st;
    uint32_t raw = 0;
    wchar_t buf[32] = L"?";
    if (cvars::GetStorage(var, st) && cvars::SafeCopy(&raw, st.render, st.size)) {
        float f;
        memcpy(&f, &raw, 4);
        if (IsFloat(var))
            swprintf(buf, 32, L"%g", f);
        else
            swprintf(buf, 32, L"%d", st.size == 1 ? static_cast<int32_t>(raw & 0xff) : static_cast<int32_t>(raw));
    }
    return buf;
}

// Main value (what Set stored) and the value the renderer reads, with the priority.
std::string Live(void* var) {
    cvars::Storage st;
    uint32_t main = 0;
    if (!cvars::GetStorage(var, st) || !cvars::SafeCopy(&main, st.main, st.size)) return "unreadable";
    char buf[96];
    if (IsFloat(var)) {
        float f;
        memcpy(&f, &main, 4);
        snprintf(buf, sizeof(buf), "main %g / renderer %ls, priority 0x%02x", f, RenderText(var).c_str(),
                 cvars::Flags(var) >> 24);
    } else {
        snprintf(buf, sizeof(buf), "main %d / renderer %ls, priority 0x%02x",
                 st.size == 1 ? static_cast<int32_t>(main & 0xff) : static_cast<int32_t>(main), RenderText(var).c_str(),
                 cvars::Flags(var) >> 24);
    }
    return buf;
}

std::wstring ValueText(const Setting& s, float v) {
    wchar_t buf[32];
    if (s.kind == Kind::Float)
        swprintf(buf, 32, L"%g", v);
    else
        swprintf(buf, 32, L"%d", static_cast<int>(lroundf(v)));
    return buf;
}

// The value the setting should force while overridden.
float Wanted(const Setting& s) { return s.kind == Kind::Toggle ? s.min : s.value; }

bool Resolve(Setting& s) {
    if (s.var || s.missing) return s.var != nullptr;
    s.var = cvars::FindVariable(g_manager, s.cvar);
    if (!s.var) {
        s.missing = true;
        Log("settings: %ls is not a cvar in this build", s.cvar);
    }
    return s.var != nullptr;
}

void ApplySettings() {
    if (!g_manager) return;
    for (Setting& s : g_settings) {
        bool want = s.overridden;
        float v = Wanted(s);
        if (want == s.active && (!want || v == s.applied)) continue;
        if (!Resolve(s)) continue;
        std::string before = Live(s.var);
        if (want) {
            if (!s.active) {  // first time: remember what the game had
                s.originalFlags = cvars::Flags(s.var);
                s.original = RenderText(s.var);
            }
            std::wstring text = ValueText(s, v);
            bool ok = cvars::Set(s.var, text.c_str(), cvars::kSetByConsole) && cvars::MirrorToRenderer(s.var);
            Log("settings: %ls = %ls%s: %s -> %s", s.cvar, text.c_str(), ok ? "" : " (Set or mirror failed)",
                before.c_str(), Live(s.var).c_str());
            s.active = ok;
            s.applied = v;
            if (!ok) s.overridden = false;
        } else {
            // Back to the game's priority first, or Set would refuse the lower one.
            cvars::Flags(s.var) = s.originalFlags;
            bool ok =
                cvars::Set(s.var, s.original.c_str(), s.originalFlags & 0xff000000) && cvars::MirrorToRenderer(s.var);
            Log("settings: %ls released to %ls%s: %s -> %s", s.cvar, s.original.c_str(),
                ok ? "" : " (Set or mirror failed)", before.c_str(), Live(s.var).c_str());
            s.active = false;
        }
    }
}

// Puts a forced value back if something changed it (it shouldn't, given the priority).
// Logs the first few times per setting.
void CheckSettings() {
    for (Setting& s : g_settings) {
        if (!s.active) continue;
        std::wstring render = RenderText(s.var);
        std::wstring forced = ValueText(s, s.applied);
        if (IsFloat(s.var) ? wcstof(render.c_str(), nullptr) == s.applied : render == forced) continue;
        if (++s.repairs <= 5)
            Log("settings: %ls changed behind our back: %s; setting it again%s", s.cvar, Live(s.var).c_str(),
                s.repairs == 5 ? " (not logging further repairs)" : "");
        cvars::Set(s.var, forced.c_str(), cvars::kSetByConsole);
        cvars::MirrorToRenderer(s.var);
    }
}

std::string Narrow(const std::wstring& s) { return Narrow(s.c_str()); }

void LoadSettings() {
    int loaded = 0;
    for (Setting& s : g_settings) {
        if (!s.persist) continue;
        char value[32] = "";
        size_t size = sizeof(value);
        if (!reshade::get_config_value(nullptr, DPRT_PRODUCT, Narrow(s.cvar).c_str(), value, &size) || !value[0] ||
            strcmp(value, "game") == 0)
            continue;
        s.overridden = true;
        s.value = static_cast<float>(atof(value));
        ++loaded;
    }
    Log("settings: %d overrides loaded from ReShade.ini [" DPRT_PRODUCT "]", loaded);
}

void SaveSettings() {
    for (const Setting& s : g_settings) {
        if (!s.persist) continue;
        std::string value = s.overridden ? Narrow(ValueText(s, Wanted(s))) : "game";
        reshade::set_config_value(nullptr, DPRT_PRODUCT, Narrow(s.cvar).c_str(), value.c_str());
    }
}

void ApplyPreset(const Preset& p) {
    for (Setting& s : g_settings) {
        if (!s.persist || strcmp(s.group, "Shadows") == 0 || strcmp(s.group, "Experimental") == 0)
            continue;  // presets leave shadows and experiments alone
        s.overridden = false;
        for (auto& [cvar, v] : p.values)
            if (wcscmp(cvar, s.cvar) == 0) {
                s.overridden = true;
                s.value = static_cast<float>(v);
            }
    }
    g_settingsDirty = true;
    Log("settings: preset \"%s\"", p.name);
}

// ---------------------------------------------------------------- ray tracing pipelines

// Logs every D3D12 ray tracing pipeline (state object) the game creates, and writes each
// DXIL library once to DarkPicturesRT_dxil\<hash>.dxil, with the export names from its
// RDAT part in index.txt, so the library holding OcclusionRGS (RT shadows) can be found.
// "Log ray dispatches" records, for one frame, which pipelines dispatch rays and how big.
// UE creates a pipeline when an effect is first used, so turning RT shadows on adds the
// shadow pipeline to the log right then.

std::mutex g_rtMutex;  // pipeline events come from the game's pipeline compile threads
std::set<uint64_t> g_dumped;  // library hashes already written
uint32_t g_rtPipelines = 0;
std::map<uint64_t, std::string> g_pipelineExports;  // pipeline handle -> its export names, for the dispatch log
std::map<command_list*, uint64_t> g_boundRt;        // ray tracing pipeline bound on each command list
struct DispatchStat {
    uint32_t count = 0, width = 0, height = 0;
};
std::map<uint64_t, DispatchStat> g_dispatches;  // pipeline -> dispatches in the capture frame
enum CaptureState { kCaptureOff, kCaptureArmed, kCaptureRecording };
volatile long g_capture = kCaptureOff;

uint64_t Fnv1a(const void* data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001b3ull;
    return h;
}

// Printable strings in the DXIL container's RDAT part: function and export names
// (plain, such as "OcclusionRGS", and mangled, such as "?OcclusionRGS@@YAXXZ").
std::vector<std::string> RdatNames(const void* code, size_t size) {
    std::vector<std::string> names;
    auto b = static_cast<const uint8_t*>(code);
    if (!b || size < 32 || memcmp(b, "DXBC", 4) != 0) return names;
    uint32_t parts;
    memcpy(&parts, b + 28, 4);
    for (uint32_t i = 0; i < parts && 32 + 4 * (i + 1) <= size; ++i) {
        uint32_t off;
        memcpy(&off, b + 32 + 4 * i, 4);
        if (off + 8 > size || memcmp(b + off, "RDAT", 4) != 0) continue;
        uint32_t partSize;
        memcpy(&partSize, b + off + 4, 4);
        const uint8_t* p = b + off + 8;
        const uint8_t* end = p + (off + 8 + partSize <= size ? partSize : size - off - 8);
        std::string cur;
        for (; p < end; ++p) {
            if (*p >= 0x20 && *p < 0x7f) {
                cur.push_back(static_cast<char>(*p));
                continue;
            }
            if (*p == 0 && cur.size() >= 4 && std::find(names.begin(), names.end(), cur) == names.end())
                names.push_back(cur);
            cur.clear();
        }
    }
    return names;
}

// The plain (unmangled) names, joined, for the log.
std::string PlainNames(const std::vector<std::string>& names, size_t max = 8) {
    std::string out;
    size_t n = 0;
    for (const std::string& s : names) {
        if (s.find_first_of("?@") != std::string::npos) continue;
        if (n++ == max) {
            out += " ...";
            break;
        }
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

void DumpLibrary(uint64_t hash, const void* code, size_t size, const std::vector<std::string>& names) {
    if (!g_dumped.insert(hash).second) return;
    std::wstring dir = g_dir + L"" DPRT_PRODUCT "_dxil\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t file[32];
    swprintf(file, 32, L"%016llx.dxil", static_cast<unsigned long long>(hash));
    if (GetFileAttributesW((dir + file).c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen((dir + file).c_str(), L"wb")) {
            fwrite(code, 1, size, f);
            fclose(f);
        }
        if (FILE* f = _wfopen((dir + L"index.txt").c_str(), L"a")) {
            fprintf(f, "%016llx %zu", static_cast<unsigned long long>(hash), size);
            for (const std::string& s : names) fprintf(f, " %s", s.c_str());
            fprintf(f, "\n");
            fclose(f);
        }
    }
}

const char* SubobjectName(pipeline_subobject_type t) {
    switch (t) {
        case pipeline_subobject_type::raygen_shader: return "raygen";
        case pipeline_subobject_type::any_hit_shader: return "any hit";
        case pipeline_subobject_type::closest_hit_shader: return "closest hit";
        case pipeline_subobject_type::miss_shader: return "miss";
        case pipeline_subobject_type::intersection_shader: return "intersection";
        case pipeline_subobject_type::callable_shader: return "callable";
        case pipeline_subobject_type::libraries: return "libraries";
        case pipeline_subobject_type::shader_groups: return "shader groups";
        case pipeline_subobject_type::max_payload_size: return "max payload size";
        case pipeline_subobject_type::max_attribute_size: return "max attribute size";
        case pipeline_subobject_type::max_recursion_depth: return "max recursion depth";
        case pipeline_subobject_type::flags: return "flags";
        default: return nullptr;
    }
}

bool IsShaderSubobject(pipeline_subobject_type t) {
    return t >= pipeline_subobject_type::raygen_shader && t <= pipeline_subobject_type::callable_shader;
}

// ---------------------------------------------------------------- shader replacement

// DarkPicturesRT_shaders\<hash>.dxil next to the game exe replaces the ray tracing
// library whose code hashes (Fnv1a, as in the dumps) to <hash>. Loaded once at start; to
// turn replacement off, rename or remove the folder.
//
// The swap happens in a hook on the real device's ID3D12Device5::CreateStateObject, not
// in ReShade's create_pipeline: when an add-on edits a description there, ReShade
// rebuilds the state object from its generic subobject model, which loses details UE's
// collections rely on, and linking the collections then fails with E_INVALIDARG (0.2.1).
// The hook changes only the bytecode pointer in the game's own D3D12_DXIL_LIBRARY_DESC
// for the length of the call, so everything else stays as UE wrote it. ReShade's events
// still see the original code, so the dumps and the pipeline log are unaffected.

struct Replacement {
    std::vector<uint8_t> code;  // lives until unload: D3D12 reads it during creation
    uint32_t swapped = 0;       // state objects created with it
    uint32_t accepted = 0;      // ... successfully
};
std::map<uint64_t, Replacement> g_replacements;  // original hash -> replacement

void LoadReplacements() {
    std::wstring dir = g_dir + L"" DPRT_PRODUCT "_shaders\\";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"*.dxil").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        Log("shaders: no " DPRT_PRODUCT "_shaders folder, the game's ray tracing shaders run unchanged");
        return;
    }
    do {
        uint64_t original = wcstoull(fd.cFileName, nullptr, 16);
        FILE* f = _wfopen((dir + fd.cFileName).c_str(), L"rb");
        if (!original || !f) {
            if (f) fclose(f);
            continue;
        }
        Replacement& r = g_replacements[original];
        r.code.resize(fd.nFileSizeLow);
        bool ok = fread(r.code.data(), 1, r.code.size(), f) == r.code.size();
        fclose(f);
        if (!ok || r.code.size() < 32 || memcmp(r.code.data(), "DXBC", 4) != 0) {
            Log("shaders: skipping %s (not a DXIL container)", Narrow(fd.cFileName).c_str());
            g_replacements.erase(original);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    Log("shaders: %zu replacement libraries loaded from " DPRT_PRODUCT "_shaders", g_replacements.size());
}

// Slot 62 in MSVC's C++ vtable (call offset 0x1f0), checked on this machine with
// tools\d3d12probe --hook-test. Not the C ID3D12Device5Vtbl struct in d3d12.h: its order
// differs, and counting it gave 66, which in the C++ layout is ID3D12Device7::
// AddToStateObject. 0.2.2-0.2.5 hooked that by mistake, which is why the hook saw only
// state objects growing from existing collections, and why its 4-argument signature
// broke AddToStateObject (its second argument is the state object to grow from).
constexpr size_t kCreateStateObjectSlot = 62;
using CreateStateObjectFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID,
                                                         void**);
CreateStateObjectFn g_createStateObject = nullptr;  // the original
void** g_hookedSlot = nullptr;                      // the vtable entry we replaced
uint32_t g_links = 0, g_linkFailures = 0;  // state objects that link existing collections
volatile long g_hookCalls = 0;             // all calls through the hook

HRESULT STDMETHODCALLTYPE HookCreateStateObject(ID3D12Device5* self, const D3D12_STATE_OBJECT_DESC* desc,
                                                REFIID riid, void** out) {
    struct Swap {
        D3D12_DXIL_LIBRARY_DESC* lib;
        D3D12_SHADER_BYTECODE original;
        uint64_t hash;
        Replacement* r;
    };
    Swap swaps[16];
    int swapCount = 0;
    bool links = false;
    if (desc) {
        for (UINT i = 0; i < desc->NumSubobjects; ++i) {
            const D3D12_STATE_SUBOBJECT& so = desc->pSubobjects[i];
            if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) links = true;
            if (so.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !so.pDesc || g_replacements.empty() ||
                swapCount == 16)
                continue;
            // In place: associations elsewhere in the description point at these subobjects.
            auto lib = static_cast<D3D12_DXIL_LIBRARY_DESC*>(const_cast<void*>(so.pDesc));
            if (!lib->DXILLibrary.pShaderBytecode) continue;
            uint64_t hash = Fnv1a(lib->DXILLibrary.pShaderBytecode, lib->DXILLibrary.BytecodeLength);
            auto it = g_replacements.find(hash);
            if (it == g_replacements.end()) continue;
            swaps[swapCount++] = {lib, lib->DXILLibrary, hash, &it->second};
            lib->DXILLibrary = {it->second.code.data(), it->second.code.size()};
        }
    }
    HRESULT hr = g_createStateObject(self, desc, riid, out);
    for (int i = 0; i < swapCount; ++i) swaps[i].lib->DXILLibrary = swaps[i].original;

    // What reaches the hook, for diagnosis: the first calls, and the first calls with libraries.
    bool hasLibrary = false;
    for (UINT i = 0; desc && i < desc->NumSubobjects; ++i)
        hasLibrary |= desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    static volatile long libraryCalls = 0;
    long calls = InterlockedIncrement(&g_hookCalls);
    if (desc && (calls <= 8 || (hasLibrary && InterlockedIncrement(&libraryCalls) <= 8))) {
        std::string types;
        for (UINT i = 0; i < desc->NumSubobjects; ++i)
            types += (types.empty() ? "" : ",") + std::to_string(desc->pSubobjects[i].Type);
        Log("shaders: hook call %ld: type %d, subobjects %s, %d replaced, 0x%08lx", calls,
            static_cast<int>(desc->Type), types.c_str(), swapCount, static_cast<unsigned long>(hr));
    }

    if (swapCount || links || FAILED(hr)) {
        std::lock_guard lock(g_rtMutex);
        for (int i = 0; i < swapCount; ++i) {
            Replacement& r = *swaps[i].r;
            ++r.swapped;
            if (SUCCEEDED(hr) && ++r.accepted == 1)
                Log("shaders: replacement for %016llx accepted by D3D12", static_cast<unsigned long long>(swaps[i].hash));
        }
        if (links) {
            ++g_links;
            if (FAILED(hr)) ++g_linkFailures;
        }
        if (FAILED(hr)) {
            std::string types;
            for (UINT i = 0; desc && i < desc->NumSubobjects; ++i)
                types += (types.empty() ? "" : ",") + std::to_string(desc->pSubobjects[i].Type);
            Log("shaders: CreateStateObject (type %d, subobjects %s) FAILED 0x%08lx with %d replaced libraries",
                desc ? static_cast<int>(desc->Type) : -1, types.c_str(), static_cast<unsigned long>(hr), swapCount);
        } else if (links && g_links <= 3) {
            Log("shaders: linked a pipeline from existing collections (type %d, %u subobjects)",
                static_cast<int>(desc->Type), desc->NumSubobjects);
        }
    }
    return hr;
}

bool PatchSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// All of the device's interface versions share one vtable in D3D12Core.dll (0.2.4 log),
// so one entry covers every caller, ReShade included.
void InstallStateObjectHook(device* dev) {
    if (g_hookedSlot || g_replacements.empty() || dev->get_api() != device_api::d3d12) return;
    auto native = reinterpret_cast<ID3D12Device*>(dev->get_native());
    ID3D12Device5* device5 = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&device5)))) {
        Log("shaders: no ID3D12Device5 (no ray tracing), replacements unused");
        return;
    }
    void** slot = *reinterpret_cast<void***>(device5) + kCreateStateObjectSlot;
    device5->Release();
    void* original = *slot;
    if (!PatchSlot(slot, reinterpret_cast<void*>(&HookCreateStateObject))) {
        Log("shaders: cannot hook CreateStateObject (VirtualProtect failed)");
        return;
    }
    g_createStateObject = reinterpret_cast<CreateStateObjectFn>(original);
    g_hookedSlot = slot;
    Log("shaders: hooked ID3D12Device5::CreateStateObject (vtable slot %zu)", kCreateStateObjectSlot);
}

// The vtable would otherwise point into the unloaded add-on.
void RemoveStateObjectHook() {
    if (!g_hookedSlot) return;
    PatchSlot(g_hookedSlot, reinterpret_cast<void*>(g_createStateObject));
    g_hookedSlot = nullptr;
}

void OnInitPipeline(device*, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects,
                    pipeline pipe) {
    bool rt = false;
    for (uint32_t i = 0; i < count; ++i)
        rt |= IsShaderSubobject(subobjects[i].type) || subobjects[i].type == pipeline_subobject_type::libraries ||
              subobjects[i].type == pipeline_subobject_type::shader_groups;
    if (!rt) return;

    std::lock_guard lock(g_rtMutex);
    ++g_rtPipelines;
    Log("rt pipeline %016llx (layout %016llx), %u subobjects:", static_cast<unsigned long long>(pipe.handle),
        static_cast<unsigned long long>(layout.handle), count);
    std::vector<std::string> pipelineNames;
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_subobject& s = subobjects[i];
        const char* name = SubobjectName(s.type);
        if (IsShaderSubobject(s.type)) {
            auto shaders = static_cast<const shader_desc*>(s.data);
            for (uint32_t j = 0; j < s.count; ++j) {
                const shader_desc& d = shaders[j];
                uint64_t hash = d.code ? Fnv1a(d.code, d.code_size) : 0;
                std::vector<std::string> names = RdatNames(d.code, d.code_size);
                if (d.code) DumpLibrary(hash, d.code, d.code_size, names);
                std::string plain = PlainNames(names);
                if (d.entry_point) pipelineNames.push_back(d.entry_point);
                for (const std::string& n : names)
                    if (n.find_first_of("?@") == std::string::npos) pipelineNames.push_back(n);
                Log("  %s[%u]: %zu bytes, %016llx, entry %s, names: %s", name, j, d.code_size,
                    static_cast<unsigned long long>(hash), d.entry_point ? d.entry_point : "-", plain.c_str());
            }
        } else if (s.type == pipeline_subobject_type::libraries) {
            auto libs = static_cast<const pipeline*>(s.data);
            for (uint32_t j = 0; j < s.count; ++j) {
                auto it = g_pipelineExports.find(libs[j].handle);
                Log("  library[%u]: pipeline %016llx (%s)", j, static_cast<unsigned long long>(libs[j].handle),
                    it != g_pipelineExports.end() ? it->second.c_str() : "not seen");
                if (it != g_pipelineExports.end()) pipelineNames.push_back(it->second);
            }
        } else if (s.type == pipeline_subobject_type::shader_groups) {
            auto groups = static_cast<const shader_group*>(s.data);
            std::string line;
            for (uint32_t j = 0; j < s.count; ++j) {
                char buf[48];
                const shader_group& g = groups[j];
                if (g.type == shader_group_type::raygen)
                    snprintf(buf, sizeof(buf), " rg%u", g.raygen.shader_index);
                else if (g.type == shader_group_type::miss)
                    snprintf(buf, sizeof(buf), " ms%u", g.miss.shader_index);
                else if (g.type == shader_group_type::callable)
                    snprintf(buf, sizeof(buf), " cl%u", g.callable.shader_index);
                else
                    snprintf(buf, sizeof(buf), " hit(%d,%d,%d)", static_cast<int>(g.hit_group.closest_hit_shader_index),
                             static_cast<int>(g.hit_group.any_hit_shader_index),
                             static_cast<int>(g.hit_group.intersection_shader_index));
                line += buf;
                if (line.size() > 400) {
                    line += " ...";
                    break;
                }
            }
            Log("  shader groups (%u):%s", s.count, line.c_str());
        } else if (name && s.data && s.type != pipeline_subobject_type::flags) {
            Log("  %s: %u", name, *static_cast<const uint32_t*>(s.data));
        } else {
            Log("  subobject type %u, count %u", static_cast<uint32_t>(s.type), s.count);
        }
    }
    std::string joined;
    for (const std::string& n : pipelineNames) {
        if (joined.find(n) != std::string::npos) continue;
        if (joined.size() > 200) {
            joined += ", ...";
            break;
        }
        joined += joined.empty() ? n : ", " + n;
    }
    g_pipelineExports[pipe.handle] = joined;
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline pipe) {
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::ray_tracing_shader)) == 0) return;
    std::lock_guard lock(g_rtMutex);
    g_boundRt[cmd] = pipe.handle;
}

bool OnDispatchRays(command_list* cmd, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t width, uint32_t height,
                    uint32_t) {
    if (g_capture != kCaptureRecording) return false;
    std::lock_guard lock(g_rtMutex);
    DispatchStat& s = g_dispatches[g_boundRt[cmd]];
    ++s.count;
    s.width = width;
    s.height = height;
    return false;
}

// Called at present: arms the capture for the next frame, then logs it.
void StepCapture() {
    if (g_capture == kCaptureArmed) {
        std::lock_guard lock(g_rtMutex);
        g_dispatches.clear();
        g_capture = kCaptureRecording;
    } else if (g_capture == kCaptureRecording) {
        g_capture = kCaptureOff;
        std::lock_guard lock(g_rtMutex);
        Log("ray dispatches in one frame: %zu pipelines", g_dispatches.size());
        for (auto& [handle, s] : g_dispatches) {
            auto it = g_pipelineExports.find(handle);
            Log("  pipeline %016llx: %u dispatches, last %ux%u (%s)", static_cast<unsigned long long>(handle), s.count,
                s.width, s.height, it != g_pipelineExports.end() ? it->second.c_str() : "not seen");
        }
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
    InstallStateObjectHook(dev);
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
    if (g_frames == kCensusFrame || (g_snapshotRequested && g_frames > kCensusFrame)) {
        g_snapshotRequested = false;
        Snapshot();
        if (!g_textDumped) g_textDumped = DumpText();
    }
    if (FindManager()) {
        ApplySettings();
        if (g_frames % 60 == 0) CheckSettings();
    }
    if (g_settingsDirty) {
        g_settingsDirty = false;
        SaveSettings();
    }
    StepCapture();
}

// The game's current value of a setting's cvar, as a number (what it had before we forced
// it, if we did).
float GameValue(Setting& s) {
    if (!g_manager || !Resolve(s)) return 0;
    return static_cast<float>(wcstod((s.active ? s.original : RenderText(s.var)).c_str(), nullptr));
}

const char* ChoiceLabel(const Setting& s, int v) {
    for (const Choice& c : s.choices)
        if (c.value == v) return c.label;
    return nullptr;
}

void DrawSetting(Setting& s) {
    ImGui::PushID(&s);
    float game = GameValue(s);
    char gameText[48];
    if (s.kind == Kind::Choice && ChoiceLabel(s, static_cast<int>(lroundf(game))))
        snprintf(gameText, sizeof(gameText), "Game (%s)", ChoiceLabel(s, static_cast<int>(lroundf(game))));
    else if (s.kind == Kind::Float)
        snprintf(gameText, sizeof(gameText), "Game (%g)", game);
    else
        snprintf(gameText, sizeof(gameText), "Game (%d)", static_cast<int>(lroundf(game)));

    bool changed = false;
    switch (s.kind) {
        case Kind::Toggle: {
            bool on = s.overridden;
            if (ImGui::Checkbox(s.label, &on)) {
                s.overridden = on;
                changed = true;
                // RT shadows: put spot lights on shadow maps along with it (the safe setting).
                if (on && wcscmp(s.cvar, L"r.RayTracing.Shadows") == 0)
                    for (Setting& o : g_settings)
                        if (wcscmp(o.cvar, L"r.RayTracing.Shadows.Lights.Spot") == 0) o.overridden = true;
            }
            break;
        }
        case Kind::Choice: {
            const char* preview = s.overridden ? ChoiceLabel(s, static_cast<int>(lroundf(s.value))) : gameText;
            if (ImGui::BeginCombo(s.label, preview ? preview : "?")) {
                if (ImGui::Selectable(gameText, !s.overridden)) {
                    s.overridden = false;
                    changed = true;
                }
                for (const Choice& c : s.choices)
                    if (ImGui::Selectable(c.label, s.overridden && lroundf(s.value) == c.value)) {
                        s.overridden = true;
                        s.value = static_cast<float>(c.value);
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            break;
        }
        case Kind::Int:
        case Kind::Float: {
            bool on = s.overridden;
            if (ImGui::Checkbox("##override", &on)) {
                s.overridden = on;
                if (on) s.value = game < s.min || game > s.max ? s.min : game;  // start from the game's value
                changed = true;
            }
            ImGui::SetItemTooltip("Override the game's value");
            ImGui::SameLine();
            if (!s.overridden) ImGui::BeginDisabled();
            if (s.kind == Kind::Int) {
                int v = static_cast<int>(lroundf(s.overridden ? s.value : game));
                if (ImGui::SliderInt(s.label, &v, static_cast<int>(s.min), static_cast<int>(s.max),
                                     s.overridden ? "%d" : "game: %d")) {
                    s.value = static_cast<float>(v);
                    changed = true;
                }
            } else {
                float v = s.overridden ? s.value : game;
                if (ImGui::SliderFloat(s.label, &v, s.min, s.max, s.overridden ? "%.2f" : "game: %.2f")) {
                    s.value = v;
                    changed = true;
                }
            }
            if (!s.overridden) ImGui::EndDisabled();
            break;
        }
    }
    if (s.help) ImGui::SetItemTooltip("%s", s.help);
    if (s.missing) {
        ImGui::SameLine();
        ImGui::TextDisabled("(not in this build)");
    }
    if (changed && s.persist) g_settingsDirty = true;
    ImGui::PopID();
}

void DrawGroup(const char* group, bool open) {
    if (!ImGui::CollapsingHeader(group, open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
    for (Setting& s : g_settings)
        if (strcmp(s.group, group) == 0) DrawSetting(s);
}

void DrawDiagnostics() {
    if (!ImGui::CollapsingHeader("Diagnostics")) return;
    for (Setting& s : g_settings)
        if (strcmp(s.group, "Diagnostics") == 0) DrawSetting(s);
    ImGui::Separator();
    {
        std::lock_guard rtLock(g_rtMutex);
        ImGui::Text("Ray tracing pipelines: %u, DXIL libraries dumped: %zu", g_rtPipelines, g_dumped.size());
        if (!g_replacements.empty()) {
            uint32_t swapped = 0, accepted = 0;
            for (auto& [hash, r] : g_replacements) {
                swapped += r.swapped != 0;
                accepted += r.accepted != 0;
            }
            ImGui::Text("Replacement shaders: %zu loaded, %u swapped in, %u accepted; %u links, %u failed",
                        g_replacements.size(), swapped, accepted, g_links, g_linkFailures);
            if (!g_hookedSlot)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "CreateStateObject not hooked: see the log");
            if (swapped != accepted || g_linkFailures)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "A state object with replacements failed: see the log");
        }
    }
    if (ImGui::Button("Log ray dispatches (one frame)") && g_capture == kCaptureOff) g_capture = kCaptureArmed;
    ImGui::SameLine();
    if (ImGui::Button("Snapshot cvars")) g_snapshotRequested = true;
    ImGui::SetItemTooltip("Logs what changed since the last snapshot, e.g. after changing a game menu option.");
    ImGui::TextDisabled("%s", g_frames < kCensusFrame ? "Census after 10 s" : g_status.c_str());
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextUnformatted(DPRT_PRODUCT " " DPRT_VERSION);
    if (!g_manager) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Waiting for the game's console variables...");
        return;
    }
    ImGui::SeparatorText("Presets");
    for (size_t i = 0; i < sizeof(kPresets) / sizeof(kPresets[0]); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(kPresets[i].name)) ApplyPreset(kPresets[i]);
        ImGui::SetItemTooltip("%s", kPresets[i].help);
    }
    ImGui::TextDisabled("Presets change reflections, translucency and AO; shadows and experiments stay as set below.");

    DrawGroup("Reflections", true);
    DrawGroup("Translucency", true);
    DrawGroup("Ambient occlusion", true);
    DrawGroup("Shadows", true);
    DrawGroup("Experimental", false);
    DrawDiagnostics();
    ImGui::Separator();
    ImGui::TextDisabled("Overrides are saved in ReShade.ini [" DPRT_PRODUCT "] and applied at start.");
}

void Install() {
    OpenLog();
    LogGameInfo();
    DefineSettings();
    LoadSettings();
    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    LoadReplacements();
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
    reshade::register_overlay(DPRT_PRODUCT, &DrawOverlay);
}

void Uninstall() {
    reshade::unregister_overlay(DPRT_PRODUCT, &DrawOverlay);
    RemoveStateObjectHook();  // the vtable would otherwise point into the unloaded add-on
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
