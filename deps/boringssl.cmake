# BoringSSL -- crypto primitives for src/jws/ on all platforms.
# Also serves as curl's TLS backend on Android (replaces OpenSSL there).
# On Windows, macOS, iOS, and Linux, curl uses the platform-native TLS
# stack (Schannel, Secure Transport, or system OpenSSL); BoringSSL on
# those platforms is used only by Sneeze's own crypto code.

set (BORINGSSL_INSTALL_DIR "${LIBS_DIR}/boringssl/install")

# iOS cannot assemble BoringSSL's hand-written asm with Apple's assembler
# without extra scaffolding. The C fallback is fully functional.
set (BORINGSSL_EXTRA_ARGS)
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
   list (APPEND BORINGSSL_EXTRA_ARGS -DOPENSSL_NO_ASM=1)
   # On iOS, CMake defaults MACOSX_BUNDLE=ON for every executable, but
   # BoringSSL's install(TARGETS bssl ...) doesn't specify a BUNDLE
   # DESTINATION, so configure dies with:
   #   install TARGETS given no BUNDLE DESTINATION for MACOSX_BUNDLE
   #   executable target "bssl".
   # We don't ship bssl (Sneeze only needs libcrypto.a + libssl.a from
   # the install), so disable bundle packaging for iOS sub-targets.
   list (APPEND BORINGSSL_EXTRA_ARGS -DCMAKE_MACOSX_BUNDLE=OFF)
endif ()

# Windows: BoringSSL's CMakeLists does enable_language(ASM_NASM) without
# probing common install paths, so nasm has to be on PATH or explicitly
# passed. Winget's default NASM install drops nasm.exe under
# %LOCALAPPDATA%/bin/NASM/, which is not on PATH out of the box. Look for
# it in the usual places and forward the absolute path if found.
if (WIN32 AND NOT CMAKE_CROSSCOMPILING)
   find_program (NASM_EXECUTABLE nasm
      HINTS
         "$ENV{LOCALAPPDATA}/bin/NASM"
         "$ENV{ProgramFiles}/NASM"
         "$ENV{ProgramFiles\(x86\)}/NASM"
   )
   if (NASM_EXECUTABLE)
      list (APPEND BORINGSSL_EXTRA_ARGS
         -DCMAKE_ASM_NASM_COMPILER=${NASM_EXECUTABLE})
      message (STATUS "BoringSSL: using NASM at ${NASM_EXECUTABLE}")
   endif ()
endif ()

set (_repo "${SNEEZE_DEP_REPO}/boringssl")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/google/boringssl.git
      GIT_TAG        main
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (boringssl
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/boringssl/build"
   INSTALL_DIR      "${BORINGSSL_INSTALL_DIR}"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DBUILD_SHARED_LIBS=OFF
      ${BORINGSSL_EXTRA_ARGS}
      ${CROSS_COMPILE_ARGS}
)
