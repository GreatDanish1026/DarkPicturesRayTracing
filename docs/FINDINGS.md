# Findings

## The Devil in Me (SMG028), PE timestamp 0x64bfa2b4

### Ray tracing is cooked in
- Engine: Supermassive's UE 4.26 branch (`++UE4+Release-4.26`, source path
  `D:/Dev/Tech/dp_sub/UE4`). The game runs D3D12 with `-dx12`; the project default is
  `DefaultGraphicsRHI_DX11`.
- `SMG028/Config/DefaultEngine.ini` (pakchunk0, pak v11, index not encrypted) ships
  `r.RayTracing=True` with every effect except reflections off:
  `r.RayTracing.Shadows=0`, `.SkyLight=0`, `.GlobalIllumination=0`, `.Translucency=0`,
  `r.RayTracing.Reflections.ExperimentalDeferred=1`, `.ScreenPercentage=25`,
  `r.Reflections.Denoiser=0`. Skin cache is on (`r.SkinCache.Mode=1`), so skinned
  characters can enter the BVH.
- Because `r.RayTracing` was on at cook time, materials carry their ray tracing hit
  shaders, and the global shader map holds every effect's ray generation shaders. Turning
  the other effects on should need no new shaders. Not yet verified in game.
- The pak has no shared shader code library (no `ShaderArchive-*.ushaderbytecode`), so
  shaders are inline in the material packages.
- The same ini block (`r.RayTracing=True`, the same effect defaults) ships in Man of Medan,
  Little Hope and House of Ashes (House of Ashes: reflections at `ScreenPercentage=50`).
- The menu option is `RTReflectionsQuality` (Off/Low/Medium/High) in
  `GameUserSettings.ini`, set by `UGFActionToggleRTReflections` / `SetRTReflectionsQuality`.
- Supermassive's own GI: `r.DynamicGIType` (0 off, 1 VPLGI rasterised, 2 VPLGI ray traced,
  3 VSGL, 4 VSGL ray traced, 5 surfels and probes), with `r.VPLGI.*`, `r.VSGL.Supported`
  and `r.SurfelsAndProbes.Supported`.
- About 160 `r.RayTracing*` strings are in the exe; 75 of them for the six effects and their
  denoisers.

### The exe is SteamStub-protected
`.text` has entropy 8.00 on disk and the entry point is in the `.bind` section, so
code can only be read from memory after the stub runs. The phase 0 probe dumps the
decrypted `.text` (`DarkPicturesRT_text_<timestamp>.bin`, read by `tools/disasm.py`).

### Console variable access (from QuarryDLSS, verified in this game)
- `IConsoleManager::Singleton` is found from `mov rcx,[rip+x]` before
  `lea rdx,[rip+L"r.ScreenPercentage"]`.
- `IConsoleManager` +0xb0 is `FindConsoleVariable(name, bTrackFrequentCalls)`.
- `IConsoleVariable` +0x58 is `AsVariableInt`, and +0x60 is `AsVariableFloat`. Each returns
  `TConsoleVariableData<T>*`, with [0] the game-thread copy and [1] the render-thread copy.
  Writing both copies every frame works (QuarryDLSS forces `r.ScreenPercentage` this way).
- `FAutoConsoleVariableRef` cvars have no data object; see the phase 0 results.

## Phase 0 results (probe 0.0.1, 2026-10-17)
`IConsoleManager::Singleton` at exe+0x7418be8. The probe found 223 matching cvars:
112 int data, 29 float data, 1 string data, and 81 Ref cvars (59 int, 17 float, 5 bool).
Each kind has its own vtable:

| vtable | kind | count |
|---|---|---|
| exe+0x5c1c5d0 | `FConsoleVariable<int32>` (data) | 112 |
| exe+0x5c1cae0 | `FConsoleVariable<float>` (data) | 29 |
| exe+0x5c1ca28 | `FConsoleVariable<FString>` (data) | 1 |
| exe+0x5c1cb98 | `FConsoleVariableRef<int32>` | 59 |
| exe+0x5c1c740 | `FConsoleVariableRef<float>` | 17 |
| exe+0x5c1cc50 | `FConsoleVariableRef<bool>` | 5 |

