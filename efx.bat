@echo off
setlocal EnableDelayedExpansion

set PROJNAME=EconoPET
set PROJDIR=%~dp0\gw\%PROJNAME%
set BINDIR=C:\Efinity\2025.1\bin

:: Move to the root of the Efinity project so that 'efx_run' artifacts are generated
:: within the \work_sim\ subdirectory.
pushd %PROJDIR%
cmd /c %BINDIR%\efx_run --prj %PROJNAME%.xml %*
popd
echo.
