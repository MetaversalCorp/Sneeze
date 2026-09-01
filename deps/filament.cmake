# Filament -- PBR rendering engine. Depended on by Halogen (via FindFilament)
# and built as a standard ExternalProject. Release and Debug each produce
# their own install tree under LIBS_DIR. Debug Halogen imports the Release
# matc via FILAMENT_MATC preset in deps/halogen.cmake; filament.cmake itself
# builds the same way for both configs -- nothing special here.

# Backend selection per platform. Vulkan everywhere so bluevk (used by
# Halogen) is always available; iOS is Metal-only because
# VulkanPlatformApple pulls Cocoa headers (macOS-only) and Halogen's iOS
# path doesn't link bluevk/bluegl anyway.
if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (FILAMENT_BACKEND_ARGS
      -DFILAMENT_SUPPORTS_METAL=ON
      -DFILAMENT_SUPPORTS_VULKAN=ON)
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
   # FILAMENT_IOS preprocessor define gates iOS-specific code in the Metal
   # backend (MetalEnums.h guards BC formats + Depth24Unorm_Stencil8).
   set (FILAMENT_BACKEND_ARGS
      -DFILAMENT_SUPPORTS_METAL=ON
      -DFILAMENT_SUPPORTS_VULKAN=OFF
      -DFILAMENT_SUPPORTS_OPENGL=OFF
      -DIOS=1
      -DDIST_DIR=arm64
      -DDIST_ARCH=arm64
      "-DCMAKE_C_FLAGS=-DFILAMENT_IOS"
      "-DCMAKE_CXX_FLAGS=-DFILAMENT_IOS"
      "-DCMAKE_OBJC_FLAGS=-DFILAMENT_IOS"
      "-DCMAKE_OBJCXX_FLAGS=-DFILAMENT_IOS")
elseif (ANDROID)
   # Filament defaults DIST_ARCH to the host processor (x86_64 on Linux CI
   # runners), installing to lib/x86_64/. Override so libraries land at
   # lib/arm64-v8a/ where Halogen's FindFilament looks. EGL=TRUE compiles
   # PlatformEGL.cpp (needed by PlatformEGLAndroid base).
   set (FILAMENT_BACKEND_ARGS
      -DFILAMENT_SUPPORTS_VULKAN=ON
      -DDIST_DIR=arm64-v8a
      -DDIST_ARCH=aarch64
      -DEGL=TRUE)
   # Filament's IAS .incbin shaders.bin fails on Windows-hosted NDK clang
   # unless this flag (set by Filament's own Android toolchain files) is on.
   if (CMAKE_HOST_WIN32)
      list (APPEND FILAMENT_BACKEND_ARGS -DANDROID_ON_WINDOWS=ON)
   endif ()
else ()
   set (FILAMENT_BACKEND_ARGS -DFILAMENT_SUPPORTS_VULKAN=ON)
endif ()

# Windows: CRT + install layout. USE_STATIC_CRT=OFF selects /MD (Release)
# and /MDd (Debug) so filament's CRT matches Sneeze/Halogen. DIST_DIR=
# x86_64/md places libs at lib/x86_64/md/ where Halogen's FindFilament
# expects them.
set (FILAMENT_CRT_ARGS)
if (WIN32)
   list (APPEND FILAMENT_CRT_ARGS -DUSE_STATIC_CRT=OFF -DDIST_DIR=x86_64/md)
   # Filament's FILAMENT_SHORTEN_MSVC_COMPILATION appends
   # /D_ITERATOR_DEBUG_LEVEL=0 to Debug compiles, which mismatches
   # Sneeze/Halogen (both use standard /MDd + _ITERATOR_DEBUG_LEVEL=2).
   # Same CRT DLL, different STL struct layouts -> LNK2038. Disable the
   # shortening so all three match at level 2.
   if (SNEEZE_CONFIG STREQUAL "Debug")
      list (APPEND FILAMENT_CRT_ARGS -DFILAMENT_SHORTEN_MSVC_COMPILATION=OFF)
   endif ()
endif ()

