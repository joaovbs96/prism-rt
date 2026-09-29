![Prism Logo](./assets/logo/PrismRT_logo_white.png#gh-dark-mode-only)
![Prism Logo](./assets/logo/PrismRT_logo_black.png#gh-light-mode-only)

👋 Introduction
---------------------------------------

PrismRT is a CPU-based Path Tracer with support for rendering OpenUSD, glTF and OBJ files, powered by the Embree Ray Tracing framework, the oneTBB threading library and the OpenImageIO image processing library.

The optional PrismRT Hydra plugin renders in stock OpenUSD 26.05 usdview. Both frontends use the same CPU transport and MaterialX-to-MDL shading backend. See [Hydra usage](docs/hydra.md), [material support](docs/hydra_materials.md), and [Windows deployment](docs/deployment.md).

See [texture cache configuration](docs/texture_service.md) and the
[validation report](docs/round3_validation.md) for measured performance,
standalone/Hydra image comparisons, and remaining acceptance work.


✅ Feature List
---------------------------------------

* Intel Embree BVH & Ray Tracing kernels.
* 3D Model Mesh importer:
    * OpenUSD (.usd, .usdz, .usda, .usdc) files
    * glTF (.gltf, .glb) files
    * .obj files
* Analytical Geometry:
    * Spheres
    * Quads
* Parallelism with oneTBB support.
* OpenImageIO image loading and saving.


⚠️ System Requirements
---------------------------------------

* Windows x64 and a C++20 compiler (Visual Studio 2022).
* One stock OpenUSD 26.05 installation, with Python, imaging and GL enabled for usdview.
* Embree 4, oneTBB, OpenImageIO, MaterialX and the NVIDIA MDL SDK.
* Matching development packages for dependencies required by the selected USD build, including OpenSubdiv.


🛠️ Build Instructions (WIP)
---------------------------------------

1. Clone this repository recursively to fetch the importer dependencies.
2. Build or select the dependency installations. PrismRT does not bundle SDKs or patch USD.
3. Copy `cmake/PrismLocalDependencies.cmake.example` to `cmake/PrismLocalDependencies.cmake` and fill in package roots, or use standard `CMAKE_PREFIX_PATH` discovery.
4. Run `cmake --preset prism-hydra`, then `cmake --build --preset prism-hydra`.
5. Launch `powershell -ExecutionPolicy Bypass -File run_usdview.ps1 path/to/scene.usda` and select **PrismRT**.

For standalone rendering, initialize the same environment with `. ./prism_env.ps1`, then run:

```powershell
./build/RelWithDebInfo/PrismRT.exe --export-scene InOneWeekend --seed 0 --output scene.usda
./build/RelWithDebInfo/PrismRT.exe --scene scene.usda --camera /World/Camera --spp 256 --output render.exr
```

No arguments display CLI help. USD, glTF/GLB and OBJ scene files are supported; rendering requires an explicit camera source. EXRs retain linear HDR values after the configurable sample clamp. Use `--max-sample-radiance 0` for energy/variance measurements. Local asset launchers belong in ignored `local/`; a template is provided under `tools/templates/`.


🐛 Known Issues & Limitations
---------------------------------------

* Curves, volumes, displacement, motion blur, subdivision refinement, and analytic-light/emissive-mesh sampling are not implemented.
* Legacy rough-metal and glTF samplers remain BSDF-only when no matching PDF is available.
* The setPosition method of AABBs doesn't set correctly the position, if it's not initially in zero.


⚖️ Licensing
---------------------------------------

This project is licensed under the MIT License. Please see the file [LICENSE](./LICENSE) for the full license text.

