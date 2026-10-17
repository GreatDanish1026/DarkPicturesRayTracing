# DarkPicturesRT

A ReShade add-on that tunes Unreal Engine 4.26 ray tracing in The Dark Pictures
Anthology (DirectX 12). Every game in the series ships with `r.RayTracing=True`,
so the engine's ray-traced reflections, ambient occlusion, shadows and more are
compiled in. The game's menu only offers ray-traced reflections. This add-on
exposes the rest through the engine's console variables, live, from a ReShade
overlay tab.

**Status: 0.5.3.** Tested in The Devil in Me, House of Ashes, Little Hope and Man of Medan. See [docs/FINDINGS.md](docs/FINDINGS.md)
for how it works and what was found along the way.

## Install
Needs ReShade 6.8+ *with full add-on support*. Start the game with `-dx12`.
Copy `bin\DarkPicturesRT.addon64` next to the game's `*-Win64-Shipping.exe`,
or run `.\build.ps1 -Install`.

## Use
Open ReShade (Home), go to the **DarkPicturesRT** tab. The tab opens in a simple view:

- **Quality**: presets for reflections and AO. The button matching the current settings
  is highlighted ("custom" if none does).
  - *Game default*: the game's own settings.
  - *Sharper reflections*: twice the game's reflection resolution (The Devil in Me 25% → 50%,
    House of Ashes 50% → 100%).
  - *High*: reflections at full resolution with less history (0.9, game 0.95) to cut smearing.
  - *Ultra*: High with 16 reflection resolve samples (game 8), history 0.85, and AO at 2 samples.
- **Ray traced shadows**: on for the game's lights, with spot lights kept on shadow maps
  (the game's key lights are spot lights; under RT some props, such as hats, lose
  their shadows from them).
- **Ray traced translucency** (manual, not in presets): proper glass; fixes display cases
  The Devil in Me renders blown out. It replaces all translucent rendering, so it breaks
  haze, fog and dust effects: seen in House of Ashes, Little Hope and Man of Medan, and
  likely in foggy scenes of The Devil in Me. Turn it on only where glass looks wrong.
- **Global illumination**: Off, or final gather (ray traced bounce light, with firefly
  suppression and filter width 2, which make it clean).
- **Ray traced sky light**: for outdoor scenes (noisy indoors).
- A count of changed settings, with **Reset all** (asks for a second click).

**Show advanced settings** opens every setting individually (reflections, translucency,
AO, shadows, GI, and experimental sky light options), with sliders that stay locked
until you tick their override checkbox. Inside it, **Show diagnostics** opens the
troubleshooting tools.

All settings are saved in `ReShade.ini`, section `[DarkPicturesRT]`, and applied at
start. Changed settings outrank the game's menu, so the menu can't change them until
you set them back to "Game" (or press Reset all).

### RT shadow shader fix (optional)
With RT shadows on for spot lights, a light fixture around the light can block its shadow rays.
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
