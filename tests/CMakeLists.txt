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
# SneezeTest — unified test executable. Run with no args to execute all
# suites, or pass --wasm, --spv, --xr, --net, --ui, --compute, --vox,
# --jws, --cache to select individual suites.
# ---------------------------------------------------------------------------

set (TEST_SOURCES
   TestRunner.cpp
   SpvPipelineTest.cpp
   HttpClientTest.cpp
   UiContextTest.cpp
   ComputeTest.cpp
   VoxTest.cpp
   JwsTest.cpp
   NetworkTest.cpp
   StorageTest.cpp
   ConsoleTest.cpp
   GltfTest.cpp
   BasisTest.cpp
)

if (SNEEZE_ENABLE_WASM)
   list (APPEND TEST_SOURCES WasmRuntimeTest.cpp)
endif ()
if (SNEEZE_ENABLE_XR)
   list (APPEND TEST_SOURCES XrRuntimeTest.cpp)
endif ()

add_executable (SneezeTest ${TEST_SOURCES})
target_link_libraries (SneezeTest PRIVATE Sneeze)
target_compile_definitions (SneezeTest PRIVATE SNEEZE_TEST_CERTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/certs")
target_compile_definitions (SneezeTest PRIVATE SNEEZE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

set (_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include (CTest)
add_test (NAME SneezeTest COMMAND SneezeTest WORKING_DIRECTORY "${_REPO_ROOT}")

# ---------------------------------------------------------------------------
# GenCerts — regenerates test CA + leaf + expired certs under tests/certs/.
# Linked against Sneeze so it picks up BoringSSL transitively. Not run as
# part of CTest — invoke manually when fixtures need to be refreshed.
# ---------------------------------------------------------------------------

add_executable (GenCerts GenCerts.cpp)
target_link_libraries (GenCerts PRIVATE Sneeze)

# ---------------------------------------------------------------------------
# Copy runtime DLLs to test output directory
# ---------------------------------------------------------------------------

if (WIN32 AND SNEEZE_ENABLE_WASM)
   add_custom_command (TARGET SneezeTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "${WASMTIME_ROOT}/lib/wasmtime.dll"
         $<TARGET_FILE_DIR:SneezeTest>
   )
endif ()
