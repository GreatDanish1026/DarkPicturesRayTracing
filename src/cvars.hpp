// Finds Unreal console variables in the running game and returns their data
// (TConsoleVariableData<T>: two copies of the value, [0] game thread, [1] render
// thread), so the probe can force them every frame.
// Copied from QuarryDLSS (src/cvars.hpp), where it runs in The Devil in Me too.
//
// IConsoleManager::Singleton is found from the game's own code: every cvar
// lookup loads it with `mov rcx, [rip+Singleton]` shortly before
// `lea rdx, [rip+L"r.ScreenPercentage"]`. Vtable offsets are UE 4.26's, as seen
// in The Quarry (build 11105655):
//   IConsoleManager +0xb0  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//   IConsoleVariable +0x58 AsVariableInt()    -> TConsoleVariableData<int32>*
//   IConsoleVariable +0x60 AsVariableFloat()  -> TConsoleVariableData<float>*
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0xb0;
constexpr uint32_t kAsVariableInt = 0x58;
constexpr uint32_t kAsVariableFloat = 0x60;
// Confirmed in The Devil in Me (docs/FINDINGS.md, phase 0), for data and Ref cvars alike.
constexpr uint32_t kSet = 0x80;  // Set(const TCHAR* Value, EConsoleVariableFlags SetBy)
constexpr uint32_t kGetInt = 0x90;
constexpr uint32_t kGetFloat = 0x98;
constexpr uint32_t kFlags = 0x18;  // EConsoleVariableFlags; top byte = SetBy priority
constexpr uint32_t kSetByConsole = 0x09000000;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

// IConsoleManager::Singleton (the variable holding the pointer), or null.
inline void** FindConsoleManagerSingleton(const uint8_t* base) {
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return nullptr;
    const uint8_t* name = FindWideString(rdata, L"r.ScreenPercentage");
    if (!name) return nullptr;
    for (size_t i = 0x40; i + 7 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea r64, [rip+disp32] -> the name
        if ((p[0] != 0x48 && p[0] != 0x4c) || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        if (p + 7 + disp != name) continue;
        // mov rcx, [rip+disp32] within the 0x40 bytes before it
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            return reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp));
        }
    }
    return nullptr;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using AsDataFn = void*(__fastcall*)(void* self);

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// The data of console variable <name>, or null. Guarded: a wrong vtable layout
// faults here instead of taking the game down.
inline void* FindData(void* manager, const wchar_t* name, bool isFloat) {
    __try {
        void* var = Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, true);
        if (!var) return nullptr;
        return Virtual<AsDataFn>(var, isFloat ? kAsVariableFloat : kAsVariableInt)(var);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The IConsoleVariable object for <name> (null if absent or a command), and its
// data through AsVariableInt / AsVariableFloat. Both return null for variables
// without TConsoleVariableData (FAutoConsoleVariableRef, bool, string), which is
// how the probe tells the two kinds apart. Guarded like FindData.
inline void* FindVariable(void* manager, const wchar_t* name) {
    __try {
        return Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline void* AsData(void* var, bool isFloat) {
    __try {
        return Virtual<AsDataFn>(var, isFloat ? kAsVariableFloat : kAsVariableInt)(var);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// memcpy that reports a fault instead of raising it.
inline bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using SetFn = void(__fastcall*)(void* self, const wchar_t* value, uint32_t setBy);
using GetIntFn = int32_t(__fastcall*)(void* self);

// IConsoleVariable::Set: refuses a SetBy priority lower than the last one (CanChange),
// stores the main value (data [0] / Ref +0x50), records the priority and runs the change
// delegates. Off the game thread OnCVarChange skips the copy the renderer reads, so
// follow it with MirrorToRenderer. False if it faulted.
inline bool Set(void* var, const wchar_t* value, uint32_t setBy) {
    __try {
        Virtual<SetFn>(var, kSet)(var, value, setBy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool GetInt(void* var, int32_t& out) {
    __try {
        out = Virtual<GetIntFn>(var, kGetInt)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The flags word; the top byte is the priority of the last Set.
inline uint32_t& Flags(void* var) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(var) + kFlags); }

// Where the renderer reads the value, and where Set keeps the main value:
//   data cvar  render copy +0x4c, main (game thread) +0x48
//   Ref cvar   the global at *(+0x48), main +0x50
// FConsoleVariableRef<bool> holds 1-byte values; it is the only kind whose GetBool and
// GetInt slots are the same function.
struct Storage {
    uint8_t* render = nullptr;
    const uint8_t* main = nullptr;
    size_t size = 0;
};

inline bool GetStorage(void* var, Storage& out) {
    auto obj = static_cast<uint8_t*>(var);
    if (AsData(var, false) || AsData(var, true)) {
        out = {obj + 0x4c, obj + 0x48, 4};
        return true;
    }
    uint8_t* ref = nullptr;
    void* slots[2] = {};
    if (!SafeCopy(&ref, obj + 0x48, 8) || !ref) return false;
    if (!SafeCopy(slots, *reinterpret_cast<uint8_t**>(var) + 0x88, sizeof(slots))) return false;
    out = {ref, obj + 0x50, slots[0] == slots[1] ? size_t(1) : size_t(4)};
    return true;
}

// Copies the main value to where the renderer reads it (what OnCVarChange does on the
// game thread). A 4-byte aligned store, so the render thread sees old or new, not a mix.
inline bool MirrorToRenderer(void* var) {
    Storage s;
    if (!GetStorage(var, s)) return false;
    uint8_t value[4];
    return SafeCopy(value, s.main, s.size) && SafeCopy(s.render, value, s.size);
}

// Whether the cvar holds a float. Data cvars say so through AsVariableFloat; Ref cvars
// don't, so this looks at their GetFloat (+0x98): a float Ref's is movss xmm0,[rcx+0x50]
// (F3 0F 10), an int Ref's converts (movd + cvtdq2ps), a bool Ref's uses movzx.
inline bool IsFloatVar(void* var) {
    if (AsData(var, true)) return true;
    if (AsData(var, false)) return false;
    const uint8_t* fn = nullptr;
    uint8_t code[3] = {};
    return SafeCopy(&fn, *reinterpret_cast<uint8_t**>(var) + kGetFloat, sizeof(fn)) && SafeCopy(code, fn, 3) &&
           code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x10;
}

// The value the renderer reads, as an int (bool and int cvars).
inline bool RenderInt(void* var, int32_t& out) {
    Storage s;
    if (!GetStorage(var, s)) return false;
    out = 0;
    return SafeCopy(&out, s.render, s.size);
}

}  // namespace cvars
