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
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

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

   // Same-material primitives on one mesh share local space, so they can be
   // one surface. Different meshes / nodes stay separate so GPU instancing
   // (shared vertex pointers, per-node transform) is not baked away.
   bool Primitive_Compatible (const DEP::GLTF_PRIMITIVE& primA, const DEP::GLTF_PRIMITIVE& primB)
   {
      bool bResult = false;

      if (!primA.aPosition.empty ()  &&  !primB.aPosition.empty ()
       &&  primA.nMaterial == primB.nMaterial
       &&  primA.aNormal.empty () == primB.aNormal.empty ()
       &&  primA.aTexCoord.empty () == primB.aTexCoord.empty ()
       &&  primA.aJoint.empty () == primB.aJoint.empty ()
       &&  primA.aWeight.empty () == primB.aWeight.empty ()
       &&  (primA.aPosition.size () % 3) == 0
       &&  (primB.aPosition.size () % 3) == 0)
      {
         bool bIndexOk = true;
         if (!primA.aIndex.empty ()  &&  (primA.aIndex.size () % 3) != 0)
            bIndexOk = false;
         if (!primB.aIndex.empty ()  &&  (primB.aIndex.size () % 3) != 0)
            bIndexOk = false;
         if (primA.aIndex.empty ()  &&  ((primA.aPosition.size () / 3) % 3) != 0)
            bIndexOk = false;
         if (primB.aIndex.empty ()  &&  ((primB.aPosition.size () / 3) % 3) != 0)
            bIndexOk = false;
         bResult = bIndexOk;
      }

      return bResult;
   }

   void Bound_ExpandVertex (DEP::GLTF_PRIMITIVE& out, float fX, float fY, float fZ)
   {
      if (!out.bBound)
      {
         out.aBoundMin[0] = fX;
         out.aBoundMin[1] = fY;
         out.aBoundMin[2] = fZ;
         out.aBoundMax[0] = fX;
         out.aBoundMax[1] = fY;
         out.aBoundMax[2] = fZ;
         out.bBound       = true;
      }
      else
      {
         if (fX < out.aBoundMin[0]) out.aBoundMin[0] = fX;
         if (fY < out.aBoundMin[1]) out.aBoundMin[1] = fY;
         if (fZ < out.aBoundMin[2]) out.aBoundMin[2] = fZ;
         if (fX > out.aBoundMax[0]) out.aBoundMax[0] = fX;
         if (fY > out.aBoundMax[1]) out.aBoundMax[1] = fY;
         if (fZ > out.aBoundMax[2]) out.aBoundMax[2] = fZ;
      }
   }

   DEP::GLTF_PRIMITIVE Primitive_Concat (const std::vector<DEP::GLTF_PRIMITIVE>& aPrim, const std::vector<size_t>& aGroup)
   {
      DEP::GLTF_PRIMITIVE out;

      if (!aGroup.empty ())
      {
         bool bAnyIndex = false;
         for (size_t nG : aGroup)
         {
            if (!aPrim[nG].aIndex.empty ())
               bAnyIndex = true;
         }

         out.nMaterial = aPrim[aGroup[0]].nMaterial;

         for (size_t nG : aGroup)
         {
            const DEP::GLTF_PRIMITIVE& prim = aPrim[nG];
            const uint32_t nVertexBase = static_cast<uint32_t> (out.aPosition.size () / 3);

            out.aPosition.insert (out.aPosition.end (), prim.aPosition.begin (), prim.aPosition.end ());
            if (!prim.aNormal.empty ())
               out.aNormal.insert (out.aNormal.end (), prim.aNormal.begin (), prim.aNormal.end ());
            if (!prim.aTexCoord.empty ())
               out.aTexCoord.insert (out.aTexCoord.end (), prim.aTexCoord.begin (), prim.aTexCoord.end ());
            if (!prim.aJoint.empty ())
               out.aJoint.insert (out.aJoint.end (), prim.aJoint.begin (), prim.aJoint.end ());
            if (!prim.aWeight.empty ())
               out.aWeight.insert (out.aWeight.end (), prim.aWeight.begin (), prim.aWeight.end ());

            if (bAnyIndex)
            {
               if (!prim.aIndex.empty ())
               {
                  for (uint32_t nIndex : prim.aIndex)
                     out.aIndex.push_back (nIndex + nVertexBase);
               }
               else
               {
                  const uint32_t nVertex = static_cast<uint32_t> (prim.aPosition.size () / 3);
                  for (uint32_t n = 0; n < nVertex; n++)
                     out.aIndex.push_back (n + nVertexBase);
               }
            }

            if (prim.bBound)
            {
               Bound_ExpandVertex (out, prim.aBoundMin[0], prim.aBoundMin[1], prim.aBoundMin[2]);
               Bound_ExpandVertex (out, prim.aBoundMax[0], prim.aBoundMax[1], prim.aBoundMax[2]);
            }
            else
            {
               const size_t nVertex = prim.aPosition.size () / 3;
               for (size_t nV = 0; nV < nVertex; nV++)
                  Bound_ExpandVertex (out, prim.aPosition[nV * 3], prim.aPosition[nV * 3 + 1], prim.aPosition[nV * 3 + 2]);
            }
         }
      }

      return out;
   }

   void Mesh_MergeSameMaterial (DEP::GLTF_MESH& mesh)
   {
      const size_t nCount = mesh.aPrimitive.size ();
      if (nCount >= 2)
      {
         std::vector<DEP::GLTF_PRIMITIVE> aMerged;
         std::vector<uint8_t>             aUsed (nCount, 0);

         for (size_t nI = 0; nI < nCount; nI++)
         {
            if (!aUsed[nI]  &&  !mesh.aPrimitive[nI].aPosition.empty ())
            {
               std::vector<size_t> aGroup;
               aGroup.push_back (nI);
               for (size_t nJ = nI + 1; nJ < nCount; nJ++)
               {
                  if (!aUsed[nJ]  &&  Primitive_Compatible (mesh.aPrimitive[nI], mesh.aPrimitive[nJ]))
                     aGroup.push_back (nJ);
               }

               if (aGroup.size () == 1)
                  aMerged.push_back (std::move (mesh.aPrimitive[nI]));
               else
                  aMerged.push_back (Primitive_Concat (mesh.aPrimitive, aGroup));

               for (size_t nG : aGroup)
                  aUsed[nG] = 1;
            }
         }

         mesh.aPrimitive = std::move (aMerged);
      }
   }

   void Model_MergeSameMaterial (DEP::GLTF_MODEL& model)
   {
      for (DEP::GLTF_MESH& mesh : model.aMesh)
         Mesh_MergeSameMaterial (mesh);
   }

   MAT4 Mat4_Identity ()
   {
      MAT4 mat = {};
      mat.d[0]  = 1.0;
      mat.d[5]  = 1.0;
      mat.d[10] = 1.0;
      mat.d[15] = 1.0;
      return mat;
   }

   void Mat4_TransformPoint (const MAT4& mat, float fX, float fY, float fZ, float& fOutX, float& fOutY, float& fOutZ)
   {
      const double dX = fX;
      const double dY = fY;
      const double dZ = fZ;
      fOutX = static_cast<float> (mat.d[0] * dX + mat.d[4] * dY + mat.d[8]  * dZ + mat.d[12]);
      fOutY = static_cast<float> (mat.d[1] * dX + mat.d[5] * dY + mat.d[9]  * dZ + mat.d[13]);
      fOutZ = static_cast<float> (mat.d[2] * dX + mat.d[6] * dY + mat.d[10] * dZ + mat.d[14]);
   }

   void Mat4_TransformVector (const MAT4& mat, float fX, float fY, float fZ, float& fOutX, float& fOutY, float& fOutZ)
   {
      const double dX = fX;
      const double dY = fY;
      const double dZ = fZ;
      fOutX = static_cast<float> (mat.d[0] * dX + mat.d[4] * dY + mat.d[8]  * dZ);
      fOutY = static_cast<float> (mat.d[1] * dX + mat.d[5] * dY + mat.d[9]  * dZ);
      fOutZ = static_cast<float> (mat.d[2] * dX + mat.d[6] * dY + mat.d[10] * dZ);
   }

   void Node_Globals (const DEP::GLTF_MODEL& model, std::vector<MAT4>& aGlobal)
   {
      const int nNode = static_cast<int> (model.aNode.size ());
      std::vector<int> aParent (static_cast<size_t> (nNode), -1);
      for (int nI = 0; nI < nNode; nI++)
      {
         for (int nChild : model.aNode[static_cast<size_t> (nI)].aChild)
         {
            if (nChild >= 0  &&  nChild < nNode)
               aParent[static_cast<size_t> (nChild)] = nI;
         }
      }

      aGlobal.assign (static_cast<size_t> (nNode), Mat4_Identity ());
      std::vector<uint8_t> aDone (static_cast<size_t> (nNode), 0);

      for (int nStart = 0; nStart < nNode; nStart++)
      {
         std::vector<int> aStack;
         int nWalk = nStart;
         while (nWalk >= 0  &&  aDone[static_cast<size_t> (nWalk)] == 0)
         {
            aStack.push_back (nWalk);
            nWalk = aParent[static_cast<size_t> (nWalk)];
         }

         while (!aStack.empty ())
         {
            const int nNodeIx = aStack.back ();
            aStack.pop_back ();
            const int nParent = aParent[static_cast<size_t> (nNodeIx)];
            if (nParent >= 0)
               aGlobal[static_cast<size_t> (nNodeIx)] = Mat4_Multiply (aGlobal[static_cast<size_t> (nParent)], model.aNode[static_cast<size_t> (nNodeIx)].transform);
            else
               aGlobal[static_cast<size_t> (nNodeIx)] = model.aNode[static_cast<size_t> (nNodeIx)].transform;
            aDone[static_cast<size_t> (nNodeIx)] = 1;
         }
      }
   }

   void Skin_Palette (const DEP::GLTF_SKIN& skin, const std::vector<MAT4>& aGlobal, std::vector<MAT4>& aPalette)
   {
      aPalette.assign (skin.aJoint.size (), Mat4_Identity ());
      for (size_t nI = 0; nI < skin.aJoint.size (); nI++)
      {
         const int nJoint = skin.aJoint[nI];
         MAT4 matIbm = Mat4_Identity ();
         if (nI < skin.aInverseBind.size ())
            matIbm = skin.aInverseBind[nI];

         if (nJoint >= 0  &&  nJoint < static_cast<int> (aGlobal.size ()))
            aPalette[nI] = Mat4_Multiply (aGlobal[static_cast<size_t> (nJoint)], matIbm);
         else
            aPalette[nI] = matIbm;
      }
   }

   void Primitive_Skin (const DEP::GLTF_PRIMITIVE& prim, const std::vector<MAT4>& aPalette, std::vector<float>& aPosition, std::vector<float>& aNormal)
   {
      const size_t nVertex = prim.aPosition.size () / 3;
      aPosition.assign (nVertex * 3, 0.0f);

      const bool bNormal = !prim.aNormal.empty ()  &&  prim.aNormal.size () == prim.aPosition.size ();
      if (bNormal)
         aNormal.assign (nVertex * 3, 0.0f);
      else
         aNormal.clear ();

      for (size_t nV = 0; nV < nVertex; nV++)
      {
         const float fPx = prim.aPosition[nV * 3 + 0];
         const float fPy = prim.aPosition[nV * 3 + 1];
         const float fPz = prim.aPosition[nV * 3 + 2];
         float fOx = 0.0f;
         float fOy = 0.0f;
         float fOz = 0.0f;
         float fNx = 0.0f;
         float fNy = 0.0f;
         float fNz = 0.0f;
         float fWeightSum = 0.0f;

         for (int nInfluence = 0; nInfluence < 4; nInfluence++)
         {
            const float    fWeight = prim.aWeight[nV * 4 + static_cast<size_t> (nInfluence)];
            const uint16_t nJoint  = prim.aJoint[nV * 4 + static_cast<size_t> (nInfluence)];
            if (fWeight != 0.0f  &&  static_cast<size_t> (nJoint) < aPalette.size ())
            {
               float fTx = 0.0f;
               float fTy = 0.0f;
               float fTz = 0.0f;
               Mat4_TransformPoint (aPalette[nJoint], fPx, fPy, fPz, fTx, fTy, fTz);
               fOx += fWeight * fTx;
               fOy += fWeight * fTy;
               fOz += fWeight * fTz;
               fWeightSum += fWeight;

               if (bNormal)
               {
                  float fVx = 0.0f;
                  float fVy = 0.0f;
                  float fVz = 0.0f;
                  Mat4_TransformVector (aPalette[nJoint],
                     prim.aNormal[nV * 3 + 0], prim.aNormal[nV * 3 + 1], prim.aNormal[nV * 3 + 2],
                     fVx, fVy, fVz);
                  fNx += fWeight * fVx;
                  fNy += fWeight * fVy;
                  fNz += fWeight * fVz;
               }
            }
         }

         if (fWeightSum > 0.0f)
         {
            aPosition[nV * 3 + 0] = fOx / fWeightSum;
            aPosition[nV * 3 + 1] = fOy / fWeightSum;
            aPosition[nV * 3 + 2] = fOz / fWeightSum;
            if (bNormal)
            {
               const float fLen = std::sqrt (fNx * fNx + fNy * fNy + fNz * fNz);
               if (fLen > 0.0f)
               {
                  aNormal[nV * 3 + 0] = fNx / fLen;
                  aNormal[nV * 3 + 1] = fNy / fLen;
                  aNormal[nV * 3 + 2] = fNz / fLen;
               }
               else
               {
                  aNormal[nV * 3 + 0] = prim.aNormal[nV * 3 + 0];
                  aNormal[nV * 3 + 1] = prim.aNormal[nV * 3 + 1];
                  aNormal[nV * 3 + 2] = prim.aNormal[nV * 3 + 2];
               }
            }
         }
         else
         {
            aPosition[nV * 3 + 0] = fPx;
            aPosition[nV * 3 + 1] = fPy;
            aPosition[nV * 3 + 2] = fPz;
            if (bNormal)
            {
               aNormal[nV * 3 + 0] = prim.aNormal[nV * 3 + 0];
               aNormal[nV * 3 + 1] = prim.aNormal[nV * 3 + 1];
               aNormal[nV * 3 + 2] = prim.aNormal[nV * 3 + 2];
            }
         }
      }
   }

   void Bound_FromStream (MESH_DATA& data, const float* pfPosition, uint32_t nVertex)
   {
      data.bBound = false;
      if (pfPosition  &&  nVertex > 0)
      {
         data.aBoundMin[0] = pfPosition[0];
         data.aBoundMin[1] = pfPosition[1];
         data.aBoundMin[2] = pfPosition[2];
         data.aBoundMax[0] = data.aBoundMin[0];
         data.aBoundMax[1] = data.aBoundMin[1];
         data.aBoundMax[2] = data.aBoundMin[2];
         for (uint32_t nV = 1; nV < nVertex; nV++)
         {
            const float fX = pfPosition[nV * 3 + 0];
            const float fY = pfPosition[nV * 3 + 1];
            const float fZ = pfPosition[nV * 3 + 2];
            if (fX < data.aBoundMin[0]) data.aBoundMin[0] = fX;
            if (fY < data.aBoundMin[1]) data.aBoundMin[1] = fY;
            if (fZ < data.aBoundMin[2]) data.aBoundMin[2] = fZ;
            if (fX > data.aBoundMax[0]) data.aBoundMax[0] = fX;
            if (fY > data.aBoundMax[1]) data.aBoundMax[1] = fY;
            if (fZ > data.aBoundMax[2]) data.aBoundMax[2] = fZ;
         }
         data.bBound = true;
      }
   }

   void Mesh_Emit (GLTF_RENDER_MODEL& out, int nMesh, const MAT4& matWorld, const MAT4& matInstance, const std::vector<MAT4>* pPalette)
   {
      const DEP::GLTF_MESH& mesh = out.model.aMesh[nMesh];
      for (const DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
      {
         if (prim.aPosition.empty ())
            continue;

         MESH_DATA data;
         data.uCount_Vertex = static_cast<uint32_t> (prim.aPosition.size () / 3);
         data.pfPosition    = prim.aPosition.data ();
         data.bBound        = prim.bBound;
         data.aBoundMin[0]  = prim.aBoundMin[0];
         data.aBoundMin[1]  = prim.aBoundMin[1];
         data.aBoundMin[2]  = prim.aBoundMin[2];
         data.aBoundMax[0]  = prim.aBoundMax[0];
         data.aBoundMax[1]  = prim.aBoundMax[1];
         data.aBoundMax[2]  = prim.aBoundMax[2];

         if (!prim.aNormal.empty ())
            data.pfNormal = prim.aNormal.data ();

         const bool bSkinned = pPalette != nullptr
            &&  !prim.aJoint.empty ()
            &&  prim.aJoint.size () == static_cast<size_t> (data.uCount_Vertex) * 4
            &&  prim.aWeight.size () == prim.aJoint.size ();

         const MAT4& matDraw = bSkinned ? matInstance : matWorld;
         for (int n = 0; n < 16; n++)
            data.mWorld.f[n] = static_cast<float> (matDraw.d[n]);

         if (bSkinned)
         {
            std::vector<float> aPosition;
            std::vector<float> aNormal;
            Primitive_Skin (prim, *pPalette, aPosition, aNormal);
            out.aSkinnedPosition.push_back (std::move (aPosition));
            data.pfPosition = out.aSkinnedPosition.back ().data ();
            Bound_FromStream (data, data.pfPosition, data.uCount_Vertex);
            if (!aNormal.empty ())
            {
               out.aSkinnedNormal.push_back (std::move (aNormal));
               data.pfNormal = out.aSkinnedNormal.back ().data ();
            }
            else
               out.aSkinnedNormal.push_back (std::vector<float> ());
         }
         if (!prim.aTexCoord.empty ())
            data.pfTexCoord = prim.aTexCoord.data ();
         if (!prim.aIndex.empty ())
         {
            data.puIndex      = prim.aIndex.data ();
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

   void Node_Walk (GLTF_RENDER_MODEL& out, int nNode, const MAT4& matParent, const MAT4& matInstance, const std::vector<std::vector<MAT4>>& aPalette)
   {
      if (nNode < 0  ||  nNode >= static_cast<int> (out.model.aNode.size ()))
         return;

      const DEP::GLTF_NODE& node = out.model.aNode[nNode];
      MAT4 matWorld = Mat4_Multiply (matParent, node.transform);

      if (node.nMesh >= 0  &&  node.nMesh < static_cast<int> (out.model.aMesh.size ()))
      {
         // A node with nSkin poses its mesh in joint space. Mesh_Emit CPU-skins
         // those primitives and uses matInstance (Y-up convert) as mWorld.
         const std::vector<MAT4>* pPalette = nullptr;
         if (node.nSkin >= 0  &&  node.nSkin < static_cast<int> (aPalette.size ()))
            pPalette = &aPalette[static_cast<size_t> (node.nSkin)];
         Mesh_Emit (out, node.nMesh, matWorld, matInstance, pPalette);
      }

      for (int nChild : node.aChild)
         Node_Walk (out, nChild, matWorld, matInstance, aPalette);
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
      IMAGE::Decode (out.model.aTexture[i].aEncoded, out.aTextureWidth[i], out.aTextureHeight[i], out.aTexturePixel[i]);

   // glTF UV convention: V=0 at top of image. ANARI/Filament: V=0 at bottom.
   // Flip once on the CPU primitive so every Mesh_Emit of that primitive
   // shares the same texcoord pointer (GPU instancing keys off that pointer).
   TexCoord_FlipV (out.model);

   // Concatenate same-material primitives within each mesh before emit so
   // kit-style glTFs become one ANARI surface per material. Must run on the
   // CPU model (not the flattened draw list) so two nodes that instance the
   // same mesh still share vertex pointers.
   Model_MergeSameMaterial (out.model);

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

   std::vector<MAT4> aGlobal;
   Node_Globals (out.model, aGlobal);

   std::vector<std::vector<MAT4>> aPalette (out.model.aSkin.size ());
   for (size_t nSkin = 0; nSkin < out.model.aSkin.size (); nSkin++)
      Skin_Palette (out.model.aSkin[nSkin], aGlobal, aPalette[nSkin]);

   for (int nRoot : out.model.aRoot)
      Node_Walk (out, nRoot, matRoot, matRoot, aPalette);

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
