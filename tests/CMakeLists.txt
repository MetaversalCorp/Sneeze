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

# ---------------------------------------------------------------------------
# Wasmtime test executable
# ---------------------------------------------------------------------------

add_executable (WasmTest WasmRuntimeTest.cpp)

target_include_directories (WasmTest PRIVATE ${WASMTIME_INCLUDE})
target_compile_definitions (WasmTest PRIVATE WASM_API_EXTERN= WASI_API_EXTERN=)
target_link_libraries (WasmTest PRIVATE ${WASMTIME_LIB})
if (WIN32)
   target_link_libraries (WasmTest PRIVATE ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt)
elseif (APPLE)
   target_link_libraries (WasmTest PRIVATE pthread dl)
elseif (ANDROID)
   # Android bionic includes pthread in libc — no separate -lpthread
   target_link_libraries (WasmTest PRIVATE dl m log)
else ()
   target_link_libraries (WasmTest PRIVATE pthread dl m)
endif ()

# ---------------------------------------------------------------------------
# SPIR-V test executable
# ---------------------------------------------------------------------------

add_executable (SpvTest SpvPipelineTest.cpp)

target_include_directories (SpvTest PRIVATE
   ${SPIRV_TOOLS_INCLUDE}
)
target_link_libraries (SpvTest PRIVATE
   ${SPIRV_TOOLS_LIB}
   ${SPIRV_TOOLS_OPT_LIB}
)

# ---------------------------------------------------------------------------
# OpenXR test executable — skipped when XR is disabled. iOS forces it off
# (no VR support on iOS); consumers can also -DSNEEZE_ENABLE_XR=OFF on
# other platforms (e.g. plain Android APK without OpenXR loader).
# ---------------------------------------------------------------------------

if (SNEEZE_ENABLE_XR)
   add_executable (XrTest XrRuntimeTest.cpp)

   target_include_directories (XrTest PRIVATE ${OPENXR_INCLUDE})
   target_link_libraries (XrTest PRIVATE ${OPENXR_LOADER_LIB})
   if (WIN32)
      target_link_libraries (XrTest PRIVATE advapi32)
   elseif (ANDROID)
      # openxr_loader uses Android AssetManager + logging
      target_link_libraries (XrTest PRIVATE android log)
   endif ()
endif ()

# ---------------------------------------------------------------------------
# curl test executable
# ---------------------------------------------------------------------------

add_executable (NetTest HttpClientTest.cpp)

target_include_directories (NetTest PRIVATE ${CURL_INCLUDE})
target_compile_definitions (NetTest PRIVATE CURL_STATICLIB)
target_link_libraries (NetTest PRIVATE ${CURL_LIB})
if (WIN32)
   target_link_libraries (NetTest PRIVATE ws2_32 wldap32 crypt32 normaliz advapi32)
elseif (APPLE)
   find_library (_NET_SECURITY_FW Security)
   find_library (_NET_CF_FW CoreFoundation)
   find_library (_NET_SC_FW SystemConfiguration)
   target_link_libraries (NetTest PRIVATE pthread
      ${_NET_SECURITY_FW} ${_NET_CF_FW} ${_NET_SC_FW})
elseif (ANDROID)
   # curl on Android links against our BoringSSL artifact (replaces OpenSSL);
   # no system OpenSSL search needed. pthread is in libc.
   set (_ANDROID_BORINGSSL "${LIBS_DIR}/boringssl/install")
   target_link_libraries (NetTest PRIVATE
      "${_ANDROID_BORINGSSL}/lib/libssl.a"
      "${_ANDROID_BORINGSSL}/lib/libcrypto.a"
      dl log)
else ()
   find_package (OpenSSL QUIET)
   find_package (ZLIB QUIET)
   if (OpenSSL_FOUND)
      target_link_libraries (NetTest PRIVATE OpenSSL::SSL OpenSSL::Crypto)
   endif ()
   if (ZLIB_FOUND)
      target_link_libraries (NetTest PRIVATE ZLIB::ZLIB)
   endif ()
   target_link_libraries (NetTest PRIVATE pthread dl)
endif ()

# ---------------------------------------------------------------------------
# RmlUi test executable
# ---------------------------------------------------------------------------

add_executable (UiTest UiContextTest.cpp)

target_include_directories (UiTest PRIVATE ${RMLUI_INCLUDE})
target_compile_definitions (UiTest PRIVATE RMLUI_STATIC_LIB)
target_link_libraries (UiTest PRIVATE
   ${RMLUI_CORE_LIB}
   ${RMLUI_DEBUGGER_LIB}
)

# ---------------------------------------------------------------------------
# Compute test executable
# ---------------------------------------------------------------------------

set (SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable (ComputeTest ComputeTest.cpp)
target_include_directories (ComputeTest PRIVATE ${SRC_DIR})
target_link_libraries (ComputeTest PRIVATE Sneeze anari::anari_static)

# ---------------------------------------------------------------------------
# Vox test executable — verifies Vox links through Sneeze and that the
# device factory produces a backend on machines with a GPU driver. The
# test skips (without failing) on headless runners. Mirrors the upstream
# VoxTest so we catch API drift when Vox is rolled forward.
# ---------------------------------------------------------------------------

add_executable (VoxTest VoxTest.cpp)
target_link_libraries (VoxTest PRIVATE Sneeze)

# ---------------------------------------------------------------------------
# JWS test executable — exercises sign/verify, x5c chain validation, and
# fingerprint stability using fixtures under tests/certs/.
# ---------------------------------------------------------------------------

add_executable (JwsTest JwsTest.cpp)
target_link_libraries (JwsTest PRIVATE Sneeze)

# Tests load cert fixtures via the relative path "tests/certs/..." — run
# from the repo root, or from build-sneeze/tests/ with a symlink; CTest
# here sets WORKING_DIRECTORY to the repo root so ReadFile() resolves.
set (_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include (CTest)
add_test (NAME JwsTest COMMAND JwsTest WORKING_DIRECTORY "${_REPO_ROOT}")

# ---------------------------------------------------------------------------
# GenCerts — regenerates test CA + leaf + expired certs under tests/certs/.
# Linked against Sneeze so it picks up BoringSSL transitively. Not run as
# part of CTest — invoke manually when fixtures need to be refreshed.
# ---------------------------------------------------------------------------

add_executable (GenCerts GenCerts.cpp)
target_link_libraries (GenCerts PRIVATE Sneeze)

# ---------------------------------------------------------------------------
# Copy runtime DLLs to test output directories
# ---------------------------------------------------------------------------

if (WIN32)
   add_custom_command (TARGET WasmTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "${WASMTIME_ROOT}/lib/wasmtime.dll"
         $<TARGET_FILE_DIR:WasmTest>
   )
endif ()
