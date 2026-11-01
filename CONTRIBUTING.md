# Contributing to PrismRT

Thanks for your interest in PrismRT. Bug reports, test scenes, documentation fixes and code are all welcome.

## Reporting bugs

Open a GitHub issue with:

* what you ran (the command line, or the Hydra host and its render settings) and on which device (`cpu`, `vulkan`, `metal`);
* your platform, GPU and driver, and the PrismRT commit or release;
* the log output, and a small scene that reproduces the problem if you can share one.

For a material that renders wrong, the `.mtlx` document (or the USD material network) is usually the most useful attachment. Run with `PRISM_MDL_VERBOSE=1` to see every MaterialX and MDL message.

## Building and testing

Follow the build steps in the [README](README.md) and, on Windows, [docs/windows_vulkan.md](docs/windows_vulkan.md). Run the tests with:

```
ctest --test-dir build -C RelWithDebInfo -j 1
```

Tests that may open a GPU device carry the `gpu` label; `-LE gpu` runs only the CPU ones. The MaterialX loader also has a small standalone test project in `tests/` that needs only MaterialX.

## Making changes

* **Keep the two front ends in step.** USD scenes load through two paths: the Hydra delegate (`src/hydra/`) and the command line and SDK loader (`src/usd/`, `src/mesh/`). A scene feature changes both, and adds a parity fixture (`prism_add_parity_fixture` in `CMakeLists.txt`) that renders the same scene through each and compares the images.
* **Keep the devices in step.** Every device runs the same path tracer (`shaders/integrator/path_tracer.h`). Integrator and material changes must give the same image on the CPU and on the GPU, within sampling noise (`prism_compare_images`).
* **Prove when images should not change.** Refactors and performance work are checked bit for bit (`prism_compare_images --exact`) against the previous build.
* **Pin sampling in tests.** Use `--adaptive-threshold 0`, a fixed `--spp` and a fixed seed, since adaptive sampling is on by default.
* **Never rewrite assets.** Fixes for MaterialX content belong in the loader's in-memory repairs (`DocumentLoader`, [docs/materialx_compatibility.md](docs/materialx_compatibility.md)), never in `.mtlx` or USD files.
* **Use the project's types.** Use the `Float2`/`Float3`/`Float4` math types (no glm or Eigen), and `tbb::parallel_for` for render parallelism.
* **Metal.** Changes to code the Metal device shares (`shaders/`, `src/device/gpu/`, `src/hydra/`, `src/materialx/`) get an entry in the Metal checklist in [docs/gpu_devices.md](docs/gpu_devices.md), so they can be checked on a Mac.
* **Record design decisions** in [docs/DECISIONS.md](docs/DECISIONS.md), and keep the user-facing docs in `docs/` up to date with behaviour changes.

## Commits and pull requests

* Write small commits with short, imperative subjects that say what changed ("Keep render settings a host sets before its render index exists").
* Keep a pull request to one topic, and say in its description how you tested it: which tests, devices and scenes.
* Don't commit unrelated line-ending or formatting changes; check `git diff --stat` before you push.
* By contributing, you agree that your contributions are licensed under the [Apache License, Version 2.0](LICENSE).
