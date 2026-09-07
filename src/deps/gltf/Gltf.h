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

#ifndef SNEEZE_GLTF_H
#define SNEEZE_GLTF_H

#include <cstdint>
#include <string>
#include <vector>

#include "Types.h"

namespace SNEEZE
{
   class ENGINE;

   namespace DEP
   {
      // A single drawable surface within a mesh. Only triangle primitives are
      // loaded. Vertex streams are flat and renderer-ready: positions/normals
      // are x,y,z triples, texcoords are u,v pairs, indices are 32-bit.
      // aBoundMin/Max is the position AABB (model space) when bBound is true.
      // Normals and texcoords may be empty when the source primitive omits them.
      // aJoint / aWeight are JOINTS_0 / WEIGHTS_0 (4 influences per vertex) when
      // the primitive is skinned; both empty means a rigid mesh.
      struct GLTF_PRIMITIVE
      {
         std::vector<float>    aPosition;
         std::vector<float>    aNormal;
         std::vector<float>    aTexCoord;
         std::vector<uint32_t> aIndex;
         std::vector<uint16_t> aJoint;            // 4 indices per vertex, or empty
         std::vector<float>    aWeight;           // 4 weights per vertex, or empty
         int                   nMaterial = -1;   // index into GLTF_MODEL::aMaterial, -1 = none
         float                 aBoundMin[3] = { 0.0f, 0.0f, 0.0f };
         float                 aBoundMax[3] = { 0.0f, 0.0f, 0.0f };
         bool                  bBound       = false;
      };

      // Metallic-roughness PBR factors plus a base-color texture reference.
      // bUnlit is KHR_materials_unlit only. VRMC_materials_mtoon is a lit
      // dielectric (UniVRM also stamps KHR unlit as a naive-viewer fallback;
      // a VRM loader must ignore that and keep lighting).
      struct GLTF_MATERIAL
      {
         float baseColor[4]      = { 1.0f, 1.0f, 1.0f, 1.0f, };
         float dMetallic         = 1.0f;
         float dRoughness        = 1.0f;
         float emissive[3]       = { 0.0f, 0.0f, 0.0f, };
         float shadeColor[3]     = { 1.0f, 1.0f, 1.0f, };   // VRMC_materials_mtoon shadeColorFactor
         int   nBaseColorTexture = -1;            // index into GLTF_MODEL::aTexture, -1 = none
         bool  bUnlit            = false;
      };

      // One VRMC_node_constraint on a destination node. Rotation and roll copy
      // a delta from rest (no-op at bind). Aim orients nAxis at nSource in
      // world space and does change the bind pose.
      struct GLTF_CONSTRAINT
      {
         enum eKIND
         {
            kNONE     = 0,
            kROTATION = 1,
            kAIM      = 2,
            kROLL     = 3,
         };

         int    nNode   = -1;                     // destination node index
         int    nSource = -1;
         eKIND  eKind   = kNONE;
         int    nAxis   = 0;                      // aim: 0=+X .. 5=-Z; roll: 0=X, 1=Y, 2=Z
         double dWeight = 1.0;
      };

      // Raw encoded image bytes (PNG/JPEG/...) as embedded in the glTF. Decoding
      // to RGBA8 happens later, at the renderer layer, via SNEEZE::IMAGE::Decode.
      struct GLTF_TEXTURE
      {
         std::vector<uint8_t> aEncoded;
      };

      struct GLTF_MESH
      {
         std::vector<GLTF_PRIMITIVE> aPrimitive;
      };

      // A node in the glTF hierarchy. transform is the node's local transform
      // (column-major, translation in d[12..14]); children compose under it.
      struct GLTF_NODE
      {
         MAT4             transform = {};
         int              nMesh     = -1;         // index into GLTF_MODEL::aMesh, -1 = none
         int              nSkin     = -1;         // index into GLTF_MODEL::aSkin, -1 = none
         std::vector<int> aChild;
      };

      // A glTF skin: joint node indices and matching inverse-bind matrices
      // (identity when the accessor is omitted). nSkeleton is the optional
      // skeleton root node, or -1.
      struct GLTF_SKIN
      {
         std::vector<int>  aJoint;
         std::vector<MAT4> aInverseBind;
         int               nSkeleton = -1;
      };

      // A faithful CPU image of a loaded glTF/GLB: the geometry, materials,
      // textures, skins, and the node hierarchy of the default scene.
      struct GLTF_MODEL
      {
         std::vector<GLTF_MESH>       aMesh;
         std::vector<GLTF_MATERIAL>   aMaterial;
         std::vector<GLTF_TEXTURE>    aTexture;
         std::vector<GLTF_NODE>       aNode;
         std::vector<GLTF_SKIN>       aSkin;
         std::vector<int>             aRoot;      // root node indices of the default scene
         std::vector<GLTF_CONSTRAINT> aConstraint;
      };

      class GLTF
      {
      public:
         GLTF (ENGINE* pEngine);
         ~GLTF ();

         bool Initialize ();

         // Parses a glTF, GLB, or VRM 1.0 (.vrm = GLB) blob into a GLTF_MODEL.
         static bool Load (const uint8_t* pData, size_t nLen, GLTF_MODEL& model, std::string& sError);

      private:
         ENGINE* m_pEngine;
         bool    m_bInitialized;
      };
   } // namespace DEP
}
#endif // SNEEZE_GLTF_H
