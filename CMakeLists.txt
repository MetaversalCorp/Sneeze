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
project (Sneeze-SuperBuild LANGUAGES NONE)

include (ExternalProject)

set (LIBS_DIR "${CMAKE_SOURCE_DIR}/libs")

# ---------------------------------------------------------------------------
# SPIRV-Headers (header-only, required by SPIRV-Tools)
# ---------------------------------------------------------------------------

ExternalProject_Add (spirv-headers
   GIT_REPOSITORY   https://github.com/KhronosGroup/SPIRV-Headers.git
   GIT_TAG          vulkan-sdk-1.4.341.0
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/SPIRV-Headers/src"
   BINARY_DIR       "${LIBS_DIR}/SPIRV-Headers/build"
   INSTALL_DIR      "${LIBS_DIR}/SPIRV-Headers/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
)

# ---------------------------------------------------------------------------
# SPIRV-Tools (depends on SPIRV-Headers)
# ---------------------------------------------------------------------------

ExternalProject_Add (spirv-tools
   GIT_REPOSITORY   https://github.com/KhronosGroup/SPIRV-Tools.git
   GIT_TAG          vulkan-sdk-1.4.341.0
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/SPIRV-Tools/src"
   BINARY_DIR       "${LIBS_DIR}/SPIRV-Tools/build"
   INSTALL_DIR      "${LIBS_DIR}/SPIRV-Tools/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DSPIRV-Headers_SOURCE_DIR=${LIBS_DIR}/SPIRV-Headers/src
      -DSPIRV_SKIP_TESTS=ON
      -DSPIRV_SKIP_EXECUTABLES=ON
   DEPENDS spirv-headers
)

# ---------------------------------------------------------------------------
# glslang (depends on SPIRV-Tools for optimizer integration)
# ---------------------------------------------------------------------------

ExternalProject_Add (glslang
   GIT_REPOSITORY   https://github.com/KhronosGroup/glslang.git
   GIT_TAG          vulkan-sdk-1.4.341.0
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/glslang/src"
   BINARY_DIR       "${LIBS_DIR}/glslang/build"
   INSTALL_DIR      "${LIBS_DIR}/glslang/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DENABLE_CTEST=OFF
      -DENABLE_OPT=OFF
      -DENABLE_HLSL=OFF
   DEPENDS spirv-tools
)

# ---------------------------------------------------------------------------
# ANARI-SDK (with helide device)
# ---------------------------------------------------------------------------

ExternalProject_Add (anari-sdk
   GIT_REPOSITORY   https://github.com/KhronosGroup/ANARI-SDK.git
   GIT_TAG          next_release
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/ANARI-SDK/src"
   BINARY_DIR       "${LIBS_DIR}/ANARI-SDK/build"
   INSTALL_DIR      "${LIBS_DIR}/ANARI-SDK/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_HELIDE_DEVICE=ON
      -DBUILD_TESTING=OFF
      -DBUILD_EXAMPLES=OFF
)

# ---------------------------------------------------------------------------
# OpenXR-SDK
# ---------------------------------------------------------------------------

ExternalProject_Add (openxr-sdk
   GIT_REPOSITORY   https://github.com/KhronosGroup/OpenXR-SDK.git
   GIT_TAG          release-1.1.58
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/OpenXR-SDK/src"
   BINARY_DIR       "${LIBS_DIR}/OpenXR-SDK/build"
   INSTALL_DIR      "${LIBS_DIR}/OpenXR-SDK/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DDYNAMIC_LOADER=OFF
)

# ---------------------------------------------------------------------------
# curl
# ---------------------------------------------------------------------------

if (WIN32)
   set (CURL_SSL_ARGS -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF)
else ()
   set (CURL_SSL_ARGS -DCURL_USE_SCHANNEL=OFF -DCURL_USE_OPENSSL=ON)
endif ()

ExternalProject_Add (curl
   GIT_REPOSITORY   https://github.com/curl/curl.git
   GIT_TAG          curl-8_9_1
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/curl/src"
   BINARY_DIR       "${LIBS_DIR}/curl/build"
   INSTALL_DIR      "${LIBS_DIR}/curl/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_CURL_EXE=OFF
      ${CURL_SSL_ARGS}
)

# ---------------------------------------------------------------------------
# RmlUi
# ---------------------------------------------------------------------------

ExternalProject_Add (rmlui
   GIT_REPOSITORY   https://github.com/mikke89/RmlUi.git
   GIT_TAG          6.2
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/RmlUi/src"
   BINARY_DIR       "${LIBS_DIR}/RmlUi/build"
   INSTALL_DIR      "${LIBS_DIR}/RmlUi/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_SAMPLES=OFF
      -DRMLUI_FONT_ENGINE=none
)

