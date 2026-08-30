# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required (VERSION 3.20)
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
   cmake_policy (SET CMP0144 NEW)
endif ()
file (STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" SNEEZE_VER)

# Limit multi-config generators (VS, Xcode, Ninja Multi-Config) to the two
# configs we actually support. The path generalization block below may use
# $<LOWER_CASE:$<CONFIG>> in INTERFACE_INCLUDE_DIRECTORIES on imported
# targets when both debug and release deps trees exist on disk; CMake
# validates those paths against every entry in CMAKE_CONFIGURATION_TYPES at
# generate time. Default VS values include MinSizeRel and RelWithDebInfo,
# which resolve to deps paths that don't exist on disk (the deps trees are
# only built for debug and release). Must be set BEFORE project() so it takes
# effect for this build.
set (CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING
   "Semicolon separated list of supported configuration types" FORCE)

project (Sneeze VERSION ${SNEEZE_VER} LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Release PDBs (MSVC)
# ---------------------------------------------------------------------------
# Generate PDB symbol files for Release builds so crash dumps and attach-to-
# process debugging can resolve symbols against optimized binaries. /Zi emits
# debug info into .obj; /DEBUG tells the linker to produce a .pdb next to the
# binary. /DEBUG alone silently disables /OPT:REF and /OPT:ICF -- we force them
# back on so codegen and binary size stay identical to a no-PDB Release build.
if (MSVC)
   add_compile_options ($<$<CONFIG:Release>:/Zi>)
   add_link_options    (
      $<$<CONFIG:Release>:/DEBUG>
      $<$<CONFIG:Release>:/OPT:REF>
      $<$<CONFIG:Release>:/OPT:ICF>
   )
endif ()

option (USE_LOCAL_DEPS "Find dependencies in libs/ (ON) or via vcpkg find_package (OFF)" ON)
option (SNEEZE_ENABLE_WASM "Enable WebAssembly runtime (Wasmtime)" ON)
option (SNEEZE_ENABLE_XR   "Enable OpenXR integration" ON)

if (NOT DEFINED LIBS_DIR)
   set (LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs")
endif ()

# Read-only dependency gate. Sneeze links the pre-built dep libraries, so verify
# (OFFLINE, no network) that every dep clone is on the ref the manifest
# (deps/dependencies.json) pins before configuring -- this is the guard for the
# Sneeze.sln / IDE build path that bypasses the build scripts. Uncloned deps
# (e.g. a fresh CI checkout that consumes prebuilt artifacts) skip cleanly.
# Only meaningful for local-deps builds; vcpkg builds use their own packages.
if (USE_LOCAL_DEPS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../deps/DepVerify.cmake")
   include ("${CMAKE_CURRENT_SOURCE_DIR}/../deps/DepVerify.cmake")
   sneeze_verify_all (OFFLINE)
endif ()

# ---------------------------------------------------------------------------
# Force output directories
# ---------------------------------------------------------------------------
# The build scripts pass SNEEZE_BUILD_ROOT = <repo>/builds/<platform>.
# Outputs land in <root>/install/<config>/{bin,lib} where <config> is the
# active build config (debug/release), resolved at BUILD time so a single
# multi-config CMake tree (Visual Studio / Xcode / Ninja Multi-Config) can
# produce both Debug and Release binaries into distinct install trees.

if (NOT DEFINED SNEEZE_BUILD_ROOT)
   # Direct configure (no wrapper script). Fall back to the CMake binary dir.
   set (SNEEZE_BUILD_ROOT "${CMAKE_BINARY_DIR}")
endif ()

# SNEEZE_CONFIG names the config used to resolve find_package during THIS
# configure (the deps tree is still per-config). It is NOT the config binaries
# are emitted for -- that is decided per `cmake --build --config`. Precedence:
#   1. Caller passes -DSNEEZE_CONFIG=<cfg>.
#   2. CMAKE_BUILD_TYPE (single-config generators).
#   3. Derive from LIBS_DIR's .../{debug,release}/libs/... segment.
#   4. Default: release.
if (NOT DEFINED SNEEZE_CONFIG OR SNEEZE_CONFIG STREQUAL "")
   if (CMAKE_BUILD_TYPE)
      set (SNEEZE_CONFIG "${CMAKE_BUILD_TYPE}")
   elseif (LIBS_DIR MATCHES "/(debug|release|relwithdebinfo|minsizerel)/")
      set (SNEEZE_CONFIG "${CMAKE_MATCH_1}")
   else ()
      set (SNEEZE_CONFIG "Release")
   endif ()
endif ()
if (NOT DEFINED SNEEZE_PLATFORM)
   set (SNEEZE_PLATFORM "unknown")
endif ()

string (TOLOWER "${SNEEZE_CONFIG}" SNEEZE_CONFIG_LC)

message (STATUS "Sneeze: platform      = ${SNEEZE_PLATFORM}")
message (STATUS "Sneeze: find config   = ${SNEEZE_CONFIG}")
message (STATUS "Sneeze: libs dir      = ${LIBS_DIR}")
message (STATUS "Sneeze: build root    = ${SNEEZE_BUILD_ROOT}")

# Per-config output dirs. Generator expression $<LOWER_CASE:$<CONFIG>> resolves
# at build time, so Debug and Release get their own install trees. Also set
# the generator-level default so single-config generators without an explicit
# per-config value still land in the right place.
set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${SNEEZE_BUILD_ROOT}/install/$<LOWER_CASE:$<CONFIG>>/lib")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SNEEZE_BUILD_ROOT}/install/$<LOWER_CASE:$<CONFIG>>/lib")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SNEEZE_BUILD_ROOT}/install/$<LOWER_CASE:$<CONFIG>>/bin")
foreach (_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
   string (TOLOWER "${_cfg}" _cfg_lc)
   set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${SNEEZE_BUILD_ROOT}/install/${_cfg_lc}/lib")
   set (CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${SNEEZE_BUILD_ROOT}/install/${_cfg_lc}/lib")
   set (CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${SNEEZE_BUILD_ROOT}/install/${_cfg_lc}/bin")
endforeach ()

# Automatically disable XR on iOS (VisionOS deferred) and Android (OpenXR
# loader is not part of the android-arm64 deps tree used by Rubidium).
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
   set (SNEEZE_ENABLE_XR OFF CACHE BOOL "" FORCE)
endif ()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Find modules live in src/cmake/. For local deps (USE_LOCAL_DEPS=ON, default),
# they search LIBS_DIR/<dep>/install/. For system deps, standard find_package.
# ---------------------------------------------------------------------------

list (APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Cross-compile: Android/iOS toolchains default CMAKE_FIND_ROOT_PATH_MODE_*
# to ONLY for PACKAGE/LIBRARY/INCLUDE, which blocks find_package / find_library
# / find_path from resolving user-set paths outside the sysroot. Set BOTH so
# LIBS_DIR-backed finds (anari, wasmtime, etc.) can resolve.
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
   set (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
   set (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
endif ()

if (USE_LOCAL_DEPS)
   set (ANARI_ROOT "${LIBS_DIR}/ANARI-SDK/install")
   list (APPEND CMAKE_PREFIX_PATH "${ANARI_ROOT}")
endif ()
find_package (anari REQUIRED)

if (SNEEZE_ENABLE_WASM)
   if (USE_LOCAL_DEPS)
      find_package (Wasmtime REQUIRED)
   else ()
      # Wasmtime has no system package — always from LIBS_DIR
      find_package (Wasmtime REQUIRED)
   endif ()
endif ()

if (USE_LOCAL_DEPS)
   find_package (SpirvTools REQUIRED)
   if (SNEEZE_ENABLE_XR)
      find_package (SneezeOpenXR REQUIRED)
   endif ()
   find_package (SneezeCurl REQUIRED)
   find_package (SneezeFreeType REQUIRED)
   find_package (SneezeRmlUi REQUIRED)
   find_package (Glslang REQUIRED)
   find_package (NlohmannJson REQUIRED)
   find_package (Fastgltf REQUIRED)
   find_package (BoringSSL REQUIRED)
   find_package (JwtCpp REQUIRED)

   # Vox ships its own package config (vox-config.cmake). It performs
   # consumer-side SPIRV-Cross discovery using SpirvCross_ROOT as a hint,
   # so we don't need a Sneeze-local FindVox / FindSpirvCross module.
   set (SpirvCross_ROOT "${LIBS_DIR}/SPIRV-Cross/install")
   find_package (Vox CONFIG REQUIRED
      PATHS "${LIBS_DIR}/Vox/install"
      NO_DEFAULT_PATH)

   # RMAP ships its own package config (RMAPConfig.cmake -> RMAP::RMAP). It was
   # built against Sneeze's own copies of its deps (full sharing, see
   # deps/rmap.cmake), so its config re-finds nlohmann_json / asio / websocketpp
   # via find_dependency -- put those install trees on CMAKE_PREFIX_PATH so those
   # imported targets resolve. RMAP's boringssl/curl/socketio come baked into the
   # link interface as absolute paths (relocated into LIBS_DIR and generalized
   # per-config by the dualize pass below, same as anari/vox).
   list (APPEND CMAKE_PREFIX_PATH
      "${LIBS_DIR}/nlohmann-json/install"
      "${LIBS_DIR}/asio/install"
      "${LIBS_DIR}/websocketpp/install"
      "${LIBS_DIR}/RMAP/install")
   find_package (RMAP CONFIG REQUIRED
      PATHS "${LIBS_DIR}/RMAP/install"
      NO_DEFAULT_PATH)

   # Map mirrors RMAP's integration (Map::Map target, MapConfig.cmake,
   # full-sharing of nlohmann-json/asio/websocketpp/rmap/boringssl/curl/socketio
   # -- see deps/map.cmake). CMAKE_PREFIX_PATH was extended above.
   find_package (Map CONFIG REQUIRED
      PATHS "${LIBS_DIR}/Map/install"
      NO_DEFAULT_PATH)
else ()
   find_package (spirv_tools REQUIRED CONFIG)
   if (SNEEZE_ENABLE_XR)
      find_package (OpenXR REQUIRED CONFIG)
   endif ()
   find_package (CURL REQUIRED)
   find_package (RmlUi REQUIRED)
   find_program (GLSLANG_VALIDATOR glslang REQUIRED)
   find_package (nlohmann_json REQUIRED)
   find_package (fastgltf CONFIG REQUIRED)
   set (FASTGLTF_LIB fastgltf::fastgltf)
   # jwt-cpp and BoringSSL have no upstream vcpkg configs that match our
   # pinned layout; USE_LOCAL_DEPS=OFF requires them on the system include
   # path already. Users configuring without LOCAL_DEPS must provide
   # BORINGSSL_INCLUDE / BORINGSSL_CRYPTO_LIB / JWT_CPP_INCLUDE manually.
endif ()

# ---------------------------------------------------------------------------
# Dual-config path generalization + artifact path relocation
# ---------------------------------------------------------------------------
# find_package + find_library resolved absolute paths against LIBS_DIR, which
# contains the SNEEZE_CONFIG_LC segment (e.g. "/release/"). To let ONE CMake
# tree build both Debug and Release (VS/Xcode/Ninja MC dropdown), we rewrite
# that config segment to a generator expression that expands at build time.
# At configure time LIBS_DIR still resolves to whichever config we configured
# against; at build time, Debug builds pull from deps/builds/<plat>/debug/libs,
# Release from .../release/libs.
#
# REQUIREMENT: for multi-config IDE builds (Debug + Release dropdown), both
# configs' dep trees should exist on disk before flipping configs. Jenkins and
# other Release-only CI agents may configure with only release/deps; include
# dirs stay concrete in that case (see _sneeze_dualize_target).
#
# ARTIFACT RELOCATION: RMAP/Map (and similar package configs) bake absolute
# paths from the machine that built the dep artifact — e.g.
# /home/runner/work/Sneeze/Sneeze/libs-linux/boringssl/install/lib/libssl.a.
# Rubidium CI downloads those artifacts under a different workspace root, so
# the baked paths do not exist. Flat CI layouts (libs-linux / libs-macos) also
# have no /release/ segment, so the dualize rewrite alone never touches them.
# _sneeze_relocate_to_libs_dir re-roots any missing absolute path onto the
# current LIBS_DIR when the same dep-relative suffix exists there.

set (SNEEZE_SDK_INCLUDE "${LIBS_DIR}/SneezeSDK/install/include")
string (REPLACE "\\" "/" SNEEZE_SDK_INCLUDE "${SNEEZE_SDK_INCLUDE}")

if (USE_LOCAL_DEPS)
   set (_sneeze_cfg_seg     "/${SNEEZE_CONFIG_LC}/")
   set (_sneeze_cfg_gen     "/$<LOWER_CASE:$<CONFIG>>/")
   set (_sneeze_cfg_debug   "/debug/")
   set (_sneeze_cfg_release "/release/")

   # Given a path that was naively string-substituted to target a different
   # config, return the actual on-disk path. Handles Sneeze's debug-suffix
   # deps: libcurl -> libcurl-d, openxr_loader -> openxr_loaderd,
   # spirv-cross-core -> spirv-cross-cored, etc. Falls back to the input.
   function (_sneeze_resolve_variant in_path out_var)
      if (EXISTS "${in_path}")
         set (${out_var} "${in_path}" PARENT_SCOPE)
         return ()
      endif ()
      foreach (_suffix IN ITEMS "d" "-d")
         string (REGEX REPLACE "\\.lib$" "${_suffix}.lib" _alt "${in_path}")
         if (EXISTS "${_alt}")
            set (${out_var} "${_alt}" PARENT_SCOPE)
            return ()
         endif ()
      endforeach ()
      string (REGEX REPLACE "([^/\\\\])-d\\.lib$" "\\1.lib" _alt "${in_path}")
      if (EXISTS "${_alt}")
         set (${out_var} "${_alt}" PARENT_SCOPE)
         return ()
      endif ()
      string (REGEX REPLACE "([^/\\\\])d\\.lib$" "\\1.lib" _alt "${in_path}")
      if (EXISTS "${_alt}")
         set (${out_var} "${_alt}" PARENT_SCOPE)
         return ()
      endif ()
      set (${out_var} "${in_path}" PARENT_SCOPE)
   endfunction ()

   # Re-root a foreign absolute path into the current LIBS_DIR tree.
   # Matches a LIBS_DIR child directory name inside the foreign path and, when
   # LIBS_DIR/<suffix> exists on disk, returns that relocated path. Leaves
   # flags, target names, generator expressions, and already-valid paths alone.
   function (_sneeze_relocate_to_libs_dir in_path out_var)
      string (REPLACE "\\" "/" _p "${in_path}")
      string (REPLACE "\\" "/" _libs "${LIBS_DIR}")

      # Flags, imported-target names, genex — not relocatable filesystem paths.
      if (NOT _p MATCHES "^(/|[A-Za-z]:)")
         set (${out_var} "${in_path}" PARENT_SCOPE)
         return ()
      endif ()
      if (_p MATCHES "\\$<")
         set (${out_var} "${in_path}" PARENT_SCOPE)
         return ()
      endif ()

      # Already valid on this machine (in-tree or coincidentally same path).
      if (EXISTS "${_p}")
         set (${out_var} "${_p}" PARENT_SCOPE)
         return ()
      endif ()

      # Already under current LIBS_DIR but missing — do not invent a substitute.
      string (FIND "${_p}" "${_libs}/" _idx)
      if (_idx EQUAL 0)
         set (${out_var} "${_p}" PARENT_SCOPE)
         return ()
      endif ()

      file (GLOB _dep_dirs LIST_DIRECTORIES true "${_libs}/*")
      foreach (_dep_abs IN LISTS _dep_dirs)
         if (NOT IS_DIRECTORY "${_dep_abs}")
            continue ()
         endif ()
         get_filename_component (_dep_name "${_dep_abs}" NAME)
         string (FIND "${_p}" "/${_dep_name}/" _dep_idx)
         if (_dep_idx LESS 0)
            continue ()
         endif ()
         math (EXPR _rel_start "${_dep_idx} + 1")
         string (SUBSTRING "${_p}" ${_rel_start} -1 _rel)
         if (EXISTS "${_libs}/${_rel}")
            set (${out_var} "${_libs}/${_rel}" PARENT_SCOPE)
            return ()
         endif ()
      endforeach ()

      set (${out_var} "${in_path}" PARENT_SCOPE)
   endfunction ()

   # Build a per-config library path expression from an existing absolute
   # path that points into one of the config trees.
   function (_sneeze_make_dual_lib in_path out_var)
      string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_debug}"   _d_path "${in_path}")
      string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_release}" _r_path "${in_path}")
      _sneeze_resolve_variant ("${_d_path}" _d)
      _sneeze_resolve_variant ("${_r_path}" _r)
      set (${out_var} "$<IF:$<CONFIG:Debug>,${_d},${_r}>" PARENT_SCOPE)
   endfunction ()

   # Rewrite scalar path variables. Library paths get a $<IF:$<CONFIG:Debug>...>
   # generator expression to accommodate Debug filename suffixes (e.g.
   # libcurl-d.lib vs libcurl.lib). Include dirs and other paths get a
   # simple segment replacement.
   foreach (_v IN ITEMS
      SPIRV_TOOLS_INCLUDE  SPIRV_TOOLS_LIB  SPIRV_TOOLS_OPT_LIB
      CURL_INCLUDE         CURL_LIB
      FREETYPE_INCLUDE     FREETYPE_LIB
      RMLUI_INCLUDE        RMLUI_CORE_LIB   RMLUI_DEBUGGER_LIB
      NLOHMANN_JSON_INCLUDE
      FASTGLTF_INCLUDE     FASTGLTF_LIB
      BORINGSSL_INCLUDE    BORINGSSL_CRYPTO_LIB  BORINGSSL_SSL_LIB
      JWT_CPP_INCLUDE
      SNEEZE_SDK_INCLUDE
      WASMTIME_INCLUDE     WASMTIME_LIB
      OPENXR_INCLUDE       OPENXR_LOADER_LIB
   )
      if (DEFINED ${_v})
         set (_val "${${_v}}")
         if (_val MATCHES "\\.(lib|a|so|dylib)$" AND _val MATCHES "${_sneeze_cfg_seg}")
            _sneeze_make_dual_lib ("${_val}" _new)
            set (${_v} "${_new}")
         else ()
            string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_gen}" _new "${_val}")
            set (${_v} "${_new}")
         endif ()
      endif ()
   endforeach ()

   # Rewrite imported targets (anari + vox + RMAP + Map). Walk each target's
   # INTERFACE_* properties and its transitive INTERFACE_LINK_LIBRARIES,
   # relocating foreign absolute paths into LIBS_DIR, then setting both
   # IMPORTED_LOCATION_DEBUG and IMPORTED_LOCATION_RELEASE from the single
   # path we currently have. Generator expressions go into include dirs and
   # INTERFACE_LINK_LIBRARIES entries that are raw paths.

   function (_sneeze_dualize_target tgt)
      if (NOT TARGET ${tgt})
         return ()
      endif ()
      get_target_property (_aliased ${tgt} ALIASED_TARGET)
      if (_aliased)
         set (tgt "${_aliased}")
      endif ()

      get_property (_already GLOBAL PROPERTY _SNEEZE_DUALIZED_${tgt} SET)
      if (_already)
         return ()
      endif ()
      set_property (GLOBAL PROPERTY _SNEEZE_DUALIZED_${tgt} TRUE)

      get_target_property (_type ${tgt} TYPE)

      # Walk transitive deps first.
      foreach (_prop IN ITEMS INTERFACE_LINK_LIBRARIES IMPORTED_LINK_DEPENDENT_LIBRARIES)
         get_target_property (_deps ${tgt} ${_prop})
         if (_deps)
            foreach (_d IN LISTS _deps)
               # Strip generator expressions like $<LINK_ONLY:foo>
               string (REGEX REPLACE "^\\\$<[^:]+:(.*)>$" "\\1" _d_clean "${_d}")
               if (TARGET ${_d_clean})
                  _sneeze_dualize_target (${_d_clean})
               endif ()
            endforeach ()
         endif ()
      endforeach ()

      # Per-config IMPORTED_LOCATION_* on library targets.
      if (_type MATCHES "LIBRARY")
         set (_ref "")
         foreach (_prop IN ITEMS
            IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG
            IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION_MINSIZEREL
            IMPORTED_LOCATION_NOCONFIG IMPORTED_LOCATION)
            get_target_property (_v ${tgt} ${_prop})
            if (_v)
               set (_ref "${_v}")
               break ()
            endif ()
         endforeach ()
         if (_ref)
            set (_ref_orig "${_ref}")
            _sneeze_relocate_to_libs_dir ("${_ref}" _ref)
            if (_ref MATCHES "${_sneeze_cfg_seg}")
               string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_debug}"   _d_raw "${_ref}")
               string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_release}" _r_raw "${_ref}")
               _sneeze_resolve_variant ("${_d_raw}" _d)
               _sneeze_resolve_variant ("${_r_raw}" _r)
               set_target_properties (${tgt} PROPERTIES
                  IMPORTED_LOCATION_DEBUG   "${_d}"
                  IMPORTED_LOCATION_RELEASE "${_r}"
                  IMPORTED_CONFIGURATIONS   "Debug;Release")
            elseif (NOT _ref STREQUAL _ref_orig)
               # Flat CI layout: foreign absolute path relocated into LIBS_DIR.
               set_target_properties (${tgt} PROPERTIES IMPORTED_LOCATION "${_ref}")
            endif ()
         endif ()

         # Similarly IMPORTED_IMPLIB_* (Windows shared libs have both a .dll
         # IMPORTED_LOCATION and a .lib IMPORTED_IMPLIB).
         set (_ref "")
         foreach (_prop IN ITEMS
            IMPORTED_IMPLIB_RELEASE IMPORTED_IMPLIB_DEBUG
            IMPORTED_IMPLIB_RELWITHDEBINFO IMPORTED_IMPLIB_MINSIZEREL
            IMPORTED_IMPLIB_NOCONFIG IMPORTED_IMPLIB)
            get_target_property (_v ${tgt} ${_prop})
            if (_v)
               set (_ref "${_v}")
               break ()
            endif ()
         endforeach ()
         if (_ref)
            set (_ref_orig "${_ref}")
            _sneeze_relocate_to_libs_dir ("${_ref}" _ref)
            if (_ref MATCHES "${_sneeze_cfg_seg}")
               string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_debug}"   _d_raw "${_ref}")
               string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_release}" _r_raw "${_ref}")
               _sneeze_resolve_variant ("${_d_raw}" _d)
               _sneeze_resolve_variant ("${_r_raw}" _r)
               set_target_properties (${tgt} PROPERTIES
                  IMPORTED_IMPLIB_DEBUG   "${_d}"
                  IMPORTED_IMPLIB_RELEASE "${_r}")
            elseif (NOT _ref STREQUAL _ref_orig)
               set_target_properties (${tgt} PROPERTIES IMPORTED_IMPLIB "${_ref}")
            endif ()
         endif ()
      endif ()

      # CMake validates INTERFACE_INCLUDE_DIRECTORIES on imported targets at
      # generate time, evaluating any genex per entry in CMAKE_CONFIGURATION_TYPES.
      # If we unconditionally rewrite "/release/" to "/$<LOWER_CASE:$<CONFIG>>/",
      # configuring against a tree that has only one config's deps on disk fails
      # with "Imported target ... includes non-existent path .../debug/...".
      # Apply the genex only when BOTH config-resolved paths exist on disk.
      # Otherwise leave the include path concrete to whatever find_package just
      # resolved -- always valid for the configure-time config, and headers are
      # config-invariant for the deps Sneeze ships.
      get_target_property (_inc ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
      if (_inc)
         set (_inc_new "")
         foreach (_p IN LISTS _inc)
            _sneeze_relocate_to_libs_dir ("${_p}" _p)
            if (_p MATCHES "${_sneeze_cfg_seg}")
               string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_debug}"   _p_dbg "${_p}")
               string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_release}" _p_rel "${_p}")
               if (EXISTS "${_p_dbg}" AND EXISTS "${_p_rel}")
                  string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_gen}" _p_new "${_p}")
                  list (APPEND _inc_new "${_p_new}")
               else ()
                  list (APPEND _inc_new "${_p}")
               endif ()
            else ()
               list (APPEND _inc_new "${_p}")
            endif ()
         endforeach ()
         set_target_properties (${tgt} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_inc_new}")
      endif ()

      # INTERFACE_LINK_LIBRARIES entries that are raw filesystem paths
      # (not target names). Relocate foreign absolute paths into LIBS_DIR,
      # then rewrite each config-segment entry to a generator expression.
      # Library paths (.lib/.a/.so/.dylib) get a $<IF:$<CONFIG:Debug>...>
      # expression so per-config filename suffixes resolve correctly.
      get_target_property (_ilinks ${tgt} INTERFACE_LINK_LIBRARIES)
      if (_ilinks)
         set (_ilinks_new "")
         foreach (_l IN LISTS _ilinks)
            _sneeze_relocate_to_libs_dir ("${_l}" _l)
            if (_l MATCHES "${_sneeze_cfg_seg}")
               if (_l MATCHES "\\.(lib|a|so|dylib)$")
                  _sneeze_make_dual_lib ("${_l}" _l_new)
               else ()
                  string (REPLACE "${_sneeze_cfg_seg}" "${_sneeze_cfg_gen}" _l_new "${_l}")
               endif ()
               list (APPEND _ilinks_new "${_l_new}")
            else ()
               list (APPEND _ilinks_new "${_l}")
            endif ()
         endforeach ()
         set_target_properties (${tgt} PROPERTIES INTERFACE_LINK_LIBRARIES "${_ilinks_new}")
      endif ()
   endfunction ()

   foreach (_tgt IN ITEMS anari::anari_static vox::vox RMAP::RMAP Map::Map)
      if (TARGET ${_tgt})
         _sneeze_dualize_target (${_tgt})
      endif ()
   endforeach ()

   # Android Bionic has pthreads in libc; NDK r21+ ships no libpthread.
   # RMAP/Map/curl/ANARI export INTERFACE $-LINK_ONLY:pthread- which then
   # fails the Rubidium link with --fatal-warnings. Strip it here rather
   # than rebuilding those ExternalProjects.
   if (ANDROID)
      foreach (_tgt IN ITEMS RMAP::RMAP Map::Map CURL::libcurl anari::anari_static)
         if (TARGET ${_tgt})
            get_target_property (_ilinks ${_tgt} INTERFACE_LINK_LIBRARIES)
            if (_ilinks)
               list (FILTER _ilinks EXCLUDE REGEX "pthread")
               list (FILTER _ilinks EXCLUDE REGEX "Threads::Threads")
               set_target_properties (${_tgt} PROPERTIES INTERFACE_LINK_LIBRARIES "${_ilinks}")
            endif ()
         endif ()
      endforeach ()
      if (TARGET Threads::Threads)
         set_target_properties (Threads::Threads PROPERTIES INTERFACE_LINK_LIBRARIES "")
      endif ()
   endif ()
