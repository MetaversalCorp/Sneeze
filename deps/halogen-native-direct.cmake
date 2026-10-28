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
# ExternalProject PATCH_COMMAND for Halogen. On the native-surface path,
# post-processing, 4x MSAA, FXAA, and bloom draw into an offscreen buffer and
# then copy onto the drawable. On Metal the window shows only the swapchain
# clear, so that path draws straight into the drawable until the copy is fixed.
# Idempotent: an already-patched tree is a no-op. Does not move HEAD.
#
# Invoke as:
#   cmake -DSOURCE_DIR=<halogen clone> -P halogen-native-direct.cmake

if (NOT DEFINED SOURCE_DIR)
   message (FATAL_ERROR "halogen-native-direct.cmake requires -DSOURCE_DIR=<halogen clone>")
endif ()

set (_file "${SOURCE_DIR}/src/Frame.cpp")
if (NOT EXISTS "${_file}")
   message (FATAL_ERROR "halogen-native-direct.cmake: missing ${_file}")
endif ()

file (READ "${_file}" _src)

# SNEEZE_NATIVE_LOG is the later of the two edits. A tree that already has
# SNEEZE_NATIVE_DIRECT still needs the once-a-second line.
if (_src MATCHES "SNEEZE_NATIVE_LOG")
   return ()
endif ()

set (_out "${_src}")

if (NOT _out MATCHES "SNEEZE_NATIVE_DIRECT")

set (_old [===[    if (wantFloat) {
        // Float output: disable post-processing to preserve exact linear HDR
        // values in the readback buffer.  MSAA is not needed for pixel-exact
        // float output (Blender composites the raw values).
        mView->setPostProcessingEnabled(false);
    } else {
]===])

set (_new [===[    if (wantFloat || nativeSurface) {
        // Float output: disable post-processing to preserve exact linear HDR
        // values in the readback buffer.  MSAA is not needed for pixel-exact
        // float output (Blender composites the raw values).
        //
        // SNEEZE_NATIVE_DIRECT: the native swapchain takes this path too.
        // Post-processing, 4x MSAA, FXAA, and bloom draw into an offscreen
        // buffer and then copy onto the drawable. On Metal the window shows
        // only the swapchain clear, so draw straight into the drawable.
        mView->setPostProcessingEnabled(false);
        mView->setAntiAliasing(filament::View::AntiAliasing::NONE);
        mView->setDithering(filament::View::Dithering::NONE);
        filament::View::MultiSampleAntiAliasingOptions msaa;
        msaa.enabled = false;
        mView->setMultiSampleAntiAliasingOptions(msaa);
        filament::View::BloomOptions bloom;
        bloom.enabled = false;
        mView->setBloomOptions(bloom);
    } else {
]===])

   string (FIND "${_out}" "${_old}" _ix)
   if (_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp does not contain the native-surface post-process block. Update this script.")
   endif ()

   string (REPLACE "${_old}" "${_new}" _out "${_out}")
endif ()

if (NOT _out MATCHES "#include <cstdio>")
   string (FIND "${_out}" "#include <cstring>" _inc)
   if (_inc EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp has no cstring include. Update this script.")
   endif ()
   string (REPLACE "#include <cstring>" "#include <cstring>\n#include <chrono>\n#include <cstdio>" _out "${_out}")
endif ()

set (_log_old [===[        mView->setRenderTarget(nullptr);

        filament::SwapChain *sc = mNativeSurface->swapChain();
]===])

set (_log_new [===[        mView->setRenderTarget(nullptr);

        // SNEEZE_NATIVE_LOG: once a second, so a Mac run can tell a rebuilt
        // library from a scene Filament never made drawable.
        {
            using clock = std::chrono::steady_clock;
            static clock::time_point sLastLog{};
            const clock::time_point now = clock::now();
            if (sLastLog.time_since_epoch().count() == 0
                    || now - sLastLog >= std::chrono::seconds(1)) {
                sLastLog = now;
                std::fprintf(stderr,
                    "halogen postProcess=%d renderables=%zu viewport=%ux%u\n",
                    mView->isPostProcessingEnabled() ? 1 : 0,
                    mWorld->filamentScene()->getRenderableCount(),
                    mWidth, mHeight);
                std::fflush(stderr);
            }
        }

        filament::SwapChain *sc = mNativeSurface->swapChain();
]===])

string (FIND "${_out}" "${_log_old}" _log_ix)
if (_log_ix EQUAL -1)
   message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp does not contain the native-surface render block. Update this script.")
endif ()

string (REPLACE "${_log_old}" "${_log_new}" _out "${_out}")

# Keep the clone's original newlines. file(WRITE) on Windows rewrites the
# whole file as CRLF, which would dirty every line of Frame.cpp.
if (_src MATCHES "\r\n")
   set (_nl CRLF)
else ()
   set (_nl UNIX)
endif ()
file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
