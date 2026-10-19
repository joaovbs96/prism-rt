@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0env_setup.bat"
if errorlevel 1 exit /b 1

rem No arguments: render the bundled Cornell box to cornell_box.png here.
if not "%~1"=="" goto :pass
"%PRISM_PKG%bin\PrismRT.exe" --scene "%PRISM_PKG%bin\assets\scenes\CornellBox.usda" --camera /World/Camera --spp 128 --output cornell_box.png
exit /b %ERRORLEVEL%

:pass
"%PRISM_PKG%bin\PrismRT.exe" %*
exit /b %ERRORLEVEL%
