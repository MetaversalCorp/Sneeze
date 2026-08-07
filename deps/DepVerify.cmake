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
# DepVerify.cmake -- the ONE read-only "is this dependency's checkout what the
# manifest pins?" implementation, shared by CMake configure (deps + src) and by
# the build scripts / CI (via deps/verify.cmake -P). It NEVER modifies a clone;
# moving a checkout is exclusively the job of the scripts' -Sync path.
#
# Two modes (see project.mdc "Two moments"):
#   OFFLINE   - local only, no network. HEAD must equal the pinned commit that
#               the ref resolves to locally. Used on EVERY build. A tag/SHA the
#               manifest bumped but the clone never moved is caught here; a
#               branch's *upstream* movement is NOT (that needs the network).
#   FRESHNESS - OFFLINE plus, for a branch ref, one read-only `git ls-remote`
#               to compare against the upstream tip: equal or ahead = OK, behind
#               = flagged. Used only by the explicit sync step (-Verify/-Sync).
#
# Core:
#   sneeze_verify_checkout(<dep> <mode> <out_status> <out_msg>)
#       status in { OK, SKIP, MISMATCH, BEHIND, UNKNOWN }; sets no side effects.
# Gates (fail the configure with a fix hint on the first hard failure):
#   sneeze_verify_dependencies(<target> <mode>)  - the transitive closure of
#                                                  <target> (a dep name).
#   sneeze_verify_all(<mode>)                    - every dep in the manifest
#                                                  (used by the Sneeze src build).
# ---------------------------------------------------------------------------

if (DEFINED SNEEZE_DEPVERIFY_INCLUDED)
   return ()
endif ()
set (SNEEZE_DEPVERIFY_INCLUDED TRUE)

if (NOT DEFINED SNEEZE_DEPGRAPH_INCLUDED)
   include ("${CMAKE_CURRENT_LIST_DIR}/DepGraph.cmake")
endif ()

find_package (Git QUIET)

# Where the source clones live. In the deps CMake project this is already set;
# in a `cmake -P` context (verify.cmake) or the src build, default to deps/repos
# next to this file.
if (NOT DEFINED SNEEZE_DEP_REPO OR SNEEZE_DEP_REPO STREQUAL "")
   set (SNEEZE_DEP_REPO "${CMAKE_CURRENT_LIST_DIR}/repos")
endif ()

# --- internals -------------------------------------------------------------

