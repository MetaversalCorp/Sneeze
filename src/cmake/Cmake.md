# CMake — Find Modules and Build Configuration

The `cmake` directory contains custom `Find*.cmake` modules used by the Sneeze
build system to locate third-party dependencies. Invoked by `src/CMakeLists.txt`
via `find_package()`.

## Modules

| File | Dependency | Purpose |
|------|-----------|---------|
| `FindBoringSSL.cmake` | BoringSSL | Crypto: SHA-256, X.509, RSA/ECDSA |
| `FindSneezeCurl.cmake` | libcurl | HTTP client (network module) |
| `FindSneezeOpenXR.cmake` | OpenXR SDK | XR runtime detection |
| `FindWasmtime.cmake` | Wasmtime | WASM compilation and execution |
| `FindJwtCpp.cmake` | jwt-cpp | JWS/JWT parsing (MSF module) |
| `FindSneezeRmlUi.cmake` | RmlUi | UI toolkit |
| `FindSneezeFreeType.cmake` | FreeType | Font rasterization (dep of RmlUi) |
| `FindNlohmannJson.cmake` | nlohmann/json | JSON parsing |
| `FindSpirvTools.cmake` | SPIRV-Tools | SPIR-V validation |
| `FindGlslang.cmake` | glslang | GLSL-to-SPIR-V compilation |
| `FindCapture.cmake` | Capture | Portable live-camera lib (`LIBS_DIR/Capture/install`, MetaversalCorp/Capture) |

## Convention

Each module sets:
- `<NAME>_FOUND` — `TRUE` if located
- `<NAME>_INCLUDE_DIRS` — header search paths
- `<NAME>_LIBRARIES` — library files to link

Dependencies are found in `${LIBS_DIR}/<Name>/install/`.

## Multi-Config Rewriter

`src/CMakeLists.txt` contains a path generalization block that rewrites dep
paths using `$<LOWER_CASE:$<CONFIG>>` generator expressions, allowing a single
CMake tree to emit both Debug and Release. The `_sneeze_resolve_variant` helper
probes for Debug-suffix filename variants (`libcurl-d.lib`, `openxr_loaderd.lib`).
