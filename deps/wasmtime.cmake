# ---------------------------------------------------------------------------
# Wasmtime (Rust/Cargo -- custom build, not CMake)
# ---------------------------------------------------------------------------
# Always built in Cargo release mode regardless of SNEEZE_CONFIG. Debug
# Rust builds produce ~500 MB of artifacts and run ~50x slower; Sneeze
# never needs to step into Wasmtime internals.

# Map CMake target to Rust target triple for cross-compilation.
# Special value "universal-apple-darwin" triggers a macOS fat-binary build
# (arm64 + x86_64 built separately, then lipo'd together).
set (WASMTIME_CARGO_TARGET "" CACHE STRING "Rust target triple for Wasmtime cross-compilation")
option (WASMTIME_MACOS_UNIVERSAL "Build Wasmtime as a macOS universal (arm64+x86_64) dylib" OFF)

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_wasmtime}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_wasmtime}
      GIT_TAG        ${DEP_REF_wasmtime}
      GIT_SHALLOW    ON
      GIT_SUBMODULES ""
   )
endif ()

if (WASMTIME_MACOS_UNIVERSAL)
   # Build both arches, lipo into a universal dylib.
   file (WRITE "${CMAKE_BINARY_DIR}/wasmtime-lipo.cmake" [=[
cmake_minimum_required (VERSION 3.20)
file (MAKE_DIRECTORY "${DST}/lib")
execute_process (
   COMMAND lipo -create
      "${SRC}/target/aarch64-apple-darwin/release/libwasmtime.dylib"
      "${SRC}/target/x86_64-apple-darwin/release/libwasmtime.dylib"
      -output "${DST}/lib/libwasmtime.dylib"
   RESULT_VARIABLE _r
)
if (_r)
   message (FATAL_ERROR "lipo failed: ${_r}")
endif ()
]=])

   file (WRITE "${CMAKE_BINARY_DIR}/wasmtime-install.cmake" [=[
cmake_minimum_required (VERSION 3.20)
file (MAKE_DIRECTORY "${DST}/include")
file (MAKE_DIRECTORY "${DST}/lib")
file (COPY "${SRC}/crates/c-api/include/" DESTINATION "${DST}/include")
foreach (_td "aarch64-apple-darwin/release" "release")
   file (GLOB _gen_dirs "${SRC}/target/${_td}/build/wasmtime-c-api-impl-*/out/include")
   if (_gen_dirs)
      list (GET _gen_dirs 0 _gen)
      file (COPY "${_gen}/" DESTINATION "${DST}/include")
      break ()
   endif ()
endforeach ()
]=])

   # Locate cargo
   if (NOT CARGO_EXECUTABLE)
      find_program (CARGO_EXECUTABLE cargo
         PATHS
            "$ENV{HOME}/.cargo/bin"
            "/root/.cargo/bin"
            "/usr/local/bin"
            "/usr/bin"
         NO_DEFAULT_PATH
      )
      if (NOT CARGO_EXECUTABLE)
         find_program (CARGO_EXECUTABLE cargo)
      endif ()
   endif ()
   if (NOT CARGO_EXECUTABLE)
      message (FATAL_ERROR "cargo not found. Install Rust: https://rustup.rs")
   endif ()
   message (STATUS "Using cargo: ${CARGO_EXECUTABLE}")

   ExternalProject_Add (wasmtime
      ${_git_args}
      SOURCE_DIR       "${_repo}"
      BINARY_DIR       "${LIBS_DIR}/Wasmtime/build"
      INSTALL_DIR      "${LIBS_DIR}/Wasmtime/install"
      CONFIGURE_COMMAND ""
      BUILD_COMMAND
         ${CARGO_EXECUTABLE} build --release -p wasmtime-c-api
            --manifest-path <SOURCE_DIR>/Cargo.toml
            --target aarch64-apple-darwin
         COMMAND
         ${CARGO_EXECUTABLE} build --release -p wasmtime-c-api
            --manifest-path <SOURCE_DIR>/Cargo.toml
            --target x86_64-apple-darwin
      INSTALL_COMMAND
         ${CMAKE_COMMAND}
            -DSRC=<SOURCE_DIR>
            -DDST=<INSTALL_DIR>
            -P "${CMAKE_BINARY_DIR}/wasmtime-install.cmake"
         COMMAND ${CMAKE_COMMAND}
            -DSRC=<SOURCE_DIR>
            -DDST=<INSTALL_DIR>
            -P "${CMAKE_BINARY_DIR}/wasmtime-lipo.cmake"
      BUILD_IN_SOURCE   OFF
   )

else ()

set (WASMTIME_TARGET_DIR "release")
set (_wasmtime_cargo_root "")
if (WASMTIME_CARGO_TARGET)
   set (WASMTIME_TARGET_DIR "${WASMTIME_CARGO_TARGET}/release")
   # Isolate Cargo artifacts from a host (Windows/macOS) Wasmtime build in
   # the shared deps/repos/Wasmtime/target tree.
   set (_wasmtime_cargo_root "${LIBS_DIR}/Wasmtime/cargo-target")
   file (TO_CMAKE_PATH "${_wasmtime_cargo_root}" _wasmtime_cargo_root)
   set (WASMTIME_CARGO_TARGET_ARG --target ${WASMTIME_CARGO_TARGET} --target-dir ${_wasmtime_cargo_root})
