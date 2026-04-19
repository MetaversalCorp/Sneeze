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

# Halogen links against the regular anari-sdk install -- matches SNEEZE_CONFIG
# (Debug anari when building Debug, Release anari when building Release).
set (_halogen_anari_root "${LIBS_DIR}/ANARI-SDK/install")
set (_halogen_anari_dir "${_halogen_anari_root}/lib/cmake/anari-0.16.0")

set (_repo "${SNEEZE_DEP_REPO}/Halogen")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   # Halogen has two submodules: external/corrade (required) and
   # external/anari-sdk (skipped because we pass ANARI_ROOT, which takes
   # the find_package branch and bypasses add_subdirectory(external/anari-sdk)).
   # Limit the submodule fetch to corrade only -- avoids cloning the full
   # anari-sdk we'd otherwise never use.
   set (_git_args
      GIT_REPOSITORY https://github.com/MetaversalCorp/Halogen.git
      GIT_TAG        master
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
      -DANARI_ROOT=${_halogen_anari_root}
      -Danari_DIR=${_halogen_anari_dir}
      -DFILAMENT_ROOT=${LIBS_DIR}/filament/install
      -DFILAMENT_SDK_DIR=${LIBS_DIR}/filament/install
      ${CROSS_COMPILE_ARGS}
   BUILD_COMMAND    ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${SNEEZE_CONFIG}
   INSTALL_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${SNEEZE_CONFIG} --target install
)
