if (WIN32)
   set (CURL_SSL_ARGS -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF)
elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
   # iOS: Apple's Secure Transport (no OpenSSL dep).
   set (CURL_SSL_ARGS -DCURL_USE_SECTRANSP=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_SCHANNEL=OFF)
elseif (APPLE)
   # macOS desktop: BoringSSL via the OpenSSL find interface (same as Linux).
   # Secure Transport is easy to silently disable (CURL_USE_SECTRANSP is a
   # dependent option; stale ExternalProject caches leave curl HTTP-only and
   # https:// requests fail with CURLE_UNSUPPORTED_PROTOCOL). BoringSSL is
   # already built for jwt/MSF crypto and produces universal static libs.
   set (BORINGSSL_INSTALL_DIR "${LIBS_DIR}/boringssl/install")
   set (CURL_SSL_ARGS
      -DCURL_ENABLE_SSL=ON
      -DCURL_USE_OPENSSL=ON
      -DCURL_USE_SECTRANSP=OFF
      -DCURL_USE_SCHANNEL=OFF
      -DOPENSSL_ROOT_DIR=${BORINGSSL_INSTALL_DIR}
      -DOPENSSL_USE_STATIC_LIBS=TRUE
      -DOPENSSL_INCLUDE_DIR=${BORINGSSL_INSTALL_DIR}/include
      -DOPENSSL_CRYPTO_LIBRARY=${BORINGSSL_INSTALL_DIR}/lib/libcrypto.a
      -DOPENSSL_SSL_LIBRARY=${BORINGSSL_INSTALL_DIR}/lib/libssl.a
   )
else ()
   # Linux + Android: curl links against BoringSSL (built by deps/boringssl.cmake).
   # BoringSSL answers to the same FindOpenSSL interface as OpenSSL, so
   # CURL_USE_OPENSSL=ON still works -- we just point it at the BoringSSL
   # install tree. The Android NDK toolchain sets
   # CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY which makes find_package(OpenSSL)
   # ignore OPENSSL_ROOT_DIR, so we also pass the explicit lib and include
   # paths to short-circuit the search. Using BoringSSL on Linux (instead
   # of system OpenSSL) keeps the build hermetic per the no-apt-packages
   # policy and matches what gets shipped to consumers.
   set (BORINGSSL_INSTALL_DIR "${LIBS_DIR}/boringssl/install")
   set (CURL_SSL_ARGS
      -DCURL_USE_OPENSSL=ON
      -DCURL_USE_SCHANNEL=OFF
      -DOPENSSL_ROOT_DIR=${BORINGSSL_INSTALL_DIR}
      -DOPENSSL_USE_STATIC_LIBS=TRUE
      -DOPENSSL_INCLUDE_DIR=${BORINGSSL_INSTALL_DIR}/include
      -DOPENSSL_CRYPTO_LIBRARY=${BORINGSSL_INSTALL_DIR}/lib/libcrypto.a
      -DOPENSSL_SSL_LIBRARY=${BORINGSSL_INSTALL_DIR}/lib/libssl.a
   )
endif ()

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_curl}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_curl}
      GIT_TAG        ${DEP_REF_curl}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (curl
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/curl/build"
   INSTALL_DIR      "${LIBS_DIR}/curl/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_CURL_EXE=OFF
      # Minimize optional deps -- we only need HTTP/HTTPS for a web client
      -DCURL_DISABLE_LDAP=ON
      -DCURL_DISABLE_LDAPS=ON
      -DUSE_LIBIDN2=OFF
      -DCURL_USE_LIBPSL=OFF
      -DCURL_USE_LIBSSH2=OFF
      -DCURL_ZLIB=OFF
      -DCURL_BROTLI=OFF
      -DCURL_ZSTD=OFF
      ${CURL_SSL_ARGS}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