# Cross-compile (iOS, Android) can't build matc for the target, since matc
# runs on the build machine at build time. CI passes
# IMPORT_EXECUTABLES_HOST_FILE pointing at ImportExecutables-Release.cmake
# produced by a prior host build; copy it into filament's source root
# before configure so filament's top-level CMake includes it on the
# CMAKE_CROSSCOMPILING branch.
#
# Vulkan importTextureR overlay: Filament v1.71.0.mv.2 stubs Vulkan
# Texture::import() with an assert. OpenXR needs that import to wrap a
# swapchain VkImage. The overlay lives in Sneeze (filament-vulkan-import.cmake)
# so the Filament GitHub repo does not need a new tag. Applied at deps
# configure (existing clone) and again as PATCH_COMMAND (first clone).
set (_filament_vk_import "${CMAKE_CURRENT_LIST_DIR}/filament-vulkan-import.cmake")
set (FILAMENT_PATCH_COMMAND
   ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR> -P "${_filament_vk_import}")
if (IMPORT_EXECUTABLES_HOST_FILE AND EXISTS "${IMPORT_EXECUTABLES_HOST_FILE}")
   list (APPEND FILAMENT_PATCH_COMMAND
      COMMAND ${CMAKE_COMMAND} -E copy
         "${IMPORT_EXECUTABLES_HOST_FILE}"
         "<SOURCE_DIR>/ImportExecutables-Release.cmake")
endif ()

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_filament}")
if (EXISTS "${_repo}/filament/backend/src/vulkan/VulkanDriver.cpp")
   execute_process (
      COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${_repo} -P "${_filament_vk_import}"
      RESULT_VARIABLE _filament_vk_import_rc)
   if (NOT _filament_vk_import_rc EQUAL 0)
      message (FATAL_ERROR "filament Vulkan import overlay failed (see filament-vulkan-import.cmake)")
   endif ()
endif ()
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   # Pin to a tag on our fork: the stable Filament release we forked plus our
   # own patches (currently the CreateMergeReturnPass inliner patch). Like
   # every other dep this is an immutable tag, never a branch -- a given Sneeze
   # commit always builds the exact same Filament (reproducible), and we never
   # pull Google's post-fork upstream. To advance to a newer fork tag, bump this
   # dep's ref in dependencies.json; an existing clone is NOT auto-updated by that edit, so the
   # build script checks the checkout against this tag and refuses to silently
   # build the wrong version.
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_filament}
      GIT_TAG        ${DEP_REF_filament}
      GIT_SHALLOW    ON
   )
endif ()

# NOTE: multi-config generators (Visual Studio, Xcode) ignore
# CMAKE_BUILD_TYPE; pin the inner config via BUILD_COMMAND / INSTALL_COMMAND
# so ExternalProject doesn't inherit a stale value.
ExternalProject_Add (filament
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/filament/build"
   INSTALL_DIR      "${LIBS_DIR}/filament/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DFILAMENT_BUILD_TESTING=OFF
      -DFILAMENT_SKIP_SAMPLES=ON
      -DFILAMENT_SKIP_SDL2=ON
      -DFILAMENT_ENABLE_MATDBG=OFF
      ${FILAMENT_BACKEND_ARGS}
      ${FILAMENT_CRT_ARGS}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
   BUILD_COMMAND    ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${SNEEZE_CONFIG}
   INSTALL_COMMAND  ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${SNEEZE_CONFIG} --target install
   PATCH_COMMAND ${FILAMENT_PATCH_COMMAND}
)

# VulkanPlatform.h includes <bluevk/BlueVK.h>. Filament's install() does not
# ship those headers, so Halogen cannot query VkInstance/VkDevice from the SDK
# tree. Stage BlueVK's public include dir into the install prefix.
ExternalProject_Add_Step (filament stage_bluevk_headers
   COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${_repo}/libs/bluevk/include/bluevk"
      "${LIBS_DIR}/filament/install/include/bluevk"
   DEPENDEES install
   COMMENT "Staging bluevk headers into filament install tree"
)

# Native Release builds emit ImportExecutables-Release.cmake into the source
# tree via filament's own export(TARGETS ...). CI needs this file to produce
# a host artifact for cross-compile jobs (iOS, Android). Copy it into the
# install tree so artifact packaging picks it up naturally.
if (NOT CMAKE_CROSSCOMPILING AND SNEEZE_CONFIG STREQUAL "Release")
   ExternalProject_Add_Step (filament stage_import_executables
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "${_repo}/ImportExecutables-Release.cmake"
         "${LIBS_DIR}/filament/install/ImportExecutables-Release.cmake"
      DEPENDEES install
      COMMENT "Staging ImportExecutables-Release.cmake into filament install tree"
   )
endif ()
