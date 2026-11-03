# Halogen tracks the outer SNEEZE_CONFIG. Halogen still ships as
# anari_library_halogen.dll and crosses the ANARI plain-C ABI, so Sneeze
# Debug loading a Release halogen.dll (or vice versa) continues to work --
# but building Debug Halogen (with matching symbols + full STL iterator
# debugging) is useful when actually stepping into halogen code. This
# used to be blocked by filament's hybrid-CRT Debug build; that is fixed
# in deps/filament.cmake by disabling FILAMENT_SHORTEN_MSVC_COMPILATION
# on Debug.

# Select rendering backend per platform. Vulkan everywhere except iOS because
# halogen's iOS path doesn't link bluevk/bluegl.
if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (FILAMENT_BACKEND_ARGS -DFILAMENT_SUPPORTS_METAL=ON -DFILAMENT_SUPPORTS_VULKAN=OFF)
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (FILAMENT_BACKEND_ARGS -DFILAMENT_SUPPORTS_METAL=ON -DFILAMENT_SUPPORTS_VULKAN=OFF)
else ()
   set (FILAMENT_BACKEND_ARGS -DFILAMENT_SUPPORTS_VULKAN=ON)
endif ()

if (WIN32)
   set (FILAMENT_CRT_ARGS -DUSE_STATIC_CRT=OFF -DDIST_DIR=x86_64/md)
   # MSVC_RUNTIME_LIBRARY via generator expression so inner multi-config
   # picks /MDd for Debug and /MD for Release, matching filament's CRT.
   set (MSVC_CRT_ARGS
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
      "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()

# Cross-compile: the Android/iOS toolchain restricts find_package to the
# sysroot via CMAKE_FIND_ROOT_PATH_MODE_PACKAGE -- set BOTH so halogen's
# find_package(anari) sees ANARI_ROOT pointing into our libs-* install tree.
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (HALOGEN_FIND_ARGS -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH)
endif ()

# Debug Halogen's material compilation is dominated by matc runtime. Debug
# matc on Windows runs roughly 10-100x slower than Release matc
# (unoptimized codegen + MSVC _ITERATOR_DEBUG_LEVEL=2 on the SPIR-V
# optimizer's std::vector<uint32_t>-heavy passes). Preset FILAMENT_MATC to
# the Release filament's matc so Halogen's find_program(FILAMENT_MATC matc
# HINTS ...) picks up the Release binary -- find_program is a no-op when
# the cache variable is already set. Release matc links the Release CRT
# (MSVCP140.dll, not MSVCP140D.dll) so it runs standalone regardless of
# the outer config's toolset state.
#
# Release filament is thus a prerequisite for Debug Halogen, enforced
# here at configure time with a clear build-this-first hint.
set (HALOGEN_MATC_ARGS)
# Explicit -DFILAMENT_MATC= (Android/iOS host matc) wins. Otherwise Debug
# host builds reuse Release matc as before.
if (FILAMENT_MATC)
   list (APPEND HALOGEN_MATC_ARGS -DFILAMENT_MATC=${FILAMENT_MATC})
elseif (SNEEZE_CONFIG STREQUAL "Debug" AND NOT CMAKE_CROSSCOMPILING)
   get_filename_component (_sneeze_build_root "${LIBS_DIR}/../.." ABSOLUTE)
   set (_release_matc
      "${_sneeze_build_root}/release/libs/filament/install/bin/matc${CMAKE_EXECUTABLE_SUFFIX}")
   if (NOT EXISTS "${_release_matc}")
      if (WIN32)
         set (_build_hint "scripts/build-windows.ps1 -Deps -Only filament -Config Release")
      elseif (APPLE)
         set (_build_hint "scripts/build-macos.sh --deps --only filament --config Release")
      else ()
         set (_build_hint "scripts/build-linux.sh --deps --only filament --config Release")
      endif ()
      message (FATAL_ERROR
         "Halogen Debug requires Release filament to be built first.\n"
         "Expected matc at:\n"
         "  ${_release_matc}\n"
         "Run:\n"
         "  ${_build_hint}")
   endif ()
   list (APPEND HALOGEN_MATC_ARGS -DFILAMENT_MATC=${_release_matc})
endif ()

# Halogen links against the regular anari-sdk install -- matches SNEEZE_CONFIG
# (Debug anari when building Debug, Release anari when building Release).
set (_halogen_anari_root "${LIBS_DIR}/ANARI-SDK/install")
set (_halogen_anari_dir "${_halogen_anari_root}/lib/cmake/anari-0.16.0")

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_halogen}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   # Pin to an immutable Halogen release tag, never the master branch, so a
   # given Sneeze commit always builds the same Halogen (reproducible) like
   # every other dep. To advance, bump this dep's ref in dependencies.json to a newer release tag; an
   # existing clone is NOT auto-moved by that edit, so the build script checks
   # the checkout against this tag and refuses to silently build the wrong one.
   #
   # Halogen has two submodules: external/corrade (required) and
   # external/anari-sdk (skipped because we pass ANARI_ROOT, which takes
   # the find_package branch and bypasses add_subdirectory(external/anari-sdk)).
   # Limit the submodule fetch to corrade only -- avoids cloning the full
   # anari-sdk we'd otherwise never use.
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_halogen}
      GIT_TAG        ${DEP_REF_halogen}
      GIT_SHALLOW    ON
      GIT_SUBMODULES external/corrade
   )
endif ()

# NOTE: multi-config generators ignore CMAKE_BUILD_TYPE; pin the inner
# config via BUILD_COMMAND / INSTALL_COMMAND so ExternalProject doesn't
# inherit a stale value.
ExternalProject_Add (halogen
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/Halogen/build"
   INSTALL_DIR      "${LIBS_DIR}/Halogen/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_TESTS=OFF
      ${MSVC_CRT_ARGS}
      ${HALOGEN_FIND_ARGS}
      ${HALOGEN_MATC_ARGS}
      -DANARI_ROOT=${_halogen_anari_root}
      -Danari_DIR=${_halogen_anari_dir}
      -DFILAMENT_ROOT=${LIBS_DIR}/filament/install
      -DFILAMENT_SDK_DIR=${LIBS_DIR}/filament/install
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
   BUILD_COMMAND    ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${SNEEZE_CONFIG}
   INSTALL_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${SNEEZE_CONFIG} --target install
)
