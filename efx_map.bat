@echo off
setlocal EnableDelayedExpansion

set PROJNAME=EconoPET
set PROJDIR=%~dp0\gw\%PROJNAME%
set BINDIR=C:\Efinity\2023.1\bin

:: Move to the root of the Efinity project so that 'efx_run' artifacts are generated
:: within the \work_sim\ subdirectory.
pushd %PROJDIR%
rd /q /s outflow
cmd /c %BINDIR%\efx_run %PROJNAME%.xml -f map
type outflow\EconoPET.warn.log
type outflow\EconoPET.err.log
popd
echo.