# RMAP -- Metaversal Corp C++ realtime-model-access / networking library.
#
# Static from Sneeze's perspective (BUILD_SHARED_LIBS OFF -> RMAP.lib). Installs
# a find_package(RMAP CONFIG) package exporting target RMAP::RMAP, consumed by
# src/CMakeLists.txt. RMAP folds three service modules (SB, REST, SocketIO) into
# the single RMAP.lib.
#
# Dependency policy: FULL SHARING. Rather than let RMAP git-fetch and rebuild its
# own json/asio/websocketpp/boringssl/curl/socketio, we point every
# RMAP_USE_SYSTEM_* at the copies Sneeze already builds:
#   - json / asio / websocketpp  -> find_package(... CONFIG) via CMAKE_PREFIX_PATH
#        (asio + websocketpp ship no upstream config; deps/asio.cmake and
#         deps/websocketpp.cmake synthesize minimal ones into their installs).
#   - boringssl / curl / socketio -> RMAP's *_USE_SYSTEM_* has no find_package
#        path for these compiled deps, so RMAP's CMakeLists (patched here in the
#        deps/repos/RMAP clone) reads RMAP_SYSTEM_*_ROOT install trees instead.
#        NOTE: those RMAP CMake edits must be pushed to MetaversalCorp/RMAP or a
#        fresh/CI clone (which re-clones when .git is absent) will lack them.
#
# RMAP.h exposes nlohmann::ordered_json across its public ABI, so RMAP and Sneeze
# must stay pinned to the same nlohmann_json (3.11.3). Keep them in step on bumps.

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_rmap}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_rmap}
      GIT_TAG        ${DEP_REF_rmap}        # no release tag cut yet; pin one when available
      GIT_SHALLOW    ON
   )
endif ()

# Install trees of Sneeze's copies of RMAP's dependencies.
set (_json_root "${LIBS_DIR}/nlohmann-json/install")
set (_asio_root "${LIBS_DIR}/asio/install")
set (_wspp_root "${LIBS_DIR}/websocketpp/install")

# find_package(json/asio/websocketpp CONFIG) search roots. Joined with '|' so the
# multi-entry path survives ExternalProject arg splitting (LIST_SEPARATOR below).
set (_rmap_prefix_path "${_json_root}|${_asio_root}|${_wspp_root}")

# Cross toolchains (iOS/Android) restrict find_package to the sysroot.
set (_rmap_find_args)
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (_rmap_find_args -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH)
endif ()

ExternalProject_Add (rmap
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/RMAP/build"
   INSTALL_DIR      "${LIBS_DIR}/RMAP/install"
   LIST_SEPARATOR   |
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DCMAKE_PREFIX_PATH=${_rmap_prefix_path}
      -DRMAP_USE_SYSTEM_JSON=ON
      -DRMAP_USE_SYSTEM_ASIO=ON
      -DRMAP_USE_SYSTEM_WEBSOCKETPP=ON
      -DRMAP_USE_SYSTEM_BORINGSSL=ON
      -DRMAP_USE_SYSTEM_CURL=ON
      -DRMAP_USE_SYSTEM_SIOCLIENT=ON
      -DRMAP_SYSTEM_BORINGSSL_ROOT=${LIBS_DIR}/boringssl/install
      -DRMAP_SYSTEM_CURL_ROOT=${LIBS_DIR}/curl/install
      -DRMAP_SYSTEM_SIOCLIENT_ROOT=${LIBS_DIR}/socketio/install
      ${_rmap_find_args}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
   BUILD_BYPRODUCTS
      "${LIBS_DIR}/RMAP/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}RMAP${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
