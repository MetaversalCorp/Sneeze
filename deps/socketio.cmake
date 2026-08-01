# socket.io-client-cpp ("sioclient") -- COMPILED WebSocket/Socket.IO client.
#
# Consumed by RMAP (RMAP_USE_SYSTEM_SIOCLIENT). Built into a static
# sioclient_tls (its TLS variant) and installed to LIBS_DIR/socketio/install.
# TLS is backed by BoringSSL on EVERY platform (socket.io has no Schannel path),
# reusing the BoringSSL that deps/boringssl.cmake already builds.
#
# Pinned to a specific master commit rather than the 3.1.0 tag: that tag hard-
# caps its TLS target at CXX_STANDARD 11, but modern BoringSSL's C++ headers
# (openssl/span.h) require C++17. master relaxed that to a cxx_std_11 *minimum*,
# so we raise the whole build to C++17 and compile against BoringSSL unpatched.
# An arbitrary SHA is not fetchable via a shallow clone, so GIT_SHALLOW is OFF.
# It pulls its own submodules (lib/websocketpp, lib/rapidjson, lib/asio),
# internal to its build and separate from Sneeze's asio/websocketpp.

set (BORINGSSL_INSTALL_DIR "${LIBS_DIR}/boringssl/install")
set (_crypto_lib "${BORINGSSL_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX}")
set (_ssl_lib    "${BORINGSSL_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX}")

# MSVC: setting CMAKE_CXX_FLAGS REPLACES CMake's defaults, so re-state
# /DWIN32 /D_WINDOWS /EHsc. NOCRYPT + WIN32_LEAN_AND_MEAN keep <wincrypt.h>'s
# X509_NAME/PKCS7 macros from colliding with BoringSSL types; NOMINMAX stops
# <windows.h>'s max() macro mangling std::numeric_limits<>::max() in span.h.
set (SIO_CXX_FLAGS_ARG)
if (MSVC)
   set (SIO_CXX_FLAGS_ARG "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /permissive- /Zc:__cplusplus /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNOCRYPT")
endif ()

set (_repo "${SNEEZE_DEP_REPO}/socketio")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/socketio/socket.io-client-cpp.git
      GIT_TAG        3b7be7e4173b5bdeed393966e3274f65d513a280
      # Only rapidjson is taken from socket.io. Its bundled asio (1.11) and
      # websocketpp submodules are deliberately NOT fetched -- they are replaced
      # below (PATCH_COMMAND) with the pinned copies the rest of the stack uses.
      GIT_SUBMODULES "lib/rapidjson"
   )
endif ()

# Version-unify socket.io's header-only deps with the rest of the stack.
# socket.io-client-cpp vendors its OWN asio (1.11.0, 2017) and websocketpp.
# Linking that sioclient_tls alongside RMAP/Map -- built against asio 1.30.2 +
# websocketpp 0.8.2 (deps/asio.cmake, deps/websocketpp.cmake) -- would put two
# incompatible asio layouts in one binary: the linker folds the shared asio::
# symbols, so a strand/io_context built by one asio has its win_mutex read at the
# wrong offset by the other -> crash in asio::detail::win_mutex::lock() the moment
# the socket.io run loop starts. Overwrite sioclient's bundled asio + websocketpp
# headers with Sneeze's pinned copies so the whole link resolves to a single
# asio/websocketpp (rapidjson is used nowhere else, so it stays bundled). Sources
# are the install trees deps/asio.cmake and deps/websocketpp.cmake produce; the
# add_dependencies (socketio asio websocketpp) in deps/CMakeLists.txt guarantees
# they exist before this patch runs.
set (_sio_asio_hdrs "${LIBS_DIR}/asio/install/include")
set (_sio_wspp_hdrs "${LIBS_DIR}/websocketpp/install/include/websocketpp")

ExternalProject_Add (socketio
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/socketio/build"
   INSTALL_DIR      "${LIBS_DIR}/socketio/install"
   PATCH_COMMAND
      ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>/lib/asio
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${_sio_asio_hdrs}" <SOURCE_DIR>/lib/asio/asio/include
      COMMAND ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>/lib/websocketpp
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${_sio_wspp_hdrs}" <SOURCE_DIR>/lib/websocketpp/websocketpp
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DCMAKE_CXX_STANDARD=17
      -DCMAKE_CXX_STANDARD_REQUIRED=ON
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_UNIT_TESTS=OFF
      -DBUILD_TESTING=OFF
      -DUSE_SUBMODULES=ON
      -DOPENSSL_ROOT_DIR=${BORINGSSL_INSTALL_DIR}
      -DOPENSSL_USE_STATIC_LIBS=TRUE
      -DOPENSSL_INCLUDE_DIR=${BORINGSSL_INSTALL_DIR}/include
      -DOPENSSL_CRYPTO_LIBRARY=${_crypto_lib}
      -DOPENSSL_SSL_LIBRARY=${_ssl_lib}
      ${SIO_CXX_FLAGS_ARG}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
