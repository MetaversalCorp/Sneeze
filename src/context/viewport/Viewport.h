// Copyright 2026 Metaversal Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SNEEZE_VIEWPORT_PRIVATE_H
#define SNEEZE_VIEWPORT_PRIVATE_H

#include "Types.h"
#include "gltf/Gltf.h"
#include "Scene.h"

#include <cstdint>

struct UV_SPHERE
{
   std::vector<float>                                       aPositions;
   std::vector<float>                                       aNormals;
   std::vector<float>                                       aTexCoords;
   std::vector<uint32_t>                                    aIndices;
};

void GenerateUVSphere (UV_SPHERE& sphere, float dRadius, int nStacks, int nSlices, float dCenterX, float dCenterY, float dCenterZ);

// Centered unit cube spanning [-0.5, 0.5] on each axis, with per-face normals.
// Callers supply a world transform that maps this canonical cube to the target
// oriented box (dimensions and pivot are baked into that transform).
void GenerateUnitBox (UV_SPHERE& box);

namespace SNEEZE
{
   struct SPHERE_DATA
   {
      RMAP::MAP::MAP_OBJECT::VEC3                           vPosition;
      float                                                 fRadius;
      RGB                                                   rgbColor;

      const uint8_t*                                        pbTexturePixels = nullptr;
      DIM2                                                  dimTexture      = { 0, 0 };
      bool                                                  bEmissive       = false;
   };

   struct CURVE_POINT
   {
      RMAP::MAP::MAP_OBJECT::VEC3                           vPosition;
      float                                                 fRadius;
   };

   struct CURVE_DATA
   {
      std::vector<CURVE_POINT>                              aPoints;
      RGB                                                   rgbColor;
   };

   struct BOX_DATA
   {
      MAT4F                                                 mWorld;                // column-major world transform (render space)
      RGB                                                   rgbColor;
   };

   // An in-scene UI panel: a textured, alpha-blended quad. The compositor bakes
   // the panel's world size into m16 (unit quad in the local XY plane, +Z
   // normal), so the renderer stays UI-agnostic -- it only sees a transform and
   // a pixel buffer, identical in spirit to a textured box.
   struct PANEL_DATA
   {
      MAT4F                                                 mWorld;                    // column-major world transform (render space), size baked in
      const uint8_t*                                        pbPixels        = nullptr; // straight-alpha RGBA8, row-major, top-down
      DIM2                                                  dim             = { 0, 0 };
   };

   // One drawable surface extracted from a loaded glTF/GLB: an indexed triangle
   // mesh with a baked world transform and a metallic-roughness material. Vertex
   // streams and the optional decoded base-color texture are borrowed pointers --
   // the caller owns the backing storage for the lifetime of the submission
   // (mirrors PANEL_DATA). Normals/texcoords/indices/texture may be absent.
   struct MESH_DATA
   {
      MAT4F                                                 mWorld          = {};        // column-major world transform (render space)
      const float*                                          pfPosition      = nullptr;   // xyz triples
      const float*                                          pfNormal        = nullptr;   // xyz triples, or null
      const float*                                          pfTexCoord      = nullptr;   // uv pairs, or null
      uint32_t                                              uCount_Vertex   = 0;
      uint32_t                                              uCount_Index    = 0;         // total indices (multiple of 3)
      const uint32_t*                                       puIndex         = nullptr;
      RGBA                                                  rgbaBaseColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
      float                                                 fMetallic       = 1.0f;
      float                                                 fRoughness      = 1.0f;
      RGB                                                   rgbEmissive     = { 0.0f, 0.0f, 0.0f };
      const uint8_t*                                        pbTexturePixels = nullptr;   // decoded RGBA8 (straight alpha), or null
      DIM2                                                  dimTexture      = { 0, 0 };
      // Stable per placed draw so SyncMeshes can instance the same vertex
      // buffers N times. pInstanceOwner is the scene NODE*; nDrawIx is the
      // slot in that node's GLTF_RENDER_MODEL::aMesh.
      const void*                                           pInstanceOwner  = nullptr;
      uint32_t                                              nDrawIx         = 0;
      float                                                 aBoundMin[3]    = { 0.0f, 0.0f, 0.0f };
      float                                                 aBoundMax[3]    = { 0.0f, 0.0f, 0.0f };
      bool                                                  bBound          = false;
   };

