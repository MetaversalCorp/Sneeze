# jwt-cpp -- header-only JWS/JWT library.
#
# We bypass jwt-cpp's own CMake integration because its FindOpenSSL.cmake
# path clashes with BoringSSL (different version strings, no FindPackage
# files). Since the library is purely headers, all we need is a copy of
# the include/ tree into LIBS_DIR/jwt-cpp/install/include.

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_jwt-cpp}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_jwt-cpp}
      GIT_TAG        ${DEP_REF_jwt-cpp}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (jwt-cpp
   ${_git_args}
   SOURCE_DIR        "${_repo}"
   BINARY_DIR        "${LIBS_DIR}/jwt-cpp/build"
   INSTALL_DIR       "${LIBS_DIR}/jwt-cpp/install"
   CONFIGURE_COMMAND ""
   BUILD_COMMAND     ""
   INSTALL_COMMAND
      ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/include
      COMMAND ${CMAKE_COMMAND} -E copy_directory
         <SOURCE_DIR>/include <INSTALL_DIR>/include
)
