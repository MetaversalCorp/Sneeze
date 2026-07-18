# SneezeSDK -- Metaversal Corp collection of Wasm guest SDKs (headers).
#
# Header-only from Sneeze's perspective: the repo ships C headers under
# include/ that Wasm guest modules compile against. We just clone the repo
# into deps/repos/SneezeSDK and copy its include/ tree into the install
# prefix, mirroring jwt-cpp. The repo's Rust submodule is NOT needed for the
# C headers, so submodule checkout is skipped (GIT_SUBMODULES "").

set (_repo "${SNEEZE_DEP_REPO}/SneezeSDK")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY https://github.com/MetaversalCorp/SneezeSDK.git
      GIT_TAG        main
      GIT_SHALLOW    ON
      GIT_SUBMODULES ""
   )
endif ()

ExternalProject_Add (sneeze-sdk
   ${_git_args}
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
