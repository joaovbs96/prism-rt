@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0env_setup.bat"
if errorlevel 1 exit /b 1

rem No scene (or only options) given: open the bundled Cornell box through its camera
rem (usdview's free camera starts inside the sky for this scene).
set "FIRST=%~1"
if not defined FIRST goto :defaultscene
if "%FIRST:~0,1%"=="-" goto :defaultscene
"%PRISM_PKG%python\python.exe" "%PRISM_PKG%usdview.py" %* --renderer PrismRT
exit /b %ERRORLEVEL%

:defaultscene
"%PRISM_PKG%python\python.exe" "%PRISM_PKG%usdview.py" "%PRISM_PKG%bin\assets\scenes\CornellBox.usda" --camera /World/Camera %* --renderer PrismRT
exit /b %ERRORLEVEL%
