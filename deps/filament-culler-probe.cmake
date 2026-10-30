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
#
# DIAGNOSTIC ExternalProject PATCH_COMMAND for filament. Instruments the
# single-box Culler::intersects(Frustum, Box) with a one-shot stderr dump that
# compares the VECTORIZED result (res0, from the real intersects() helper)
# against a SCALAR recompute done right here in the same fast-math translation
# unit (svis), plus the six per-plane dots. On Apple Silicon the vectorized
# Culler culls every renderable while the identical hand-rolled test in
# Halogen's (non-fast-math) TU passes, so this localizes the miscompile:
#   svis=1, res0=0  -> the #pragma clang loop vectorize path is miscompiled
#   svis=0 + dots<0 -> the scalar comparison is miscompiled under fast-math
#   svis=0 + dots>0 -> the dot product itself flips sign under fast-math
# Idempotent (keyed on SNEEZE_CULLER_PROBE). Does not move HEAD. Diagnostic
# only -- remove before shipping.
#
# Invoke as:
#   cmake -DSOURCE_DIR=<filament clone> -P filament-culler-probe.cmake

if (NOT DEFINED SOURCE_DIR)
   message (FATAL_ERROR "filament-culler-probe.cmake requires -DSOURCE_DIR=<filament clone>")
endif ()

set (_file "${SOURCE_DIR}/filament/src/Culler.cpp")
if (NOT EXISTS "${_file}")
   message (FATAL_ERROR "filament-culler-probe.cmake: missing ${_file}")
endif ()

file (READ "${_file}" _src)

if (_src MATCHES "SNEEZE_CULLER_PROBE")
   return ()
endif ()

# 1) pull in cstdio / cmath for the dump
if (NOT _src MATCHES "#include <cstdio>")
   string (REPLACE
"#include <math/fast.h>"
"#include <math/fast.h>
#include <cstdio>
#include <cmath>" _src "${_src}")
endif ()

# 2) wrap the single-box return with the probe
set (_old [===[    intersects(results, frustum, centers, extents, MODULO, 0);
    return bool(results[0] & 1);]===])

set (_new [===[    intersects(results, frustum, centers, extents, MODULO, 0);
    // SNEEZE_CULLER_PROBE
    {
        static int sProbe = 0;
        if (sProbe < 40) {
            ++sProbe;
            float4 const* const pl = frustum.mPlanes;
            int svis = 1;
            float dd[6];
            for (int j = 0; j < 6; ++j) {
                float const d =
                        pl[j].x * box.center.x - std::abs(pl[j].x) * box.halfExtent.x +
                        pl[j].y * box.center.y - std::abs(pl[j].y) * box.halfExtent.y +
                        pl[j].z * box.center.z - std::abs(pl[j].z) * box.halfExtent.z +
                        pl[j].w;
                dd[j] = d;
                if (!(d < 0.0f)) {
                    svis = 0;
                }
            }
            std::fprintf(stderr,
                    "CULLER c=%.3g,%.3g,%.3g e=%.3g,%.3g,%.3g svis=%d res0=%d "
                    "dots=%.3g,%.3g,%.3g,%.3g,%.3g,%.3g\n",
                    box.center.x, box.center.y, box.center.z,
                    box.halfExtent.x, box.halfExtent.y, box.halfExtent.z,
                    svis, int(results[0] & 1),
                    dd[0], dd[1], dd[2], dd[3], dd[4], dd[5]);
            std::fflush(stderr);
        }
    }
    return bool(results[0] & 1);]===])

string (FIND "${_src}" "${_old}" _ix)
if (_ix EQUAL -1)
   message (FATAL_ERROR "filament-culler-probe.cmake: Culler.cpp single-box intersects did not match. Update this script.")
endif ()

string (REPLACE "${_old}" "${_new}" _out "${_src}")
file (WRITE "${_file}" "${_out}")

# ---------------------------------------------------------------------------
# Frustum::setProjection sanitize + one-shot plane dump.
#
# With this scene's extreme depth range (near 0.05, far 5.59e12) the far
# plane's normal is near-degenerate; clang's fast-math 1/length(f.xyz) turns
# the normalization into Inf/NaN, poisoning mPlanes[4]. Culler's dot<0 test
# then fails (NaN<0 is false) and every renderable is culled on Apple Silicon.
# MSVC computes it finite, so Windows is unaffected. setProjection is one-time
# scalar code (not the vectorized loop), so the integer exponent check is
# immune to fast-math. A dead plane is replaced with (0,0,0,-1), which both the
# box and sphere tests read as "always inside" -- correct for a degenerate or
# infinite far plane. Idempotent (SNEEZE_FRUSTUM_SANITIZE).
set (_ffile "${SOURCE_DIR}/filament/src/Frustum.cpp")
if (NOT EXISTS "${_ffile}")
   message (FATAL_ERROR "filament-culler-probe.cmake: missing ${_ffile}")
endif ()

file (READ "${_ffile}" _fsrc)
if (NOT _fsrc MATCHES "SNEEZE_FRUSTUM_SANITIZE")
   if (NOT _fsrc MATCHES "#include <cstdio>")
      string (REPLACE
"#include <math/mat4.h>"
"#include <math/mat4.h>
#include <cstdio>
#include <cstring>" _fsrc "${_fsrc}")
   endif ()

   set (_fold [===[    mPlanes[4] = f;
    mPlanes[5] = n;
}]===])

   set (_fnew [===[    mPlanes[4] = f;
    mPlanes[5] = n;

    // SNEEZE_FRUSTUM_SANITIZE: dump the planes once, then replace any plane
    // poisoned to Inf/NaN by fast-math normalization with one that always
    // reports "inside" (dot = -1 < 0). Integer exponent test is fast-math safe.
    {
        static bool sDumpedPlanes = false;
        if (!sDumpedPlanes) {
            sDumpedPlanes = true;
            for (int i = 0; i < 6; ++i) {
                std::fprintf(stderr, "FRUSTUM plane[%d]=%.6g,%.6g,%.6g,%.6g\n",
                        i, mPlanes[i].x, mPlanes[i].y, mPlanes[i].z, mPlanes[i].w);
            }
            std::fflush(stderr);
        }
    }
    for (int i = 0; i < 6; ++i) {
        uint32_t b[4];
        std::memcpy(&b[0], &mPlanes[i].x, 4);
        std::memcpy(&b[1], &mPlanes[i].y, 4);
        std::memcpy(&b[2], &mPlanes[i].z, 4);
        std::memcpy(&b[3], &mPlanes[i].w, 4);
        bool bad = false;
        for (int k = 0; k < 4; ++k) {
            if ((b[k] & 0x7F800000u) == 0x7F800000u) {
                bad = true;
            }
        }
        if (bad) {
            mPlanes[i] = float4{0.0f, 0.0f, 0.0f, -1.0f};
        }
    }
}]===])

   string (FIND "${_fsrc}" "${_fold}" _fix)
   if (_fix EQUAL -1)
      message (FATAL_ERROR "filament-culler-probe.cmake: Frustum.cpp setProjection tail did not match. Update this script.")
   endif ()

   string (REPLACE "${_fold}" "${_fnew}" _fout "${_fsrc}")
   file (WRITE "${_ffile}" "${_fout}")
endif ()
