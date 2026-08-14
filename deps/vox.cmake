# Vox -- standalone GPU compute dispatch library (Metaversal Corp).
#
# Vox takes SPIR-V bytecode and dispatches it on whichever GPU API is
# available: Vulkan (native SPIR-V), DX12 (SPIRV-Cross -> HLSL ->
# D3DCompile), or Metal (SPIRV-Cross -> MSL -> MTLLibrary). Sneeze's
# compute/ module uses Vox to replace the dead ANARI compute path.
#
# Vox picks up SPIRV-Cross from SPIRV_CROSS_ROOT for its own build.
# Its install tree ships a hand-written vox-config.cmake that does
# consumer-side SPIRV-Cross discovery, so Sneeze just sets
# SpirvCross_ROOT and calls find_package(Vox CONFIG). No FindVox or
# FindSpirvCross module is needed on the Sneeze side.

set (VOX_EXTRA_ARGS)
if (WIN32)
   # Vox vendors no runtime; match curl/boringssl so Release links against
   # the DLL runtime and Debug against the Debug DLL runtime.
   list (APPEND VOX_EXTRA_ARGS
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW)
endif ()

set (_repo "${SNEEZE_DEP_REPO}/${DEP_FOLDER_vox}")
if (EXISTS "${_repo}/.git")
   set (_git_args)
else ()
   set (_git_args
      GIT_REPOSITORY ${DEP_URL_vox}
      GIT_TAG        ${DEP_REF_vox}
      GIT_SHALLOW    ON
   )
endif ()

ExternalProject_Add (vox
   ${_git_args}
   SOURCE_DIR       "${_repo}"
   BINARY_DIR       "${LIBS_DIR}/Vox/build"
   INSTALL_DIR      "${LIBS_DIR}/Vox/install"
   CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=${SNEEZE_CONFIG}
      -DSPIRV_CROSS_ROOT=${LIBS_DIR}/SPIRV-Cross/install
      -DBUILD_TESTING=OFF
      ${VOX_EXTRA_ARGS}
      ${CROSS_COMPILE_ARGS}
   CMAKE_CACHE_ARGS
      ${CROSS_COMPILE_CACHE_ARGS}
)
