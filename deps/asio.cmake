# asio -- standalone (non-Boost), header-only networking library.
#
# Consumed by RMAP (RMAP_USE_SYSTEM_ASIO). Header-only from Sneeze's
# perspective: at tag asio-1-30-2 the headers live under asio/include/ (there is
# no top-level include/), so we copy that subtree into
# LIBS_DIR/asio/install/include. Standalone mode (ASIO_STANDALONE) is a compile
# define on the consumer, not a build option here.
#
# asio ships no CMake package, but RMAP does find_package(asio 1.30.2 CONFIG).
# We synthesize a minimal config package (target asio::asio) so RMAP can consume
# Sneeze's headers instead of git-fetching its own copy. Staged at configure
# time, copied into install/lib/cmake/asio by the INSTALL_COMMAND below.

set (_asio_cmake_stage "${LIBS_DIR}/asio/cmake-stage")
file (WRITE "${_asio_cmake_stage}/asioConfig.cmake"
[=[get_filename_component (_asio_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if (NOT TARGET asio::asio)
   add_library (asio::asio INTERFACE IMPORTED)
   set_target_properties (asio::asio PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_asio_prefix}/include"
      INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE")
endif ()
]=])
file (WRITE "${_asio_cmake_stage}/asioConfigVersion.cmake"
[=[set (PACKAGE_VERSION "1.30.2")
if (PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
   set (PACKAGE_VERSION_COMPATIBLE FALSE)
else ()
   set (PACKAGE_VERSION_COMPATIBLE TRUE)
   if (PACKAGE_VERSION VERSION_EQUAL PACKAGE_FIND_VERSION)
      set (PACKAGE_VERSION_EXACT TRUE)
   endif ()
endif ()
]=])

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_asio}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_asio}
      GIT_TAG        ${DEP_REF_asio}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (asio
   ${_git_args}
   SOURCE_DIR        "${_repo}"
   BINARY_DIR        "${LIBS_DIR}/asio/build"
   INSTALL_DIR       "${LIBS_DIR}/asio/install"
   CONFIGURE_COMMAND ""
   BUILD_COMMAND     ""
   INSTALL_COMMAND
      ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/include
      COMMAND ${CMAKE_COMMAND} -E copy_directory
         <SOURCE_DIR>/asio/include <INSTALL_DIR>/include
      COMMAND ${CMAKE_COMMAND} -E copy_directory
         ${_asio_cmake_stage} <INSTALL_DIR>/lib/cmake/asio
)
