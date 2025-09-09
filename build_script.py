import sys
import os
import subprocess
import tempfile

PROJECT_ROOT = os.path.dirname(__file__)

VS_SHELL = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat".replace("\\", "/")
EXTERNAL_DEPS = rf"{PROJECT_ROOT}\external".replace("\\", "/")
EMBREE_PATH = rf"{EXTERNAL_DEPS}\embree".replace("\\", "/")
TBB_DIR = rf"{EXTERNAL_DEPS}/tbb/lib/cmake/tbb".replace("\\", "/")
TBB_INCLUDE_DIRS = rf"{EXTERNAL_DEPS}\tbb/include\tbb".replace("\\", "/")

def run_vs_shell(cmd:str):
    temp_bat = os.path.join(os.path.dirname(__file__), "build_script.bat")
    # cmd = cmd.replace("\\", "\\\\")
    with open(temp_bat, "w") as temp_bat_obj:
        temp_bat_obj.write(f"call \"{VS_SHELL}\"\n{cmd}")
    print(f"call \"{temp_bat}\"")
    return subprocess.check_call(f"call \"{temp_bat}\"", shell=True)

# configure and build OpenSubdiv
osd_path = os.path.join(EXTERNAL_DEPS, "OpenSubdiv").replace("\\", "/")
osd_cmake_path = os.path.join(osd_path, r"install\lib\cmake\OpenSubdiv").replace("\\", "/")
osd_include_path = os.path.join(osd_path, r"install\include\opensubdiv").replace("\\", "/")
additional_options = f'-DTBB_DIR="{TBB_DIR}" -DTBB_INCLUDE_DIRS="{TBB_INCLUDE_DIRS}" '
additional_options += "-DNO_TUTORIALS=1 -DNO_EXAMPLES=1 -DNO_REGRESSION=1 "
run_vs_shell(fr'cd "{osd_path}" & cmake -B build {additional_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(fr'cd "{osd_path}" & cmake --build build --target install --config RelWithDebInfo')

# configure and build OpenUSD
usd_path = os.path.join(EXTERNAL_DEPS, "OpenUSD").replace("\\", "/")
usd_cmake_path = os.path.join(usd_path, r"install\lib\cmake").replace("\\", "/")
additional_options = f'-DTBB_DIR="{TBB_DIR}" -DOpenSubdiv_DIR="{osd_cmake_path}" -DOPENSUBDIV_INCLUDE_DIR="{osd_include_path}" '
additional_options += f'-DTBB_INCLUDE_DIRS="{TBB_INCLUDE_DIRS}" '
additional_options += "-DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF "
additional_options += "-DPXR_BUILD_USD_TOOLS=OFF -DPXR_ENABLE_PYTHON_SUPPORT=OFF "
run_vs_shell(fr'cd "{usd_path}" & cmake -B build {additional_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(fr'cd "{usd_path}" & cmake --build build --target install --config RelWithDebInfo')