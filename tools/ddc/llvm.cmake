# SPDX-License-Identifier: GPL-3.0-only
# llvm.cmake - the fixed CMake options for every LLVM build in the DDC
# (decision 0026, layer 3): stage 1 and stage 2 on both paths.
# usage: cmake -C tools/ddc/llvm.cmake ...   (tools/ddc/build-llvm.sh)
#
# What depends on the build machine is set by build-llvm.sh, not here:
# the compiler, the prefix maps of the source and build directories, and
# SOURCE_DATE_EPOCH.

# A plain release build: no assertions, no PGO, no LTO.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(LLVM_ENABLE_ASSERTIONS OFF CACHE BOOL "")
set(LLVM_ENABLE_LTO OFF CACHE STRING "")
set(LLVM_BUILD_INSTRUMENTED OFF CACHE STRING "")

# Clang and lld, for x86 only: all Joist needs.
set(LLVM_ENABLE_PROJECTS "clang;lld" CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD X86 CACHE STRING "")

# Every stage runs in live-bootstrap's i386 musl system (record.md), so
# every compiler is for it by default, whatever compiler built it.
set(LLVM_HOST_TRIPLE i686-unknown-linux-musl CACHE STRING "")
set(LLVM_DEFAULT_TARGET_TRIPLE i686-unknown-linux-musl CACHE STRING "")
# Static: a stage 1 built elsewhere runs there without a loader or
# shared libraries of its own.
set(LLVM_BUILD_STATIC ON CACHE BOOL "")

# Nothing taken from the build machine: no revision, no optional
# libraries found or not found on it.
set(LLVM_APPEND_VC_REV OFF CACHE BOOL "")
set(LLVM_ENABLE_ZLIB OFF CACHE STRING "")
set(LLVM_ENABLE_ZSTD OFF CACHE STRING "")
set(LLVM_ENABLE_LIBXML2 OFF CACHE STRING "")
set(LLVM_ENABLE_LIBEDIT OFF CACHE BOOL "")
set(LLVM_ENABLE_LIBPFM OFF CACHE BOOL "")
set(LLVM_ENABLE_FFI OFF CACHE BOOL "")
set(LLVM_ENABLE_CURL OFF CACHE BOOL "")
set(LLVM_ENABLE_HTTPLIB OFF CACHE BOOL "")
set(LLVM_ENABLE_PLUGINS OFF CACHE BOOL "")
set(CLANG_PLUGIN_SUPPORT OFF CACHE BOOL "")
set(LLVM_ENABLE_BINDINGS OFF CACHE BOOL "")
set(LLVM_ENABLE_OCAMLDOC OFF CACHE BOOL "")

# Not needed, and hours saved.
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "")
set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "")
set(CLANG_INCLUDE_TESTS OFF CACHE BOOL "")
set(CLANG_INCLUDE_DOCS OFF CACHE BOOL "")
set(CLANG_ENABLE_ARCMT OFF CACHE BOOL "")
set(CLANG_ENABLE_STATIC_ANALYZER OFF CACHE BOOL "")

# One link at a time, to fit the guest's memory.
set(LLVM_PARALLEL_LINK_JOBS 1 CACHE STRING "")

# What is installed, and compared: the tools 0026 names, llvm-readelf
# (Limine's build needs it), and the headers Clang needs for freestanding
# code (stdint.h and the like).
set(LLVM_DISTRIBUTION_COMPONENTS
    "clang;clang-resource-headers;lld;llvm-objcopy;llvm-objdump;llvm-ar;llvm-readobj;llvm-readelf"
    CACHE STRING "")
