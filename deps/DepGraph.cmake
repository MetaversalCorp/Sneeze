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
# DepGraph.cmake -- parse deps/dependencies.json (the single source of truth
# for every dependency's URL, clone folder, pinned ref, and the direct-edge
# dependency graph) and expose it to the rest of the build as CMake variables.
#
# After include, for every dep <name> in the manifest's "versions" table:
#   DEP_URL_<name>      git remote URL
#   DEP_FOLDER_<name>   clone folder under SNEEZE_DEP_REPO
#   DEP_REF_<name>      pinned ref (tag, commit SHA, or branch)
# and for every dep with edges in the "dependencies" table:
#   DEP_DEPENDS_<name>  list of DIRECT dependencies (may be unset if none)
# plus:
#   DEP_NAMES           list of every dep name in the manifest
#   SNEEZE_DEP_MANIFEST absolute path to dependencies.json
#
# Functions:
#   sneeze_dep_closure(<out> <dep>)  -> full transitive closure of <dep>
#                                        (its dependencies' dependencies, ...),
#                                        excluding <dep> itself.
# ---------------------------------------------------------------------------

if (DEFINED SNEEZE_DEPGRAPH_INCLUDED)
   return ()
endif ()
set (SNEEZE_DEPGRAPH_INCLUDED TRUE)

# IN_LIST if() operator. Needed explicitly because DepGraph is also included
# from `cmake -P` script contexts (deps/verify.cmake), where no project() call
# has set this policy to NEW.
if (POLICY CMP0057)
   cmake_policy (SET CMP0057 NEW)
endif ()

set (SNEEZE_DEP_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/dependencies.json")
if (NOT EXISTS "${SNEEZE_DEP_MANIFEST}")
   message (FATAL_ERROR "Dependency manifest not found: ${SNEEZE_DEP_MANIFEST}")
endif ()

file (READ "${SNEEZE_DEP_MANIFEST}" _sneeze_manifest_json)

# versions table -----------------------------------------------------------

set (DEP_NAMES)

string (JSON _ver_count LENGTH "${_sneeze_manifest_json}" versions)
math (EXPR _ver_last "${_ver_count} - 1")
foreach (_i RANGE 0 ${_ver_last})
   string (JSON _name   MEMBER "${_sneeze_manifest_json}" versions ${_i})
   string (JSON _url    GET    "${_sneeze_manifest_json}" versions "${_name}" url)
   string (JSON _folder GET    "${_sneeze_manifest_json}" versions "${_name}" folder)
   string (JSON _ref    GET    "${_sneeze_manifest_json}" versions "${_name}" ref)
   set (DEP_URL_${_name}    "${_url}")
   set (DEP_FOLDER_${_name} "${_folder}")
   set (DEP_REF_${_name}    "${_ref}")
   list (APPEND DEP_NAMES "${_name}")
endforeach ()

# dependencies table (direct edges only) -----------------------------------

string (JSON _dep_count LENGTH "${_sneeze_manifest_json}" dependencies)
if (_dep_count GREATER 0)
   math (EXPR _dep_last "${_dep_count} - 1")
   foreach (_i RANGE 0 ${_dep_last})
      string (JSON _name      MEMBER "${_sneeze_manifest_json}" dependencies ${_i})
      string (JSON _arr_count LENGTH "${_sneeze_manifest_json}" dependencies "${_name}")
      set (_edges)
      if (_arr_count GREATER 0)
         math (EXPR _arr_last "${_arr_count} - 1")
         foreach (_j RANGE 0 ${_arr_last})
            string (JSON _edge GET "${_sneeze_manifest_json}" dependencies "${_name}" ${_j})
            list (APPEND _edges "${_edge}")
         endforeach ()
      endif ()
      set (DEP_DEPENDS_${_name} "${_edges}")
   endforeach ()
endif ()

# Transitive closure of a dep's direct edges (worklist over DEP_DEPENDS_*).
# Functions read caller-scope variables, so DEP_DEPENDS_<x> resolve here.
function (sneeze_dep_closure _out _dep)
   set (_result)
   set (_worklist "${_dep}")
   while (_worklist)
      list (POP_FRONT _worklist _cur)
      foreach (_d IN LISTS DEP_DEPENDS_${_cur})
         if (NOT _d IN_LIST _result)
            list (APPEND _result "${_d}")
            list (APPEND _worklist "${_d}")
         endif ()
      endforeach ()
   endwhile ()
   set (${_out} "${_result}" PARENT_SCOPE)
endfunction ()
