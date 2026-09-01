set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_webp}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_webp}
      GIT_TAG        ${DEP_REF_webp}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (webp
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/webp/build"
   INSTALL_DIR      "${LIBS_DIR}/webp/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      # Decode-only: Sneeze consumes libwebpdecoder (self-contained decoder).
      # Everything else -- the encoder-side tools, mux/demux, extras, JS -- is
      # dead weight, so switch it all off to keep the build fast and the
      # install tree minimal.
      -DWEBP_BUILD_ANIM_UTILS=OFF
      -DWEBP_BUILD_CWEBP=OFF
      -DWEBP_BUILD_DWEBP=OFF
      -DWEBP_BUILD_GIF2WEBP=OFF
      -DWEBP_BUILD_IMG2WEBP=OFF
      -DWEBP_BUILD_VWEBP=OFF
      -DWEBP_BUILD_WEBPINFO=OFF
      -DWEBP_BUILD_LIBWEBPMUX=OFF
      -DWEBP_BUILD_WEBPMUX=OFF
      -DWEBP_BUILD_EXTRAS=OFF
      -DWEBP_BUILD_WEBP_JS=OFF
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
