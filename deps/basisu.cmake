set (_repo "${SNEEZE_DEP_REPO}/basisu")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
      GIT_TAG        v2_1_0r
      GIT_SHALLOW    ON
   )
endif ()

# Upstream has no install() and no transcoder-only target. We inject a small
# wrapper (deps/basisu/CMakeLists.txt) into <clone>/sneeze_build/ and configure
# that via SOURCE_SUBDIR -- this adds a directory to the clone but never
# overwrites an upstream file, so the pinned-tag checkout stays clean.
ExternalProject_Add (basisu
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   SOURCE_SUBDIR    "sneeze_build"
   BINARY_DIR       "${LIBS_DIR}/basisu/build"
   INSTALL_DIR      "${LIBS_DIR}/basisu/install"
   PATCH_COMMAND    ${CMAKE_COMMAND} -E copy_directory
                       "${CMAKE_CURRENT_LIST_DIR}/basisu"
                       "${_repo}/sneeze_build"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
