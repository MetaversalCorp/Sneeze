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

#include "Viewport.h"
#include <Image.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

using namespace SNEEZE;

namespace
{
   struct MODEL_CACHE_ENTRY
   {
      GLTF_RENDER_MODEL* pModel = nullptr;
      int                nRef   = 0;
      std::string        sKey;
   };

   std::mutex                                                    s_mutexCache;
   std::unordered_map<std::string, MODEL_CACHE_ENTRY*>           s_mapCache;
   std::unordered_map<GLTF_RENDER_MODEL*, MODEL_CACHE_ENTRY*>    s_mapCacheByPtr;

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

   float SampleTexChannel (const std::vector<uint8_t>& px, int nW, int nH, float fU, float fV, int nChannel)
   {
      fU = std::clamp (fU, 0.0f, 1.0f);
      fV = std::clamp (fV, 0.0f, 1.0f);
      const int nX = std::clamp (static_cast<int> (fU * static_cast<float> (nW - 1)), 0, nW - 1);
      const int nY = std::clamp (static_cast<int> (fV * static_cast<float> (nH - 1)), 0, nH - 1);
      return px[(static_cast<size_t> (nY * nW + nX) * 4) + nChannel] / 255.0f;
   }

   void Mesh_RoughnessPointers_Fixup (GLTF_RENDER_MODEL& out)
   {
      size_t nAttr = 0;
      for (MESH_DATA& mesh : out.aMesh)
      {
         if (mesh.bUseRoughnessAttribute  &&  nAttr < out.aMeshRoughnessAttr.size ())
            mesh.pfRoughnessAttribute = out.aMeshRoughnessAttr[nAttr++].data ();
      }
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
         data.bBound        = prim.bBound;
         data.aBoundMin[0]  = prim.aBoundMin[0];
         data.aBoundMin[1]  = prim.aBoundMin[1];
         data.aBoundMin[2]  = prim.aBoundMin[2];
         data.aBoundMax[0]  = prim.aBoundMax[0];
         data.aBoundMax[1]  = prim.aBoundMax[1];
         data.aBoundMax[2]  = prim.aBoundMax[2];

         if (!prim.aNormal.empty ())
            data.pfNormal = prim.aNormal.data ();
         if (!prim.aTexCoord.empty ())
            data.pfTexCoord = prim.aTexCoord.data ();
         if (!prim.aIndex.empty ())
         {
            data.puIndex      = prim.aIndex.data ();
            data.uCount_Index = static_cast<uint32_t> (prim.aIndex.size ());
         }

         bool bHasAlbedo = false;
         bool bHasMrTex  = false;
         int  nAlbedoTex = -1;

         if (prim.nMaterial >= 0  &&  prim.nMaterial < static_cast<int> (out.model.aMaterial.size ()))
         {
            const DEP::GLTF_MATERIAL& mat = out.model.aMaterial[prim.nMaterial];
            data.rgbaBaseColor.fR = mat.baseColor[0];
            data.rgbaBaseColor.fG = mat.baseColor[1];
            data.rgbaBaseColor.fB = mat.baseColor[2];
            data.rgbaBaseColor.fA = mat.baseColor[3];
            // Capsule theater screens ship emissive (1,1,1); without tonemap headroom
            // that floods the frame white and hides the rest of the environment.
            data.rgbEmissive.fR   = std::min (mat.emissive[0], 0.35f);
            data.rgbEmissive.fG   = std::min (mat.emissive[1], 0.35f);
            data.rgbEmissive.fB   = std::min (mat.emissive[2], 0.35f);

            auto TextureReady = [&out] (int nTex) -> bool
            {
               return nTex >= 0
                   &&  nTex < static_cast<int> (out.aTexturePixel.size ())
                   &&  out.aTextureWidth[nTex] > 0
                   &&  out.aTextureHeight[nTex] > 0
                   &&  !out.aTexturePixel[nTex].empty ();
            };

            auto SampleChannel = SampleTexChannel;

            nAlbedoTex = mat.nBaseColorTexture;
            bHasAlbedo = TextureReady (nAlbedoTex);

            if (bHasAlbedo)
            {
               data.pbTexturePixels = out.aTexturePixel[nAlbedoTex].data ();
               data.dimTexture.nW = out.aTextureWidth[nAlbedoTex];
               data.dimTexture.nH = out.aTextureHeight[nAlbedoTex];
            }

            const int nNormalTex = mat.nNormalTexture;
            if (TextureReady (nNormalTex))
            {
               data.pbNormalTexturePixels = out.aTexturePixel[nNormalTex].data ();
               data.dimNormalTexture.nW = out.aTextureWidth[nNormalTex];
               data.dimNormalTexture.nH = out.aTextureHeight[nNormalTex];
            }

            float fMetallic  = mat.dMetallic;
            float fRoughness = mat.dRoughness;

            const int nMrTex = mat.nMetallicRoughnessTexture;
            bHasMrTex = TextureReady (nMrTex);

            // Halogen has no IBL: metallic reads black; textured props stay dielectric.
            // MR green channel still drives per-vertex roughness for surface variation.
            if (bHasAlbedo)
               fMetallic = 0.0f;
            else
               fMetallic = std::min (fMetallic, 0.15f);

            if (bHasMrTex)
            {
               const std::vector<uint8_t>& mrPx = out.aTexturePixel[nMrTex];
               const int nMrW = out.aTextureWidth[nMrTex];
               const int nMrH = out.aTextureHeight[nMrTex];

               if (data.pfTexCoord  &&  data.uCount_Vertex > 0)
               {
                  std::vector<float> aRoughAttr (data.uCount_Vertex);
                  for (uint32_t v = 0; v < data.uCount_Vertex; v++)
                  {
                     const float fU = data.pfTexCoord[v * 2];
                     const float fV = data.pfTexCoord[v * 2 + 1];
                     // Scene Assembler: roughnessMap = MR, roughness = 1.0 → sample green channel.
                     const float fG = SampleChannel (mrPx, nMrW, nMrH, fU, fV, 1);
                     aRoughAttr[v] = std::clamp (fG, 0.35f, 1.0f);
                  }
                  out.aMeshRoughnessAttr.push_back (std::move (aRoughAttr));
                  data.pfRoughnessAttribute   = out.aMeshRoughnessAttr.back ().data ();
                  data.bUseRoughnessAttribute = true;
                  data.fRoughness             = 0.5f;
               }
               else
               {
                  const float fG = SampleChannel (mrPx, nMrW, nMrH, 0.5f, 0.5f, 1);
                  fRoughness = std::clamp (fG, 0.35f, 1.0f);
               }
            }
            else if (bHasAlbedo)
            {
               fRoughness = std::clamp (fRoughness * 0.75f, 0.78f, 0.92f);
            }
            else
            {
               fRoughness = std::max (fRoughness, 0.35f);
            }

            data.fMetallic = fMetallic;
            if (!data.bUseRoughnessAttribute)
               data.fRoughness = fRoughness;
         }

         out.aMesh.push_back (data);
      }
   }

   // World-space (post-draw-transform) AABB of the built draw list, reduced to a
   // center and a bounding-sphere radius so a caller can frame the model.
   // Uses each primitive's CPU AABB (8 corners) instead of walking every vertex.
   void Bounds_Compute (GLTF_RENDER_MODEL& out)
   {
      double dMin[3] = {  std::numeric_limits<double>::max (),  std::numeric_limits<double>::max (),  std::numeric_limits<double>::max (), };
      double dMax[3] = { -std::numeric_limits<double>::max (), -std::numeric_limits<double>::max (), -std::numeric_limits<double>::max (), };
      bool   bAny    = false;

      for (const MESH_DATA& mesh : out.aMesh)
      {
         if (!mesh.bBound)
            continue;

         for (int nCorner = 0; nCorner < 8; nCorner++)
         {
            double px = (nCorner & 1) ? mesh.aBoundMax[0] : mesh.aBoundMin[0];
            double py = (nCorner & 2) ? mesh.aBoundMax[1] : mesh.aBoundMin[1];
            double pz = (nCorner & 4) ? mesh.aBoundMax[2] : mesh.aBoundMin[2];

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

   void TexCoord_FlipV (DEP::GLTF_MODEL& model)
   {
      for (DEP::GLTF_MESH& mesh : model.aMesh)
      {
         for (DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
         {
            const size_t nUVCount = prim.aTexCoord.size () / 2;
            for (size_t i = 0; i < nUVCount; i++)
               prim.aTexCoord[i * 2 + 1] = 1.0f - prim.aTexCoord[i * 2 + 1];
         }
      }
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
   {
      if (!IMAGE::Decode (out.model.aTexture[i].aEncoded, out.aTextureWidth[i], out.aTextureHeight[i], out.aTexturePixel[i]))
      {
         out.aTextureWidth[i] = 0;
         out.aTextureHeight[i] = 0;
         out.aTexturePixel[i].clear ();
      }
   }

   // glTF UV convention: V=0 at top of image. ANARI/Filament: V=0 at bottom.
   // Flip once on the CPU primitive so every Mesh_Emit of that primitive
   // shares the same texcoord pointer (GPU instancing keys off that pointer).
   TexCoord_FlipV (out.model);

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

   Mesh_RoughnessPointers_Fixup (out);
   Bounds_Compute (out);

   return !out.aMesh.empty ();
}

bool SNEEZE::Gltf_Render_Model_Acquire (const std::string& sKey, GLTF_RENDER_MODEL*& pOut)
{
   bool bResult = false;

   pOut = nullptr;

   if (!sKey.empty ())
   {
      std::lock_guard<std::mutex> guard (s_mutexCache);
      auto it = s_mapCache.find (sKey);
      if (it != s_mapCache.end ())
      {
         it->second->nRef++;
         pOut = it->second->pModel;
         bResult = true;
      }
   }

   return bResult;
}

void SNEEZE::Gltf_Render_Model_Publish (GLTF_RENDER_MODEL* pModel, const std::string& sKey, GLTF_RENDER_MODEL*& pOut)
{
   pOut = pModel;

   if (pModel  &&  !sKey.empty ())
   {
      std::lock_guard<std::mutex> guard (s_mutexCache);
      auto it = s_mapCache.find (sKey);
      if (it != s_mapCache.end ())
      {
         it->second->nRef++;
         pOut = it->second->pModel;
         delete pModel;
      }
      else
      {
         MODEL_CACHE_ENTRY* pEntry = new MODEL_CACHE_ENTRY ();
         pEntry->pModel = pModel;
         pEntry->nRef   = 1;
         pEntry->sKey   = sKey;
         s_mapCache[sKey] = pEntry;
         s_mapCacheByPtr[pModel] = pEntry;
      }
   }
}

void SNEEZE::Gltf_Render_Model_Release (GLTF_RENDER_MODEL* pModel)
{
   if (pModel)
   {
      bool bCached = false;

      {
         std::lock_guard<std::mutex> guard (s_mutexCache);
         auto it = s_mapCacheByPtr.find (pModel);
         if (it != s_mapCacheByPtr.end ())
         {
            MODEL_CACHE_ENTRY* pEntry = it->second;
            bCached = true;
            pEntry->nRef--;
            if (pEntry->nRef <= 0)
            {
               s_mapCache.erase (pEntry->sKey);
               s_mapCacheByPtr.erase (it);
               delete pEntry->pModel;
               delete pEntry;
            }
         }
      }

      if (!bCached)
         delete pModel;
   }
}

void SNEEZE::Gltf_Render_Model_ClearCache ()
{
   std::lock_guard<std::mutex> guard (s_mutexCache);
   for (auto& pair : s_mapCache)
   {
      delete pair.second->pModel;
      delete pair.second;
   }
   s_mapCache.clear ();
   s_mapCacheByPtr.clear ();
}