else ()
   set (WASMTIME_CARGO_TARGET_ARG)
endif ()

# Determine library filename based on target
if (WIN32 AND NOT WASMTIME_CARGO_TARGET)
   set (WASMTIME_COPY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "<SOURCE_DIR>/target/${WASMTIME_TARGET_DIR}/wasmtime.dll"
         "<INSTALL_DIR>/lib/wasmtime.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "<SOURCE_DIR>/target/${WASMTIME_TARGET_DIR}/wasmtime.dll.lib"
         "<INSTALL_DIR>/lib/wasmtime.dll.lib"
   )
elseif (APPLE OR (WASMTIME_CARGO_TARGET MATCHES "apple"))
   # iOS requires static library (no custom dylibs allowed)
   if (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR (WASMTIME_CARGO_TARGET MATCHES "ios"))
      set (WASMTIME_COPY_CMDS
         COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "<SOURCE_DIR>/target/${WASMTIME_TARGET_DIR}/libwasmtime.a"
            "<INSTALL_DIR>/lib/libwasmtime.a"
      )
   else ()
      set (WASMTIME_COPY_CMDS
         COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "<SOURCE_DIR>/target/${WASMTIME_TARGET_DIR}/libwasmtime.dylib"
            "<INSTALL_DIR>/lib/libwasmtime.dylib"
      )
   endif ()
elseif (ANDROID OR (WASMTIME_CARGO_TARGET MATCHES "android"))
   if (_wasmtime_cargo_root)
      set (_wasmtime_lib_src "${_wasmtime_cargo_root}/${WASMTIME_CARGO_TARGET}/release")
   else ()
      set (_wasmtime_lib_src "<SOURCE_DIR>/target/${WASMTIME_TARGET_DIR}")
   endif ()
   file (TO_CMAKE_PATH "${_wasmtime_lib_src}" _wasmtime_lib_src_cmake)
   if (NOT _wasmtime_lib_src MATCHES "^<SOURCE_DIR>")
      set (_wasmtime_lib_src "${_wasmtime_lib_src_cmake}")
   endif ()
   file (WRITE "${CMAKE_BINARY_DIR}/wasmtime-copy-lib.cmake" [=[
cmake_minimum_required (VERSION 3.20)
file (MAKE_DIRECTORY "${DST}/lib")
if (EXISTS "${LIB_SRC}/libwasmtime.a")
   file (COPY_FILE "${LIB_SRC}/libwasmtime.a" "${DST}/lib/libwasmtime.a" ONLY_IF_DIFFERENT)
elseif (EXISTS "${LIB_SRC}/libwasmtime.so")
   file (COPY_FILE "${LIB_SRC}/libwasmtime.so" "${DST}/lib/libwasmtime.so" ONLY_IF_DIFFERENT)
else ()
   message (FATAL_ERROR "Wasmtime Android build produced neither libwasmtime.a nor libwasmtime.so in ${LIB_SRC}")
endif ()
]=])
   set (WASMTIME_COPY_CMDS
      COMMAND ${CMAKE_COMMAND}
         "-DLIB_SRC=${_wasmtime_lib_src}"
         "-DDST=<INSTALL_DIR>"
         -P "${CMAKE_BINARY_DIR}/wasmtime-copy-lib.cmake"
   )
else ()
   set (WASMTIME_COPY_CMDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "<SOURCE_DIR>/target/${WASMTIME_TARGET_DIR}/libwasmtime.so"
         "<INSTALL_DIR>/lib/libwasmtime.so"
   )
endif ()

# Cargo generates conf.h (from conf.h.in) inside a hashed build dir.
file (WRITE "${CMAKE_BINARY_DIR}/wasmtime-install.cmake" [=[
cmake_minimum_required (VERSION 3.20)
file (MAKE_DIRECTORY "${DST}/include")
file (MAKE_DIRECTORY "${DST}/lib")
file (COPY "${SRC}/crates/c-api/include/" DESTINATION "${DST}/include")
file (GLOB _gen_dirs "${SRC}/target/${TARGET_DIR}/build/wasmtime-c-api-impl-*/out/include")
if (NOT _gen_dirs AND DEFINED CARGO_ROOT AND NOT CARGO_ROOT STREQUAL "")
   file (GLOB _gen_dirs "${CARGO_ROOT}/${TARGET_DIR}/build/wasmtime-c-api-impl-*/out/include")
endif ()
if (NOT _gen_dirs)
   file (GLOB _gen_dirs "${SRC}/target/release/build/wasmtime-c-api-impl-*/out/include")
endif ()
list (LENGTH _gen_dirs _n)
if (_n GREATER 0)
   list (GET _gen_dirs 0 _gen)
   file (COPY "${_gen}/" DESTINATION "${DST}/include")
endif ()
]=])