endif ()

# ---------------------------------------------------------------------------
# Compile GLSL compute kernels to SPIR-V at build time
# ---------------------------------------------------------------------------

set (KERNEL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/compute/kernels")
set (KERNEL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file (MAKE_DIRECTORY "${KERNEL_OUTPUT_DIR}")

set (KERNEL_SOURCES
   "${KERNEL_SOURCE_DIR}/test_proximity.comp"
)

set (KERNEL_SPV_OUTPUTS)
foreach (KERNEL_SRC ${KERNEL_SOURCES})
   get_filename_component (KERNEL_NAME "${KERNEL_SRC}" NAME_WE)
   set (KERNEL_SPV "${KERNEL_OUTPUT_DIR}/${KERNEL_NAME}.spv")
   add_custom_command (
      OUTPUT "${KERNEL_SPV}"
      COMMAND "${GLSLANG_VALIDATOR}" -V "${KERNEL_SRC}" -o "${KERNEL_SPV}"
      DEPENDS "${KERNEL_SRC}"
      COMMENT "Compiling ${KERNEL_NAME}.comp -> ${KERNEL_NAME}.spv"
      VERBATIM
   )
   list (APPEND KERNEL_SPV_OUTPUTS "${KERNEL_SPV}")
endforeach ()

add_custom_target (CompileKernels ALL DEPENDS ${KERNEL_SPV_OUTPUTS})

# ---------------------------------------------------------------------------
# SPIR-V embedding — Win32 .rc or cross-platform generated C source
# ---------------------------------------------------------------------------

if (WIN32)
   set (KERNEL_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/kernels.rc")
   file (WRITE "${KERNEL_RC_FILE}" "// Auto-generated by CMake - embeds compiled SPIR-V kernels\n")
   foreach (KERNEL_SRC ${KERNEL_SOURCES})
      get_filename_component (KERNEL_NAME "${KERNEL_SRC}" NAME_WE)
      string (TOUPPER "${KERNEL_NAME}" KERNEL_ID)
      file (APPEND "${KERNEL_RC_FILE}"
         "${KERNEL_ID} SPV_KERNEL \"${KERNEL_OUTPUT_DIR}/${KERNEL_NAME}.spv\"\n"
      )
   endforeach ()
   set (KERNEL_EMBED_FILES "${KERNEL_RC_FILE}")
else ()
   # Generate a C source file that embeds each SPIR-V binary as a byte array.
   # A CMake script runs after CompileKernels and reads the .spv files.
   set (KERNEL_C_FILE "${CMAKE_CURRENT_BINARY_DIR}/kernels_embedded.c")
   set (KERNEL_EMBED_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/embed_kernels.cmake")

   # Build the list of kernel names for the embed script
   set (_kernel_names)
   foreach (KERNEL_SRC ${KERNEL_SOURCES})
      get_filename_component (_kn "${KERNEL_SRC}" NAME_WE)
      list (APPEND _kernel_names "${_kn}")
   endforeach ()

   file (WRITE "${KERNEL_EMBED_SCRIPT}" [=[
cmake_minimum_required (VERSION 3.20)
set (_out "// Auto-generated by CMake — embedded SPIR-V kernels\n")
string (APPEND _out "#include <stdint.h>\n#include <stddef.h>\n\n")
set (_reg "")
foreach (_name ${KERNEL_NAMES})
   string (TOUPPER "${_name}" _ID)
   file (READ "${SPV_DIR}/${_name}.spv" _bytes HEX)
   string (LENGTH "${_bytes}" _hex_len)
   math (EXPR _byte_count "${_hex_len} / 2")
   string (REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex_list "${_bytes}")
   string (APPEND _out "static const uint8_t k_spv_${_ID}[] = {${_hex_list}};\n")
   string (APPEND _out "static const size_t  k_spv_${_ID}_size = ${_byte_count};\n\n")
   string (APPEND _reg "{\"${_ID}\", k_spv_${_ID}, k_spv_${_ID}_size},\n")
endforeach ()
string (APPEND _out "struct KernelEntry { const char* name; const uint8_t* data; size_t size; };\n")
string (APPEND _out "const struct KernelEntry g_embedded_kernels[] = {\n${_reg}};\n")
math (EXPR _count "0")
foreach (_name ${KERNEL_NAMES})
   math (EXPR _count "${_count} + 1")
endforeach ()
string (APPEND _out "const size_t g_embedded_kernel_count = ${_count};\n")
file (WRITE "${OUT_FILE}" "${_out}")
]=])

   add_custom_command (
      OUTPUT "${KERNEL_C_FILE}"
      COMMAND ${CMAKE_COMMAND}
         -DKERNEL_NAMES=${_kernel_names}
         -DSPV_DIR=${KERNEL_OUTPUT_DIR}
         -DOUT_FILE=${KERNEL_C_FILE}
         -P ${KERNEL_EMBED_SCRIPT}
      DEPENDS ${KERNEL_SPV_OUTPUTS}
      COMMENT "Generating cross-platform SPIR-V embedding source"
      VERBATIM
   )
   set (KERNEL_EMBED_FILES "${KERNEL_C_FILE}")
endif ()

# ---------------------------------------------------------------------------
# Source files by module
# ---------------------------------------------------------------------------

set (CORE_SOURCES
   sneeze/Types.cpp
   sneeze/Engine.cpp
   context/Context.cpp
   context/Container.cpp
   sneeze/Thread.cpp
   sneeze/control/Agent.cpp
   sneeze/control/Control.cpp
   sneeze/control/Compositor.cpp
   sneeze/control/IJob.cpp
   sneeze/control/Scrub.cpp
   sneeze/control/AgentC.cpp
   sneeze/control/AgentTimer.cpp
   sneeze/control/Pool.cpp
   sneeze/control/Pool_Queue.cpp
   sneeze/control/Pool_Cycle.cpp
   sneeze/control/Fetch.cpp)
set (CORE_HEADERS
   sneeze/Pch.h
   sneeze/Types.h
   sneeze/Engine.h
   sneeze/control/Control.h)

set (SOM_SOURCES
   context/viewport/Viewport.cpp
   context/scene/Node.cpp
   context/scene/Fabric.cpp
   context/scene/Scene.cpp
   context/scene/RmcObject.cpp
   context/scene/MapSvc.cpp
   context/scene/AccessControl.cpp)
set (SOM_HEADERS
   context/scene/RmcObject.h
   context/scene/MapSvc.h
   context/scene/AccessControl.h)

# Embed Mozilla's CA bundle (cacert.pem) as a string blob so libcurl can
# verify HTTPS certificates on platforms whose TLS stack has no OS trust
# store wired into curl (Linux/Android/macOS desktop with BoringSSL, in
# particular). Win32 uses SChannel + the Windows store; iOS uses
# SecureTransport + Keychain; both ignore CAINFO_BLOB, but compiling the
# symbol unconditionally avoids a per-platform link surface.
set (SNEEZE_CACERT_PEM "${CMAKE_CURRENT_BINARY_DIR}/cacert.pem")
if (NOT EXISTS "${SNEEZE_CACERT_PEM}")
   message (STATUS "Fetching cacert.pem from curl.se")
   file (DOWNLOAD https://curl.se/ca/cacert.pem "${SNEEZE_CACERT_PEM}"
         TIMEOUT 30
         TLS_VERIFY ON
         STATUS _cacert_status)
   list (GET _cacert_status 0 _cacert_code)
   if (NOT _cacert_code EQUAL 0)
      file (REMOVE "${SNEEZE_CACERT_PEM}")
      message (FATAL_ERROR "Failed to download cacert.pem: ${_cacert_status}")
   endif ()
endif ()
file (READ "${SNEEZE_CACERT_PEM}" _cacert_raw)
string (LENGTH "${_cacert_raw}" _cacert_len)

set (_cacert_cpp "${CMAKE_CURRENT_BINARY_DIR}/cacert_data.cpp")
set (_chunk_size 8000)
file (WRITE "${_cacert_cpp}"
   "// Auto-generated — do not edit. Mozilla NSS CA bundle for CURLOPT_CAINFO_BLOB.\n"
   "namespace SNEEZE {\n"
   "   extern const char* const g_szCaCertPem;\n"
   "   extern const unsigned long g_nCaCertPemLen;\n"
   "}\n"
   "namespace SNEEZE {\n"
   "   static const char s_data[] =\n")
set (_off 0)
while (_off LESS _cacert_len)
   string (SUBSTRING "${_cacert_raw}" ${_off} ${_chunk_size} _chunk)
   string (REPLACE "\\" "\\\\" _chunk "${_chunk}")
   string (REPLACE "\"" "\\\"" _chunk "${_chunk}")
   string (REPLACE "\n" "\\n\"\n      \"" _chunk "${_chunk}")
   file (APPEND "${_cacert_cpp}" "      \"${_chunk}\"\n")
   math (EXPR _off "${_off} + ${_chunk_size}")
endwhile ()
file (APPEND "${_cacert_cpp}"
   "      ;\n"
   "   const char* const g_szCaCertPem = s_data;\n"
   "   const unsigned long g_nCaCertPemLen = sizeof(s_data) - 1;\n"
   "}\n")
# Kept out of NETWORK_SOURCES because that list also feeds source_group(TREE),
# which requires every file to live under CMAKE_CURRENT_SOURCE_DIR. The
# generated source is added directly to the Sneeze target below instead.
set (NETWORK_GENERATED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/cacert_data.cpp")

set (CONTAINER_HEADERS)

set (CONSOLE_SOURCES  sneeze/console/Console.cpp sneeze/console/Entry.cpp sneeze/console/Stream.cpp sneeze/console/Block.cpp)
set (CONSOLE_HEADERS  )

set (NETWORK_SOURCES  sneeze/network/Network.cpp sneeze/network/Cache.cpp sneeze/network/File.cpp sneeze/network/Asset.cpp)
set (NETWORK_HEADERS  sneeze/network/Network.h)

set (STORAGE_SOURCES  sneeze/storage/Storage.cpp sneeze/storage/Silo.cpp sneeze/storage/Unit.cpp)
set (STORAGE_HEADERS)

set (PERSONA_SOURCES  persona/Persona.cpp)
set (PERSONA_HEADERS)

set (RENDERER_SOURCES  context/viewport/AnariRenderer.cpp  context/viewport/UVSphere.cpp  context/viewport/GltfMesh.cpp)
set (RENDERER_HEADERS  context/viewport/Viewport.h  context/viewport/AnariRenderer.h)

if (SNEEZE_ENABLE_WASM)
   set (WASM_SOURCES
      deps/wasm/Wasm_Runtime.cpp
      deps/wasm/Wasm_Store.cpp
      deps/wasm/Wasm_Instance.cpp
      deps/wasm/Wasm_Timers.cpp
      deps/wasm/HostFunctions.cpp
      deps/wasm/Chrono.cpp)
   set (WASM_HEADERS
      deps/wasm/Wasm.h
      deps/wasm/HostFunctions.h
      deps/wasm/Chrono.h)
endif ()

set (SPIRV_SOURCES   deps/spirv/SpvPipeline.cpp)
set (SPIRV_HEADERS   deps/spirv/SpvPipeline.h)

set (XR_HEADERS deps/xr/XrRuntime.h)
if (SNEEZE_ENABLE_XR)
   # Real OpenXR loader. Pulls in libopenxr_loader via OPENXR_LOADER_LIB.
   # XrRuntime_Android.cpp: Galaxy XR face/body on __ANDROID__; stubs otherwise.
   set (XR_SOURCES
      deps/xr/XrRuntime.cpp
      deps/xr/XrRuntime_Android.cpp
      deps/xr/XrFaceParameterMap.cpp)
else ()
   # No-op stub for builds without OpenXR (e.g. iOS). Same XR_RUNTIME class,
   # HasRuntime () returns false, Initialize () succeeds silently. Lets
   # Engine.cpp drop its #ifdef SNEEZE_HAS_XR guards entirely.
   set (XR_SOURCES
      deps/xr/XrRuntime_Stub.cpp
      deps/xr/XrFaceParameterMap.cpp)
endif ()

set (UI_SOURCES      deps/ui/Ui_Context.cpp deps/ui/Ui_Render.cpp deps/ui/Ui_Panel.cpp)
set (UI_HEADERS      deps/ui/Ui_Context.h deps/ui/Ui_Render.h deps/ui/Ui_Panel.h)

set (COMPUTE_SOURCES deps/compute/EmbeddedKernels.cpp deps/compute/ComputeDispatch.cpp)
set (COMPUTE_HEADERS deps/compute/EmbeddedKernels.h deps/compute/ComputeDispatch.h)

set (MSF_SOURCES
   context/msf/MsfFile.cpp
   context/msf/Chain.cpp)
set (MSF_HEADERS)

set (STB_SOURCES  deps/stb/stb_image_impl.cpp deps/stb/Image.cpp)
set (STB_HEADERS  deps/stb/stb_image.h)

set (GLTF_SOURCES  deps/gltf/Gltf.cpp)
set (GLTF_HEADERS  deps/gltf/Gltf.h)

# Decode-only meshoptimizer (EXT_meshopt_compression). Vendored under
# src/deps/meshoptimizer so cmake -S src (CI sneeze job) does not need
# deps/repos/filament. The four decode units compile into Sneeze; encoder
# and optimizer units are omitted.
set (MESHOPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/meshoptimizer")
set (MESHOPT_SOURCES
   ${MESHOPT_DIR}/allocator.cpp
   ${MESHOPT_DIR}/vertexcodec.cpp
   ${MESHOPT_DIR}/indexcodec.cpp
   ${MESHOPT_DIR}/vertexfilter.cpp
)

# Decode-only Draco (KHR_draco_mesh_compression). Vendored under src/deps/draco.
# Encoder, I/O, and animation units are omitted. Features header is checked in
# at deps/gltf/draco_config so CMake and the hand-maintained vcxproj share one
# file (filament's cmake would generate it).
set (DRACO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/draco")
set (DRACO_SRC "${DRACO_DIR}/src/draco")
set (DRACO_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/gltf/draco_config")
set (DRACO_SOURCES
   ${DRACO_SRC}/attributes/attribute_octahedron_transform.cc
   ${DRACO_SRC}/attributes/attribute_quantization_transform.cc
   ${DRACO_SRC}/attributes/attribute_transform.cc
   ${DRACO_SRC}/attributes/geometry_attribute.cc
   ${DRACO_SRC}/attributes/point_attribute.cc
   ${DRACO_SRC}/compression/attributes/attributes_decoder.cc
   ${DRACO_SRC}/compression/attributes/kd_tree_attributes_decoder.cc
   ${DRACO_SRC}/compression/attributes/sequential_attribute_decoder.cc
   ${DRACO_SRC}/compression/attributes/sequential_attribute_decoders_controller.cc
   ${DRACO_SRC}/compression/attributes/sequential_integer_attribute_decoder.cc
   ${DRACO_SRC}/compression/attributes/sequential_normal_attribute_decoder.cc
   ${DRACO_SRC}/compression/attributes/sequential_quantization_attribute_decoder.cc
   ${DRACO_SRC}/compression/bit_coders/adaptive_rans_bit_decoder.cc
   ${DRACO_SRC}/compression/bit_coders/direct_bit_decoder.cc
   ${DRACO_SRC}/compression/bit_coders/rans_bit_decoder.cc
   ${DRACO_SRC}/compression/bit_coders/symbol_bit_decoder.cc
   ${DRACO_SRC}/compression/decode.cc
   ${DRACO_SRC}/compression/draco_compression_options.cc
   ${DRACO_SRC}/compression/entropy/shannon_entropy.cc
   ${DRACO_SRC}/compression/entropy/symbol_decoding.cc
   ${DRACO_SRC}/compression/mesh/mesh_decoder.cc
   ${DRACO_SRC}/compression/mesh/mesh_edgebreaker_decoder.cc
   ${DRACO_SRC}/compression/mesh/mesh_edgebreaker_decoder_impl.cc
   ${DRACO_SRC}/compression/mesh/mesh_sequential_decoder.cc
   ${DRACO_SRC}/compression/point_cloud/point_cloud_decoder.cc
   ${DRACO_SRC}/core/bit_utils.cc
   ${DRACO_SRC}/core/bounding_box.cc
   ${DRACO_SRC}/core/cycle_timer.cc
   ${DRACO_SRC}/core/data_buffer.cc
   ${DRACO_SRC}/core/decoder_buffer.cc
   ${DRACO_SRC}/core/divide.cc
   ${DRACO_SRC}/core/draco_types.cc
   ${DRACO_SRC}/core/encoder_buffer.cc
   ${DRACO_SRC}/core/hash_utils.cc
   ${DRACO_SRC}/core/options.cc
   ${DRACO_SRC}/core/quantization_utils.cc
   ${DRACO_SRC}/mesh/corner_table.cc
   ${DRACO_SRC}/mesh/mesh.cc
   ${DRACO_SRC}/mesh/mesh_are_equivalent.cc
   ${DRACO_SRC}/mesh/mesh_attribute_corner_table.cc
   ${DRACO_SRC}/mesh/mesh_cleanup.cc
   ${DRACO_SRC}/mesh/mesh_features.cc
   ${DRACO_SRC}/mesh/mesh_misc_functions.cc
   ${DRACO_SRC}/mesh/mesh_stripifier.cc
   ${DRACO_SRC}/mesh/triangle_soup_mesh_builder.cc
   ${DRACO_SRC}/metadata/geometry_metadata.cc
   ${DRACO_SRC}/metadata/metadata.cc
   ${DRACO_SRC}/metadata/metadata_decoder.cc
   ${DRACO_SRC}/metadata/property_attribute.cc
   ${DRACO_SRC}/metadata/property_table.cc
   ${DRACO_SRC}/metadata/structural_metadata.cc
   ${DRACO_SRC}/metadata/structural_metadata_schema.cc
   ${DRACO_SRC}/point_cloud/point_cloud.cc
   ${DRACO_SRC}/point_cloud/point_cloud_builder.cc
)

set (INCLUDE_HEADERS
   ../include/Sneeze.h
   ../include/XrTracking.h
   ../include/Context.h
   ../include/Persona.h
   ../include/Network.h
   ../include/Container.h
   ../include/Scene.h
   ../include/Viewport.h
   ../include/Storage.h
   ../include/Console.h
   ../include/Image.h
   ../include/Msf.h)

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------

add_library (Sneeze STATIC
   ${CORE_SOURCES}      ${CORE_HEADERS}
   ${SOM_SOURCES}       ${SOM_HEADERS}
   ${NETWORK_SOURCES}   ${NETWORK_HEADERS}
   ${CONTAINER_HEADERS}
   ${STORAGE_SOURCES}   ${STORAGE_HEADERS}
   ${CONSOLE_SOURCES}   ${CONSOLE_HEADERS}
   ${PERSONA_SOURCES}   ${PERSONA_HEADERS}
   ${RENDERER_SOURCES}  ${RENDERER_HEADERS}
   ${WASM_SOURCES}      ${WASM_HEADERS}
   ${SPIRV_SOURCES}     ${SPIRV_HEADERS}
   ${XR_SOURCES}        ${XR_HEADERS}
   ${UI_SOURCES}        ${UI_HEADERS}
   ${COMPUTE_SOURCES}   ${COMPUTE_HEADERS}
   ${MSF_SOURCES}       ${MSF_HEADERS}
   ${STB_SOURCES}       ${STB_HEADERS}
   ${GLTF_SOURCES}      ${GLTF_HEADERS}
   ${MESHOPT_SOURCES}
   ${DRACO_SOURCES}
   ${INCLUDE_HEADERS}
   ${KERNEL_EMBED_FILES}
   ${NETWORK_GENERATED_SOURCES}
)
add_dependencies (Sneeze CompileKernels)

# ---------------------------------------------------------------------------
# Precompiled header (MSVC: speeds up rebuilds by ~3-5x; same gain on
# Clang/GCC via -include). CMake synthesizes cmake_pch.cxx and force-includes
# Pch.h ahead of every Sneeze .cpp -- no per-file edits required. Guard on
# CXX so the C embed file and Windows .rc resource skip the PCH.
# ---------------------------------------------------------------------------
target_precompile_headers (Sneeze PRIVATE
   "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/sneeze/Pch.h>")
set_source_files_properties (${MESHOPT_SOURCES} ${DRACO_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# On Windows, .res objects inside static libraries don't get pulled into the
# final executable by the linker. Expose the .rc as an INTERFACE source so
# executables linking Sneeze automatically compile and link the resource.
if (WIN32)
   target_sources (Sneeze INTERFACE ${KERNEL_EMBED_FILES})
endif ()

target_include_directories (Sneeze PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
   ${SNEEZE_SDK_INCLUDE}
   ${CMAKE_CURRENT_SOURCE_DIR}/sneeze
   ${CMAKE_CURRENT_SOURCE_DIR}/context
   ${CMAKE_CURRENT_SOURCE_DIR}/context/viewport
   ${CMAKE_CURRENT_SOURCE_DIR}/deps
   ${SPIRV_TOOLS_INCLUDE}
   ${CURL_INCLUDE}
   ${FREETYPE_INCLUDE}
   ${RMLUI_INCLUDE}
   ${NLOHMANN_JSON_INCLUDE}
   ${FASTGLTF_INCLUDE}
   ${BORINGSSL_INCLUDE}
   ${JWT_CPP_INCLUDE}
   ${MESHOPT_DIR}
)
target_include_directories (Sneeze PRIVATE
   ${DRACO_DIR}/src
   ${DRACO_CONFIG_DIR}
)
if (SNEEZE_ENABLE_WASM)
   target_include_directories (Sneeze PUBLIC ${WASMTIME_INCLUDE})
endif ()
if (SNEEZE_ENABLE_XR)
   target_include_directories (Sneeze PUBLIC ${OPENXR_INCLUDE})
endif ()

target_compile_definitions (Sneeze PUBLIC
   CURL_STATICLIB
   RMLUI_STATIC_LIB
   SNEEZE_VERSION="${PROJECT_VERSION}"
)
if (SNEEZE_ENABLE_WASM)
   target_compile_definitions (Sneeze PUBLIC SNEEZE_HAS_WASM WASM_API_EXTERN= WASI_API_EXTERN=)
endif ()
# SNEEZE_HAS_XR compile def is gone — XR_RUNTIME class is always present, the
# real-vs-stub .cpp selection above is what gates the OpenXR linkage.

target_link_libraries (Sneeze PUBLIC
   anari::anari_static
   ${SPIRV_TOOLS_LIB}
   ${SPIRV_TOOLS_OPT_LIB}
   ${CURL_LIB}
   ${RMLUI_CORE_LIB}
   ${RMLUI_DEBUGGER_LIB}
   ${FREETYPE_LIB}
   ${BORINGSSL_SSL_LIB}
   ${BORINGSSL_CRYPTO_LIB}
   ${FASTGLTF_LIB}
   vox::vox
   RMAP::RMAP
   Map::Map
)

if (SNEEZE_ENABLE_WASM)
   target_link_libraries (Sneeze PUBLIC ${WASMTIME_LIB})
endif ()
if (SNEEZE_ENABLE_XR)
   target_link_libraries (Sneeze PUBLIC ${OPENXR_LOADER_LIB})
endif ()

# Platform-specific system libraries.
# DX12 / Metal / Foundation / Vulkan are all pulled in transitively by
# vox::vox's INTERFACE_LINK_LIBRARIES (see Vox's vox-config.cmake).
if (WIN32)
   # Wasmtime Windows deps
   target_link_libraries (Sneeze PUBLIC ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt)
   # curl Windows deps
   target_link_libraries (Sneeze PUBLIC wldap32 crypt32 normaliz)
   # DWM (compositor vsync)
   target_link_libraries (Sneeze PUBLIC dwmapi)
elseif (APPLE)
   find_library (SECURITY_FW Security)
   find_library (COREFOUNDATION_FW CoreFoundation)
   find_library (SYSTEMCONFIGURATION_FW SystemConfiguration)
   target_link_libraries (Sneeze PUBLIC
      pthread dl
      ${SECURITY_FW} ${COREFOUNDATION_FW} ${SYSTEMCONFIGURATION_FW}
   )
elseif (ANDROID)
   target_link_libraries (Sneeze PUBLIC log android)
else ()
   target_link_libraries (Sneeze PUBLIC pthread dl m)
endif ()

# ---------------------------------------------------------------------------
# IDE source grouping
# ---------------------------------------------------------------------------

source_group (TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
   ${CORE_SOURCES}      ${CORE_HEADERS}
   ${SOM_SOURCES}       ${SOM_HEADERS}
   ${NETWORK_SOURCES}   ${NETWORK_HEADERS}
   ${CONTAINER_HEADERS}
   ${STORAGE_SOURCES}   ${STORAGE_HEADERS}
   ${CONSOLE_SOURCES}   ${CONSOLE_HEADERS}
   ${PERSONA_SOURCES}   ${PERSONA_HEADERS}
   ${RENDERER_SOURCES}  ${RENDERER_HEADERS}
   ${WASM_SOURCES}      ${WASM_HEADERS}
   ${SPIRV_SOURCES}     ${SPIRV_HEADERS}
   ${XR_SOURCES}        ${XR_HEADERS}
   ${UI_SOURCES}        ${UI_HEADERS}
   ${COMPUTE_SOURCES}   ${COMPUTE_HEADERS}
   ${MSF_SOURCES}       ${MSF_HEADERS}
   ${STB_SOURCES}       ${STB_HEADERS}
   ${GLTF_SOURCES}      ${GLTF_HEADERS}
)
source_group ("include" FILES ${INCLUDE_HEADERS})

# ---------------------------------------------------------------------------
# Tools (desktop-only CLI utilities built on top of the Sneeze library)
# ---------------------------------------------------------------------------

if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
   set (_DEFAULT_BUILD_TOOLS OFF)
else ()
   set (_DEFAULT_BUILD_TOOLS ON)
endif ()
option (SNEEZE_BUILD_TOOLS "Build desktop CLI tools (SignMsf, GenCerts, ...)" ${_DEFAULT_BUILD_TOOLS})

if (SNEEZE_BUILD_TOOLS)
   add_subdirectory ("${CMAKE_CURRENT_SOURCE_DIR}/../tools/SignMsf" "${CMAKE_CURRENT_BINARY_DIR}/tools/SignMsf")
   add_subdirectory ("${CMAKE_CURRENT_SOURCE_DIR}/../tools/GenCerts" "${CMAKE_CURRENT_BINARY_DIR}/tools/GenCerts")
endif ()

# ---------------------------------------------------------------------------
# Tests (separate directory, nested build output under build/tests/)
# ---------------------------------------------------------------------------

option (SNEEZE_BUILD_TESTS "Build SneezeTest" ON)

if (SNEEZE_BUILD_TESTS)
   add_subdirectory ("${CMAKE_CURRENT_SOURCE_DIR}/../tests" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif ()
