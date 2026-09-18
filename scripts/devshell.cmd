@echo off
rem Copyright 2026 Summon Software Labs.
rem SPDX-License-Identifier: Apache-2.0
rem
rem Developer helper: locate a Visual Studio installation on this machine,
rem enter its x64 developer environment, and run the command passed as
rem arguments. Nothing in the build or test path depends on this script.

setlocal enabledelayedexpansion
set "VSPATH="

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if not defined VSPATH (
  for %%p in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
  ) do (
    if not defined VSPATH if exist "%%~p\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~p"
  )
)

if not defined VSPATH (
  echo devshell: no Visual Studio C++ toolset found 1>&2
  exit /b 127
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
%*
exit /b %ERRORLEVEL%
