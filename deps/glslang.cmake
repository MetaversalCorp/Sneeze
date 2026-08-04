# glslang -- build-time tool, must run on the HOST when cross-compiling.
# Set GLSLANG_HOST_DIR to point to a pre-built host glslang to skip this step.

set (GLSLANG_HOST_DIR "" CACHE PATH "Pre-built host glslang directory (skip build)")

if (GLSLANG_HOST_DIR)
   # Use pre-built glslang from the specified path
   add_custom_target (glslang)
else ()
   set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_glslang}")
   if (EXISTS "${_repo}/.git")
      set (_git_args)
   else ()
      set (_git_args
         GIT_REPOSITORY ${DEP_URL_glslang}
         GIT_TAG        ${DEP_REF_glslang}
         GIT_SHALLOW    ON
      )
   endif ()

   # glslang is a build tool -- always build for the host, not the target
   ExternalProject_Add (glslang
      ${_git_args}
      SOURCE_DIR       "${_repo}"
      BINARY_DIR       "${LIBS_DIR}/glslang/build"
      INSTALL_DIR      "${LIBS_DIR}/glslang/install"
      CMAKE_ARGS
         -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
         -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
         -DENABLE_CTEST=OFF
         -DENABLE_OPT=OFF
         -DENABLE_HLSL=OFF
   )
endif ()
