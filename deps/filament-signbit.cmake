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
# ExternalProject PATCH_COMMAND for filament. Makes fast::signbit robust.
#
# filament::math::fast::signbit() returns __builtin_signbitf(x) on compilers
# that have the builtin (Clang). Culler::intersects() folds the result into an
# int with `visible &= signbit(dot)` and finally returns `bool(results[0] & 1)`
# after truncating to uint8_t -- so it depends on bit 0 of signbit's return
# being set for negatives. Under the Clang fast-math build on Apple Silicon,
# __builtin_signbitf returns the sign in a form whose bit 0 is clear, so every
# box fails the `& 1` test and Frustum::intersects(Box) rejects every
# renderable (verified: a hand-rolled plane test accepts ~23/33 boxes, Filament
# accepts 0/33, on the identical frustum and boxes). MSVC lacks the builtin and
# falls back to std::signbit, which is why Windows is unaffected.
#
# The fix extracts the IEEE-754 sign bit directly as 0 or 1. It is pure integer
# work (no floating-point ops), so it is immune to fast-math, and it yields a
# bit-0-correct result on every platform -- making the single code path both
# correct and faster than the builtin call. Idempotent; does not move HEAD.
#
# Invoke as:
#   cmake -DSOURCE_DIR=<filament clone> -P filament-signbit.cmake

if (NOT DEFINED SOURCE_DIR)
   message (FATAL_ERROR "filament-signbit.cmake requires -DSOURCE_DIR=<filament clone>")
endif ()

set (_file "${SOURCE_DIR}/libs/math/include/math/fast.h")
if (NOT EXISTS "${_file}")
   message (FATAL_ERROR "filament-signbit.cmake: missing ${_file}")
endif ()

file (READ "${_file}" _src)

if (_src MATCHES "SNEEZE_SIGNBIT_FIX")
   return ()
endif ()

set (_old [===[inline int signbit(float x) noexcept {
#if __has_builtin(__builtin_signbitf)
    // Note: on Android NDK, signbit() is a function call -- not what we want.
    return __builtin_signbitf(x);
#else
    return std::signbit(x);
#endif
}]===])

set (_new [===[inline int signbit(float x) noexcept {
    // SNEEZE_SIGNBIT_FIX: extract the IEEE-754 sign bit directly as 0 or 1.
    // __builtin_signbitf under the Clang fast-math build on Apple Silicon
    // returns a value whose bit 0 is clear for negatives, which breaks
    // Culler::intersects (it ANDs signbit into an int and returns `& 1`) and
    // culls every renderable. A union bitcast is pure integer work -- immune
    // to fast-math, bit-0-correct, and portable across MSVC and Clang.
    union { float f; unsigned u; } caster = { x };
    return int(caster.u >> 31);
}]===])

string (FIND "${_src}" "${_old}" _ix)
if (_ix EQUAL -1)
   message (FATAL_ERROR "filament-signbit.cmake: fast::signbit does not match the expected text. Update this script.")
endif ()

string (REPLACE "${_old}" "${_new}" _out "${_src}")
file (WRITE "${_file}" "${_out}")
