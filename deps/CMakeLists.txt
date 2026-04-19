# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required (VERSION 3.20)
project (SneezeDeps LANGUAGES C CXX)

include (ExternalProject)

# ---------------------------------------------------------------------------
# This CMakeLists.txt knows only about files under deps/. It never looks
# into src/ and it never touches the Sneeze build tree. The scripts in
# scripts/ are the only glue between the deps branch and the Sneeze branch.
#
# Invocation modes:
#
#   cmake -S deps -B <build>                      Build all dependencies
#   cmake -S deps -B <build> -DDEP=<name>         Build a single dependency (CI)
#
# Optional in both modes:
#   -DSNEEZE_CONFIG=Debug|Release                 (default Release)
#   -DSNEEZE_PLATFORM=<slug>                      (auto-detect if unset)
#   -DSNEEZE_DEP_REPO=<path>                      (default: deps/repos)
#   -DLIBS_DIR=<path>                             (default: deps/builds/<plat>/<cfg>/libs)
#   -DCMAKE_TOOLCHAIN_FILE=<path>                 (forwarded to each dep)
# ---------------------------------------------------------------------------

# SNEEZE_CONFIG -----------------------------------------------------------

if (NOT DEFINED SNEEZE_CONFIG OR SNEEZE_CONFIG STREQUAL "")
   set (SNEEZE_CONFIG "Release")
endif ()
if (NOT SNEEZE_CONFIG MATCHES "^(Debug|Release)$")
   message (FATAL_ERROR "SNEEZE_CONFIG must be Debug or Release, got '${SNEEZE_CONFIG}'")
endif ()

# SNEEZE_PLATFORM (auto-detect) -------------------------------------------

if (NOT DEFINED SNEEZE_PLATFORM OR SNEEZE_PLATFORM STREQUAL "")
   if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
      set (_plat "android-arm64")
   elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
      set (_plat "ios-arm64")
   elseif (WIN32)
      set (_plat "windows-x64")
   elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
      if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
         set (_plat "linux-arm64")
      else ()
         set (_plat "linux-x64")
      endif ()
   elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64"
            OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64")
         set (_plat "macos-arm64")
      else ()
         set (_plat "macos-x64")
      endif ()
   else ()
      message (FATAL_ERROR "Unknown platform; set -DSNEEZE_PLATFORM=<slug> explicitly.")
   endif ()
   set (SNEEZE_PLATFORM "${_plat}" CACHE STRING
      "Platform slug matching Artemis manifest.json (windows-x64, linux-x64, macos-arm64, ...)")
endif ()

string (TOLOWER "${SNEEZE_CONFIG}" _cfg_lower)

# Path defaults (all relative to deps/) -----------------------------------

if (NOT DEFINED SNEEZE_DEP_REPO OR SNEEZE_DEP_REPO STREQUAL "")
   set (SNEEZE_DEP_REPO "${CMAKE_CURRENT_SOURCE_DIR}/repos" CACHE PATH
      "Shared dependency source clones (reused across platform+config).")
endif ()
if (NOT DEFINED LIBS_DIR OR LIBS_DIR STREQUAL "")
   set (LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/builds/${SNEEZE_PLATFORM}/${_cfg_lower}/libs"
      CACHE PATH "Per-dep build + install trees for this platform+config.")
endif ()

# Wrapper scripts pass native paths (backslashes on Windows from PowerShell's
# Join-Path). Some deps embed these paths into generated CMake strings where
# a backslash starts an escape sequence -- e.g. SPIRV-Tools fails with
# "Invalid character escape '\D'" on "E:\Dev\...". Normalize once, up front.
file (TO_CMAKE_PATH "${SNEEZE_DEP_REPO}" SNEEZE_DEP_REPO)
file (TO_CMAKE_PATH "${LIBS_DIR}" LIBS_DIR)

message (STATUS "Sneeze deps: platform = ${SNEEZE_PLATFORM}")
message (STATUS "Sneeze deps: config   = ${SNEEZE_CONFIG}")
message (STATUS "Sneeze deps: dep repo = ${SNEEZE_DEP_REPO}")
message (STATUS "Sneeze deps: libs dir = ${LIBS_DIR}")

# Feature gates -----------------------------------------------------------

option (SNEEZE_ENABLE_WASM "Enable WebAssembly runtime (Wasmtime)" ON)
option (SNEEZE_ENABLE_XR   "Enable OpenXR integration" ON)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (SNEEZE_ENABLE_XR OFF CACHE BOOL "" FORCE)
endif ()

# Cross-compilation variable forwarding -----------------------------------
# Forwarded to every CMake-based ExternalProject_Add so all deps build for
# the target platform with the same toolchain, generator, and SDK settings.

set (CROSS_COMPILE_ARGS)

if (CMAKE_GENERATOR)
   list (APPEND CROSS_COMPILE_ARGS -G ${CMAKE_GENERATOR})
