set (_repo "${SNEEZE_DEP_REPO}/RmlUi")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
      GIT_TAG        6.2
      GIT_SHALLOW    ON
   )
endif ()

set (FREETYPE_ROOT "${LIBS_DIR}/FreeType/install")
if (WIN32)
   set (FREETYPE_LIB_NAME "freetype.lib")
else ()
   set (FREETYPE_LIB_NAME "libfreetype.a")
endif ()

ExternalProject_Add (rmlui
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/RmlUi/build"
   INSTALL_DIR      "${LIBS_DIR}/RmlUi/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_SAMPLES=OFF
      -DRMLUI_FONT_ENGINE=freetype
      -DCMAKE_PREFIX_PATH=${FREETYPE_ROOT}
      # Cross-compile toolchains (iOS / Android) set CMAKE_FIND_ROOT_PATH_MODE
      # so find_package (Freetype) refuses to look outside the sysroot. Pass
      # the variables FindFreetype.cmake reports as missing directly to
      # bypass the search and keep configure deterministic on every host.
      -DFREETYPE_LIBRARY=${FREETYPE_ROOT}/lib/${FREETYPE_LIB_NAME}
      -DFREETYPE_INCLUDE_DIRS=${FREETYPE_ROOT}/include/freetype2
      ${CROSS_COMPILE_ARGS}
)
