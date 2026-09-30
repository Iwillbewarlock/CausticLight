@echo off
rem CausticLight - configure + Release build + offline math test.
rem Usage: build.cmd            (incremental)
rem        build.cmd repair     (wipe build\release and reconfigure)
setlocal EnableDelayedExpansion

set "ROOT=%~dp0"
set "BUILD=%ROOT%build\release"
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\Dev\vcpkg"

rem --- Visual Studio: prefer 18 (this PC), fall back to 2022 --------------------------
set "VCVARS="
for %%V in (18 2022) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        for %%P in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
            if not defined VCVARS (
                if exist "%%~P\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
                    set "VCVARS=%%~P\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
                )
            )
        )
    )
)
if not defined VCVARS (
    echo ERROR: no Visual Studio vcvars64.bat found.
    exit /b 1
)
call "%VCVARS%" >nul || (echo ERROR: vcvars64 failed.& exit /b 1)

cd /d "%ROOT%"

if /i "%~1"=="repair" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

rem --- The cache must belong to this folder (project moved -> configure fails) ----------
if exist "%BUILD%\CMakeCache.txt" (
    set "HOME=%ROOT:~0,-1%"
    set "HOME=!HOME:\=/!"
    findstr /i /c:"CMAKE_HOME_DIRECTORY:INTERNAL=!HOME!" "%BUILD%\CMakeCache.txt" >nul || (
        echo Cache belongs to another folder - deleting build\ and reconfiguring.
        rmdir /s /q "%ROOT%build"
    )
)
if exist "%BUILD%\CMakeCache.txt" (
    findstr /c:"CMAKE_TOOLCHAIN_FILE:" "%BUILD%\CMakeCache.txt" >nul || (
        echo Cache is missing the vcpkg toolchain - deleting it.
        rmdir /s /q "%BUILD%"
    )
)

if not exist "%BUILD%\CMakeCache.txt" (
    cmake --preset release || exit /b 1
)

cmake --build "%BUILD%" > "%BUILD%\build.log" 2>&1
if errorlevel 1 (
    echo BUILD FAILED. Error lines:
    findstr /i /r /c:"error" "%BUILD%\build.log"
    exit /b 1
)

echo.
"%BUILD%\CausticMathTest.exe" || (echo TESTS FAILED& exit /b 1)

echo.
"%BUILD%\PassRender.exe" "%ROOT%..\dist\CausticLight\textures\CausticLight\calm.dds" "%BUILD%" || (echo GPU PASS CHECK FAILED& exit /b 1)

echo.
echo BUILD-OK: %BUILD%\CausticLight.dll
endlocal
exit /b 0
