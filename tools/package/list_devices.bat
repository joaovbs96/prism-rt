@echo off
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0env_setup.bat"
if errorlevel 1 exit /b 1
"%PRISM_PKG%bin\PrismRT.exe" --list-devices
exit /b %ERRORLEVEL%
