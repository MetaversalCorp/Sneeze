set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_freetype}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_freetype}
      GIT_TAG        ${DEP_REF_freetype}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (freetype
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/FreeType/build"
   INSTALL_DIR      "${LIBS_DIR}/FreeType/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DFT_DISABLE_ZLIB=ON
      -DFT_DISABLE_BZIP2=ON
      -DFT_DISABLE_PNG=ON
      -DFT_DISABLE_HARFBUZZ=ON
      -DFT_DISABLE_BROTLI=ON
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
