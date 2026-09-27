set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_capture}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_capture}
      GIT_TAG        ${DEP_REF_capture}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (capture
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/Capture/build"
   INSTALL_DIR      "${LIBS_DIR}/Capture/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
