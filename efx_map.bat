@echo off
setlocal EnableDelayedExpansion

set PROJNAME=EconoPET
set PROJDIR=%~dp0\gw\%PROJNAME%

rd /q /s %PROJDIR%\outflow 2>NUL
call %~dp0efx.bat -f map
type %PROJDIR%\outflow\EconoPET.warn.log
type %PROJDIR%\outflow\EconoPET.err.log
echo.