endif ()

if (CMAKE_TOOLCHAIN_FILE)
   list (APPEND CROSS_COMPILE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif ()

if (CMAKE_CROSSCOMPILING)
   if (CMAKE_SYSTEM_NAME)
      list (APPEND CROSS_COMPILE_ARGS -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})
   endif ()
   if (CMAKE_SYSTEM_PROCESSOR)
      list (APPEND CROSS_COMPILE_ARGS -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR})
   endif ()
   if (CMAKE_OSX_SYSROOT)
      list (APPEND CROSS_COMPILE_ARGS -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
   endif ()
   if (CMAKE_ANDROID_NDK)
      list (APPEND CROSS_COMPILE_ARGS -DCMAKE_ANDROID_NDK=${CMAKE_ANDROID_NDK})
   endif ()
   if (ANDROID_ABI)
      list (APPEND CROSS_COMPILE_ARGS -DANDROID_ABI=${ANDROID_ABI})
   endif ()
   if (ANDROID_PLATFORM)
      list (APPEND CROSS_COMPILE_ARGS -DANDROID_PLATFORM=${ANDROID_PLATFORM})
   endif ()
endif ()

# Always forward macOS/iOS arch + deployment target -- matters for both
# native (macOS universal) and cross (iOS) builds.
if (CMAKE_OSX_ARCHITECTURES)
   list (APPEND CROSS_COMPILE_ARGS "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif ()
if (CMAKE_OSX_DEPLOYMENT_TARGET)
   list (APPEND CROSS_COMPILE_ARGS -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif ()

# ---------------------------------------------------------------------------
# Dependency list
# ---------------------------------------------------------------------------
# Each dep is defined in deps/<name>.cmake as a single ExternalProject_Add.
# The per-dep file is the single source of truth. CI invokes them
# individually via -DDEP=<name> so it can parallelize and cache per dep.

set (SNEEZE_DEPS
   spirv-headers
   spirv-tools
   spirv-cross
   glslang
   anari-sdk
   curl
   rmlui
   nlohmann-json
   boringssl
   jwt-cpp
   filament
   halogen
   vox
)
if (SNEEZE_ENABLE_WASM)
   list (APPEND SNEEZE_DEPS wasmtime)
endif ()
if (SNEEZE_ENABLE_XR)
   list (APPEND SNEEZE_DEPS openxr-sdk)
endif ()

# anari-sdk-release is a Release-only shadow of anari-sdk, consumed only by
# Halogen (always Release). In Release outer builds the regular anari-sdk is
# already Release so Halogen uses that; we only need the shadow in Debug.
if (SNEEZE_CONFIG STREQUAL "Debug")
   list (APPEND SNEEZE_DEPS anari-sdk-release)
endif ()

# ---------------------------------------------------------------------------
# Mode selection: single dep (CI) or all deps (scripts / dev).
# ---------------------------------------------------------------------------

if (DEP)
   list (FIND SNEEZE_DEPS "${DEP}" _dep_idx)
   if (_dep_idx LESS 0)
      # Disabled for this platform (e.g. openxr-sdk on iOS) vs. typo.
      # If the per-dep .cmake file exists, treat it as disabled -> no-op
      # so CI can keep a uniform matrix without per-platform excludes.
      if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${DEP}.cmake")
         message (STATUS "DEP '${DEP}' is disabled for platform '${SNEEZE_PLATFORM}' (config '${SNEEZE_CONFIG}') -- skipping")
         return ()
      endif ()
      message (FATAL_ERROR "DEP '${DEP}' is not a known Sneeze dep (enabled set: ${SNEEZE_DEPS})")
   endif ()
   include ("${CMAKE_CURRENT_SOURCE_DIR}/${DEP}.cmake")
else ()
   foreach (dep IN LISTS SNEEZE_DEPS)
      include ("${CMAKE_CURRENT_SOURCE_DIR}/${dep}.cmake")
   endforeach ()

   # Cross-dep ordering. Each deps/<dep>.cmake declares a single target in
   # isolation; we express transitive ordering here for the all-deps build.
   add_dependencies (spirv-tools spirv-headers)
   add_dependencies (glslang     spirv-tools)
   add_dependencies (vox         spirv-cross)

   # Halogen (always Release) needs a Release filament and a Release
   # anari_backend. In Debug outer builds that Release anari comes from the
   # anari-sdk-release shadow; in Release builds the regular anari-sdk is
   # already Release.
   if (SNEEZE_CONFIG STREQUAL "Debug")
      add_dependencies (halogen anari-sdk-release filament)
   else ()
      add_dependencies (halogen anari-sdk filament)
   endif ()

   # curl's Android branch links BoringSSL as its TLS/crypto backend.
   if (ANDROID)
      add_dependencies (curl boringssl)
   endif ()
endif ()
