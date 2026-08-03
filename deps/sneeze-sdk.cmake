# SneezeSDK -- Metaversal Corp collection of Wasm guest SDKs (headers).
#
# Header-only from Sneeze's perspective: the repo ships C headers under
# include/ that Wasm guest modules compile against. We just clone the repo
# into deps/repos/SneezeSDK and copy its include/ tree into the install
# prefix, mirroring jwt-cpp. The repo's C/Cpp/Rust submodules are NOT needed
# for the C headers, so submodule checkout is skipped.
#
# IMPORTANT: do NOT put GIT_SUBMODULES "" inside a CMake list variable
# (${_git_args}). Empty list elements are dropped, so ExternalProject would
# omit GIT_SUBMODULES and default to cloning every submodule (SneezeSDK_C /
# SneezeSDK_CPP are private and 403 in CI). Pass GIT_SUBMODULES inline.

set (_repo "${SNEEZE_DEP_REPO}/SneezeSDK")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/MetaversalCorp/SneezeSDK.git
      GIT_TAG        main
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (sneeze-sdk
   ${_git_args}
   # Empty string = initialize/update no submodules (CMake 3.16+).
   GIT_SUBMODULES  ""
   SOURCE_DIR        "${_repo}"
   BINARY_DIR        "${LIBS_DIR}/SneezeSDK/build"
   INSTALL_DIR       "${LIBS_DIR}/SneezeSDK/install"
   CONFIGURE_COMMAND ""
   BUILD_COMMAND     ""
   INSTALL_COMMAND
      ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/include
      COMMAND ${CMAKE_COMMAND} -E copy_directory
         <SOURCE_DIR>/include <INSTALL_DIR>/include
)
