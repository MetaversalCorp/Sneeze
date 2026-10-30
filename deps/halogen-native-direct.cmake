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
# post-processing (tone mapping, 4x MSAA, FXAA, dithering, bloom) renders into
# an offscreen buffer that is then blitted onto the drawable. That blit is not
# implemented on the Metal backend, so the window would show only the swapchain
# clear. Disable post-processing on the native-surface path so Filament draws
# straight into the drawable. Idempotent: an already-patched tree is a no-op.
# Does not move HEAD.
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

if (NOT _src MATCHES "SNEEZE_NATIVE_DIRECT")

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

   string (FIND "${_src}" "${_old}" _ix)
   if (_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp does not contain the native-surface post-process block. Update this script.")
   endif ()

   string (REPLACE "${_old}" "${_new}" _out "${_src}")

   # Keep the clone's original newlines. file(WRITE) on Windows rewrites the
   # whole file as CRLF, which would dirty every line of Frame.cpp.
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()
