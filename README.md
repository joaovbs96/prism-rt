![Prism Logo](./assets/logo/PrismRT_logo_white.png#gh-dark-mode-only)
![Prism Logo](./assets/logo/PrismRT_logo_black.png#gh-light-mode-only)

👋 Introduction
---------------------------------------

PrismRT is a CPU-based Path Tracer with support for rendering OpenUSD, glTF and OBJ files, powered by the Embree Ray Tracing framework, the oneTBB threading library and the OpenImageIO image processing library.

Lorem ipsum dolor sit amet, consectetur adipiscing elit. Suspendisse congue ultricies sollicitudin. Aliquam erat volutpat. Quisque pellentesque velit vitae efficitur commodo. Sed tincidunt mollis tellus sit amet euismod. Maecenas blandit a nisl non rutrum. Integer pellentesque sem at dictum facilisis. Maecenas consequat porta justo, eu faucibus velit auctor in. Fusce mollis justo neque, aliquam bibendum urna accumsan at. In hac habitasse platea dictumst. Vivamus lorem dui, tempus vel diam eu, mollis hendrerit mi. Morbi euismod, tortor quis facilisis venenatis, diam massa pretium libero, sollicitudin placerat libero orci eget ex. Ut consectetur purus et sagittis sodales. Duis ut scelerisque mi, eget egestas tortor. Suspendisse a vehicula odio, id mollis felis. Nam elementum nisi at pellentesque iaculis.


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

* Proin facilisis justo eget ligula sodales, id posuere enim aliquet.
* Praesent sagittis tellus id tellus porttitor molestie.
* Donec laoreet nisl id feugiat convallis.
* Aenean eget dui vel nibh vulputate facilisis.


🛠️ Build Instructions (WIP)
---------------------------------------

1. Clone this repo. Make sure to use the recursive flag: `git clone https://github.com/joaovbs96/prism-rt.git --recursive`
2. cd into the repo folder.
3. [Install the latest CMake release](https://cmake.org/download/).
4. Install vcpkg:
    1. Clone the vcpkg repo: `git clone https://github.com/microsoft/vcpkg.git`
    2. Perform the initial vcpkg setup: `.\vcpkg\bootstrap-vcpkg.bat`
    3. Install vcpkg dependencies: `.\vcpkg\vcpkg.exe install`
5. Download and extract the newest Embree v4.X release into `external\embree`
    * Note: if your version number is different than the one in the `.vscode\settings.json` file, you will need to update the path to the folder containing the `.cmake` files for Embree. For example, if your Embree version is v4.4.0, your cmake path will be: `${workspaceRoot}\external\embree\lib\cmake\embree-4.4.0`
6. Download and extract the newest oneTBB release into `external\tbb`

Cras imperdiet eros nec risus tincidunt tristique. Quisque rutrum auctor odio, sit amet lacinia enim. Aenean vel viverra lectus. Maecenas et accumsan mi, non elementum ex. Suspendisse sed nibh ac felis porttitor imperdiet. Donec massa sem, lacinia a ligula non, hendrerit suscipit justo. Suspendisse dui nibh, fermentum quis iaculis sed, tempor eget magna.


🐛 Known Issues & Limitations
---------------------------------------

* Sed vitae dui porta, tempus augue sed, fringilla leo.
* Quisque sit amet mauris sodales neque lobortis bibendum.
* Suspendisse malesuada justo et consequat pretium.
* Pellentesque fermentum neque tincidunt nulla commodo lobortis.


⚖️ Licensing
---------------------------------------

This project is licensed under the MIT License. Please see the file [LICENSE](./LICENSE) for the full license text.

