@echo off
py -3 "%~dp0build_script.py" %*
exit /b %ERRORLEVEL%
