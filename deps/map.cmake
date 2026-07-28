# MAP -- Metaversal Corp C++ library. Mirrors RMAP's integration pattern.
#
# Static from Sneeze's perspective (BUILD_SHARED_LIBS OFF -> MAP.lib). Installs
# a find_package(MAP CONFIG) package exporting target MAP::MAP, consumed by
# src/CMakeLists.txt.
#
# Dependency policy: FULL SHARING (identical to RMAP). Rather than let MAP
# git-fetch and rebuild its own json/asio/websocketpp/boringssl/curl/socketio,
# we point every MAP_USE_SYSTEM_* at the copies Sneeze already builds:
#   - json / asio / websocketpp  -> find_package(... CONFIG) via CMAKE_PREFIX_PATH
#        (asio + websocketpp ship no upstream config; deps/asio.cmake and
#         deps/websocketpp.cmake synthesize minimal ones into their installs).
#   - boringssl / curl / socketio -> MAP's *_USE_SYSTEM_* has no find_package
#        path for these compiled deps, so MAP's CMakeLists reads MAP_SYSTEM_*_ROOT
#        install trees instead.
#
# MAP.h exposes nlohmann::ordered_json across its public ABI, so MAP and Sneeze
# must stay pinned to the same nlohmann_json (3.11.3). Keep them in step on bumps.

set (_repo "${SNEEZE_DEP_REPO}/Map")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/MetaversalCorp/Map.git
      GIT_TAG        main        # no release tag cut yet; pin one when available
      GIT_SHALLOW    ON
   )
endif ()

# Install trees of Sneeze's copies of MAP's dependencies.
set (_json_root "${LIBS_DIR}/nlohmann-json/install")
set (_asio_root "${LIBS_DIR}/asio/install")
set (_wspp_root "${LIBS_DIR}/websocketpp/install")

# find_package(json/asio/websocketpp CONFIG) search roots. Joined with '|' so the
# multi-entry path survives ExternalProject arg splitting (LIST_SEPARATOR below).
set (_map_prefix_path "${_json_root}|${_asio_root}|${_wspp_root}")

# Cross toolchains (iOS/Android) restrict find_package to the sysroot.
set (_map_find_args)
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (_map_find_args -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH)
endif ()

ExternalProject_Add (map
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/Map/build"
   INSTALL_DIR      "${LIBS_DIR}/Map/install"
   LIST_SEPARATOR   |
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DCMAKE_PREFIX_PATH=${_map_prefix_path}
      -DMAP_USE_SYSTEM_JSON=ON
      -DMAP_USE_SYSTEM_ASIO=ON
      -DMAP_USE_SYSTEM_WEBSOCKETPP=ON
      -DMAP_USE_SYSTEM_BORINGSSL=ON
      -DMAP_USE_SYSTEM_CURL=ON
      -DMAP_USE_SYSTEM_SIOCLIENT=ON
      -DMAP_SYSTEM_BORINGSSL_ROOT=${LIBS_DIR}/boringssl/install
      -DMAP_SYSTEM_CURL_ROOT=${LIBS_DIR}/curl/install
      -DMAP_SYSTEM_SIOCLIENT_ROOT=${LIBS_DIR}/socketio/install
      ${_map_find_args}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
   BUILD_BYPRODUCTS
      "${LIBS_DIR}/Map/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}MAP${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
