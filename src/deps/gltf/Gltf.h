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
      // A single drawable surface within a mesh. Vertex streams are flat and
      // renderer-ready: positions/normals are x,y,z triples, texcoords are u,v
      // pairs, indices are 32-bit. Normals and texcoords may be empty when the
      // source primitive omits them.
      struct GLTF_PRIMITIVE
      {
         std::vector<float>    aPosition;
         std::vector<float>    aNormal;
         std::vector<float>    aTexCoord;
         std::vector<uint32_t> aIndex;
         int                   nMaterial = -1;   // index into GLTF_MODEL::aMaterial, -1 = none
      };

      // Metallic-roughness PBR factors plus a base-color texture reference.
      struct GLTF_MATERIAL
      {
         float baseColor[4]      = { 1.0f, 1.0f, 1.0f, 1.0f, };
         float dMetallic         = 1.0f;
         float dRoughness        = 1.0f;
         float emissive[3]       = { 0.0f, 0.0f, 0.0f, };
         int   nBaseColorTexture = -1;            // index into GLTF_MODEL::aTexture, -1 = none
         bool  bDoubleSided      = false;         // emit reversed winding (Halogen culls backfaces)
         int   nAlphaMode        = 0;             // 0=opaque, 1=mask, 2=blend
         float fAlphaCutoff      = 0.5f;
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
         std::vector<int> aChild;
      };

      // A faithful CPU image of a loaded glTF/GLB: the geometry, materials,
      // textures, and the node hierarchy of the default scene.
      struct GLTF_MODEL
      {
         std::vector<GLTF_MESH>     aMesh;
         std::vector<GLTF_MATERIAL> aMaterial;
         std::vector<GLTF_TEXTURE>  aTexture;
         std::vector<GLTF_NODE>     aNode;
         std::vector<int>           aRoot;        // root node indices of the default scene
      };

      class GLTF
      {
      public:
         GLTF (ENGINE* pEngine);
         ~GLTF ();

         bool Initialize ();

         // Parses a glTF or GLB blob held in memory into a GLTF_MODEL. On
         // failure leaves model empty, fills sError, and returns false.
         static bool Load (const uint8_t* pData, size_t nLen, GLTF_MODEL& model, std::string& sError);

      private:
         ENGINE* m_pEngine;
         bool    m_bInitialized;
      };
   } // namespace DEP
}
#endif // SNEEZE_GLTF_H
