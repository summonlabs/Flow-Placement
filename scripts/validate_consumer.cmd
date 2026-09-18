@echo off
rem Copyright 2026 Summon Software Labs.
rem SPDX-License-Identifier: Apache-2.0
rem
rem Regenerates nothing: this script consumes an already installed package and
rem validates that an independent downstream project can find, link, and run
rem against it. Run it through scripts\devshell.cmd so that a C++ toolchain is
rem in the environment.

setlocal
set "REPO=%~dp0.."
set "PREFIX=%REPO%\build\install"
set "CONSUMER_BUILD=%REPO%\build\consumer"

if not exist "%PREFIX%\lib\cmake\flowplace\flowplaceConfig.cmake" (
  echo validate_consumer: no installed package at "%PREFIX%" 1>&2
  exit /b 1
)

cmake -S "%REPO%\tests\consumer" -B "%CONSUMER_BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -Dflowplace_DIR="%PREFIX%\lib\cmake\flowplace"
if errorlevel 1 exit /b 1

cmake --build "%CONSUMER_BUILD%"
if errorlevel 1 exit /b 1

"%CONSUMER_BUILD%\flowplace_consumer.exe"
if errorlevel 1 exit /b 1

echo validate_consumer: downstream find_package consumer OK
exit /b 0
