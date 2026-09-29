@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0ImageEditing.exe" %*
set "appExitCode=%errorlevel%"
popd
exit /b %appExitCode%