### Object layout (disassembled from the `.text` dump)
- `+0x18` flags. The top byte is the SetBy priority of the last change.
- Data cvars: `+0x48` holds `TConsoleVariableData` inline: [0] game thread at +0x48,
  [1] render thread at +0x4c.
- Ref cvars: `+0x48` is a pointer to the referenced global (what the renderer reads),
  and `+0x50` is `MainValue`.

### IConsoleVariable vtable (shared by all six kinds)
| slot | function | evidence |
|---|---|---|
| +0x58 / +0x60 / +0x68 | `AsVariableInt` / `Float` / `String` | non-null only in the matching data vtable |
| +0x80 | `Set(const TCHAR*, EConsoleVariableFlags)` | int Ref: `CanChange`, `FromString` into +0x50, `OnCVarChange(*[+0x48], [+0x50], flags, SetBy)`, base `OnChanged` |
| +0x88 | `GetBool` | Ref int: `cmp [rcx+0x50],0; setne al` |
| +0x90 | `GetInt` | Ref int: `mov eax,[rcx+0x50]`; data int: `[rbx+rdx*4+0x48]`, index = is render thread |
| +0x98 | `GetFloat` | Ref float: `movss xmm0,[rcx+0x50]` |
| +0xa0 | `GetString` | called by `CanChange` for its warning message |

- `CanChange` (exe+0x256c980) allows a change only if `SetBy & 0xff000000` is at least
  `flags & 0xff000000`.
- `OnCVarChange` (exe+0x25797c0) copies the main value to the copy the renderer reads
  **only when called on the game thread**: through the manager's thread propagation
  callback for `ECVF_RenderThreadSafe` (0x20) cvars, or directly otherwise. On any other
  thread it skips the copy (`je 0x257983e` past both paths) and only sets the manager's
  "call sinks" flag (+0xc8). (An earlier reading of this function had it writing straight
  through. The first phase 1 test disproved that: `r.RayTracing.Shadows` showed game 1 /
  render 0 after `Set`.)
- **How phase 1 writes:** from ReShade's present callback, call
  `Set(L"<value>", ECVF_SetByConsole = 0x09000000)` through +0x80, then copy the main value
  to the renderer's copy (`cvars::MirrorToRenderer`): data +0x48 → +0x4c, Ref
  +0x50 → `*(+0x48)`, 1 byte for `FConsoleVariableRef<bool>`. `Set` provides the priority
  and the change delegates; the copy is the same direct write QuarryDLSS uses. The game
  writes its menu values at priority 0x02 (GameSetting) and ini defaults at 0x03
  (ProjectSetting), so it cannot overwrite ours. To hand a cvar back to the game, the
  add-on restores the priority byte at +0x18, sets the original value at that priority,
  and copies it again.
- Data cvars' `GetInt` reads `[IsInGameThread() ? 0 : 1]`, so from the present thread it
  returns the render copy.

### What the game sets (snapshot run, RT reflections High)
- The menu's RT reflections toggle changes exactly one cvar: `r.RayTracing.Reflections`
  (int Ref, global exe+0x6f99e94), 1 → 0 at priority 0x02.
- **`r.RayTracing.AmbientOcclusion` is already 1** (priority 0x02, set by the game's
  settings, global exe+0x6f99e0c), so RT AO is on in this configuration. Which menu
  option drives it is not yet known; the reflections toggle doesn't.
- Off: `r.RayTracing.Shadows` 0, `.SkyLight` 0 (priority 0x03, from the ini),
  `.GlobalIllumination` 0, `.Translucency` 0. `r.RayTracing.ForceAllRayTracingEffects` -1.
- Denoisers: `r.Shadow.Denoiser` 2, `r.AmbientOcclusion.Denoiser` 2, GI and sky light
  denoisers 1, `r.Reflections.Denoiser` 0 (the deferred reflections path does its own
  spatial resolve).
