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

# SNEEZE_NATIVE_CAMERA: the depth test and the back-face cull were both off
# and the window was still only the clear. Log where the camera is aimed.
# A tree that already has SNEEZE_NATIVE_LOG still needs this.
file (READ "${_file}" _src)
if (NOT _src MATCHES "SNEEZE_NATIVE_CAMERA")
   set (_cam_old [===[                std::fprintf(stderr,
                    "halogen postProcess=%d renderables=%zu viewport=%ux%u\n",
                    mView->isPostProcessingEnabled() ? 1 : 0,
                    mWorld->filamentScene()->getRenderableCount(),
                    mWidth, mHeight);
                std::fflush(stderr);]===])
   set (_cam_new [===[                // SNEEZE_NATIVE_CAMERA: position, aim, and clip range. Objects
                // outside this view are dropped before depth or facing.
                filament::Camera *cam = mCamera ? mCamera->filamentCamera() : nullptr;
                filament::math::double3 eye{0, 0, 0};
                filament::math::float3 fwd{0, 0, 0};
                double clipNear = 0;
                double clipFar = 0;
                float fov = 0;
                if (cam) {
                    eye = cam->getPosition();
                    fwd = cam->getForwardVector();
                    clipNear = cam->getNear();
                    clipFar = cam->getCullingFar();
                    fov = cam->getFieldOfViewInDegrees(filament::Camera::Fov::VERTICAL);
                }
                std::fprintf(stderr,
                    "halogen postProcess=%d renderables=%zu viewport=%ux%u eye=%.1f,%.1f,%.1f fwd=%.2f,%.2f,%.2f near=%.3g far=%.3g fov=%.1f\n",
                    mView->isPostProcessingEnabled() ? 1 : 0,
                    mWorld->filamentScene()->getRenderableCount(),
                    mWidth, mHeight,
                    eye.x, eye.y, eye.z,
                    fwd.x, fwd.y, fwd.z,
                    clipNear, clipFar, fov);
                std::fflush(stderr);]===])

   string (FIND "${_src}" "${_cam_old}" _cam_ix)
   if (_cam_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp does not contain the halogen log line. Update this script.")
   endif ()

   string (REPLACE "${_cam_old}" "${_cam_new}" _out "${_src}")

   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# filament::Camera is only forward-declared in Halogen's Camera.h. The log
# calls getPosition(), so the full header has to be present too.
file (READ "${_file}" _src)
if (_src MATCHES "SNEEZE_NATIVE_CAMERA" AND NOT _src MATCHES "#include <filament/Camera.h>")
   string (FIND "${_src}" "#include <filament/View.h>" _view_ix)
   if (_view_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp has no filament/View.h include. Update this script.")
   endif ()
   string (REPLACE "#include <filament/View.h>" "#include <filament/Camera.h>\n#include <filament/View.h>" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_TRIANGLE: one pale triangle from Filament's own unlit
# material, kept in front of the camera. A tree that already logs the
# camera still needs this.
file (READ "${_file}" _src)
if (NOT _src MATCHES "SNEEZE_NATIVE_TRIANGLE" AND NOT _src MATCHES "SNEEZE_NATIVE_NO_TRIANGLE")
   set (_tri_old [===[                renderer->setClearOptions(clearOpts);
            }
            renderer->render(mView.get());]===])
   set (_tri_new [===[                renderer->setClearOptions(clearOpts);
            }
            // SNEEZE_NATIVE_TRIANGLE: Filament's default material is unlit
            // and writes rgb 0.8. It stays in front of the camera. A pale
            // shape on the clear means a draw can land; a solid clear means
            // no draw is reaching the window.
            {
                filament::Camera *triCam = mCamera ? mCamera->filamentCamera() : nullptr;
                static utils::Entity sProbe{};
                if (!sProbe && triCam) {
                    static const filament::math::float3 kPos[3] = {
                        {-2.0f, -2.0f, -5.0f},
                        { 2.0f, -2.0f, -5.0f},
                        { 0.0f,  2.0f, -5.0f}
                    };
                    static const uint16_t kIdx[3] = {0, 1, 2};
                    auto *vb = filament::VertexBuffer::Builder()
                        .vertexCount(3)
                        .bufferCount(1)
                        .attribute(filament::VertexAttribute::POSITION, 0,
                            filament::VertexBuffer::AttributeType::FLOAT3, 0,
                            sizeof(filament::math::float3))
                        .build(*engine);
                    vb->setBufferAt(*engine, 0,
                        filament::VertexBuffer::BufferDescriptor(kPos, sizeof(kPos)));
                    auto *ib = filament::IndexBuffer::Builder()
                        .indexCount(3)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine);
                    ib->setBuffer(*engine,
                        filament::IndexBuffer::BufferDescriptor(kIdx, sizeof(kIdx)));
                    auto *mi = engine->getDefaultMaterial()->createInstance();
                    mi->setDepthCulling(false);
                    mi->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
                    sProbe = utils::EntityManager::get().create();
                    filament::RenderableManager::Builder(1)
                        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                            vb, ib, 0, 3)
                        .material(0, mi)
                        .boundingBox({{0, 0, -5}, {2, 2, 1}})
                        .culling(false)
                        .castShadows(false)
                        .receiveShadows(false)
                        .build(*engine, sProbe);
                    engine->getTransformManager().create(sProbe);
                    mWorld->filamentScene()->addEntity(sProbe);
                }
                if (sProbe && triCam) {
                    auto& tcm = engine->getTransformManager();
                    tcm.setTransform(tcm.getInstance(sProbe), triCam->getModelMatrix());
                }
            }
            renderer->render(mView.get());]===])

   string (FIND "${_src}" "${_tri_old}" _tri_ix)
   if (_tri_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp does not contain the native render call. Update this script.")
   endif ()

   string (REPLACE "${_tri_old}" "${_tri_new}" _out "${_src}")

   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

file (READ "${_file}" _src)
if (_src MATCHES "SNEEZE_NATIVE_TRIANGLE" AND NOT _src MATCHES "#include <filament/IndexBuffer.h>")
   string (FIND "${_src}" "#include <filament/Camera.h>" _cam_inc)
   if (_cam_inc EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp has no filament/Camera.h include. Update this script.")
   endif ()
   string (REPLACE "#include <filament/Camera.h>" [===[#include <filament/Camera.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <utils/EntityManager.h>]===] _out "${_src}")
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

# SNEEZE_NATIVE_DEPTH: the back-face cull was not hiding the scene. Stop
# rejecting fragments that fail the depth test. A tree that already has
# SNEEZE_NATIVE_CULL still needs this line.
file (READ "${_mat}" _mat_src)
if (NOT _mat_src MATCHES "SNEEZE_NATIVE_DEPTH")
   set (_depth_old [===[    mMaterialInstance->setCullingMode(filament::MaterialInstance::CullingMode::NONE);]===])
   set (_depth_new [===[    mMaterialInstance->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
    // SNEEZE_NATIVE_DEPTH: diagnostic. Stop rejecting fragments that fail
    // the depth test until the Mac scene is visible.
    mMaterialInstance->setDepthCulling(false);]===])

   string (FIND "${_mat_src}" "${_depth_old}" _depth_ix)
   if (_depth_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Material.cpp does not contain the disabled cull line. Update this script.")
   endif ()

   string (REPLACE "${_depth_old}" "${_depth_new}" _mat_out "${_mat_src}")

   if (_mat_src MATCHES "\r\n")
      set (_mat_nl CRLF)
   else ()
      set (_mat_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_mat}" CONTENT "${_mat_out}" @ONLY NEWLINE_STYLE ${_mat_nl})
endif ()

# SNEEZE_NATIVE_UNLIT: the probe triangle lands and the scene does not.
# Draw every mesh with Filament's unlit default material. Depth and facing
# stay off, matching the triangle, so a missing logo is not those tests.
set (_world "${SOURCE_DIR}/src/World.cpp")
set (_surface "${SOURCE_DIR}/src/Surface.cpp")
if (NOT EXISTS "${_world}")
   message (FATAL_ERROR "halogen-native-direct.cmake: missing ${_world}")
endif ()
if (NOT EXISTS "${_surface}")
   message (FATAL_ERROR "halogen-native-direct.cmake: missing ${_surface}")
endif ()

file (READ "${_world}" _world_src)
if (NOT _world_src MATCHES "SNEEZE_NATIVE_UNLIT" AND NOT _world_src MATCHES "SNEEZE_NATIVE_LIT")
   set (_world_old [===[        filament::RenderableManager::Builder builder(1);
        builder.geometry(0,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                geom->vertexBuffer(), geom->indexBuffer(),
                0, geom->indexCount())
            .material(0, mat->materialInstance())]===])
   set (_world_new [===[        // SNEEZE_NATIVE_UNLIT: diagnostic. Same unlit material as the
        // probe triangle. A pale logo means the vertices land and the lit
        // material was the failure.
        auto *unlit = engine->getDefaultMaterial()->createInstance();
        unlit->setDepthCulling(false);
        unlit->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
        filament::RenderableManager::Builder builder(1);
        builder.geometry(0,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                geom->vertexBuffer(), geom->indexBuffer(),
                0, geom->indexCount())
            .material(0, unlit)]===])
   string (FIND "${_world_src}" "${_world_old}" _world_ix)
   if (_world_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: World.cpp does not contain the mesh material binding. Update this script.")
   endif ()
   string (REPLACE "${_world_old}" "${_world_new}" _world_out "${_world_src}")
   if (_world_src MATCHES "\r\n")
      set (_world_nl CRLF)
   else ()
      set (_world_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_world}" CONTENT "${_world_out}" @ONLY NEWLINE_STYLE ${_world_nl})
endif ()

file (READ "${_world}" _world_src)
if (_world_src MATCHES "SNEEZE_NATIVE_UNLIT" AND NOT _world_src MATCHES "#include <filament/Material.h>")
   string (FIND "${_world_src}" "#include <filament/RenderableManager.h>" _world_inc)
   if (_world_inc EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: World.cpp has no RenderableManager include. Update this script.")
   endif ()
   string (REPLACE "#include <filament/RenderableManager.h>" [===[#include <filament/RenderableManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>]===] _world_out "${_world_src}")
   if (_world_src MATCHES "\r\n")
      set (_world_nl CRLF)
   else ()
      set (_world_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_world}" CONTENT "${_world_out}" @ONLY NEWLINE_STYLE ${_world_nl})
endif ()

file (READ "${_surface}" _surface_src)
if (NOT _surface_src MATCHES "SNEEZE_NATIVE_UNLIT" AND NOT _surface_src MATCHES "SNEEZE_NATIVE_LIT")
   set (_surface_old [===[    const bool castShadows = idxCount <= 196608u;

    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
            vb, ib, 0, idxCount)
        .material(0, mMaterial->materialInstance())]===])
   set (_surface_new [===[    const bool castShadows = idxCount <= 196608u;

    // SNEEZE_NATIVE_UNLIT: diagnostic. Same unlit material as the
    // probe triangle. A pale logo means the vertices land and the lit
    // material was the failure.
    auto *unlit = engine->getDefaultMaterial()->createInstance();
    unlit->setDepthCulling(false);
    unlit->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
            vb, ib, 0, idxCount)
        .material(0, unlit)]===])
   string (FIND "${_surface_src}" "${_surface_old}" _surface_ix)
   if (_surface_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Surface.cpp does not contain the mesh material binding. Update this script.")
   endif ()
   string (REPLACE "${_surface_old}" "${_surface_new}" _surface_out "${_surface_src}")
   if (_surface_src MATCHES "\r\n")
      set (_surface_nl CRLF)
   else ()
      set (_surface_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_surface}" CONTENT "${_surface_out}" @ONLY NEWLINE_STYLE ${_surface_nl})
endif ()

file (READ "${_surface}" _surface_src)
if (_surface_src MATCHES "SNEEZE_NATIVE_UNLIT" AND NOT _surface_src MATCHES "#include <filament/Material.h>")
   string (FIND "${_surface_src}" "#include <filament/RenderableManager.h>" _surface_inc)
   if (_surface_inc EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Surface.cpp has no RenderableManager include. Update this script.")
   endif ()
   string (REPLACE "#include <filament/RenderableManager.h>" [===[#include <filament/RenderableManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>]===] _surface_out "${_surface_src}")
   if (_surface_src MATCHES "\r\n")
      set (_surface_nl CRLF)
   else ()
      set (_surface_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_surface}" CONTENT "${_surface_out}" @ONLY NEWLINE_STYLE ${_surface_nl})
endif ()

# SNEEZE_NATIVE_MESH_CULL: the unlit meshes still did not appear. The probe
# triangle is exempt from frustum culling; the meshes were not. A tree that
# already has SNEEZE_NATIVE_UNLIT still needs this.
file (READ "${_world}" _world_src)
if (NOT _world_src MATCHES "SNEEZE_NATIVE_MESH_CULL")
   set (_mesh_world_old [===[            .material(0, unlit)
            .boundingBox(box)
            .receiveShadows(true)
            .castShadows(castShadows);]===])
   set (_mesh_world_new [===[            .material(0, unlit)
            .boundingBox(box)
            .receiveShadows(true)
            .castShadows(castShadows)
            .culling(false); // SNEEZE_NATIVE_MESH_CULL]===])
   string (FIND "${_world_src}" "${_mesh_world_old}" _mesh_world_ix)
   if (_mesh_world_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: World.cpp does not contain the unlit mesh builder. Update this script.")
   endif ()
   string (REPLACE "${_mesh_world_old}" "${_mesh_world_new}" _world_out "${_world_src}")
   if (_world_src MATCHES "\r\n")
      set (_world_nl CRLF)
   else ()
      set (_world_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_world}" CONTENT "${_world_out}" @ONLY NEWLINE_STYLE ${_world_nl})
endif ()

file (READ "${_surface}" _surface_src)
if (NOT _surface_src MATCHES "SNEEZE_NATIVE_MESH_CULL")
   set (_mesh_surface_old [===[        .material(0, unlit)
        .boundingBox(box)
        .receiveShadows(true)
        .castShadows(castShadows)
        .build(*engine, mEntity);]===])
   set (_mesh_surface_new [===[        .material(0, unlit)
        .boundingBox(box)
        .receiveShadows(true)
        .castShadows(castShadows)
        .culling(false) // SNEEZE_NATIVE_MESH_CULL
        .build(*engine, mEntity);]===])
   string (FIND "${_surface_src}" "${_mesh_surface_old}" _mesh_surface_ix)
   if (_mesh_surface_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Surface.cpp does not contain the unlit mesh builder. Update this script.")
   endif ()
   string (REPLACE "${_mesh_surface_old}" "${_mesh_surface_new}" _surface_out "${_surface_src}")
   if (_surface_src MATCHES "\r\n")
      set (_surface_nl CRLF)
   else ()
      set (_surface_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_surface}" CONTENT "${_surface_out}" @ONLY NEWLINE_STYLE ${_surface_nl})
endif ()

# SNEEZE_NATIVE_NO_TRIANGLE: the probe triangle covered the word. Take it
# out. A tree that already has SNEEZE_NATIVE_TRIANGLE still needs this.
file (READ "${_file}" _src)
if (_src MATCHES "SNEEZE_NATIVE_TRIANGLE")
   set (_notri_old [===[            // SNEEZE_NATIVE_TRIANGLE: Filament's default material is unlit
            // and writes rgb 0.8. It stays in front of the camera. A pale
            // shape on the clear means a draw can land; a solid clear means
            // no draw is reaching the window.
            {
                filament::Camera *triCam = mCamera ? mCamera->filamentCamera() : nullptr;
                static utils::Entity sProbe{};
                if (!sProbe && triCam) {
                    static const filament::math::float3 kPos[3] = {
                        {-2.0f, -2.0f, -5.0f},
                        { 2.0f, -2.0f, -5.0f},
                        { 0.0f,  2.0f, -5.0f}
                    };
                    static const uint16_t kIdx[3] = {0, 1, 2};
                    auto *vb = filament::VertexBuffer::Builder()
                        .vertexCount(3)
                        .bufferCount(1)
                        .attribute(filament::VertexAttribute::POSITION, 0,
                            filament::VertexBuffer::AttributeType::FLOAT3, 0,
                            sizeof(filament::math::float3))
                        .build(*engine);
                    vb->setBufferAt(*engine, 0,
                        filament::VertexBuffer::BufferDescriptor(kPos, sizeof(kPos)));
                    auto *ib = filament::IndexBuffer::Builder()
                        .indexCount(3)
                        .bufferType(filament::IndexBuffer::IndexType::USHORT)
                        .build(*engine);
                    ib->setBuffer(*engine,
                        filament::IndexBuffer::BufferDescriptor(kIdx, sizeof(kIdx)));
                    auto *mi = engine->getDefaultMaterial()->createInstance();
                    mi->setDepthCulling(false);
                    mi->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
                    sProbe = utils::EntityManager::get().create();
                    filament::RenderableManager::Builder(1)
                        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                            vb, ib, 0, 3)
                        .material(0, mi)
                        .boundingBox({{0, 0, -5}, {2, 2, 1}})
                        .culling(false)
                        .castShadows(false)
                        .receiveShadows(false)
                        .build(*engine, sProbe);
                    engine->getTransformManager().create(sProbe);
                    mWorld->filamentScene()->addEntity(sProbe);
                }
                if (sProbe && triCam) {
                    auto& tcm = engine->getTransformManager();
                    tcm.setTransform(tcm.getInstance(sProbe), triCam->getModelMatrix());
                }
            }
            renderer->render(mView.get());]===])
   set (_notri_new [===[            // SNEEZE_NATIVE_NO_TRIANGLE
            renderer->render(mView.get());]===])
   string (FIND "${_src}" "${_notri_old}" _notri_ix)
   if (_notri_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp triangle block does not match. Update this script.")
   endif ()
   string (REPLACE "${_notri_old}" "${_notri_new}" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_LIT: the vertices land once frustum culling is off. Put the
# real materials back. Culling stays off. A tree that already has the unlit
# binding still needs this.
file (READ "${_world}" _world_src)
if (_world_src MATCHES "SNEEZE_NATIVE_UNLIT" AND NOT _world_src MATCHES "SNEEZE_NATIVE_LIT")
   set (_lit_world_old [===[        // SNEEZE_NATIVE_UNLIT: diagnostic. Same unlit material as the
        // probe triangle. A pale logo means the vertices land and the lit
        // material was the failure.
        auto *unlit = engine->getDefaultMaterial()->createInstance();
        unlit->setDepthCulling(false);
        unlit->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
        filament::RenderableManager::Builder builder(1);
        builder.geometry(0,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                geom->vertexBuffer(), geom->indexBuffer(),
                0, geom->indexCount())
            .material(0, unlit)]===])
   set (_lit_world_new [===[        // SNEEZE_NATIVE_LIT: frustum culling stays off. The real
        // material is back so the logo and the sun can show their colors.
        filament::RenderableManager::Builder builder(1);
        builder.geometry(0,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                geom->vertexBuffer(), geom->indexBuffer(),
                0, geom->indexCount())
            .material(0, mat->materialInstance())]===])
   string (FIND "${_world_src}" "${_lit_world_old}" _lit_world_ix)
   if (_lit_world_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: World.cpp unlit block does not match. Update this script.")
   endif ()
   string (REPLACE "${_lit_world_old}" "${_lit_world_new}" _world_out "${_world_src}")
   if (_world_src MATCHES "\r\n")
      set (_world_nl CRLF)
   else ()
      set (_world_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_world}" CONTENT "${_world_out}" @ONLY NEWLINE_STYLE ${_world_nl})
endif ()

file (READ "${_surface}" _surface_src)
if (_surface_src MATCHES "SNEEZE_NATIVE_UNLIT" AND NOT _surface_src MATCHES "SNEEZE_NATIVE_LIT")
   set (_lit_surface_old [===[    // SNEEZE_NATIVE_UNLIT: diagnostic. Same unlit material as the
    // probe triangle. A pale logo means the vertices land and the lit
    // material was the failure.
    auto *unlit = engine->getDefaultMaterial()->createInstance();
    unlit->setDepthCulling(false);
    unlit->setCullingMode(filament::MaterialInstance::CullingMode::NONE);
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
            vb, ib, 0, idxCount)
        .material(0, unlit)]===])
   set (_lit_surface_new [===[    // SNEEZE_NATIVE_LIT: frustum culling stays off. The real
    // material is back so the logo and the sun can show their colors.
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
            vb, ib, 0, idxCount)
        .material(0, mMaterial->materialInstance())]===])
   string (FIND "${_surface_src}" "${_lit_surface_old}" _lit_surface_ix)
   if (_lit_surface_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Surface.cpp unlit block does not match. Update this script.")
   endif ()
   string (REPLACE "${_lit_surface_old}" "${_lit_surface_new}" _surface_out "${_surface_src}")
   if (_surface_src MATCHES "\r\n")
      set (_surface_nl CRLF)
   else ()
      set (_surface_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_surface}" CONTENT "${_surface_out}" @ONLY NEWLINE_STYLE ${_surface_nl})
endif ()

# SNEEZE_NATIVE_BOX: culling is off and the scene is visible. Count how many
# mesh boxes the camera frustum accepts, and print one box. Zero means the
# frustum test is wrong. A box far from the sun means the bounds are wrong.
file (READ "${_file}" _src)
if (NOT _src MATCHES "SNEEZE_NATIVE_BOX")
   set (_box_old [===[                std::fprintf(stderr,
                    "halogen postProcess=%d renderables=%zu viewport=%ux%u eye=%.1f,%.1f,%.1f fwd=%.2f,%.2f,%.2f near=%.3g far=%.3g fov=%.1f\n",
                    mView->isPostProcessingEnabled() ? 1 : 0,
                    mWorld->filamentScene()->getRenderableCount(),
                    mWidth, mHeight,
                    eye.x, eye.y, eye.z,
                    fwd.x, fwd.y, fwd.z,
                    clipNear, clipFar, fov);
                std::fflush(stderr);]===])
   set (_box_new [===[                std::fprintf(stderr,
                    "halogen postProcess=%d renderables=%zu viewport=%ux%u eye=%.1f,%.1f,%.1f fwd=%.2f,%.2f,%.2f near=%.3g far=%.3g fov=%.1f\n",
                    mView->isPostProcessingEnabled() ? 1 : 0,
                    mWorld->filamentScene()->getRenderableCount(),
                    mWidth, mHeight,
                    eye.x, eye.y, eye.z,
                    fwd.x, fwd.y, fwd.z,
                    clipNear, clipFar, fov);
                // SNEEZE_NATIVE_BOX: one mesh box against this camera's
                // culling frustum. inside=0 means every box is rejected.
                size_t nInside = 0;
                size_t nTested = 0;
                filament::Box sampleLocal{};
                filament::Box sampleWorld{};
                filament::math::float3 samplePos{0, 0, 0};
                int sampleHit = -1;
                if (cam && engine && mWorld) {
                    filament::Frustum frustum = cam->getFrustum();
                    auto& rcm = engine->getRenderableManager();
                    auto& tcm = engine->getTransformManager();
                    filament::Scene *scene = mWorld->filamentScene();
                    const utils::Entity *entities = rcm.getEntities();
                    const size_t nEntity = rcm.getComponentCount();
                    for (size_t i = 0; i < nEntity; ++i) {
                        if (!scene->hasEntity(entities[i]))
                            continue;
                        auto ri = rcm.getInstance(entities[i]);
                        if (!ri)
                            continue;
                        const filament::Box& local = rcm.getAxisAlignedBoundingBox(ri);
                        auto ti = tcm.getInstance(entities[i]);
                        filament::math::mat4f world = ti
                            ? filament::math::mat4f(tcm.getWorldTransformAccurate(ti))
                            : filament::math::mat4f();
                        filament::Box worldBox = rigidTransform(local, world);
                        const bool hit = frustum.intersects(worldBox);
                        ++nTested;
                        if (hit)
                            ++nInside;
                        if (sampleHit < 0) {
                            sampleLocal = local;
                            sampleWorld = worldBox;
                            samplePos = world[3].xyz;
                            sampleHit = hit ? 1 : 0;
                        }
                    }
                }
                std::fprintf(stderr,
                    "halogen cull inside=%zu/%zu local=%.3g,%.3g,%.3g ext=%.3g,%.3g,%.3g world=%.3g,%.3g,%.3g wext=%.3g,%.3g,%.3g pos=%.3g,%.3g,%.3g hit=%d\n",
                    nInside, nTested,
                    sampleLocal.center.x, sampleLocal.center.y, sampleLocal.center.z,
                    sampleLocal.halfExtent.x, sampleLocal.halfExtent.y, sampleLocal.halfExtent.z,
                    sampleWorld.center.x, sampleWorld.center.y, sampleWorld.center.z,
                    sampleWorld.halfExtent.x, sampleWorld.halfExtent.y, sampleWorld.halfExtent.z,
                    samplePos.x, samplePos.y, samplePos.z,
                    sampleHit);
                std::fflush(stderr);]===])
   string (FIND "${_src}" "${_box_old}" _box_ix)
   if (_box_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp does not contain the camera log. Update this script.")
   endif ()
   string (REPLACE "${_box_old}" "${_box_new}" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

file (READ "${_file}" _src)
if (_src MATCHES "SNEEZE_NATIVE_BOX" AND NOT _src MATCHES "#include <filament/Frustum.h>")
   string (FIND "${_src}" "#include <filament/Camera.h>" _frustum_inc)
   if (_frustum_inc EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp has no filament/Camera.h include. Update this script.")
   endif ()
   string (REPLACE "#include <filament/Camera.h>" "#include <filament/Camera.h>\n#include <filament/Frustum.h>" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_BOX_FIX: rigidTransform is a hidden friend of filament::Box,
# so it is only found by argument lookup, not as filament::rigidTransform. A
# tree that already has the qualified call fails to compile; drop the prefix.
file (READ "${_file}" _src)
if (_src MATCHES "filament::rigidTransform\\(local, world\\)")
   string (REPLACE "filament::Box worldBox = filament::rigidTransform(local, world);"
                   "filament::Box worldBox = rigidTransform(local, world);"
                   _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_SYNTH: a fixed box directly in front of the camera, with the
# same numbers on every platform. It runs the exact Culler plane test and
# names the rejecting plane, so a Windows-vs-Mac difference on identical
# input isolates the frustum test from the scene geometry. A tree that
# already has SNEEZE_NATIVE_BOX still needs this.
file (READ "${_file}" _src)
if (_src MATCHES "SNEEZE_NATIVE_BOX" AND NOT _src MATCHES "SNEEZE_NATIVE_SYNTH")
   set (_synth_old [===[                    samplePos.x, samplePos.y, samplePos.z,
                    sampleHit);
                std::fflush(stderr);]===])
   set (_synth_new [===[                    samplePos.x, samplePos.y, samplePos.z,
                    sampleHit);
                std::fflush(stderr);
                // SNEEZE_NATIVE_SYNTH: a fixed box in front of the camera,
                // same numbers on both platforms, to isolate the frustum
                // test from the scene geometry.
                if (cam) {
                    filament::Frustum fr = cam->getFrustum();
                    const double kDist = 1.0e11;
                    filament::math::float3 sc{
                        static_cast<float>(eye.x + fwd.x * kDist),
                        static_cast<float>(eye.y + fwd.y * kDist),
                        static_cast<float>(eye.z + fwd.z * kDist)};
                    const float se = 1.0e9f;
                    int synthVis = 1;
                    int worstPlane = 0;
                    float worstDot = -3.0e38f;
                    for (int j = 0; j < 6; ++j) {
                        filament::math::float4 pl =
                            fr.getNormalizedPlane(filament::Frustum::Plane(j));
                        const float ax = pl.x < 0 ? -pl.x : pl.x;
                        const float ay = pl.y < 0 ? -pl.y : pl.y;
                        const float az = pl.z < 0 ? -pl.z : pl.z;
                        const float d = pl.x * sc.x - ax * se
                                      + pl.y * sc.y - ay * se
                                      + pl.z * sc.z - az * se
                                      + pl.w;
                        if (d >= 0.0f)
                            synthVis = 0;
                        if (d > worstDot) {
                            worstDot = d;
                            worstPlane = j;
                        }
                    }
                    static const char *kPlaneName[6] =
                        {"left", "right", "bottom", "top", "far", "near"};
                    std::fprintf(stderr,
                        "halogen synth center=%.3g,%.3g,%.3g ext=%.3g hit=%d worst=%s dot=%.4g\n",
                        sc.x, sc.y, sc.z, se, synthVis,
                        kPlaneName[worstPlane], worstDot);
                    std::fflush(stderr);
                }]===])
   string (FIND "${_src}" "${_synth_old}" _synth_ix)
   if (_synth_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp cull log tail does not match. Update this script.")
   endif ()
   string (REPLACE "${_synth_old}" "${_synth_new}" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_SYNTH_FIX: Clang builds Halogen with -Werror -Wold-style-cast,
# so the synth box's C-style (float) casts fail. A tree that already has the
# old-style casts needs them rewritten to static_cast.
file (READ "${_file}" _src)
if (_src MATCHES "\\(float\\) \\(eye\\.x \\+ fwd\\.x \\* kDist\\)")
   string (REPLACE "(float) (eye.x + fwd.x * kDist)"
                   "static_cast<float>(eye.x + fwd.x * kDist)" _out "${_src}")
   string (REPLACE "(float) (eye.y + fwd.y * kDist)"
                   "static_cast<float>(eye.y + fwd.y * kDist)" _out "${_out}")
   string (REPLACE "(float) (eye.z + fwd.z * kDist)"
                   "static_cast<float>(eye.z + fwd.z * kDist)" _out "${_out}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_DUMP: a one-shot dump of every renderable's cull inputs,
# matched across platforms by entity id. It prints local box, world
# transform translation, world box, hit, and rejecting plane once, so a
# line-by-line Mac-vs-Windows compare shows whether the local AABB or the
# world transform is what diverges.
file (READ "${_file}" _src)
if (_src MATCHES "SNEEZE_NATIVE_SYNTH" AND NOT _src MATCHES "SNEEZE_NATIVE_DUMP")
   set (_dump_old [===[                        kPlaneName[worstPlane], worstDot);
                    std::fflush(stderr);
                }]===])
   set (_dump_new [===[                        kPlaneName[worstPlane], worstDot);
                    std::fflush(stderr);
                }
                // SNEEZE_NATIVE_DUMP: one-shot dump of every renderable's
                // cull inputs, matched across platforms by entity id.
                {
                    static bool sDumped = false;
                    const size_t nDump = (engine && mWorld)
                        ? engine->getRenderableManager().getComponentCount()
                        : 0;
                    if (!sDumped && cam && nDump > 0) {
                        sDumped = true;
                        filament::Frustum fr = cam->getFrustum();
                        auto& rcm = engine->getRenderableManager();
                        auto& tcm = engine->getTransformManager();
                        filament::Scene *scene = mWorld->filamentScene();
                        const utils::Entity *ents = rcm.getEntities();
                        const size_t n = rcm.getComponentCount();
                        static const char *kDumpPlane[6] =
                            {"left", "right", "bottom", "top", "far", "near"};
                        for (size_t i = 0; i < n; ++i) {
                            if (!scene->hasEntity(ents[i]))
                                continue;
                            auto ri = rcm.getInstance(ents[i]);
                            if (!ri)
                                continue;
                            const filament::Box& lb =
                                rcm.getAxisAlignedBoundingBox(ri);
                            auto ti = tcm.getInstance(ents[i]);
                            filament::math::mat4f wt = ti
                                ? filament::math::mat4f(tcm.getWorldTransformAccurate(ti))
                                : filament::math::mat4f();
                            filament::Box wb = rigidTransform(lb, wt);
                            const bool fhit = fr.intersects(wb);
                            int vis = 1;
                            int wp = 0;
                            float wd = -3.0e38f;
                            for (int j = 0; j < 6; ++j) {
                                filament::math::float4 pl =
                                    fr.getNormalizedPlane(filament::Frustum::Plane(j));
                                const float ax = pl.x < 0 ? -pl.x : pl.x;
                                const float ay = pl.y < 0 ? -pl.y : pl.y;
                                const float az = pl.z < 0 ? -pl.z : pl.z;
                                const float d = pl.x * wb.center.x - ax * wb.halfExtent.x
                                              + pl.y * wb.center.y - ay * wb.halfExtent.y
                                              + pl.z * wb.center.z - az * wb.halfExtent.z
                                              + pl.w;
                                if (d >= 0.0f)
                                    vis = 0;
                                if (d > wd) {
                                    wd = d;
                                    wp = j;
                                }
                            }
                            std::fprintf(stderr,
                                "halogen box[%zu] id=%u local=%.3g,%.3g,%.3g/%.3g,%.3g,%.3g tpos=%.3g,%.3g,%.3g world=%.3g,%.3g,%.3g/%.3g,%.3g,%.3g hit=%d fhit=%d worst=%s dot=%.3g\n",
                                i, ents[i].getId(),
                                lb.center.x, lb.center.y, lb.center.z,
                                lb.halfExtent.x, lb.halfExtent.y, lb.halfExtent.z,
                                wt[3].x, wt[3].y, wt[3].z,
                                wb.center.x, wb.center.y, wb.center.z,
                                wb.halfExtent.x, wb.halfExtent.y, wb.halfExtent.z,
                                vis, fhit ? 1 : 0, kDumpPlane[wp], wd);
                        }
                        std::fflush(stderr);
                    }
                }]===])
   string (FIND "${_src}" "${_dump_old}" _dump_ix)
   if (_dump_ix EQUAL -1)
      message (FATAL_ERROR "halogen-native-direct.cmake: Frame.cpp synth log tail does not match. Update this script.")
   endif ()
   string (REPLACE "${_dump_old}" "${_dump_new}" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_DUMP_FIX: the first once-a-second log tick fires before the
# fabric loads (renderables=0), which consumed the dump's one-shot flag on
# an empty scene so the real 33-entity dump never ran. Only consume the
# flag once there is at least one renderable.
file (READ "${_file}" _src)
if (_src MATCHES "if \\(!sDumped && cam && engine && mWorld\\) \\{")
   string (REPLACE
"                    static bool sDumped = false;
                    if (!sDumped && cam && engine && mWorld) {
                        sDumped = true;"
"                    static bool sDumped = false;
                    const size_t nDump = (engine && mWorld)
                        ? engine->getRenderableManager().getComponentCount()
                        : 0;
                    if (!sDumped && cam && nDump > 0) {
                        sDumped = true;" _out "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_DUMP_FHIT: log Filament's own frustum.intersects(box) result
# (fhit) next to the hand-rolled plane test (hit) on each dump line, to prove
# at the per-box level that Filament's intersects disagrees with the raw
# plane math on Apple Silicon.
file (READ "${_file}" _src)
if (_src MATCHES "filament::Box wb = rigidTransform\\(lb, wt\\);" AND NOT _src MATCHES "fhit")
   string (REPLACE
"                            filament::Box wb = rigidTransform(lb, wt);
                            int vis = 1;"
"                            filament::Box wb = rigidTransform(lb, wt);
                            const bool fhit = fr.intersects(wb);
                            int vis = 1;" _out "${_src}")
   string (REPLACE
"hit=%d worst=%s dot=%.3g\\n\",
                                i, ents[i].getId(),"
"hit=%d fhit=%d worst=%s dot=%.3g\\n\",
                                i, ents[i].getId()," _out2 "${_out}")
   string (REPLACE
"                                vis, kDumpPlane[wp], wd);"
"                                vis, fhit ? 1 : 0, kDumpPlane[wp], wd);" _out3 "${_out2}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_out3}" @ONLY NEWLINE_STYLE ${_nl})
endif ()

# SNEEZE_NATIVE_DUMP_SB: settle union-miscompile vs stale-binary. Prints, per
# box, the freshly compiled fast::signbit(wd) (sb -- the install header Halogen
# builds against) alongside a std::memcpy ground-truth sign bit of the same wd
# (rb). For a negative worst-dot: rb is always 1. If sb==0 while rb==1, the
# union type-pun in fast::signbit is miscompiled under Clang fast-math and the
# fork fix must use memcpy/__builtin_bit_cast. If sb==1 while fhit==0, the
# linked Culler is stale instead. Forces <math/fast.h> + <cstring> into Frame.
file (READ "${_file}" _src)
if (_src MATCHES "fhit" AND NOT _src MATCHES "sb=%d")
   if (NOT _src MATCHES "#include <math/fast.h>")
      string (REPLACE
"#include <filament/Frustum.h>"
"#include <filament/Frustum.h>
#include <math/fast.h>
#include <cstring>" _src "${_src}")
   endif ()
   string (REPLACE
"                            std::fprintf(stderr,
                                \"halogen box[%zu] id=%u"
"                            unsigned _sbBits = 0;
                            std::memcpy(&_sbBits, &wd, sizeof(_sbBits));
                            const int rb = int(_sbBits >> 31);
                            const int sb = filament::math::fast::signbit(wd);
                            std::fprintf(stderr,
                                \"halogen box[%zu] id=%u" _src "${_src}")
   string (REPLACE
"hit=%d fhit=%d worst=%s dot=%.3g\\n\","
"hit=%d fhit=%d sb=%d rb=%d worst=%s dot=%.3g\\n\"," _src "${_src}")
   string (REPLACE
"                                vis, fhit ? 1 : 0, kDumpPlane[wp], wd);"
"                                vis, fhit ? 1 : 0, sb, rb, kDumpPlane[wp], wd);" _src "${_src}")
   if (_src MATCHES "\r\n")
      set (_nl CRLF)
   else ()
      set (_nl UNIX)
   endif ()
   file (CONFIGURE OUTPUT "${_file}" CONTENT "${_src}" @ONLY NEWLINE_STYLE ${_nl})
endif ()
