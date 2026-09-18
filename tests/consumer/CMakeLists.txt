# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Independent downstream consumer: this project is configured and built against
# an *installed* Flow Placement package and uses only its public headers.

cmake_minimum_required(VERSION 3.20)
project(flowplace_consumer LANGUAGES CXX)

find_package(flowplace 1.0 REQUIRED CONFIG)

add_executable(flowplace_consumer main.cpp)
target_link_libraries(flowplace_consumer PRIVATE flowplace::flowplace)
target_compile_features(flowplace_consumer PRIVATE cxx_std_20)
