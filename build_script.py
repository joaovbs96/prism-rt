import sys
import os
import subprocess

# Visual Studio paths
VS_SHELL = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

# Environment variables
VCPKG_ROOT = os.environ.get("VCPKG_ROOT", "")

# Base directories
PROGRAMMING_PROJECTS = "C:/Users/joaov/Desktop/Programming_Projects"
EXTERNAL_DEPS = f"{PROGRAMMING_PROJECTS}/external_deps"

# Embree
EMBREE_DIR = f"{PROGRAMMING_PROJECTS}/Embree/build/Release"

# TBB
TBB_ROOT = f"{PROGRAMMING_PROJECTS}/include/tbb"
TBB_DIR = f"{TBB_ROOT}/lib/cmake/tbb"
TBB_INCLUDE_DIRS = f"{TBB_ROOT}/include/tbb"

# OpenSubdiv
OSD_ROOT = f"{EXTERNAL_DEPS}/OpenSubdiv"
OSD_CMAKE_PATH = f"{OSD_ROOT}/install/lib/cmake/OpenSubdiv"
OSD_INCLUDE_PATH = f"{OSD_ROOT}/install/include/opensubdiv"

# MaterialX
MTLX_ROOT = f"{EXTERNAL_DEPS}/MaterialX"
MTLX_CMAKE_PATH = f"{MTLX_ROOT}/install/lib/cmake/MaterialX"

# OpenUSD
USD_ROOT = f"{EXTERNAL_DEPS}/OpenUSD"
USD_CMAKE_PATH = f"{USD_ROOT}/install"
USD_INCLUDE_PATH = f"{USD_ROOT}/install/include/pxr"

# OSL
OSL_DIR = "C:/osl_install/lib/cmake/OSL"

# MDL SDK
MDL_SDK_ROOT = f"{PROGRAMMING_PROJECTS}/mdl-sdk"
MDL_SDK_DIR = MDL_SDK_ROOT

def run_vs_shell(cmd: str):
    """Run command in Visual Studio developer command prompt environment"""
    temp_bat = os.path.join(os.path.dirname(__file__), "build_script_temp.bat")
    
    # Write batch file
    with open(temp_bat, "w") as f:
        f.write('@echo off\n')
        f.write(f'call "{VS_SHELL}"\n')
        f.write('if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%\n')
        f.write(f'{cmd}\n')
        f.write('if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%\n')
    
    print(f"Running: {cmd}")
    
    try:
        # Run the batch file
        result = subprocess.run(
            temp_bat,
            shell=True,
            check=True
        )
        return result.returncode
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Command failed with exit code {e.returncode}")
        raise
    finally:
        # Cleanup
        if os.path.exists(temp_bat):
            try:
                os.remove(temp_bat)
            except:
                pass

# Configure and build OpenSubdiv
print("\n" + "="*60)
print("Building OpenSubdiv")
print("="*60)
additional_options = f'-DCMAKE_TOOLCHAIN_FILE="{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" '
additional_options += f'-DTBB_DIR="{TBB_DIR}" -DTBB_INCLUDE_DIRS="{TBB_INCLUDE_DIRS}" '
additional_options += "-DNO_TUTORIALS=1 -DNO_EXAMPLES=1 -DNO_REGRESSION=1 "
run_vs_shell(f'cd /d "{OSD_ROOT}" && cmake -B build {additional_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(f'cd /d "{OSD_ROOT}" && cmake --build build --target install --config RelWithDebInfo')

# Configure and build MaterialX
print("\n" + "="*60)
print("Building MaterialX")
print("="*60)
additional_options = f'-DCMAKE_TOOLCHAIN_FILE="{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" '
additional_options += "-DMATERIALX_BUILD_GEN_MDL=ON "
additional_options += "-DMATERIALX_BUILD_GEN_GLSL=ON -DMATERIALX_BUILD_GEN_OSL=ON "
additional_options += "-DMATERIALX_BUILD_GEN_ESSL=ON -DMATERIALX_BUILD_GEN_MSL=ON "
additional_options += "-DMATERIALX_BUILD_PYTHON=OFF -DMATERIALX_BUILD_VIEWER=OFF "
additional_options += "-DMATERIALX_BUILD_GRAPH_EDITOR=OFF -DMATERIALX_BUILD_RENDER=ON "
additional_options += "-DMATERIALX_BUILD_TESTS=OFF -DMATERIALX_INSTALL_PYTHON=OFF "
run_vs_shell(f'cd /d "{MTLX_ROOT}" && cmake -B build {additional_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(f'cd /d "{MTLX_ROOT}" && cmake --build build --target install --config RelWithDebInfo')

# Configure and build OpenUSD
print("\n" + "="*60)
print("Building OpenUSD")
print("="*60)
additional_options = f'-DCMAKE_TOOLCHAIN_FILE="{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" '
additional_options += f'-DTBB_DIR="{TBB_DIR}" -DTBB_INCLUDE_DIRS="{TBB_INCLUDE_DIRS}" '
additional_options += f'-DOpenSubdiv_DIR="{OSD_CMAKE_PATH}" -DOPENSUBDIV_INCLUDE_DIR="{OSD_INCLUDE_PATH}" '
additional_options += f'-DMaterialX_DIR="{MTLX_CMAKE_PATH}" '
additional_options += "-DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF "
additional_options += "-DPXR_BUILD_USD_TOOLS=OFF -DPXR_ENABLE_PYTHON_SUPPORT=OFF "
additional_options += "-DPXR_ENABLE_MATERIALX_SUPPORT=ON "
run_vs_shell(f'cd /d "{USD_ROOT}" && cmake -B build {additional_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(f'cd /d "{USD_ROOT}" && cmake --build build --target install --config RelWithDebInfo')

print("\n" + "="*60)
print("All dependencies built successfully!")
print("="*60)