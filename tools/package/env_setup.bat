rem Shared by the package launchers (call "%~dp0env_setup.bat"). Everything is
rem relative to this file, so the folder can be extracted anywhere. Inherited
rem PRISM_*, PXR_*, PYTHON* and Qt settings are cleared so a developer
rem machine cannot change what the package loads.
set "PRISM_PKG=%~dp0"
for /f "delims==" %%V in ('set PRISM_ 2^>nul') do set "%%V="
for /f "delims==" %%V in ('set PXR_ 2^>nul') do set "%%V="
for /f "delims==" %%V in ('set PYTHON 2^>nul') do set "%%V="
for /f "delims==" %%V in ('set QT_ 2^>nul') do set "%%V="
set "MDL_SDK_ROOT="
set "PRISM_PKG=%~dp0"

set "PRISM_DLLS=%PRISM_PKG%bin;%PRISM_PKG%usd\lib;%PRISM_PKG%python;%PRISM_PKG%python\DLLs;%PRISM_PKG%python\Lib\site-packages\PySide6;%PRISM_PKG%mdl\bin;%PRISM_PKG%dxc"
set "PATH=%PRISM_DLLS%;%SystemRoot%\System32;%SystemRoot%"
set "PXR_USD_WINDOWS_DLL_PATH=%PRISM_DLLS%"

set "PYTHONHOME=%PRISM_PKG%python"
set "PYTHONPATH=%PRISM_PKG%usd\lib\python;%PRISM_PKG%python\Lib\site-packages;%PRISM_PKG%lib\usd\plugins\prism\usdview_plugins"
set "PYTHONNOUSERSITE=1"
set "PYOPENGL_USE_ACCELERATE=0"
set "QT_PLUGIN_PATH=%PRISM_PKG%python\Lib\site-packages\PySide6\plugins"

set "PXR_PLUGINPATH_NAME=%PRISM_PKG%lib\usd\plugins\prism\resources;%PRISM_PKG%lib\usd\plugins\prism\usdview_plugins\prismUsdview"

set "PRISM_MATERIALX_LIBRARY_PATH=%PRISM_PKG%materialx\libraries"
set "PRISM_MATERIALX_MDL=%PRISM_PKG%materialx\libraries\mdl"
set "PRISM_MATERIALX_MDL_LIBRARIES=%PRISM_PKG%materialx\libraries\mdl"
set "MDL_SDK_ROOT=%PRISM_PKG%mdl"
set "PRISM_MDL_SDK_ROOT=%PRISM_PKG%mdl"
set "PRISM_DXC=%PRISM_PKG%dxc\dxc.exe"

rem Shader and material caches persist between runs (and between versions of
rem this package) under the user profile. Set PRISMRT_CACHE_DIR before running
rem a launcher to move them.
set "PRISM_CACHE_DIR=%LOCALAPPDATA%\PrismRT\cache"
if defined PRISMRT_CACHE_DIR set "PRISM_CACHE_DIR=%PRISMRT_CACHE_DIR%"
rem Colour management: PrismRT's default OpenColorIO config, also used by
rem usdview's openColorIO colour correction (the config's default view). Set
rem OCIO to another config or URI before running a launcher to change both.
if not defined OCIO set "OCIO=ocio://studio-config-latest"
rem usdview starts in openColorIO; PRISMRT_USDVIEW_COLOR_CORRECTION=sRGB,
rem disabled or saved (usdview's last mode) chooses otherwise.
if defined PRISMRT_USDVIEW_COLOR_CORRECTION set "PRISM_USDVIEW_COLOR_CORRECTION=%PRISMRT_USDVIEW_COLOR_CORRECTION%"
if defined PRISMRT_USDVIEW_OCIO_DISPLAY set "PRISM_USDVIEW_OCIO_DISPLAY=%PRISMRT_USDVIEW_OCIO_DISPLAY%"
if defined PRISMRT_USDVIEW_OCIO_VIEW set "PRISM_USDVIEW_OCIO_VIEW=%PRISMRT_USDVIEW_OCIO_VIEW%"
rem usdview starts with its default dome light enabled (PrismRT uses it only
rem for stages without lights of their own); PRISMRT_USDVIEW_DEFAULT_DOME_LIGHT=off
rem starts with it disabled, saved keeps usdview's last setting.
if defined PRISMRT_USDVIEW_DEFAULT_DOME_LIGHT set "PRISM_USDVIEW_DEFAULT_DOME_LIGHT=%PRISMRT_USDVIEW_DEFAULT_DOME_LIGHT%"
rem The device is chosen automatically (Vulkan GPU, else CPU). Override it with
rem PRISMRT_DEVICE=cpu or vulkan.
if defined PRISMRT_DEVICE set "PRISM_DEVICE=%PRISMRT_DEVICE%"
if /i "%PRISMRT_DEVICE%"=="cpu" set "PRISM_DISABLE_VULKAN=1"
