set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_spirv-tools}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_spirv-tools}
      GIT_TAG        ${DEP_REF_spirv-tools}
      GIT_SHALLOW    ON
   )
endif ()

# SPIRV-Tools only reads ${SPIRV-Headers_SOURCE_DIR}/include (headers +
# grammar JSON + spir-v.xml) -- all of which are present in SPIRV-Headers'
# *install* tree as well. Point at the install so this works in isolated-tier
# CI, where only the tier0 install artifact is staged (not the source clone).
# Despite the variable name, SPIRV-Tools does not require an actual source
# tree here: the DEFINED-check path in its external/CMakeLists.txt skips
# add_subdirectory.
ExternalProject_Add (spirv-tools
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/SPIRV-Tools/build"
   INSTALL_DIR      "${LIBS_DIR}/SPIRV-Tools/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DSPIRV-Headers_SOURCE_DIR=${LIBS_DIR}/SPIRV-Headers/install
      -DSPIRV_SKIP_TESTS=ON
      -DSPIRV_SKIP_EXECUTABLES=ON
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
