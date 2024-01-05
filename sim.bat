@echo off
setlocal EnableDelayedExpansion

set PROJNAME=EconoPET
set PROJDIR=%~dp0\gw\%PROJNAME%

:Parse
    if /I "%~1" == "" (
        goto EndParse
    ) else if /I "%~1" == "--view" (
        goto View
    ) else if /I "!TOPMODULE!" == "" (
        set TOPMODULE=%~1
    )
    shift
    goto Parse

:EndParse
    if /I "!TOPMODULE!" == "" (
        set TOPMODULE=sim
    )

:Update
    :: Invoke 'efx_run' to generate/update the '\work_sim\<proj>.f' file, but ignore
    :: the resulting Python exception which occures due to lack of SystemVerilog support.
    :: 
    :: (See https://www.efinixinc.com/support/forum.php?cid=6&pid=932)
    call %~dp0efx.bat --flow rtlsim 2> NUL
    echo.

:Run
    :: 'efx_run' produces relative paths to simulation files.  Therefore, we must execute
    :: iverilog from the root of the project directory.
    pushd %PROJDIR%

    iverilog.exe -g2009 -s %TOPMODULE% -o%PROJDIR%\work_sim\%PROJNAME%.vvp -f%PROJDIR%\work_sim\%PROJNAME%.f
    if %ERRORLEVEL% neq 0 popd && exit /b %ERRORLEVEL%

    vvp.exe -l%PROJDIR%\outflow\%PROJNAME%.rtl.simlog %PROJDIR%\work_sim\%PROJNAME%.vvp
    popd && exit /b %ERRORLEVEL%

:View
    start /MAX gtkwave.exe %PROJDIR%\work_sim\out.vcd
    exit /b %ERRORLEVEL%
