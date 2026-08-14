if (WIN32)
   set (MSVC_CRT_ARGS
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL)
endif ()

# Helide is the ANARI SDK's reference CPU ray tracer (built on Embree). Sneeze
# no longer ships or loads it -- Halogen (filament-based, GPU-accelerated) is
# the sole renderer. Disabling helide also skips the Embree build, which
# produces deeply nested install paths that exceed Windows' 260-char MAX_PATH
# on any checkout not rooted at a very short prefix.
set (ANARI_HELIDE_ARG -DBUILD_HELIDE_DEVICE=OFF)

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_anari-sdk}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_anari-sdk}
      GIT_TAG        ${DEP_REF_anari-sdk}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (anari-sdk
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/ANARI-SDK/build"
   INSTALL_DIR      "${LIBS_DIR}/ANARI-SDK/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      ${MSVC_CRT_ARGS}
      ${ANARI_HELIDE_ARG}
      -DBUILD_TESTING=OFF
      -DBUILD_EXAMPLES=OFF
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
