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
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
   cmake_policy (SET CMP0144 NEW)
endif ()
project (Sneeze VERSION 0.1.0 LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

option (USE_LOCAL_DEPS "Find dependencies in libs/ (ON) or via vcpkg find_package (OFF)" ON)

if (NOT DEFINED LIBS_DIR)
   set (LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs")
endif ()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

if (USE_LOCAL_DEPS)
   # ANARI — built from source in libs/
   set (ANARI_ROOT "${LIBS_DIR}/ANARI-SDK/install")
   list (APPEND CMAKE_PREFIX_PATH "${ANARI_ROOT}")
   find_package (anari REQUIRED)
else ()
   find_package (anari REQUIRED)
endif ()

# Wasmtime — always from libs/ (not available in vcpkg)
set (WASMTIME_ROOT "${LIBS_DIR}/Wasmtime/install")
find_library (WASMTIME_LIB NAMES wasmtime wasmtime.dll PATHS "${WASMTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_path (WASMTIME_INCLUDE wasmtime.h PATHS "${WASMTIME_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

if (USE_LOCAL_DEPS)
   # SPIRV-Tools
   set (SPIRV_TOOLS_ROOT "${LIBS_DIR}/SPIRV-Tools/install")
   find_library (SPIRV_TOOLS_LIB SPIRV-Tools PATHS "${SPIRV_TOOLS_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   find_library (SPIRV_TOOLS_OPT_LIB SPIRV-Tools-opt PATHS "${SPIRV_TOOLS_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   find_path (SPIRV_TOOLS_INCLUDE spirv-tools/libspirv.h PATHS "${SPIRV_TOOLS_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

   # OpenXR
   set (OPENXR_ROOT "${LIBS_DIR}/OpenXR-SDK/install")
   find_library (OPENXR_LOADER_LIB openxr_loader PATHS "${OPENXR_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   find_path (OPENXR_INCLUDE openxr/openxr.h PATHS "${OPENXR_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

   # curl
   set (CURL_ROOT "${LIBS_DIR}/curl/install")
   find_library (CURL_LIB libcurl PATHS "${CURL_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   find_path (CURL_INCLUDE curl/curl.h PATHS "${CURL_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

   # RmlUi
   set (RMLUI_ROOT "${LIBS_DIR}/RmlUi/install")
   find_library (RMLUI_CORE_LIB rmlui PATHS "${RMLUI_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   find_library (RMLUI_DEBUGGER_LIB rmlui_debugger PATHS "${RMLUI_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   find_path (RMLUI_INCLUDE RmlUi/Core.h PATHS "${RMLUI_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

   # glslang (build-time tool only)
   set (GLSLANG_ROOT "${LIBS_DIR}/glslang/install")
   find_program (GLSLANG_VALIDATOR glslang PATHS "${GLSLANG_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)

   # nlohmann/json (header-only)
   set (NLOHMANN_JSON_ROOT "${LIBS_DIR}/nlohmann-json/install")
   find_path (NLOHMANN_JSON_INCLUDE nlohmann/json.hpp PATHS "${NLOHMANN_JSON_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
else ()
   find_package (spirv_tools REQUIRED CONFIG)
   find_package (OpenXR REQUIRED CONFIG)
   find_package (CURL REQUIRED)
   find_package (RmlUi REQUIRED)
   find_program (GLSLANG_VALIDATOR glslang REQUIRED)
   find_package (nlohmann_json REQUIRED)
endif ()

# ---------------------------------------------------------------------------
# Compile GLSL compute kernels to SPIR-V at build time
# ---------------------------------------------------------------------------

set (KERNEL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/compute/kernels")
set (KERNEL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file (MAKE_DIRECTORY "${KERNEL_OUTPUT_DIR}")

set (KERNEL_SOURCES
   "${KERNEL_SOURCE_DIR}/test_proximity.comp"
)

set (KERNEL_SPV_OUTPUTS)
foreach (KERNEL_SRC ${KERNEL_SOURCES})
   get_filename_component (KERNEL_NAME "${KERNEL_SRC}" NAME_WE)
   set (KERNEL_SPV "${KERNEL_OUTPUT_DIR}/${KERNEL_NAME}.spv")
   add_custom_command (
      OUTPUT "${KERNEL_SPV}"
      COMMAND "${GLSLANG_VALIDATOR}" -V "${KERNEL_SRC}" -o "${KERNEL_SPV}"
      DEPENDS "${KERNEL_SRC}"
      COMMENT "Compiling ${KERNEL_NAME}.comp -> ${KERNEL_NAME}.spv"
      VERBATIM
   )
   list (APPEND KERNEL_SPV_OUTPUTS "${KERNEL_SPV}")
endforeach ()

add_custom_target (CompileKernels ALL DEPENDS ${KERNEL_SPV_OUTPUTS})

# ---------------------------------------------------------------------------
# Generate Win32 resource file for SPIR-V embedding
# ---------------------------------------------------------------------------

set (KERNEL_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/kernels.rc")
file (WRITE "${KERNEL_RC_FILE}" "// Auto-generated by CMake — embeds compiled SPIR-V kernels\n")
foreach (KERNEL_SRC ${KERNEL_SOURCES})
   get_filename_component (KERNEL_NAME "${KERNEL_SRC}" NAME_WE)
   string (TOUPPER "${KERNEL_NAME}" KERNEL_ID)
   file (APPEND "${KERNEL_RC_FILE}"
      "${KERNEL_ID} SPV_KERNEL \"${KERNEL_OUTPUT_DIR}/${KERNEL_NAME}.spv\"\n"
   )
endforeach ()

# ---------------------------------------------------------------------------
# Source files by module
# ---------------------------------------------------------------------------

set (CORE_SOURCES    core/Epoch.cpp)
set (CORE_HEADERS    core/Epoch.h  core/Types.h)

set (RENDERER_SOURCES  renderer/AnariRenderer.cpp)
set (RENDERER_HEADERS  renderer/Renderer.h  renderer/AnariRenderer.h)

set (VIEW_SOURCES    view/CameraOrbit.cpp)
set (VIEW_HEADERS    view/CameraOrbit.h)

set (WASM_SOURCES    wasm/WasmRuntime.cpp)
set (WASM_HEADERS    wasm/WasmRuntime.h)

set (SPIRV_SOURCES   spirv/SpvPipeline.cpp)
set (SPIRV_HEADERS   spirv/SpvPipeline.h)

set (XR_SOURCES      xr/XrRuntime.cpp)
set (XR_HEADERS      xr/XrRuntime.h)

set (NET_SOURCES     net/HttpClient.cpp)
set (NET_HEADERS     net/HttpClient.h)

set (UI_SOURCES      ui/UiContext.cpp)
set (UI_HEADERS      ui/UiContext.h)

set (COMPUTE_SOURCES compute/EmbeddedKernels.cpp)
set (COMPUTE_HEADERS compute/EmbeddedKernels.h)

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------

add_library (Sneeze STATIC
   ${CORE_SOURCES}      ${CORE_HEADERS}
   ${RENDERER_SOURCES}  ${RENDERER_HEADERS}
   ${VIEW_SOURCES}      ${VIEW_HEADERS}
   ${WASM_SOURCES}      ${WASM_HEADERS}
   ${SPIRV_SOURCES}     ${SPIRV_HEADERS}
   ${XR_SOURCES}        ${XR_HEADERS}
   ${NET_SOURCES}       ${NET_HEADERS}
   ${UI_SOURCES}        ${UI_HEADERS}
   ${COMPUTE_SOURCES}   ${COMPUTE_HEADERS}
   ${KERNEL_RC_FILE}
)
add_dependencies (Sneeze CompileKernels)

target_include_directories (Sneeze PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${WASMTIME_INCLUDE}
   ${SPIRV_TOOLS_INCLUDE}
   ${OPENXR_INCLUDE}
   ${CURL_INCLUDE}
   ${RMLUI_INCLUDE}
   ${NLOHMANN_JSON_INCLUDE}
)

target_compile_definitions (Sneeze PUBLIC
   WASM_API_EXTERN=
   WASI_API_EXTERN=
   CURL_STATICLIB
   RMLUI_STATIC_LIB
)

target_link_libraries (Sneeze PUBLIC
   anari::anari_static
   anari::anari_library_helide
   ${WASMTIME_LIB}
   ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt
   ${SPIRV_TOOLS_LIB}
   ${SPIRV_TOOLS_OPT_LIB}
   ${OPENXR_LOADER_LIB}
   ${CURL_LIB}
   wldap32 crypt32 normaliz
   ${RMLUI_CORE_LIB}
   ${RMLUI_DEBUGGER_LIB}
)

# ---------------------------------------------------------------------------
# IDE source grouping
# ---------------------------------------------------------------------------

source_group ("core"     FILES ${CORE_SOURCES} ${CORE_HEADERS})
source_group ("renderer" FILES ${RENDERER_SOURCES} ${RENDERER_HEADERS})
source_group ("view"     FILES ${VIEW_SOURCES} ${VIEW_HEADERS})
source_group ("wasm"     FILES ${WASM_SOURCES} ${WASM_HEADERS})
source_group ("spirv"    FILES ${SPIRV_SOURCES} ${SPIRV_HEADERS})
source_group ("xr"       FILES ${XR_SOURCES} ${XR_HEADERS})
source_group ("net"      FILES ${NET_SOURCES} ${NET_HEADERS})
source_group ("ui"       FILES ${UI_SOURCES} ${UI_HEADERS})
source_group ("compute"  FILES ${COMPUTE_SOURCES} ${COMPUTE_HEADERS})

# ---------------------------------------------------------------------------
# Tests (separate directory, nested build output under build/tests/)
# ---------------------------------------------------------------------------

add_subdirectory ("${CMAKE_CURRENT_SOURCE_DIR}/../tests" "${CMAKE_CURRENT_BINARY_DIR}/tests")