- The per-effect `SamplesPerPixel` / `MaxBounces` / `MaxRoughness` values are -1, which
  means "use the post-process volume". `r.RayTracing.Reflections.ScreenPercentage` 25,
  `r.RayTracing.GlobalIllumination.ScreenPercentage` 50.
- Supermassive's GI: `r.DynamicGIType` 0, and `r.VPLGI.Supported` / `r.VSGL.Supported` 0. Those
  are probably startup-only, so the ray-traced VPLGI/VSGL modes are likely out of reach
  at run time. Not tested.

## Phase 1 tests
- 0.1.0 (2026-10-17): the shadows toggle set the main value (game 1, priority 0x09), but
  the render copy stayed 0, so ray-traced shadows never turned on. See `OnCVarChange` above.
  Fixed in 0.1.1 with `MirrorToRenderer`.
- 0.1.1 (2026-10-17): the write reaches the renderer (`main 1 / renderer 1, priority 0x09`;
  releasing goes back to `0 / 0, 0x03`), and the image changes on the frame it flips.
  But in the paused mannequin scene (Act 1 stage, "Observe"), the key spotlight is shadowed
  almost everywhere: the face, shirt, waistcoat, hand and the lit curtain all go dark,
  while areas already in shadow (under the hat brim) don't change (mean luma 22.6 → 21.2;
  5.4% of pixels differ by more than 24). Some geometry the shadow maps leave out is
  blocking the light. Suspects: a light fixture or housing mesh around the spot (shadow
  maps skip it, but rays reach it), hidden or non-shadow-casting meshes that are still
  in the ray tracing scene, back faces (`EnableTwoSidedGeometry` 1), or skinned meshes
  in the BVH. Found in the census: `r.RayTracing.Shadows.Lights.{Spot,Point,Rect,Directional}` are
  all 1, `.EnableMaterials` is 1 (Ref, exe+0x6f99e78), and `r.Shadow.Denoiser` is 2.
- 0.1.2 adds toggles to find the blocker by elimination: each light type back on shadow
  maps, single-sided geometry, masked materials as opaque, and skeletal / static / Niagara
  geometry out of the ray tracing scene.
- 0.1.2 (2026-10-17), same paused scene, RT shadows on: **putting spot lights back on
  shadow maps (`r.RayTracing.Shadows.Lights.Spot 0`) fixes the dark key light.** Point,
  Rect and Directional made no difference, possibly because this shot has none of those
  lights. Single-sided geometry, tried with spot lights still on RT, didn't fix it. Most
  likely cause: the stage spotlight sits inside its own fixture mesh, which shadow maps
  skip near the light but rays don't. Not yet tested: whether "No static meshes"
  (fixture) or "No skeletal meshes" (characters) fixes it with spot lights on RT, and
  whether point lights in lamps show the same thing elsewhere. A usable setting today:
  RT shadows with spot lights on shadow maps.
- 0.1.2 fixture test (2026-10-17), RT shadows on with spot lights on RT:
  **`r.RayTracing.Geometry.StaticMeshes 0` brings the key light back**; `SkeletalMeshes 0`
  doesn't (mean luma: shadow maps 22.6, RT 21.3, RT without static meshes 22.4, RT
  without skeletal meshes 21.3). So the blocker is a static mesh. Dropping all static
  meshes isn't a fix: the hat (a static mesh on the mannequin) stops shadowing the eyes
  too.
  Remaining suspects: (a) the spotlight's own fixture or lens mesh around the light;
  (b) a mesh moved by world position offset. Ray tracing evaluates WPO only for
  components with `EvaluateWPO` enabled and within `r.RayTracing.Geometry.StaticMeshes.WPO.CullingRadius`
  (50 m); every other WPO mesh is traced in its rest pose, so a curtain or blocker
  animated out of the way in the material would still block the light; (c) an instanced
  static mesh. `...StaticMeshes.WPO 0` hides WPO meshes that have `EvaluateWPO` on, and
  `r.RayTracing.Geometry.InstancedStaticMeshes 0` drops instances.