   // A loaded glTF model prepared for rendering. Owns all backing storage: the
   // source CPU model (vertex/index/material data) and the decoded base-color
   // textures. UV V is flipped in place on model.aMesh (glTF V=0-at-top ->
   // ANARI V=0-at-bottom) so repeated Mesh_Emit of the same primitive shares
   // one texcoord pointer. Same-material primitives on one mesh are concatenated
   // before emit (one surface per material in that mesh). aMesh is the
   // flattened, renderer-ready draw list -- one MESH_DATA per remaining
   // primitive, with the node hierarchy baked into each mWorld transform.
   // Each MESH_DATA holds borrowed pointers into model and aTexturePixel, so
   // a GLTF_RENDER_MODEL must outlive any frame that submits aMesh to the
   // renderer. Process-wide cache (Acquire/Release) shares one model across
   // nodes that load the same URL.
   struct GLTF_RENDER_MODEL
   {
      DEP::GLTF_MODEL                                       model;
      std::vector<std::vector<uint8_t>>                     aTexturePixel;                          // decoded RGBA8, one per source texture
      std::vector<int>                                      aTextureWidth;
      std::vector<int>                                      aTextureHeight;
      std::vector<MESH_DATA>                                aMesh;                                  // renderer-ready draw list
      RMAP::MAP::MAP_OBJECT::VEC3                           vCenter         = { 0.0, 0.0, 0.0 };    // model-space AABB center (post-placement)
      double                                                dRadius         = 0.0;                  // bounding-sphere radius about vCenter
   };

   // Flattens model's default-scene node hierarchy (each node composed under
   // matPlacement), decodes base-color textures to RGBA8, merges same-material
   // primitives within each mesh, resolves materials, computes bounds, and
   // fills out with a renderer-ready draw list. Takes ownership of model.
   // Returns true when at least one drawable primitive was produced.
   bool Gltf_Render_Model_Build (DEP::GLTF_MODEL model, const MAT4& matPlacement, GLTF_RENDER_MODEL& out);

   // Process-wide refcounted cache of built models, keyed by resolved URL.
   // Acquire bumps an existing entry (skips parse). Publish inserts a newly
   // built model, or adopts a winner if another thread published first
   // (deletes the loser). Release drops a ref and deletes at zero; uncached
   // models (empty key, e.g. Gltf_Preview) are deleted immediately.
   bool Gltf_Render_Model_Acquire (const std::string& sKey, GLTF_RENDER_MODEL*& pOut);
   void Gltf_Render_Model_Publish (GLTF_RENDER_MODEL* pModel, const std::string& sKey, GLTF_RENDER_MODEL*& pOut);
   void Gltf_Render_Model_Release (GLTF_RENDER_MODEL* pModel);

   struct CAMERA_DATA
   {
      RMAP::MAP::MAP_OBJECT::VEC3                           vPosition;
      RMAP::MAP::MAP_OBJECT::VEC3                           vDirection;
      RMAP::MAP::MAP_OBJECT::VEC3                           vUp;
      float                                                 fFovY;
      float                                                 fAspect;
      float                                                 fNear;
      float                                                 fFar;
      float                                                 fAngleLeft  = 0.0f;
      float                                                 fAngleRight = 0.0f;
      float                                                 fAngleUp    = 0.0f;
      float                                                 fAngleDown  = 0.0f;
      bool                                                  bAsymmetricFov = false;
   };