# ---------------------------------------------------------------------------
# Wasmtime (Rust/Cargo — custom build, not CMake)
# ---------------------------------------------------------------------------

if (WIN32)
   set (WASMTIME_COPY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "<SOURCE_DIR>/target/release/wasmtime.dll"
         "<INSTALL_DIR>/lib/wasmtime.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "<SOURCE_DIR>/target/release/wasmtime.dll.lib"
         "<INSTALL_DIR>/lib/wasmtime.dll.lib"
   )
else ()
   set (WASMTIME_COPY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "<SOURCE_DIR>/target/release/libwasmtime.so"
         "<INSTALL_DIR>/lib/libwasmtime.so"
   )
endif ()

# Cargo generates conf.h (from conf.h.in) inside a hashed build dir.
# This script copies source headers first, then overlays the generated
# headers so conf.h replaces the template.
file (WRITE "${CMAKE_BINARY_DIR}/wasmtime-install.cmake" [=[
cmake_minimum_required (VERSION 3.20)
file (MAKE_DIRECTORY "${DST}/include")
file (MAKE_DIRECTORY "${DST}/lib")
file (COPY "${SRC}/crates/c-api/include/" DESTINATION "${DST}/include")
file (GLOB _gen_dirs "${SRC}/target/release/build/wasmtime-c-api-impl-*/out/include")
list (LENGTH _gen_dirs _n)
if (_n GREATER 0)
   list (GET _gen_dirs 0 _gen)
   file (COPY "${_gen}/" DESTINATION "${DST}/include")
endif ()
]=])

ExternalProject_Add (wasmtime
   GIT_REPOSITORY   https://github.com/bytecodealliance/wasmtime.git
   GIT_TAG          v43.0.0
   GIT_SHALLOW      ON
   GIT_SUBMODULES   ""
   SOURCE_DIR       "${LIBS_DIR}/Wasmtime/src"
   BINARY_DIR       "${LIBS_DIR}/Wasmtime/build"
   INSTALL_DIR      "${LIBS_DIR}/Wasmtime/install"
   CONFIGURE_COMMAND ""
   BUILD_COMMAND     cargo build --release -p wasmtime-c-api --manifest-path <SOURCE_DIR>/Cargo.toml
   INSTALL_COMMAND
      ${CMAKE_COMMAND}
         -DSRC=<SOURCE_DIR>
         -DDST=<INSTALL_DIR>
         -P "${CMAKE_BINARY_DIR}/wasmtime-install.cmake"
      ${WASMTIME_COPY_CMDS}
   BUILD_IN_SOURCE   OFF
)

# ---------------------------------------------------------------------------
# nlohmann/json (header-only JSON library)
# ---------------------------------------------------------------------------

ExternalProject_Add (nlohmann-json
   GIT_REPOSITORY   https://github.com/nlohmann/json.git
   GIT_TAG          v3.11.3
   GIT_SHALLOW      ON
   SOURCE_DIR       "${LIBS_DIR}/nlohmann-json/src"
   BINARY_DIR       "${LIBS_DIR}/nlohmann-json/build"
   INSTALL_DIR      "${LIBS_DIR}/nlohmann-json/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DJSON_BuildTests=OFF
)

# ---------------------------------------------------------------------------
# Halogen (Filament-based ANARI implementation)
# ---------------------------------------------------------------------------

ExternalProject_Add (halogen
   GIT_REPOSITORY   https://github.com/MetaversalCorp/Halogen.git
   GIT_TAG          main
   SOURCE_DIR       "${LIBS_DIR}/Halogen/src"
   BINARY_DIR       "${LIBS_DIR}/Halogen/build"
   INSTALL_DIR      "${LIBS_DIR}/Halogen/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_TESTS=OFF
)

# ---------------------------------------------------------------------------
# Sneeze (the actual project — depends on all libs)
# ---------------------------------------------------------------------------

ExternalProject_Add (sneeze
   SOURCE_DIR       "${CMAKE_SOURCE_DIR}/src"
   BINARY_DIR       "${CMAKE_SOURCE_DIR}/build/sneeze"
   INSTALL_COMMAND   ""
   CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DLIBS_DIR=${LIBS_DIR}
   DEPENDS
      anari-sdk
      wasmtime
      spirv-tools
      glslang
      openxr-sdk
      curl
      rmlui
      nlohmann-json
      halogen
)
