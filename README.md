# DarkPicturesRT

A ReShade add-on that turns on and tunes Unreal Engine 4.26 ray tracing in
The Dark Pictures Anthology (DirectX 12). Every game in the series ships with
`r.RayTracing=True`, so the engine's ray-traced shadows, ambient occlusion,
global illumination, sky light and translucency are compiled in. The game's
menu only offers ray-traced reflections.

**Status: test build (0.2.0).** The add-on logs the game's ray tracing console
variables and ray tracing pipelines, and dumps the pipelines' DXIL libraries
(`tools/dxil.py` finds and disassembles them). Its ReShade tab has test toggles
for ray-traced shadows (`r.RayTracing.Shadows`) and for finding what blocks them.
See [docs/FINDINGS.md](docs/FINDINGS.md).

## Install
Needs ReShade 6.8+ *with full add-on support*. Start the game with `-dx12`.
Copy `bin\DarkPicturesRT.addon64` next to the game's `*-Win64-Shipping.exe`,
or run `.\build.ps1 -Install`.

## Build
Visual Studio 2022+ with the C++ x64 tools: `.\build.ps1`.
