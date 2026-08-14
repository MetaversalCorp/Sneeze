set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_spirv-headers}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_spirv-headers}
      GIT_TAG        ${DEP_REF_spirv-headers}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (spirv-headers
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/SPIRV-Headers/build"
   INSTALL_DIR      "${LIBS_DIR}/SPIRV-Headers/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DSPIRV_HEADERS_ENABLE_TESTS=OFF
      -DSPIRV_HEADERS_ENABLE_INSTALL=ON
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
