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

# Decode-only: Sneeze consumes libwebpdecoder (self-contained decoder).
# Everything else -- the encoder-side tools, mux/demux, extras, JS -- is
# dead weight, so switch it all off to keep the build fast and the
# install tree minimal.
set (_webp_cmake_args
   -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
   -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
   -DBUILD_SHARED_LIBS=OFF
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
)

# libwebp's cmake/cpu.cmake compile-tests SSE2/SSE4. On a universal
# CMAKE_OSX_ARCHITECTURES=arm64;x86_64 those tests fail, so it stamps
# -mno-sse2 onto every C file. The x86_64 ABI returns values in XMM
# registers, and AppleClang then errors:
#   SSE2 register return with SSE2 disabled
# (sharpyuv_gamma.c). Same pattern as Wasmtime: build each arch, lipo.
set (_webp_macos_universal FALSE)
if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
   if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64"
         AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
      set (_webp_macos_universal TRUE)
   endif ()
endif ()

if (_webp_macos_universal)
   message (STATUS "Sneeze deps: webp universal macOS — per-arch configure + lipo")

   if (EXISTS "${_repo}/.git")
      set (_webp_src_download
         DOWNLOAD_COMMAND ""
         UPDATE_COMMAND   "")
   else ()
      set (_webp_src_download)
   endif ()

   ExternalProject_Add (webp-src
      ${_git_args}
      ${_webp_src_download}
      SOURCE_DIR       "${_repo}"
      CONFIGURE_COMMAND ""
      BUILD_COMMAND     ""
      INSTALL_COMMAND  ""
   )

   foreach (_webp_arch IN ITEMS arm64 x86_64)
      ExternalProject_Add (webp-${_webp_arch}
         DEPENDS           webp-src
         SOURCE_DIR        "${_repo}"
         DOWNLOAD_COMMAND  ""
         UPDATE_COMMAND    ""
         BINARY_DIR        "${LIBS_DIR}/${DEP_FOLDER_webp}/build-${_webp_arch}"
         INSTALL_DIR       "${LIBS_DIR}/${DEP_FOLDER_webp}/install-${_webp_arch}"
         CMAKE_ARGS        ${_webp_cmake_args}
         CMAKE_CACHE_ARGS
            -DCMAKE_OSX_ARCHITECTURES:STRING=${_webp_arch}
      )
   endforeach ()

   file (WRITE "${CMAKE_BINARY_DIR}/webp-lipo.cmake" [=[
cmake_minimum_required (VERSION 3.20)
if (NOT DEFINED ARM OR NOT DEFINED X64 OR NOT DEFINED DST)
   message (FATAL_ERROR "webp-lipo.cmake requires ARM, X64, and DST")
endif ()
if (NOT EXISTS "${ARM}/lib" OR NOT EXISTS "${X64}/lib")
   message (FATAL_ERROR "webp per-arch install missing:\n  ${ARM}/lib\n  ${X64}/lib")
endif ()

file (REMOVE_RECURSE "${DST}")
file (MAKE_DIRECTORY "${DST}/lib")
if (EXISTS "${ARM}/include")
   file (COPY "${ARM}/include" DESTINATION "${DST}")
endif ()
foreach (_subdir cmake pkgconfig)
   if (EXISTS "${ARM}/lib/${_subdir}")
      file (COPY "${ARM}/lib/${_subdir}" DESTINATION "${DST}/lib")
   endif ()
endforeach ()

file (GLOB _archives "${ARM}/lib/*.a")
if (NOT _archives)
   message (FATAL_ERROR "no static libs in ${ARM}/lib")
endif ()
foreach (_archive IN LISTS _archives)
   get_filename_component (_name "${_archive}" NAME)
   if (NOT EXISTS "${X64}/lib/${_name}")
      message (FATAL_ERROR "x86_64 webp install missing ${X64}/lib/${_name}")
   endif ()
   execute_process (
      COMMAND lipo -create
         "${ARM}/lib/${_name}"
         "${X64}/lib/${_name}"
         -output "${DST}/lib/${_name}"
      RESULT_VARIABLE _r
   )
   if (_r)
      message (FATAL_ERROR "lipo failed for ${_name}: ${_r}")
   endif ()
endforeach ()

if (NOT EXISTS "${DST}/lib/libwebpdecoder.a")
   message (FATAL_ERROR "libwebpdecoder.a missing after lipo at ${DST}/lib")
endif ()
]=])

   ExternalProject_Add (webp
      DEPENDS           webp-arm64 webp-x86_64
      DOWNLOAD_COMMAND  ""
      CONFIGURE_COMMAND ""
      BUILD_COMMAND
         ${CMAKE_COMMAND}
            -DARM=${LIBS_DIR}/${DEP_FOLDER_webp}/install-arm64
            -DX64=${LIBS_DIR}/${DEP_FOLDER_webp}/install-x86_64
            -DDST=${LIBS_DIR}/${DEP_FOLDER_webp}/install
            -P "${CMAKE_BINARY_DIR}/webp-lipo.cmake"
      INSTALL_COMMAND   ""
   )
else ()
   ExternalProject_Add (webp
      ${_git_args}
      SOURCE_DIR       "${_repo}"
      BINARY_DIR       "${LIBS_DIR}/${DEP_FOLDER_webp}/build"
      INSTALL_DIR      "${LIBS_DIR}/${DEP_FOLDER_webp}/install"
      CMAKE_ARGS       ${_webp_cmake_args}
      CMAKE_CACHE_ARGS
         ${CROSS_COMPILE_CACHE_ARGS}
   )
endif ()