# Locate cargo: prefer env var override, then common install locations.
# The SuperBuild runs via /bin/sh which may not source ~/.cargo/env.
if (NOT CARGO_EXECUTABLE)
   find_program (CARGO_EXECUTABLE cargo
      PATHS
         "$ENV{HOME}/.cargo/bin"
         "/root/.cargo/bin"
         "/usr/local/bin"
         "/usr/bin"
      NO_DEFAULT_PATH
   )
   if (NOT CARGO_EXECUTABLE)
      find_program (CARGO_EXECUTABLE cargo)
   endif ()
endif ()
if (NOT CARGO_EXECUTABLE)
   message (FATAL_ERROR "cargo not found. Install Rust: https://rustup.rs")
endif ()
message (STATUS "Using cargo: ${CARGO_EXECUTABLE}")

# ---------------------------------------------------------------------------
# Cross-compilation linker setup for Cargo
# ---------------------------------------------------------------------------
# Android: Cargo needs NDK clang as linker for the target triple.
# iOS: Cargo needs cc from Xcode SDK.
# We generate a .cargo/config.toml next to the Wasmtime source to configure
# the linker for cross-compilation targets.

set (WASMTIME_BUILD_ENV)

if (WASMTIME_CARGO_TARGET MATCHES "aarch64-linux-android")
   # Find NDK clang
   if (CMAKE_ANDROID_NDK)
      # NDK r25+ prebuilt clang path
      set (_ndk_host "linux-x86_64")
      if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
         set (_ndk_host "darwin-x86_64")
      elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
         set (_ndk_host "windows-x86_64")
      endif ()
      set (_ndk_clang "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${_ndk_host}/bin/aarch64-linux-android26-clang")
      if (CMAKE_HOST_WIN32 AND EXISTS "${_ndk_clang}.cmd")
         # Cargo on Windows must invoke the .cmd wrapper; the extensionless
         # clang is a GNU driver that produces a PE wasmtime.dll.
         set (_android_linker "${_ndk_clang}.cmd")
      elseif (EXISTS "${_ndk_clang}")
         set (_android_linker "${_ndk_clang}")
      else ()
         # Fallback: try generic clang
         set (_android_linker "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${_ndk_host}/bin/clang")
      endif ()
   else ()
      find_program (_android_linker aarch64-linux-android26-clang)
   endif ()
   if (_android_linker)
      message (STATUS "Wasmtime Android linker: ${_android_linker}")
      # Write Cargo config for Android cross-linker
      file (WRITE "${CMAKE_BINARY_DIR}/wasmtime-cargo-config.toml"
         "[target.aarch64-linux-android]\nlinker = \"${_android_linker}\"\n"
      )
      # cc crate also needs CC for C compilation in build scripts (e.g. zstd-sys)
      set (_ndk_ar "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${_ndk_host}/bin/llvm-ar")
      set (WASMTIME_BUILD_ENV
         ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=${_android_linker}"
            "CC_aarch64-linux-android=${_android_linker}"
            "AR_aarch64-linux-android=${_ndk_ar}"
      )
      if (_wasmtime_cargo_root)
         set (WASMTIME_BUILD_ENV ${WASMTIME_BUILD_ENV} "CARGO_TARGET_DIR=${_wasmtime_cargo_root}")
      endif ()
   else ()
      message (WARNING "Android NDK clang not found -- Wasmtime cross-compile may fail")
   endif ()

elseif (WASMTIME_CARGO_TARGET MATCHES "aarch64-apple-ios")
   # iOS: Cargo uses cc which finds Xcode SDK automatically via xcrun.
   # Set SDKROOT so cc picks the right sysroot.
   if (CMAKE_OSX_SYSROOT)
      set (WASMTIME_BUILD_ENV
         ${CMAKE_COMMAND} -E env "SDKROOT=${CMAKE_OSX_SYSROOT}"
      )
   endif ()
endif ()

ExternalProject_Add (wasmtime
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/Wasmtime/build"
   INSTALL_DIR      "${LIBS_DIR}/Wasmtime/install"
   CONFIGURE_COMMAND ""
   BUILD_COMMAND     ${WASMTIME_BUILD_ENV}
                     ${CARGO_EXECUTABLE} build --release -p wasmtime-c-api
                        --manifest-path <SOURCE_DIR>/Cargo.toml
                        ${WASMTIME_CARGO_TARGET_ARG}
   INSTALL_COMMAND
      ${CMAKE_COMMAND}
         -DSRC=<SOURCE_DIR>
         -DDST=<INSTALL_DIR>
         "-DTARGET_DIR=${WASMTIME_TARGET_DIR}"
         "-DCARGO_ROOT=${_wasmtime_cargo_root}"
         -P "${CMAKE_BINARY_DIR}/wasmtime-install.cmake"
      ${WASMTIME_COPY_CMDS}
   BUILD_IN_SOURCE   OFF
)

endif () # WASMTIME_MACOS_UNIVERSAL