# Resolve tip of refs/heads/<ref> via the manifest URL first (clone `origin` may
# be unreachable for private MetaversalCorp deps when the agent has no
# credential helper on that remote). Falls back to `origin`. Empty on failure.
function (_sneeze_ls_remote_heads _out_tip _repo _ref _url)
   set (${_out_tip} "" PARENT_SCOPE)
   if (NOT _url STREQUAL "")
      execute_process (
         COMMAND ${GIT_EXECUTABLE} ls-remote --heads "${_url}" "refs/heads/${_ref}"
         OUTPUT_VARIABLE _o OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
      if (_rc EQUAL 0 AND NOT _o STREQUAL "")
         string (REGEX MATCH "^[0-9a-f]+" _tip "${_o}")
         set (${_out_tip} "${_tip}" PARENT_SCOPE)
         return ()
      endif ()
   endif ()
   execute_process (
      COMMAND ${GIT_EXECUTABLE} -C "${_repo}" ls-remote --heads origin "refs/heads/${_ref}"
      OUTPUT_VARIABLE _o2 OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc2 ERROR_QUIET)
   if (_rc2 EQUAL 0 AND NOT _o2 STREQUAL "")
      string (REGEX MATCH "^[0-9a-f]+" _tip2 "${_o2}")
      set (${_out_tip} "${_tip2}" PARENT_SCOPE)
   endif ()
endfunction ()

# TRUE if the pin is a live branch (advertised as refs/heads/<ref> on the
# manifest URL or on origin). Read-only network call; FRESHNESS only.
function (_sneeze_is_branch _out _repo _ref _url)
   set (${_out} FALSE PARENT_SCOPE)
   _sneeze_ls_remote_heads (_tip "${_repo}" "${_ref}" "${_url}")
   if (NOT _tip STREQUAL "")
      set (${_out} TRUE PARENT_SCOPE)
   endif ()
endfunction ()

# Compare local HEAD against the upstream branch tip. equal/ahead -> OK,
# behind/diverged -> BEHIND, unreachable -> UNKNOWN. Never fetches: "ahead"
# means the remote tip is already an ancestor we hold; "behind" means the
# remote tip is simply absent locally.
function (_sneeze_branch_freshness _dep _repo _ref _url _head _head_short _out_status _out_msg)
   _sneeze_ls_remote_heads (_tip "${_repo}" "${_ref}" "${_url}")
   if (_tip STREQUAL "")
      set (${_out_status} "UNKNOWN" PARENT_SCOPE)
      set (${_out_msg} "${_dep}: cannot reach upstream for branch '${_ref}' (tried manifest URL + origin); freshness unknown - Sync may be required" PARENT_SCOPE)
      return ()
   endif ()
   string (SUBSTRING "${_tip}" 0 10 _tip_short)
   if (_tip STREQUAL _head)
      set (${_out_status} "OK" PARENT_SCOPE)
      set (${_out_msg} "${_dep}: branch '${_ref}' up to date with upstream (${_head_short})" PARENT_SCOPE)
      return ()
   endif ()
   # Ahead iff the remote tip is present locally and is an ancestor of HEAD.
   execute_process (
      COMMAND ${GIT_EXECUTABLE} -C "${_repo}" cat-file -e "${_tip}^{commit}"
      RESULT_VARIABLE _has ERROR_QUIET)
   if (_has EQUAL 0)
      execute_process (
         COMMAND ${GIT_EXECUTABLE} -C "${_repo}" merge-base --is-ancestor "${_tip}" "${_head}"
         RESULT_VARIABLE _anc ERROR_QUIET)
      if (_anc EQUAL 0)
         set (${_out_status} "OK" PARENT_SCOPE)
         set (${_out_msg} "${_dep}: branch '${_ref}' ahead of upstream (local ${_head_short} > upstream ${_tip_short})" PARENT_SCOPE)
         return ()
      endif ()
   endif ()
   set (${_out_status} "BEHIND" PARENT_SCOPE)
   set (${_out_msg} "${_dep}: branch '${_ref}' is BEHIND upstream (upstream ${_tip_short}, local ${_head_short})" PARENT_SCOPE)
endfunction ()

# --- public core -----------------------------------------------------------

function (sneeze_verify_checkout _dep _mode _out_status _out_msg)
   set (_folder "${DEP_FOLDER_${_dep}}")
   set (_ref    "${DEP_REF_${_dep}}")
   set (_url    "${DEP_URL_${_dep}}")
   set (_repo   "${SNEEZE_DEP_REPO}/${_folder}")

   if (NOT GIT_EXECUTABLE)
      set (${_out_status} "SKIP" PARENT_SCOPE)
      set (${_out_msg} "${_dep}: git not found; skipping checkout verification" PARENT_SCOPE)
      return ()
   endif ()
   if (NOT EXISTS "${_repo}/.git")
      set (${_out_status} "SKIP" PARENT_SCOPE)
      set (${_out_msg} "${_dep}: not cloned yet; first clone will honor ref '${_ref}'" PARENT_SCOPE)
      return ()
   endif ()

   execute_process (
      COMMAND ${GIT_EXECUTABLE} -C "${_repo}" rev-parse HEAD
      OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
   if (NOT _rc EQUAL 0 OR _head STREQUAL "")
      set (${_out_status} "SKIP" PARENT_SCOPE)
      set (${_out_msg} "${_dep}: '${_repo}' is not a readable git repo; skipping" PARENT_SCOPE)
      return ()
   endif ()
   string (SUBSTRING "${_head}" 0 10 _head_short)

   # Does the pinned ref resolve locally to exactly HEAD? Covers a detached tag,
   # a SHA, and a branch sitting on (or worked ahead to) its local tip.
   execute_process (
      COMMAND ${GIT_EXECUTABLE} -C "${_repo}" rev-parse --verify --quiet "${_ref}^{commit}"
      OUTPUT_VARIABLE _resolved OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc2 ERROR_QUIET)

   if (NOT _resolved STREQUAL "" AND _resolved STREQUAL _head)
      if (_mode STREQUAL "FRESHNESS")
         _sneeze_is_branch (_isbranch "${_repo}" "${_ref}" "${_url}")
         if (_isbranch)
            _sneeze_branch_freshness ("${_dep}" "${_repo}" "${_ref}" "${_url}" "${_head}" "${_head_short}" _bs _bm)
            set (${_out_status} "${_bs}" PARENT_SCOPE)
            set (${_out_msg} "${_bm}" PARENT_SCOPE)
            return ()
         endif ()
         # ls-remote failed (private remote without auth, offline, …). If the pin
         # is a local branch name, do NOT silently OK - that hid stale sneeze-sdk
         # / RMAP / Map / Vox tips on Jenkins and skipped -Sync.
         execute_process (
            COMMAND ${GIT_EXECUTABLE} -C "${_repo}" show-ref --verify --quiet "refs/heads/${_ref}"
            RESULT_VARIABLE _local_branch ERROR_QUIET)
         if (_local_branch EQUAL 0)
            set (${_out_status} "UNKNOWN" PARENT_SCOPE)
            set (${_out_msg} "${_dep}: on local branch '${_ref}' (${_head_short}) but cannot reach upstream to confirm tip - fix git auth to the manifest URL, then -Sync" PARENT_SCOPE)
            return ()
         endif ()
      endif ()
      set (${_out_status} "OK" PARENT_SCOPE)
      set (${_out_msg} "${_dep}: on ref '${_ref}' (${_head_short})" PARENT_SCOPE)
      return ()
   endif ()

   # HEAD is not the pinned commit: a bumped tag/SHA never synced, or a clone
   # parked at an unexpected commit. OFFLINE cannot classify further and halts.
   set (${_out_status} "MISMATCH" PARENT_SCOPE)
   set (${_out_msg} "${_dep}: checkout ${_head_short} does not match manifest ref '${_ref}' (${_repo})" PARENT_SCOPE)
endfunction ()

# --- gates (used at CMake configure) ---------------------------------------

function (_sneeze_gate _dep _mode)
   sneeze_verify_checkout (${_dep} ${_mode} _st _ms)
   if (_st STREQUAL "MISMATCH" OR _st STREQUAL "BEHIND")
      message (FATAL_ERROR
         "Dependency out of date -- build halted (nothing was modified).\n"
         "  ${_ms}\n"
         "  Manifest deps/dependencies.json pins '${_dep}' to ref '${DEP_REF_${_dep}}'.\n"
         "Bring dependencies up to date (the only step that moves a checkout):\n"
         "  Windows:      .\\scripts\\build-windows.ps1 -Verify        (report)\n"
         "                .\\scripts\\build-windows.ps1 -Only ${_dep} -Sync\n"
         "  Linux/macOS:  ./scripts/build-deps.sh --only ${_dep} --sync")
   elseif (_st STREQUAL "UNKNOWN")
      message (WARNING "${_ms}")
   else ()
      message (STATUS "dep-verify: ${_ms}")
   endif ()
endfunction ()

# Verify the transitive closure of <target> (a dep name). Used by the deps
# -DDEP=<target> path: "building a dependency checks its dependencies."
function (sneeze_verify_dependencies _target _mode)
   sneeze_dep_closure (_deps "${_target}")
   foreach (_d IN LISTS _deps)
      _sneeze_gate ("${_d}" "${_mode}")
   endforeach ()
endfunction ()

# Verify every dep in the manifest. Used by the Sneeze src build (which
# consumes the whole set); uncloned deps skip cleanly.
function (sneeze_verify_all _mode)
   foreach (_d IN LISTS DEP_NAMES)
      _sneeze_gate ("${_d}" "${_mode}")
   endforeach ()
endfunction ()