- 0.1.3 (2026-10-17), same scene, RT shadows on with spot lights on RT: `StaticMeshes.WPO 0`,
  `InstancedStaticMeshes 0` and `NiagaraSprites 0` make no difference. Ruled out: instanced
  meshes, WPO meshes that opt into ray tracing WPO, Niagara sprites, back faces, skeletal
  meshes. Left: a plain static mesh that shadow maps ignore but rays hit. Most likely
  the spotlight's own fixture or lens mesh around the light, or a WPO mesh traced in its
  rest pose.

## Next: stop shadow rays short of the light
No cvar biases the light end of a shadow ray (`r.RayTracing.NormalBias` only moves the
start). The fix is in the shader. The ray generation shader is UE's `FOcclusionRGS`
(`/Engine/Private/RayTracing/RayTracingOcclusionRGS.usf`), so the plan is to stop each
ray a few centimetres short of its sample point on the light (TMax minus a bias). That
skips geometry right around the light, as shadow maps do in effect, and would also fix
bulbs inside lamp meshes. ReShade 6.8 exposes D3D12 ray tracing pipelines (`create_pipeline`
with `libraries` / `raygen_shader` subobjects, plus a `dispatch_rays` event), so this can
use the same in-game replacement as AWTweaks:
1. Probe: log every ray tracing pipeline (DXIL library sizes, hashes, export names) and
   dump the libraries; find the one that exports `OcclusionRGS` and its permutations.
2. Patch the DXIL offline (disassemble, change TMax at the `TraceRay` call, reassemble and
   validate), and swap it in at `create_pipeline`.
3. Expose the bias in the overlay, and test this scene with spot lights on RT.
Meanwhile, RT shadows with spot lights on shadow maps works as it is.

## Ray tracing pipelines (probe 0.2.0, 2026-10-17)
- UE 4.26 builds **one collection per shader**: a state object holding one DXIL library
  (one export), payload 24, attributes 8, recursion 1. The probe saw 220 of them. The
  final pipeline that links them and dispatches **never reaches add-ons**: the
  dispatched handles aren't any pipeline seen in `init_pipeline`. To replace a shader,
  swap its code inside its collection at `create_pipeline`; the linked pipeline then
  picks it up.
- Raygen exports seen: `OcclusionRGS` (**96 collections, each with its own library**),
  `AmbientOcclusionRGS` 4, `RayTracingDeferredReflectionsRGS` 3, `RayTracingReflectionsRGS` 1,
  `GlobalIlluminationRGS` 2, `RayTracingCreateGatherPointsTraceRGS` 4. The `OcclusionRGS`
  collections are created the moment RT shadows are first turned on.
- One frame of dispatches, RT shadows off: one pipeline, 1 dispatch at 2564x1068 (render
  resolution with DLSS Quality). With shadows on, a second pipeline adds 3 dispatches at
  2564x1068, which looks like one dispatch per shadowed light.
