# websocketpp -- header-only WebSocket library layered on asio.
#
# Consumed by RMAP (RMAP_USE_SYSTEM_WEBSOCKETPP). Headers are included as
# <websocketpp/...> and live under the clone's websocketpp/ subdir, so we copy
# that subtree to LIBS_DIR/websocketpp/install/include/websocketpp (consumers
# put .../install/include on the include path). CPP11-STL mode
# (_WEBSOCKETPP_CPP11_STL_, avoids Boost) is a compile define on the consumer.
#
# websocketpp's own config requires Boost, so we synthesize our own minimal
# config package (target websocketpp::websocketpp) for RMAP's
# find_package(websocketpp 0.8.2 CONFIG). Staged at configure time, copied into
# install/lib/cmake/websocketpp by the INSTALL_COMMAND below.

set (_wspp_cmake_stage "${LIBS_DIR}/websocketpp/cmake-stage")
file (WRITE "${_wspp_cmake_stage}/websocketppConfig.cmake"
[=[get_filename_component (_wspp_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if (NOT TARGET websocketpp::websocketpp)
   add_library (websocketpp::websocketpp INTERFACE IMPORTED)
   set_target_properties (websocketpp::websocketpp PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_wspp_prefix}/include")
endif ()
]=])
file (WRITE "${_wspp_cmake_stage}/websocketppConfigVersion.cmake"
[=[set (PACKAGE_VERSION "0.8.2")
if (PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
   set (PACKAGE_VERSION_COMPATIBLE FALSE)
else ()
   set (PACKAGE_VERSION_COMPATIBLE TRUE)
   if (PACKAGE_VERSION VERSION_EQUAL PACKAGE_FIND_VERSION)
      set (PACKAGE_VERSION_EXACT TRUE)
   endif ()
endif ()
]=])

set (_repo "${SNEEZE_DEP_REPO}/websocketpp")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
      GIT_TAG        0.8.2
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (websocketpp
   ${_git_args}
   SOURCE_DIR        "${_repo}"
   BINARY_DIR        "${LIBS_DIR}/websocketpp/build"
   INSTALL_DIR       "${LIBS_DIR}/websocketpp/install"
   CONFIGURE_COMMAND ""
   BUILD_COMMAND     ""
   INSTALL_COMMAND
      ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/include/websocketpp
      COMMAND ${CMAKE_COMMAND} -E copy_directory
         <SOURCE_DIR>/websocketpp <INSTALL_DIR>/include/websocketpp
      COMMAND ${CMAKE_COMMAND} -E copy_directory
         ${_wspp_cmake_stage} <INSTALL_DIR>/lib/cmake/websocketpp
)