   struct LIGHT_DATA
   {
      enum eTYPE
      {
         kNONE              = 0,
         kPOINT             = 1,
         kSPOT              = 2,
         kPOINT__DEPRECATED = 3,
         kSPOT__DEPRECATED  = 4
      };

      int                                                   eType           = kPOINT;
      RMAP::MAP::MAP_OBJECT::VEC3                           vPosition       = { 0.0, 0.0,  0.0 };     // point / spot world position
      RMAP::MAP::MAP_OBJECT::VEC3                           vDirection      = { 0.0, 0.0, -1.0 };     // directional travel / spot aim (world, unit)
      RGB                                                   rgbColor        = { 1.0f, 1.0f, 0.95f };
      float                                                 fIntensity      = 4.0f;                   // point/spot intensity, ambient radiance, directional irradiance
      float                                                 fOpeningAngle   = 0.0f;                   // spot cone opening, radians (full half-angle)
      float                                                 fFalloffAngle   = 0.0f;                   // spot cone penumbra, radians
   };

   class VIEWPORT::RENDERER
   {
   public:
      class ANARI;

      virtual ~RENDERER () = default;

      virtual void SetNativeWindow (void* pHandle) { (void) pHandle; }
      virtual bool IsRenderingToNativeSurface () const { return false; }

      struct VULKAN
      {
         uint64_t nInstance         = 0;
         uint64_t nPhysicalDevice   = 0;
         uint64_t nDevice           = 0;
         uint64_t nQueue            = 0;
         uint32_t nQueueFamily      = 0;
         uint32_t nQueueIndex       = 0;
      };

      virtual bool VulkanHandles (VULKAN& Out) { (void) Out; return false; }
      virtual void BindExternalImage (uint64_t nImage, int nWidth, int nHeight, uint32_t nVkFormat)
      {
         (void) nImage; (void) nWidth; (void) nHeight; (void) nVkFormat;
      }
      virtual void UnbindExternalImage () {}

      virtual bool Initialize (int nWidth, int nHeight) = 0;
      virtual void Resize (int nWidth, int nHeight) = 0;

      virtual void SetCamera     (const CAMERA_DATA& pCamera) = 0;
      virtual void SetBackground (float dRed, float dGreen, float dBlue, float dAlpha) { (void) dRed; (void) dGreen; (void) dBlue; (void) dAlpha; }
      virtual void SetLights        (const std::vector<LIGHT_DATA>& aLight_Data) { (void) aLight_Data; }
      virtual void SetSceneLighting (const SCENE_LIGHT& Ambient, const SCENE_LIGHT& Directional) { (void) Ambient; (void) Directional; }
      virtual void BeginFrame    () = 0;
      virtual void SubmitSpheres (const std::vector<SPHERE_DATA>& aSphere_Data) = 0;
      virtual void SubmitCurves  (const std::vector<CURVE_DATA>&  aCurve_Data)  = 0;
      virtual void SubmitBoxes   (const std::vector<BOX_DATA>&    aBox_Data)   { (void) aBox_Data; }
      virtual void BoundingBoxOverlay (bool bEnable) { (void) bEnable; }
      virtual void SubmitPanels  (const std::vector<PANEL_DATA>&  aPanel_Data) { (void) aPanel_Data; }
      virtual void SubmitMeshes  (const std::vector<MESH_DATA>&   aMesh_Data)  { (void) aMesh_Data; }
      virtual void EndFrame () = 0;

      // Forces a full scene rebuild on the next frame (e.g. after a scene swap).
      virtual void InvalidateScene () {}

      virtual const uint32_t* GetFrameBuffer () const = 0;
      virtual int GetWidth () const = 0;
      virtual int GetHeight () const = 0;

      virtual double GetLastSubmitSeconds () const { return 0.0; }
      virtual double GetLastRenderSeconds () const { return 0.0; }
      virtual bool   LastPresented        () const { return true; }
   };
}

#endif // SNEEZE_VIEWPORT_PRIVATE_H
