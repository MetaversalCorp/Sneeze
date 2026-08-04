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

# ---------------------------------------------------------------------------
# verify.cmake -- run the shared read-only dependency gate from a script or CI:
#
#   cmake -P deps/verify.cmake [-DMODE=offline|freshness] [-DTARGET=<dep>]
#                              [-DSNEEZE_DEP_REPO=<path>]
#                              [-DSNEEZE_STAMP_DIRS=<dir1;dir2;...>]
#
# MODE   offline (default) = local checkout vs pin, no network.
#        freshness         = also checks branch upstream tips (git ls-remote).
# TARGET a dep name -> verify it plus its transitive closure. Omitted -> verify
#        every dep in the manifest.
# SNEEZE_STAMP_DIRS  optional list of per-config .dep-stamps dirs. When given, a
#        dep whose checkout is OK but that is missing its <dep>.done stamp in a
#        listed (existing) dir is reported STALE "not built" -- this is how a
#        -Sync that failed partway (correct checkouts, missing builds) stays
#        visible. Passed only by the -Verify/--verify report, never the build
#        gate, so a normal build is never blocked by a not-yet-built dep.
#
# Reports the status of every dep, then exits non-zero if any is out of date or
# stale. Never modifies a clone.
# ---------------------------------------------------------------------------

cmake_minimum_required (VERSION 3.20)

if (NOT DEFINED MODE OR MODE STREQUAL "")
   set (MODE "offline")
endif ()
string (TOUPPER "${MODE}" _MODE)
if (NOT _MODE MATCHES "^(OFFLINE|FRESHNESS)$")
   message (FATAL_ERROR "MODE must be 'offline' or 'freshness' (got '${MODE}')")
endif ()

include ("${CMAKE_CURRENT_LIST_DIR}/DepVerify.cmake")

# Build the list to check. Dereference TARGET everywhere and use list(FIND)
# (never bare `TARGET`) to avoid CMake's if(TARGET <name>) operator.
if (DEFINED TARGET AND NOT "${TARGET}" STREQUAL "")
   list (FIND DEP_NAMES "${TARGET}" _idx)
   if (_idx LESS 0)
      message (FATAL_ERROR "TARGET '${TARGET}' is not a dep in the manifest")
   endif ()
   sneeze_dep_closure (_deps "${TARGET}")
   list (APPEND _deps "${TARGET}")
   list (REMOVE_DUPLICATES _deps)
else ()
   set (_deps ${DEP_NAMES})
endif ()

# Pass 1: resolve each dep's own checkout status (offline or freshness).
foreach (_d IN LISTS _deps)
   sneeze_verify_checkout (${_d} ${_MODE} _st _ms)
   set (_st_${_d} "${_st}")
   set (_ms_${_d} "${_ms}")
endforeach ()

# Pass 2: report. A dep whose own checkout is OK is STALE when either
#   (a) an out-of-date checkout sits anywhere in its dependency closure, or
#   (b) SNEEZE_STAMP_DIRS is given and it is missing a build stamp there.
# (a) is read straight from git state + the graph (no on-disk record). (b) is
# the "-Sync failed/never finished" signal: correct checkouts but missing builds.
set (_bad "")
set (_stale "")
foreach (_d IN LISTS _deps)
   set (_st "${_st_${_d}}")
   set (_ms "${_ms_${_d}}")
   if (_st STREQUAL "OK")
      # (a) dependency out of date?
      sneeze_dep_closure (_cl "${_d}")
      set (_ood "")
      foreach (_m IN LISTS _cl)
         if (DEFINED _st_${_m} AND (_st_${_m} STREQUAL "MISMATCH" OR _st_${_m} STREQUAL "BEHIND"))
            list (APPEND _ood "${_m}")
         endif ()
      endforeach ()
      # (b) not built in some config? (only when stamp dirs were provided)
      set (_nb "")
      if (DEFINED SNEEZE_STAMP_DIRS AND NOT "${SNEEZE_STAMP_DIRS}" STREQUAL "")
         foreach (_sd IN LISTS SNEEZE_STAMP_DIRS)
            if (EXISTS "${_sd}" AND NOT EXISTS "${_sd}/${_d}.done")
               get_filename_component (_c1 "${_sd}" DIRECTORY)   # <root>/<cfg>/build
               get_filename_component (_c2 "${_c1}" DIRECTORY)   # <root>/<cfg>
               get_filename_component (_clabel "${_c2}" NAME)    # <cfg>
               list (APPEND _nb "${_clabel}")
            endif ()
         endforeach ()
      endif ()
      if (_ood OR _nb)
         set (_reason "")
         if (_ood)
            list (JOIN _ood ", " _oodj)
            set (_reason "dependency out of date (${_oodj})")
         endif ()
         if (_nb)
            list (JOIN _nb ", " _nbj)
            if (NOT "${_reason}" STREQUAL "")
               string (APPEND _reason "; ")
            endif ()
            string (APPEND _reason "not built (${_nbj})")
         endif ()
         message (STATUS "[STALE] ${_d}: checkout OK, but ${_reason}")
         list (APPEND _stale "${_d}")
         continue ()
      endif ()
   endif ()
   message (STATUS "[${_st}] ${_ms}")
   if (_st STREQUAL "MISMATCH" OR _st STREQUAL "BEHIND")
      list (APPEND _bad "${_d}")
   endif ()
endforeach ()

if (_bad OR _stale)
   # Emit the summary as individual report lines (a single multi-line
   # FATAL_ERROR renders as noisy per-line error records under PowerShell); keep
   # the FATAL_ERROR itself terse -- its only job is the non-zero exit code.
   message (STATUS "")
   if (_bad)
      list (JOIN _bad ", " _badjoin)
      message (STATUS "Out-of-date checkouts: ${_badjoin}.")
   endif ()
   if (_stale)
      list (JOIN _stale ", " _stalejoin)
      message (STATUS "Stale (need rebuild): ${_stalejoin}.")
   endif ()
   message (STATUS "Nothing was modified. Correct with -Sync (Windows) / --sync (Linux/macOS).")
   message (FATAL_ERROR "Dependencies are out of date or stale (see report above).")
endif ()
message (STATUS "All checked dependencies are current and built (mode=${_MODE}).")
