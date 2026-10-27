PrismRT demo package (Windows x64)
==================================

PrismRT is a physically-based path tracer. This package contains everything
needed to view USD scenes in usdview with the PrismRT renderer. Nothing is
installed and no environment variables are needed.

Quick start
-----------
1. Extract the whole zip to any folder (spaces are fine).
2. Double-click usdview.bat. It opens the bundled Cornell box in usdview
   rendered by PrismRT. To open your own scene, drag a .usd/.usda/.usdc/.usdz
   file onto usdview.bat, or run:  usdview.bat path\to\scene.usd
   Other usdview options can follow the scene (usdview.bat --help).
3. The first render of a scene compiles its materials and (on the GPU) the
   ray-tracing kernel, which can take several seconds. Later runs reuse the
   cache.

Other launchers
---------------
  prism_render.bat   Command-line renderer (PrismRT.exe). With no arguments it
                     renders the Cornell box to cornell_box.png in the current
                     folder. Otherwise it passes its arguments through, e.g.
                       prism_render.bat --scene my.usd --camera /Camera --spp 256 --output out.png
                     Run prism_render.bat --help for all options.
  list_devices.bat   Lists the render devices PrismRT can use.

Requirements
------------
  - Windows 10 or 11, 64-bit.
  - Optional GPU rendering: a Vulkan 1.3 GPU with hardware ray tracing
    (VK_KHR_ray_query): NVIDIA RTX, AMD Radeon RX 6000 or newer, Intel Arc,
    with an up-to-date driver. Without one, PrismRT renders on the CPU.
  - The Microsoft Visual C++ runtime DLLs are bundled.
  - About 3 GB of free disk space once extracted.

Choosing the device
-------------------
The device is picked automatically (Vulkan GPU if available, otherwise CPU).
To force one, set PRISMRT_DEVICE before starting a launcher:
    set PRISMRT_DEVICE=cpu
    usdview.bat
Valid values: auto, cpu, vulkan.

Colour
------
PrismRT renders in ACEScg. usdview shows the image through OpenColorIO
(View > Color Correction > openColorIO) with the config's default view, ACES
2.0 SDR for an sRGB display, like the PNG/JPEG images of prism_render.bat.
The OpenColorIO config is OCIO, ocio://studio-config-latest unless
you set it. Set PRISMRT_USDVIEW_COLOR_CORRECTION=sRGB (or disabled, or
saved: usdview's last mode) to start usdview in another mode, and
PRISMRT_USDVIEW_OCIO_VIEW (and PRISMRT_USDVIEW_OCIO_DISPLAY) for another view.
usdview also starts with "Enable Default Dome Light" on, so a scene without lights
is not black (PrismRT ignores it when the scene has lights of its own). Set
PRISMRT_USDVIEW_DEFAULT_DOME_LIGHT=off to start with it off, or saved to keep
usdview's last setting.

Cache
-----
Compiled shaders and materials are cached in %LOCALAPPDATA%\PrismRT\cache.
Set PRISMRT_CACHE_DIR to use another folder. The cache can be deleted at any
time.

Known limits
------------
  - Scenes with very large material sets produce a GPU kernel too big for the
    shader compiler; PrismRT then renders them on the CPU, which is slower.
  - The first render of a new scene can pause while shaders compile.
  - This is a demo build: it is not a supported product and the renderer's
    feature coverage is incomplete.

Third-party software
--------------------
This package redistributes the following components under their own licenses.
Their license files are in the licenses folder where the projects ship them.
  - OpenUSD (Pixar), Modified Apache 2.0; built with the changes in
    licenses\OpenUSD-patches
  - MaterialX (Academy Software Foundation), Apache 2.0
  - OpenSubdiv (Pixar), Modified Apache 2.0 (linked statically)
  - NVIDIA MDL SDK, BSD 3-clause
  - Intel Embree, Apache 2.0
  - Intel Open Image Denoise (CPU device), Apache 2.0
  - Intel oneTBB, Apache 2.0
  - OpenImageIO, OpenColorIO, OpenEXR/Imath and other libraries from vcpkg
    (BSD / Apache 2.0 / MIT, see licenses\vcpkg)
  - Python, PSF License
  - PySide6 / Qt 6, LGPL v3. Qt is dynamically linked and its libraries are
    replaceable files in python\Lib\site-packages\PySide6.
  - PyOpenGL (BSD), NumPy (BSD)
  - DirectX Shader Compiler (Microsoft), University of Illinois/NCSA license,
    used at run time to compile the GPU kernel
  - Khronos Vulkan loader (vulkan-1.dll, LunarG Vulkan SDK), Apache 2.0
  - Microsoft Visual C++ runtime, redistributable under the Visual Studio
    license terms

Versions
--------
@PRISM_DEPENDENCY_VERSIONS@
