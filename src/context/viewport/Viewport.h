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

struct UV_SPHERE
{
   std::vector<float>    aPositions;
   std::vector<float>    aNormals;
   std::vector<float>    aTexCoords;
   std::vector<uint32_t> aIndices;
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
      float x, y, z;
      float dRadius;
      float r, g, b;

      const uint8_t* pTexturePixels  = nullptr;
      int            nTextureWidth   = 0;
      int            nTextureHeight  = 0;
      bool           bEmissive       = false;
   };

   struct CURVE_POINT
   {
      float x, y, z;
      float dRadius;
   };

   struct CURVE_DATA
   {
      std::vector<CURVE_POINT> aPoints;
      float r, g, b;
   };

   struct BOX_DATA
   {
      float m16[16];               // column-major world transform (render space)
      float r, g, b;
   };

   // An in-scene UI panel: a textured, alpha-blended quad. The compositor bakes
   // the panel's world size into m16 (unit quad in the local XY plane, +Z
   // normal), so the renderer stays UI-agnostic -- it only sees a transform and
   // a pixel buffer, identical in spirit to a textured box.
   struct PANEL_DATA
   {
      float          m16[16];            // column-major world transform (render space), size baked in
      const uint8_t* pPixels  = nullptr; // straight-alpha RGBA8, row-major, top-down
      int            nWidth   = 0;
      int            nHeight  = 0;
   };

   // One drawable surface extracted from a loaded glTF/GLB: an indexed triangle
   // mesh with a baked world transform and a metallic-roughness material. Vertex
   // streams and the optional decoded base-color texture are borrowed pointers --
   // the caller owns the backing storage for the lifetime of the submission
   // (mirrors PANEL_DATA). Normals/texcoords/indices/texture may be absent.
   struct MESH_DATA
   {
      float           m16[16]        = {};        // column-major world transform (render space)
      const float*    pPosition      = nullptr;   // xyz triples
      const float*    pNormal        = nullptr;   // xyz triples, or null
      const float*    pTexCoord      = nullptr;   // uv pairs, or null
      uint32_t        nVertexCount   = 0;
      const uint32_t* pIndex         = nullptr;
      uint32_t        nIndexCount    = 0;         // total indices (multiple of 3)
      float           baseColor[4]   = { 1.0f, 1.0f, 1.0f, 1.0f, };
      float           dMetallic      = 1.0f;
      float           dRoughness     = 1.0f;
      float           emissive[3]    = { 0.0f, 0.0f, 0.0f, };
      const uint8_t*  pTexturePixels = nullptr;   // decoded RGBA8 (straight alpha), or null
      int             nTextureWidth  = 0;
      int             nTextureHeight = 0;

      // Compressed base-color source (KHR_texture_basisu). When set, the
      // renderer transcodes it to a GPU-supported block format and builds a
      // compressedImage2D sampler; pTexturePixels stays null in that case.
      const uint8_t*  pTextureEncoded     = nullptr;   // raw KTX2/basis blob, or null
      uint32_t        nTextureEncodedBytes = 0;
   };

   // A loaded glTF model prepared for rendering. Owns all backing storage: the
   // source CPU model (vertex/index/material data) and the decoded base-color
   // textures. aMesh is the flattened, renderer-ready draw list -- one MESH_DATA
   // per primitive, with the node hierarchy baked into each m16 transform. Each
   // MESH_DATA holds borrowed pointers into model and aTexturePixel, so a
   // GLTF_RENDER_MODEL must outlive any frame that submits aMesh to the renderer.
   struct GLTF_RENDER_MODEL
   {
      DEP::GLTF_MODEL                    model;
      std::vector<std::vector<uint8_t>> aTexturePixel;   // decoded RGBA8, one per source texture
      std::vector<int>                  aTextureWidth;
      std::vector<int>                  aTextureHeight;
      std::vector<MESH_DATA>            aMesh;            // renderer-ready draw list
      double                            aCenter[3] = { 0.0, 0.0, 0.0, };   // model-space AABB center (post-placement)
      double                            dRadius    = 0.0;                  // bounding-sphere radius about aCenter
   };

   // Flattens model's default-scene node hierarchy (each node composed under
   // matPlacement), decodes base-color textures to RGBA8, resolves materials,
   // computes bounds, and fills out with a renderer-ready draw list. Takes
   // ownership of model. Returns true when at least one drawable primitive was
   // produced.
   bool Gltf_Render_Model_Build (DEP::GLTF_MODEL model, const MAT4& matPlacement, GLTF_RENDER_MODEL& out);

   struct CAMERA_DATA
   {
      float dPosX, dPosY, dPosZ;
      float dDirX, dDirY, dDirZ;
      float dUpX,  dUpY,  dUpZ;
      float dFovY;
      float dAspect;
      float dNear;
      float dFar;
   };

   struct LIGHT_DATA
   {
      enum TYPE { kNONE = 0, kAMBIENT = 1, kDIRECTIONAL = 2, kPOINT = 3, kSPOT = 4 };

      float x = 0.0f;                 // world position (point / spot) / direction (directional)
      float y = 0.0f;
      float z = 0.0f;
      float r = 1.0f;                 // light colour
      float g = 1.0f;
      float b = 0.95f;
      float dIntensity = 4.0f;        // point/spot intensity / ambient radiance / directional irradiance
      int   eType = kPOINT;
      float dirX = 0.0f;              // spot aim direction (world, unit)
      float dirY = 0.0f;
      float dirZ = -1.0f;
      float dOpeningAngle = 0.0f;     // spot cone opening, radians (full half-angle)
      float dFalloffAngle = 0.0f;     // spot cone penumbra, radians
   };

   class VIEWPORT::RENDERER
   {
   public:
      class ANARI;

      virtual ~RENDERER () = default;

      virtual void SetNativeWindow (void* pHandle) { (void) pHandle; }
      virtual bool IsRenderingToNativeSurface () const { return false; }

      virtual bool Initialize (int nWidth, int nHeight) = 0;
      virtual void Resize (int nWidth, int nHeight) = 0;

      virtual void SetCamera     (const CAMERA_DATA& pCamera) = 0;
      virtual void SetBackground (float dRed, float dGreen, float dBlue, float dAlpha) { (void) dRed; (void) dGreen; (void) dBlue; (void) dAlpha; }
      virtual void SetLights     (const std::vector<LIGHT_DATA>&  aLight_Data)  { (void) aLight_Data; }
      virtual void BeginFrame    () = 0;
      virtual void SubmitSpheres (const std::vector<SPHERE_DATA>& aSphere_Data) = 0;
      virtual void SubmitCurves  (const std::vector<CURVE_DATA>&  aCurve_Data)  = 0;
      virtual void SubmitBoxes   (const std::vector<BOX_DATA>&    aBox_Data)   { (void) aBox_Data; }
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
   };
}

#endif // SNEEZE_VIEWPORT_PRIVATE_H
