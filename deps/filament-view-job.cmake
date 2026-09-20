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
# ExternalProject PATCH_COMMAND for filament. Shrinks the prepareVisibleLights
# JobSystem lambda in filament/src/details/View.cpp so the functor fits
# Job::storage on Clang libc++ (48 bytes). Metaversal filament v1.71.0.mv.3
# captures this plus five other 8-byte args (56 bytes) and fails -Werror.
# Idempotent: already-patched source, or a later tag that dropped the oversized
# capture, is a no-op. Does not move HEAD.
#
# Invoke as:
#   cmake -DSOURCE_DIR=<filament clone> -P filament-view-job.cmake

if (NOT DEFINED SOURCE_DIR)
   message (FATAL_ERROR "filament-view-job.cmake requires -DSOURCE_DIR=<filament clone>")
endif ()

set (_file "${SOURCE_DIR}/filament/src/details/View.cpp")
if (NOT EXISTS "${_file}")
   message (FATAL_ERROR "filament-view-job.cmake: missing ${_file}")
endif ()

file (READ "${_file}" _src)

if (_src MATCHES "SNEEZE_JOB_STORAGE_FIX")
   return ()
endif ()

set (_old [===[        prepareVisibleLightsJob = js.runAndRetain(js.createJob(nullptr,
                [this, &engine, distances, positionalLightCount, &viewMatrix = cameraInfo.view, &cullingFrustum,
                 &lightData = scene->getLightData()]
                        (JobSystem&, JobSystem::Job*) {
                    mVisibleGlobalLightCount = prepareVisibleLights(engine.getLightManager(),
                            { distances, distances + positionalLightCount },
                            viewMatrix, cullingFrustum, lightData);
                }));
]===])

set (_new [===[        // SNEEZE_JOB_STORAGE_FIX: one pointer capture so the functor fits
        // Job::storage (48 bytes on Clang libc++). Capturing this plus the
        // other arguments is 56 bytes and fails -Werror static_assert.
        struct PrepareVisibleLightsJob {
            FEngine* engine;
            float* distances;
            size_t positionalLightCount;
            mat4f const* viewMatrix;
            Frustum const* cullingFrustum;
            FScene::LightSoa* lightData;
            uint32_t* visibleGlobalLightCount;
        };
        auto* const pJob = rootArenaScope.allocate<PrepareVisibleLightsJob>(1);
        *pJob = PrepareVisibleLightsJob{
                &engine, distances, positionalLightCount,
                &cameraInfo.view, &cullingFrustum,
                &scene->getLightData(), &mVisibleGlobalLightCount};
        prepareVisibleLightsJob = js.runAndRetain(js.createJob(nullptr,
                [pJob](JobSystem&, JobSystem::Job*) {
                    *pJob->visibleGlobalLightCount = prepareVisibleLights(
                            pJob->engine->getLightManager(),
                            { pJob->distances, pJob->distances + pJob->positionalLightCount },
                            *pJob->viewMatrix, *pJob->cullingFrustum, *pJob->lightData);
                }));
]===])

string (FIND "${_src}" "${_old}" _ix)
if (_ix EQUAL -1)
   if (_src MATCHES "\\[this, &engine, distances, positionalLightCount")
      message (FATAL_ERROR "filament-view-job.cmake: View.cpp still has the oversized capture, but the expected text did not match. Update this script.")
   endif ()
   return ()
endif ()

string (REPLACE "${_old}" "${_new}" _out "${_src}")
file (WRITE "${_file}" "${_out}")
