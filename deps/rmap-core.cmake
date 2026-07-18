# RMAP_Core -- Metaversal Corp open-source C++ networking library (RMAP).
#
# Static from Sneeze's perspective (BUILD_SHARED_LIBS OFF -> RMAP.lib). Build
# entry is the cpp/ subdir (SOURCE_SUBDIR cpp), not the repo root. Installs a
# find_package(RMAP_Core CONFIG) package exporting target RMAP::Core.
#
# nlohmann: RMAP_CORE_USE_SYSTEM_JSON=ON makes RMAP link Sneeze's single
# nlohmann_json (config package) instead of its bundled copy -- RMAP.h exposes
# nlohmann::ordered_json across the ABI, so both must stay pinned to the same
# version (3.11.3). Keep them in step on any bump.

set (_repo "${SNEEZE_DEP_REPO}/RMAP_Core")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/MetaversalCorp/RMAP_Core.git
      GIT_TAG        main        # no release tag cut yet; pin one when available
      GIT_SHALLOW    ON
   )
endif ()

# Link Sneeze's single nlohmann_json (config package) instead of the bundled copy.
set (_nlohmann_root "${LIBS_DIR}/nlohmann-json/install")

# Cross toolchains (iOS/Android) restrict find_package to the sysroot.
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (_rmap_find_args -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH)
endif ()

ExternalProject_Add (rmap-core
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   SOURCE_SUBDIR    cpp
   BINARY_DIR       "${LIBS_DIR}/RMAP_Core/build"
   INSTALL_DIR      "${LIBS_DIR}/RMAP_Core/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DRMAP_CORE_USE_SYSTEM_JSON=ON
      -DRMAP_CORE_BUILD_TESTS=OFF
      -DCMAKE_PREFIX_PATH=${_nlohmann_root}
      ${_rmap_find_args}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
