![Prism Logo](./assets/logo/PrismRT_logo_white.png#gh-dark-mode-only)
![Prism Logo](./assets/logo/PrismRT_logo_black.png#gh-light-mode-only)

👋 Introduction
---------------------------------------

PrismRT is a path tracer for OpenUSD, glTF and OBJ files with MaterialX materials (through NVIDIA MDL). It renders on the CPU (Embree, oneTBB) or on the GPU with hardware ray tracing (Metal on macOS, Vulkan on Windows/Linux), all running the same path tracer.

The optional PrismRT Hydra plugin renders in stock OpenUSD 26.08 usdview. Both frontends use the same render devices and MaterialX-to-MDL shading backend. See [Hydra usage](docs/hydra.md), [material support](docs/hydra_materials.md), and [Windows deployment](docs/deployment.md).

See [texture cache configuration](docs/texture_service.md) and the
[validation report](docs/round3_validation.md) for measured performance,
standalone/Hydra image comparisons, and remaining acceptance work.


✅ Feature List
---------------------------------------

* Render devices: CPU (Intel Embree, oneTBB) and GPU with hardware ray tracing (Metal, Vulkan), running one shared path tracer (see Render Devices below).
* MaterialX materials compiled through NVIDIA MDL, on the CPU (native code) and on the GPU (HLSL).
* 3D Model Mesh importer:
    * OpenUSD (.usd, .usdz, .usda, .usdc) files
    * glTF (.gltf, .glb) files
    * .obj files
* Analytical Geometry:
    * Spheres
    * Quads
* OpenImageIO image loading and saving.

🖥️ Render Devices
---------------------------------------

Every device runs the same path tracer, [`shaders/integrator/path_tracer.h`](shaders/integrator/path_tracer.h): a C++ template that also compiles as HLSL, against a small per-device backend.

| Device | Platforms | Ray tracing | Shading |
| --- | --- | --- | --- |
| `metal` | macOS on Apple GPUs (M1 and newer; hardware ray tracing from M3) | Metal acceleration structures, inline ray queries | HLSL kernel: DXC to DXIL, Metal shader converter; MDL materials in HLSL |
| `vulkan` | Windows and Linux with `VK_KHR_ray_query` (NVIDIA RTX, AMD RX 6000+, Intel Arc) | Vulkan acceleration structures, inline ray queries | The same HLSL kernel, compiled to SPIR-V by DXC. Builds and compiles on macOS; not yet run-tested |
| `cpu` | All | Embree | The path tracer as C++, the CPU materials and native MDL code |

The default `auto` picks Metal on macOS and Vulkan elsewhere, and falls back to `cpu` when no GPU device is usable. Choose a device with:

* the standalone renderer: `--device auto|cpu|vulkan|metal` (`PrismRT --list-devices` lists the available ones);
* Hydra/usdview: the `PRISM_DEVICE` environment variable or the `prism:device` render setting.

The GPU devices compile a kernel per scene material set the first time it is seen (about 30 s for the OpenChessSet scene) and cache it; later runs start in a few seconds. Primary AOVs and picking are traced on the CPU scene in every device.

Mac GPU vs CPU on an Apple M5 (10 CPU threads, 8 GPU cores), warm shader caches. "Noise floor" is the CPU against itself with another seed: GPU differences below it are sampling noise.

| Scene | Size, spp | CPU | Metal | Speed-up | Metal vs CPU NRMSE | Noise floor NRMSE |
| --- | --- | --- | --- | --- | --- | --- |
| OpenChessSet, standalone | 1280x720, 256 | 221.3 s | 21.4 s | 10.4x | 0.068 | 0.109 |
| OpenChessSet, Hydra | 1280x720, 256 | 221.9 s | 16.9 s | 13.2x | 0.068 | — |
| CornellBox | 640x360, 128 | 28.6 s | 1.5 s | 18.8x | 0.036 | 0.118 |
| QuadTest | 640x360, 128 | 11.0 s | 1.1 s | 10.4x | 0.0001 | 0.017 |
| InOneWeekend (analytic spheres) | 640x360, 128 | 31.1 s | 11.3 s | 2.7x | 0.015 | 0.042 |
| Environment-map fixture | 256x256, 256 | 9.2 s | 3.1 s | 3.0x | 0.00008 | 0.010 |

Mean luminance differs by at most 0.05 %. Details, images and diagnostics: [docs/gpu_devices.md](docs/gpu_devices.md).


⚠️ System Requirements
---------------------------------------

* Windows x64 with Visual Studio 2022, or macOS on Apple silicon with Xcode (or the Command Line Tools); a C++20 compiler.
* One stock OpenUSD 26.08 installation, with Python, imaging and GL enabled for usdview. `tools/deps.json` lists every dependency version; `tools/build_deps_local.ps1` builds OpenUSD, MaterialX and OpenSubdiv like CI does (see [deployment](docs/deployment.md#dependency-versions)).
* Embree 4, oneTBB, OpenImageIO, MaterialX and the NVIDIA MDL SDK.
* Matching development packages for dependencies required by the selected USD build, including OpenSubdiv.
* GPU devices: DXC (in the Vulkan SDK); the Metal device also needs Apple's Metal shader converter, the Vulkan device the Vulkan SDK.


🛠️ Build Instructions (WIP)
---------------------------------------

1. Clone this repository recursively to fetch the importer dependencies.
2. Build or select the dependency installations. PrismRT does not bundle SDKs or patch USD.
3. Copy `cmake/PrismLocalDependencies.cmake.example` to `cmake/PrismLocalDependencies.cmake` and fill in package roots, or use standard `CMAKE_PREFIX_PATH` discovery.
4. Run `cmake --preset prism-hydra`, then `cmake --build --preset prism-hydra`.
5. Launch `powershell -ExecutionPolicy Bypass -File run_usdview.ps1 path/to/scene.usda` and select **PrismRT**.

For standalone rendering, initialize the same environment with `. ./prism_env.ps1`, then run:

```powershell
./build/RelWithDebInfo/PrismRT.exe --scene assets/scenes/InOneWeekend.usda --camera /World/Camera --spp 256 --output render.exr
# or every RenderProduct of the scene's RenderSettings, with its RenderVars as EXR layers:
./build/RelWithDebInfo/PrismRT.exe --scene scene.usda --output-dir renders
```

On macOS, use the `prism-hydra-mac` preset (`cmake --preset prism-hydra-mac`, `cmake --build --preset prism-hydra-mac`), `. ./prism_env.sh` for the environment, and `./run_usdview.sh scene.usda` for usdview. Pick a device for usdview with `PRISM_DEVICE`:

```bash
PRISM_DEVICE=cpu ./run_usdview.sh scene.usda --renderer PrismRT
PRISM_DEVICE=metal ./run_usdview.sh scene.usda --renderer PrismRT
```

No arguments display CLI help. USD, glTF/GLB and OBJ scene files are supported; rendering requires an explicit camera source. EXRs retain linear HDR values after the configurable sample clamp. Use `--max-sample-radiance 0` for energy/variance measurements. Local asset launchers belong in ignored `local/`; a template is provided under `tools/templates/`.


🐛 Known Issues & Limitations
---------------------------------------

* Curves, VDB volumes, motion blur, light shaping/IES and an ACEScg rendering space are not implemented; see [the feature roadmap](docs/feature_roadmap.md).
* Every material is MaterialX through MDL, apart from the built-in fallback (`displayColor`) and the magenta error material. glTF materials load as MaterialX `gltf_pbr` ([docs/gltf.md](docs/gltf.md)) and OBJ/MTL materials as `open_pbr_surface` ([docs/obj.md](docs/obj.md)); Hydra/usdview cannot open glTF or OBJ files.
* The setPosition method of AABBs doesn't set correctly the position, if it's not initially in zero.


⚖️ Licensing
---------------------------------------

This project is licensed under the Apache License, Version 2.0. Please see the file [LICENSE](./LICENSE) for the full license text and [NOTICE](./NOTICE) for the copyright notice.

