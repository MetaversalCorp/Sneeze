set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_fastgltf}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_fastgltf}
      GIT_TAG        ${DEP_REF_fastgltf}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (fastgltf
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/fastgltf/build"
   INSTALL_DIR      "${LIBS_DIR}/fastgltf/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DFASTGLTF_ENABLE_TESTS=OFF
      -DFASTGLTF_ENABLE_EXAMPLES=OFF
      -DFASTGLTF_ENABLE_DOCS=OFF
      -DFASTGLTF_ENABLE_INSTALL=ON
      # fastgltf tries find_package(simdjson) before vendoring sources. When
      # that succeeds it links simdjson PRIVATE — symbols stay out of
      # libfastgltf.a and consumers (SignMsf, SneezeTest) fail to link.
      -DCMAKE_DISABLE_FIND_PACKAGE_simdjson=ON
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)