- `OcclusionRGS` (smallest permutation, baf478b2ce1f3f1d): two `TraceRay` calls, TMin 0,
  instance mask 12. TMax is `sqrt(dot(d, d))`, where d is the light sample point (light
  position from cbuffer register 7 plus a jittered offset across the light's shape)
  minus the surface point. **Nothing biases the light end**, so anything wrapped around
  the light (a fixture) blocks the ray. That matches the 0.1.x tests.
- Across the first 80 permutations checked, 2, 4, 5 or 7 `TraceRay` calls take TMax from: `Sqrt`
  (distance to the light), `phi` of such values (branches), constants or `FMin` (opcode
  36; directional / capped distance), and `5.0 - x` (short extra rays, probably hair or
  transmission).
- The Windows SDK's `dxcompiler.dll` has `IDxcAssembler::AssembleToContainer` ("Assemble
  DXIL in LL or LLVM bitcode to DXIL container"), and `dxil.dll` validates and signs.
  So the plan: disassemble (`dxc -dumpbin`), edit the IR, reassemble, sign.

### Patch rule (planned)
For each `TraceRay` whose TMax comes from a `Sqrt` (directly or through a `phi`), replace
TMax with `max(TMin + epsilon, TMax - Bias)` (FMax, opcode 35), with Bias in cm (UE units).
Leave constants, `FMin` and `5.0 - x` TMax alone. Steps:
1. A round-trip test on all unmodified libraries: dumpbin → assemble → sign →
   dumpbin, with matching IR. If they don't round-trip, the approach is no good.
2. Patched sets at a few biases (for example 10, 25, 50 cm), and the add-on swapping them
   in at `create_pipeline` by library hash. Changing the bias needs a restart, because the
   collections are created once.

### Round trip (tools/dxilasm.exe, tools/roundtrip.py; 2026-10-17)
- `dxilasm` assembles `dxc -dumpbin` output with the SDK's `IDxcAssembler` (10.0.26100.0)
  and signs it with `IDxcValidator` (`DxcValidatorFlags_InPlaceEdit`).
- **All 96 `OcclusionRGS` libraries pass:** the reassembled IR matches the original line
  for line (comments dropped), and `RDAT` and `SFI0` are byte-identical. In every case
  the bitcode re-encodes slightly smaller, the debug name part `ILDN` is dropped, and a
  statistics part `STAT` (about 4 KB) is added. D3D12 doesn't read either part.
- Not yet shown: that D3D12 accepts a reassembled library in the game. The first in-game
  test can swap in the unmodified round-tripped set, then the patched one.


### Shader replacement
- 0.2.1 swapped the code in ReShade's `create_pipeline`. All the `OcclusionRGS` collections
  built with the round-tripped libraries were **created** (`init_pipeline` saw them), but
  then the game hit a fatal error: `RayTracingDevice->CreateStateObject(...) failed ...
  D3D12RayTracing.cpp:359 with error E_INVALIDARG`. That's the call that links the
  collections into the final pipeline, which add-ons never see. Cause (inferred): when an
  add-on edits a description, ReShade rebuilds the state object from its generic
  subobject model (in the log, the collections carry subobject types ReShade doesn't
  model as shaders: `flags` (33) and an empty `dynamic_pipeline_states` (18)). That loses
  details the collections need for linking. The reassembled libraries themselves were
  accepted.
- 0.2.2–0.2.5 hooked the wrong vtable slot. I took slot 66 from counting the C
  `ID3D12Device5Vtbl` struct in d3d12.h, but MSVC's C++ vtable orders the methods
  differently: `CreateStateObject` is called at offset 0x1f0, **slot 62**, and slot 66 is
  `ID3D12Device7::AddToStateObject` (62 CreateStateObject, 63
  GetRaytracingAccelerationStructurePrebuildInfo, 64 CheckDriverMatchingIdentifier,
  65 SetBackgroundProcessingMode, 66 AddToStateObject). That accounts for every symptom
  I had explained some other way:
  - 0.2.2's boot crash (`AddToStateObject ... 8000FFFF`): a 4-argument hook passed `riid`
    where `AddToStateObject` expects the state object to grow from. It wasn't "hidden
    runtime arguments"; 0.2.3's forwarding of extra slots only happened to restore the
    real call.
  - The hook saw only type-3 descriptions made of existing collections: those are the
    game's `AddToStateObject` calls. Collection creation goes through slot 62, which was
    never hooked. It wasn't a separate vtable or a pointer cached by ReShade. 0.2.4
    showed that every interface version shares one vtable in D3D12Core.dll.
  - The 0.2.5 inline detour sat on `AddToStateObject`'s function, so the probe's
    `CreateStateObject` call never reached it.
  Found by disassembling `tools\d3d12probe` (capstone): its three `CreateStateObject`
  calls are `call [rax+0x1f0]`.
- 0.2.6: a plain vtable hook on slot 62 with the real 4-argument signature. It swaps
  `D3D12_DXIL_LIBRARY_DESC::DXILLibrary` in the game's own description for the length of
  the call, then restores it. `tools\d3d12probe --hook-test` patches the slot, calls
  `CreateStateObject` through the C++ interface, and checks that the hook ran exactly once
  with an unchanged result: **PASS** on D3D12Core 6.2.26100.9549 (slot 62 → rva 0xcef90).
  _Pending in-game run with the 96 round-tripped libraries._
