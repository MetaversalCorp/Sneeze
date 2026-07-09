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

#include <Sneeze.h>
#include "Viewport.h"
#include <Image.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

using namespace SNEEZE;

namespace
{
   // Column-major multiply: matWorld = matParent * matLocal, matching the v' = M*v
   // convention so children compose under their parent's transform.
   MAT4 Mat4_Multiply (const MAT4& matA, const MAT4& matB)
   {
      MAT4 matR;
      for (int nCol = 0; nCol < 4; nCol++)
      {
         for (int nRow = 0; nRow < 4; nRow++)
         {
            double dSum = 0.0;
            for (int nK = 0; nK < 4; nK++)
               dSum += matA.d[nK * 4 + nRow] * matB.d[nCol * 4 + nK];
            matR.d[nCol * 4 + nRow] = dSum;
         }
      }
      return matR;
   }

   void Mesh_Emit (GLTF_RENDER_MODEL& out, int nMesh, const MAT4& matWorld)
   {
      const DEP::GLTF_MESH& mesh = out.model.aMesh[nMesh];
      for (const DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
      {
         if (prim.aPosition.empty ())
            continue;

         MESH_DATA data;
         for (int n = 0; n < 16; n++)
            data.mWorld.f[n] = static_cast<float> (matWorld.d[n]);

         data.pfPosition    = prim.aPosition.data ();
         data.uCount_Vertex = static_cast<uint32_t> (prim.aPosition.size () / 3);

         if (!prim.aNormal.empty ())
            data.pfNormal = prim.aNormal.data ();
         if (!prim.aTexCoord.empty ())
            data.pfTexCoord = prim.aTexCoord.data ();
         if (!prim.aIndex.empty ())
         {
            data.puIndex     = prim.aIndex.data ();
            data.uCount_Index = static_cast<uint32_t> (prim.aIndex.size ());
         }

         if (prim.nMaterial >= 0  &&  prim.nMaterial < static_cast<int> (out.model.aMaterial.size ()))
         {
            const DEP::GLTF_MATERIAL& mat = out.model.aMaterial[prim.nMaterial];
            data.rgbaBaseColor.fR = mat.baseColor[0];
            data.rgbaBaseColor.fG = mat.baseColor[1];
            data.rgbaBaseColor.fB = mat.baseColor[2];
            data.rgbaBaseColor.fA = mat.baseColor[3];
            data.fMetallic        = mat.dMetallic;
            data.fRoughness       = mat.dRoughness;
            data.rgbEmissive.fR   = mat.emissive[0];
            data.rgbEmissive.fG   = mat.emissive[1];
            data.rgbEmissive.fB   = mat.emissive[2];

            int nTex = mat.nBaseColorTexture;
            if (nTex >= 0  &&  nTex < static_cast<int> (out.aTexturePixel.size ())  &&  out.aTextureWidth[nTex] > 0  &&  out.aTextureHeight[nTex] > 0)
            {
               data.pbTexturePixels = out.aTexturePixel[nTex].data ();
               data.dimTexture.nW = out.aTextureWidth[nTex];
               data.dimTexture.nH = out.aTextureHeight[nTex];
            }
         }

         out.aMesh.push_back (data);
      }
   }

   // World-space (post-draw-transform) AABB of the built draw list, reduced to a
   // center and a bounding-sphere radius so a caller can frame the model.
   void Bounds_Compute (GLTF_RENDER_MODEL& out)
   {
      double dMin[3] = {  std::numeric_limits<double>::max (),  std::numeric_limits<double>::max (),  std::numeric_limits<double>::max (), };
      double dMax[3] = { -std::numeric_limits<double>::max (), -std::numeric_limits<double>::max (), -std::numeric_limits<double>::max (), };
      bool   bAny    = false;

      for (const MESH_DATA& mesh : out.aMesh)
      {
         for (uint32_t v = 0; v < mesh.uCount_Vertex; v++)
         {
            double px = mesh.pfPosition[v * 3 + 0];
            double py = mesh.pfPosition[v * 3 + 1];
            double pz = mesh.pfPosition[v * 3 + 2];

            double wx = mesh.mWorld.f[0] * px + mesh.mWorld.f[4] * py + mesh.mWorld.f[8]  * pz + mesh.mWorld.f[12];
            double wy = mesh.mWorld.f[1] * px + mesh.mWorld.f[5] * py + mesh.mWorld.f[9]  * pz + mesh.mWorld.f[13];
            double wz = mesh.mWorld.f[2] * px + mesh.mWorld.f[6] * py + mesh.mWorld.f[10] * pz + mesh.mWorld.f[14];

            if (wx < dMin[0]) dMin[0] = wx;
            if (wy < dMin[1]) dMin[1] = wy;
            if (wz < dMin[2]) dMin[2] = wz;
            if (wx > dMax[0]) dMax[0] = wx;
            if (wy > dMax[1]) dMax[1] = wy;
            if (wz > dMax[2]) dMax[2] = wz;
            bAny = true;
         }
      }

      if (bAny)
      {
         out.vCenter = { 0.5 * (dMin[0] + dMax[0]), 0.5 * (dMin[1] + dMax[1]), 0.5 * (dMin[2] + dMax[2]) };
         double dx = dMax[0] - dMin[0];
         double dy = dMax[1] - dMin[1];
         double dz = dMax[2] - dMin[2];
         out.dRadius = 0.5 * std::sqrt (dx * dx + dy * dy + dz * dz);
      }
   }

   void Node_Walk (GLTF_RENDER_MODEL& out, int nNode, const MAT4& matParent)
   {
      if (nNode < 0  ||  nNode >= static_cast<int> (out.model.aNode.size ()))
         return;

      const DEP::GLTF_NODE& node = out.model.aNode[nNode];
      MAT4 matWorld = Mat4_Multiply (matParent, node.transform);

      if (node.nMesh >= 0  &&  node.nMesh < static_cast<int> (out.model.aMesh.size ()))
         Mesh_Emit (out, node.nMesh, matWorld);

      for (int nChild : node.aChild)
         Node_Walk (out, nChild, matWorld);
   }
}

bool SNEEZE::Gltf_Render_Model_Build (DEP::GLTF_MODEL model, const MAT4& matPlacement, GLTF_RENDER_MODEL& out)
{
   out = GLTF_RENDER_MODEL ();
   out.model = std::move (model);

   size_t nTexture = out.model.aTexture.size ();
   out.aTexturePixel.resize (nTexture);
   out.aTextureWidth.assign (nTexture, 0);
   out.aTextureHeight.assign (nTexture, 0);

   for (size_t i = 0; i < nTexture; i++)
      IMAGE::Decode (out.model.aTexture[i].aEncoded, out.aTextureWidth[i], out.aTextureHeight[i], out.aTexturePixel[i]);

   // glTF is right-handed Y-up; Sneeze's world is right-handed Z-up. Convert every
   // imported model here at the import edge (Rx +90 deg: glTF (x,y,z) -> (x,-z,y)) so
   // a model's own +Y-up becomes world +Z-up. The fabric author's node rotation then
   // aims the (artist-arbitrary) facing from that aligned starting point.
   MAT4 matConvert =
   { {
      1.0,  0.0, 0.0, 0.0,
      0.0,  0.0, 1.0, 0.0,
      0.0, -1.0, 0.0, 0.0,
      0.0,  0.0, 0.0, 1.0,
   } };
   MAT4 matRoot = Mat4_Multiply (matPlacement, matConvert);

   for (int nRoot : out.model.aRoot)
      Node_Walk (out, nRoot, matRoot);

   Bounds_Compute (out);

   return !out.aMesh.empty ();
}
