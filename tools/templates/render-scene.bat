@echo off
rem Copy to ignored local/scenes/ and adjust the asset and camera values.
rem Paths are relative to that copied location; quoted paths support spaces.
set "SCENE=C:\assets\scene.usda"
set "CAMERA=/World/Camera"
set "OUTPUT=scene.exr"
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { . '%~dp0../../prism_env.ps1'; & '%~dp0../../build/RelWithDebInfo/PrismRT.exe' --scene $env:SCENE --camera $env:CAMERA --spp 256 --output $env:OUTPUT; exit $LASTEXITCODE }"
