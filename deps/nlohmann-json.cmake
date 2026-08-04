set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_nlohmann-json}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_nlohmann-json}
      GIT_TAG        ${DEP_REF_nlohmann-json}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (nlohmann-json
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/nlohmann-json/build"
   INSTALL_DIR      "${LIBS_DIR}/nlohmann-json/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DJSON_BuildTests=OFF
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
