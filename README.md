# DarkPicturesRT

A ReShade add-on that tunes Unreal Engine 4.26 ray tracing in The Dark Pictures
Anthology (DirectX 12). Every game in the series ships with `r.RayTracing=True`,
so the engine's ray-traced reflections, ambient occlusion, shadows and more are
compiled in. The game's menu only offers ray-traced reflections. This add-on
exposes the rest through the engine's console variables, live, from a ReShade
overlay tab.

**Status: 0.3.0.** Tested in The Devil in Me. See [docs/FINDINGS.md](docs/FINDINGS.md)
for how it works and what was found along the way.

## Install
Needs ReShade 6.8+ *with full add-on support*. Start the game with `-dx12`.
Copy `bin\DarkPicturesRT.addon64` next to the game's `*-Win64-Shipping.exe`,
or run `.\build.ps1 -Install`.

## Use
Open ReShade (Home), go to the **DarkPicturesRT** tab.

- **Presets** change reflections and AO; RT shadows stay as you set them.
  - *Game default*: everything back to the game's values.
  - *Sharper reflections*: the game's reflection algorithm at 50% resolution (the game uses 25%).
  - *High*: the game's algorithm at full resolution.
  - *Ultra*: full resolution with 16 resolve samples (the game uses 8); AO at 2 samples.

  All presets keep the game's fast deferred reflection algorithm. UE's standard
  algorithm is available as a setting, but it is very noisy in this game.
- **Reflections** and **Ambient occlusion**: individual settings. A setting shows
  the game's value until you override it (combo box choice, or the checkbox next to
  a slider). Hover for help.
- **Shadows (experimental)**: ray-traced shadows. Turning them on also puts spot
  lights back on shadow maps, because the game's key lights are spot lights and
  props such as hats lose their shadows under RT (see FINDINGS).
- **Diagnostics**: test switches and logging from the investigation.

Overrides are saved in `ReShade.ini`, section `[DarkPicturesRT]`, and applied at
start. Overridden values outrank the game's menu, so the menu can't change them
until you set them back to "Game".

### RT shadow shader fix (optional)
With RT shadows on, a light fixture around a spot light can block its shadow rays.
The add-on can replace UE's shadow ray shader with one that stops rays short of the
light: put patched libraries in `DarkPicturesRT_shaders\` next to the exe. Generate
them on your machine from the add-on's dumps with `tools\patch_tmax.py` (see its
help). They're derived from the game's own shaders, so they aren't distributed here.

## Build
Visual Studio 2022+ with the C++ x64 tools and the Windows 10/11 SDK: `.\build.ps1`.
Builds the add-on plus `bin\dxilasm.exe` (DXIL assembler/signer) and
`bin\d3d12probe.exe` (checks the hooked D3D12 vtable slot).

## Tools
| Tool | What |
|---|---|
| `tools/disasm.py` | Disassemble the decrypted `.text` dump |
| `tools/dxil.py` | Find and disassemble the dumped ray tracing libraries |
| `tools/roundtrip.py` | Disassemble, reassemble and compare DXIL libraries |
| `tools/patch_tmax.py` | Shorten RT shadow rays at the light end (and `--mask`) |
| `tools/uasset.py` | Read names, imports, exports and properties of cooked packages |
