if (SNEEZE_ENABLE_XR)
   set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_openxr-sdk}")
   if (EXISTS "${_repo}/.git")
      set (_git_args)
   else ()
      set (_git_args
         GIT_REPOSITORY ${DEP_URL_openxr-sdk}
         GIT_TAG        ${DEP_REF_openxr-sdk}
         GIT_SHALLOW    ON
      )
   endif ()

   ExternalProject_Add (openxr-sdk
      ${_git_args}
      SOURCE_DIR       "${_repo}"
      BINARY_DIR       "${LIBS_DIR}/OpenXR-SDK/build"
      INSTALL_DIR      "${LIBS_DIR}/OpenXR-SDK/install"
      CMAKE_ARGS
         -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
         -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
         -DDYNAMIC_LOADER=OFF
         ${CROSS_COMPILE_ARGS}
      CMAKE_CACHE_ARGS
         ${CROSS_COMPILE_CACHE_ARGS}
   )
else ()
   add_custom_target (openxr-sdk)
endif ()
