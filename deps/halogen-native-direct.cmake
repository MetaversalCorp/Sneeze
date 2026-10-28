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

# SNEEZE_NATIVE_LOG is the later of the two Frame.cpp edits. A tree that
# already has it still needs the Material.cpp cull diagnostic.
set (_patch_frame ON)
if (_src MATCHES "SNEEZE_NATIVE_LOG")
   set (_patch_frame OFF)
endif ()

if (_patch_frame)
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
endif ()

# SNEEZE_NATIVE_CULL: keep every triangle. Metal can reverse which side is the
# front, and the Mac window is still only the clear after the drawable size
# matched the view.
set (_mat "${SOURCE_DIR}/src/Material.cpp")
if (NOT EXISTS "${_mat}")
   message (FATAL_ERROR "halogen-native-direct.cmake: missing ${_mat}")
endif ()

file (READ "${_mat}" _mat_src)
if (NOT _mat_src MATCHES "SNEEZE_NATIVE_CULL")
   set (_cull_old [===[    mMaterialInstance->setCullingMode(doubleSided
            ? filament::MaterialInstance::CullingMode::NONE
            : filament::MaterialInstance::CullingMode::BACK);]===])
   set (_cull_new [===[    // SNEEZE_NATIVE_CULL: diagnostic. Metal may reverse which side is
    // the front, so keep every triangle until the Mac scene is visible.
    mMaterialInstance->setCullingMode(filament::MaterialInstance::CullingMode::NONE);]===])

   string (FIND "${_mat_src}" "${_cull_old}" _cull_ix)
   if (_cull_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Material.cpp does not contain the culling mode block. Update this script.")
   endif ()

   string (REPLACE "${_cull_old}" "${_cull_new}" _mat_out "${_mat_src}")

   if (_mat_src MATCHES "\r\n")
      set (_mat_nl CRLF)
   else ()
      set (_mat_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_mat}" CONTENT "${_mat_out}" @ONLY NEWLINE_STYLE ${_mat_nl})
endif ()
