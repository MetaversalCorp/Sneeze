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
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
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

   bool Texture_Decoded (const GLTF_RENDER_MODEL& out, int nTex);
   bool Blend_IsInvisible (const GLTF_RENDER_MODEL& out, int nMat);

   void UvMatrix_FromKhr (const DEP::GLTF_UVX& uv, float aOut[9])
   {
      const float c  = std::cos (uv.dRotation);
      const float s  = std::sin (uv.dRotation);
      const float sx = uv.dScale[0];
      const float sy = uv.dScale[1];
      aOut[0] = sx * c;
      aOut[1] = sx * s;
      aOut[2] = uv.dOffset[0];
      aOut[3] = -sy * s;
      aOut[4] = sy * c;
      aOut[5] = uv.dOffset[1];
      aOut[6] = 0.0f;
      aOut[7] = 0.0f;
      aOut[8] = 1.0f;
   }

   bool UvMatrix_Equal (const float a[9], const float b[9])
   {
      bool bOk = true;
      for (int n = 0; n < 9; n++)
      {
         if (a[n] != b[n])
            bOk = false;
      }
      return bOk;
   }

   DEP::GLTF_TEXTURE::eFILTER Filter_Mag (const DEP::GLTF_TEXTURE& tex)
   {
      return tex.eMag;
   }

   void Mesh_BindMap (MESH_MAP& map, const GLTF_RENDER_MODEL& out, int nTex, const DEP::GLTF_UVX& uv)
   {
      if (Texture_Decoded (out, nTex))
      {
         const DEP::GLTF_TEXTURE& tex = out.model.aTexture[static_cast<size_t> (nTex)];
         map.pbPixels  = out.aTexturePixel[static_cast<size_t> (nTex)].data ();
         map.dim.nW    = out.aTextureWidth[nTex];
         map.dim.nH    = out.aTextureHeight[nTex];
         map.eWrapS    = tex.eWrapS;
         map.eWrapT    = tex.eWrapT;
         map.eFilter   = Filter_Mag (tex);
         map.nTexCoord = uv.nTexCoord;
         UvMatrix_FromKhr (uv, map.aUvMatrix);
      }
   }

   bool Mesh_MapEqual (const MESH_MAP& a, const MESH_MAP& b)
   {
      bool bOk = false;
      if (a.pbPixels == b.pbPixels
       &&  a.dim.nW == b.dim.nW
       &&  a.dim.nH == b.dim.nH
       &&  a.eWrapS == b.eWrapS
       &&  a.eWrapT == b.eWrapT
       &&  a.eFilter == b.eFilter
       &&  a.nTexCoord == b.nTexCoord
       &&  UvMatrix_Equal (a.aUvMatrix, b.aUvMatrix))
         bOk = true;
      return bOk;
   }

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
   // one surface. Rigid draws on different meshes stay separate so GPU
   // instancing (shared vertex pointers, per-node transform) is not baked
   // away. Skinned draws are merged across meshes after emit -- they share
   // joint space, so concatenating same-material primitives cuts Filament
   // renderables without changing the posed result.
   bool Primitive_Compatible (const DEP::GLTF_PRIMITIVE& primA, const DEP::GLTF_PRIMITIVE& primB)
   {
      bool bResult = false;

      if (!primA.aPosition.empty ()  &&  !primB.aPosition.empty ()
       &&  primA.nMaterial == primB.nMaterial
       &&  primA.aNormal.empty () == primB.aNormal.empty ()
       &&  primA.aTexCoord.empty () == primB.aTexCoord.empty ()
       &&  primA.aTexCoord1.empty () == primB.aTexCoord1.empty ()
       &&  primA.aTangent.empty () == primB.aTangent.empty ()
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
            if (!prim.aTexCoord1.empty ())
               out.aTexCoord1.insert (out.aTexCoord1.end (), prim.aTexCoord1.begin (), prim.aTexCoord1.end ());
            if (!prim.aTangent.empty ())
               out.aTangent.insert (out.aTangent.end (), prim.aTangent.begin (), prim.aTangent.end ());
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

   struct VEC3D
   {
      double dX;
      double dY;
      double dZ;
   };

   struct QUATD
   {
      double dX;
      double dY;
      double dZ;
      double dW;
   };

   double Vec_Dot (VEC3D vA, VEC3D vB)
   {
      return vA.dX * vB.dX + vA.dY * vB.dY + vA.dZ * vB.dZ;
   }

   double Vec_Length (VEC3D v)
   {
      return std::sqrt (Vec_Dot (v, v));
   }

   VEC3D Vec_Cross (VEC3D vA, VEC3D vB)
   {
      VEC3D vR;
      vR.dX = vA.dY * vB.dZ - vA.dZ * vB.dY;
      vR.dY = vA.dZ * vB.dX - vA.dX * vB.dZ;
      vR.dZ = vA.dX * vB.dY - vA.dY * vB.dX;
      return vR;
   }

   VEC3D Vec_Normalize (VEC3D v)
   {
      VEC3D vR = v;
      double dLen = Vec_Length (v);
      if (dLen > 1.0e-12)
      {
         vR.dX /= dLen;
         vR.dY /= dLen;
         vR.dZ /= dLen;
      }
      return vR;
   }

   VEC3D Vec_Sub (VEC3D vA, VEC3D vB)
   {
      VEC3D vR = { vA.dX - vB.dX, vA.dY - vB.dY, vA.dZ - vB.dZ };
      return vR;
   }

   QUATD Quat_Identity ()
   {
      QUATD q = { 0.0, 0.0, 0.0, 1.0 };
      return q;
   }

   QUATD Quat_Normalize (QUATD q)
   {
      QUATD qR = q;
      double dLen = std::sqrt (q.dX * q.dX + q.dY * q.dY + q.dZ * q.dZ + q.dW * q.dW);
      if (dLen > 1.0e-12)
      {
         qR.dX /= dLen;
         qR.dY /= dLen;
         qR.dZ /= dLen;
         qR.dW /= dLen;
      }
      return qR;
   }

   QUATD Quat_Mul (QUATD qA, QUATD qB)
   {
      QUATD qR;
      qR.dX = qA.dW * qB.dX + qA.dX * qB.dW + qA.dY * qB.dZ - qA.dZ * qB.dY;
      qR.dY = qA.dW * qB.dY - qA.dX * qB.dZ + qA.dY * qB.dW + qA.dZ * qB.dX;
      qR.dZ = qA.dW * qB.dZ + qA.dX * qB.dY - qA.dY * qB.dX + qA.dZ * qB.dW;
      qR.dW = qA.dW * qB.dW - qA.dX * qB.dX - qA.dY * qB.dY - qA.dZ * qB.dZ;
      return qR;
   }

   QUATD Quat_Conjugate (QUATD q)
   {
      QUATD qR = { -q.dX, -q.dY, -q.dZ, q.dW };
      return qR;
   }

   VEC3D Quat_Rotate (QUATD q, VEC3D v)
   {
      QUATD qV   = { v.dX, v.dY, v.dZ, 0.0 };
      QUATD qOut = Quat_Mul (Quat_Mul (q, qV), Quat_Conjugate (q));
      VEC3D vR   = { qOut.dX, qOut.dY, qOut.dZ };
      return vR;
   }

   QUATD Quat_Slerp (QUATD qA, QUATD qB, double dT)
   {
      QUATD qR = qA;
      double dDot = qA.dX * qB.dX + qA.dY * qB.dY + qA.dZ * qB.dZ + qA.dW * qB.dW;
      QUATD qB2 = qB;
      if (dDot < 0.0)
      {
         dDot   = -dDot;
         qB2.dX = -qB.dX;
         qB2.dY = -qB.dY;
         qB2.dZ = -qB.dZ;
         qB2.dW = -qB.dW;
      }

      if (dDot > 0.9995)
      {
         qR.dX = qA.dX + dT * (qB2.dX - qA.dX);
         qR.dY = qA.dY + dT * (qB2.dY - qA.dY);
         qR.dZ = qA.dZ + dT * (qB2.dZ - qA.dZ);
         qR.dW = qA.dW + dT * (qB2.dW - qA.dW);
         qR = Quat_Normalize (qR);
      }
      else
      {
         double dTheta = std::acos (dDot);
         double dSin   = std::sin (dTheta);
         double dW0    = std::sin ((1.0 - dT) * dTheta) / dSin;
         double dW1    = std::sin (dT * dTheta) / dSin;
         qR.dX = dW0 * qA.dX + dW1 * qB2.dX;
         qR.dY = dW0 * qA.dY + dW1 * qB2.dY;
         qR.dZ = dW0 * qA.dZ + dW1 * qB2.dZ;
         qR.dW = dW0 * qA.dW + dW1 * qB2.dW;
      }

      return qR;
   }

   QUATD Quat_FromTo (VEC3D vFrom, VEC3D vTo)
   {
      QUATD q = Quat_Identity ();
      VEC3D vA = Vec_Normalize (vFrom);
      VEC3D vB = Vec_Normalize (vTo);
      double dDot = Vec_Dot (vA, vB);
      if (dDot < -0.999999)
      {
         VEC3D vOrtho = { 1.0, 0.0, 0.0 };
         if (std::fabs (vA.dX) > 0.9)
            vOrtho = { 0.0, 1.0, 0.0 };
         VEC3D vAxis = Vec_Normalize (Vec_Cross (vA, vOrtho));
         q.dX = vAxis.dX;
         q.dY = vAxis.dY;
         q.dZ = vAxis.dZ;
         q.dW = 0.0;
      }
      else if (dDot < 0.999999)
      {
         VEC3D v = Vec_Cross (vA, vB);
         q.dX = v.dX;
         q.dY = v.dY;
         q.dZ = v.dZ;
         q.dW = 1.0 + dDot;
         q = Quat_Normalize (q);
      }
      return q;
   }

   QUATD Quat_FromMatrix (const MAT4& mat)
   {
      QUATD q = Quat_Identity ();
      double dM00 = mat.d[0];
      double dM10 = mat.d[1];
      double dM20 = mat.d[2];
      double dM01 = mat.d[4];
      double dM11 = mat.d[5];
      double dM21 = mat.d[6];
      double dM02 = mat.d[8];
      double dM12 = mat.d[9];
      double dM22 = mat.d[10];
      double dTrace = dM00 + dM11 + dM22;

      if (dTrace > 0.0)
      {
         double dS = std::sqrt (dTrace + 1.0) * 2.0;
         q.dW = 0.25 * dS;
         q.dX = (dM21 - dM12) / dS;
         q.dY = (dM02 - dM20) / dS;
         q.dZ = (dM10 - dM01) / dS;
      }
      else if (dM00 > dM11  &&  dM00 > dM22)
      {
         double dS = std::sqrt (1.0 + dM00 - dM11 - dM22) * 2.0;
         q.dW = (dM21 - dM12) / dS;
         q.dX = 0.25 * dS;
         q.dY = (dM01 + dM10) / dS;
         q.dZ = (dM02 + dM20) / dS;
      }
      else if (dM11 > dM22)
      {
         double dS = std::sqrt (1.0 + dM11 - dM00 - dM22) * 2.0;
         q.dW = (dM02 - dM20) / dS;
         q.dX = (dM01 + dM10) / dS;
         q.dY = 0.25 * dS;
         q.dZ = (dM12 + dM21) / dS;
      }
      else
      {
         double dS = std::sqrt (1.0 + dM22 - dM00 - dM11) * 2.0;
         q.dW = (dM10 - dM01) / dS;
         q.dX = (dM02 + dM20) / dS;
         q.dY = (dM12 + dM21) / dS;
         q.dZ = 0.25 * dS;
      }

      return Quat_Normalize (q);
   }

   VEC3D Mat4_MulPoint (const MAT4& mat, VEC3D v)
   {
      VEC3D vR;
      vR.dX = mat.d[0] * v.dX + mat.d[4] * v.dY + mat.d[8]  * v.dZ + mat.d[12];
      vR.dY = mat.d[1] * v.dX + mat.d[5] * v.dY + mat.d[9]  * v.dZ + mat.d[13];
      vR.dZ = mat.d[2] * v.dX + mat.d[6] * v.dY + mat.d[10] * v.dZ + mat.d[14];
      return vR;
   }

   bool Mat4_Inverse (const MAT4& mat, MAT4& matOut)
   {
      bool   bResult = true;
      double a[4][8];

      for (int nR = 0; nR < 4; nR++)
      {
         for (int nC = 0; nC < 4; nC++)
         {
            a[nR][nC]     = mat.d[nC * 4 + nR];
            a[nR][nC + 4] = (nR == nC) ? 1.0 : 0.0;
         }
      }

      for (int nI = 0; nI < 4  &&  bResult; nI++)
      {
         int nPivot = nI;
         for (int nR = nI + 1; nR < 4; nR++)
         {
            if (std::fabs (a[nR][nI]) > std::fabs (a[nPivot][nI]))
               nPivot = nR;
         }
         if (std::fabs (a[nPivot][nI]) < 1.0e-12)
            bResult = false;
         else
         {
            if (nPivot != nI)
            {
               for (int nC = 0; nC < 8; nC++)
               {
                  const double dTmp = a[nI][nC];
                  a[nI][nC]     = a[nPivot][nC];
                  a[nPivot][nC] = dTmp;
               }
            }
            const double dDiv = a[nI][nI];
            for (int nC = 0; nC < 8; nC++)
               a[nI][nC] /= dDiv;
            for (int nR = 0; nR < 4; nR++)
            {
               if (nR != nI)
               {
                  const double dF = a[nR][nI];
                  for (int nC = 0; nC < 8; nC++)
                     a[nR][nC] -= dF * a[nI][nC];
               }
            }
         }
      }

      if (bResult)
      {
         for (int nC = 0; nC < 4; nC++)
            for (int nR = 0; nR < 4; nR++)
               matOut.d[nC * 4 + nR] = a[nR][nC + 4];
      }
      else
         matOut = Mat4_Identity ();

      return bResult;
   }

   MAT4 Mat4_FromTRS (VEC3D vT, QUATD qR, VEC3D vS)
   {
      qR = Quat_Normalize (qR);
      double dR00 = 1.0 - 2.0 * (qR.dY * qR.dY + qR.dZ * qR.dZ);
      double dR01 =       2.0 * (qR.dX * qR.dY - qR.dW * qR.dZ);
      double dR02 =       2.0 * (qR.dX * qR.dZ + qR.dW * qR.dY);
      double dR10 =       2.0 * (qR.dX * qR.dY + qR.dW * qR.dZ);
      double dR11 = 1.0 - 2.0 * (qR.dX * qR.dX + qR.dZ * qR.dZ);
      double dR12 =       2.0 * (qR.dY * qR.dZ - qR.dW * qR.dX);
      double dR20 =       2.0 * (qR.dX * qR.dZ - qR.dW * qR.dY);
      double dR21 =       2.0 * (qR.dY * qR.dZ + qR.dW * qR.dX);
      double dR22 = 1.0 - 2.0 * (qR.dX * qR.dX + qR.dY * qR.dY);

      MAT4 mat = {};
      mat.d[0]  = dR00 * vS.dX;  mat.d[1]  = dR10 * vS.dX;  mat.d[2]  = dR20 * vS.dX;  mat.d[3]  = 0.0;
      mat.d[4]  = dR01 * vS.dY;  mat.d[5]  = dR11 * vS.dY;  mat.d[6]  = dR21 * vS.dY;  mat.d[7]  = 0.0;
      mat.d[8]  = dR02 * vS.dZ;  mat.d[9]  = dR12 * vS.dZ;  mat.d[10] = dR22 * vS.dZ;  mat.d[11] = 0.0;
      mat.d[12] = vT.dX;         mat.d[13] = vT.dY;         mat.d[14] = vT.dZ;         mat.d[15] = 1.0;
      return mat;
   }

   void Mat4_Decompose (const MAT4& mat, VEC3D& vT, QUATD& qR, VEC3D& vS)
   {
      vT = { mat.d[12], mat.d[13], mat.d[14] };
      vS.dX = Vec_Length ({ mat.d[0], mat.d[1], mat.d[2] });
      vS.dY = Vec_Length ({ mat.d[4], mat.d[5], mat.d[6] });
      vS.dZ = Vec_Length ({ mat.d[8], mat.d[9], mat.d[10] });

      MAT4 matR = Mat4_Identity ();
      if (vS.dX > 1.0e-12)
      {
         matR.d[0] = mat.d[0] / vS.dX;
         matR.d[1] = mat.d[1] / vS.dX;
         matR.d[2] = mat.d[2] / vS.dX;
      }
      if (vS.dY > 1.0e-12)
      {
         matR.d[4] = mat.d[4] / vS.dY;
         matR.d[5] = mat.d[5] / vS.dY;
         matR.d[6] = mat.d[6] / vS.dY;
      }
      if (vS.dZ > 1.0e-12)
      {
         matR.d[8]  = mat.d[8]  / vS.dZ;
         matR.d[9]  = mat.d[9]  / vS.dZ;
         matR.d[10] = mat.d[10] / vS.dZ;
      }

      qR = Quat_FromMatrix (matR);
   }

   VEC3D Aim_Vector (int nAxis)
   {
      VEC3D v = { 0.0, 1.0, 0.0 };
      if (nAxis == 0)
         v = {  1.0,  0.0,  0.0 };
      else if (nAxis == 1)
         v = { -1.0,  0.0,  0.0 };
      else if (nAxis == 2)
         v = {  0.0,  1.0,  0.0 };
      else if (nAxis == 3)
         v = {  0.0, -1.0,  0.0 };
      else if (nAxis == 4)
         v = {  0.0,  0.0,  1.0 };
      else if (nAxis == 5)
         v = {  0.0,  0.0, -1.0 };
      return v;
   }

   VEC3D Roll_Vector (int nAxis)
   {
      VEC3D v = { 0.0, 1.0, 0.0 };
      if (nAxis == 0)
         v = { 1.0, 0.0, 0.0 };
      else if (nAxis == 2)
         v = { 0.0, 0.0, 1.0 };
      return v;
   }

   void Node_Parents (const std::vector<DEP::GLTF_NODE>& aNode, std::vector<int>& aParent)
   {
      const int nNode = static_cast<int> (aNode.size ());
      aParent.assign (static_cast<size_t> (nNode), -1);
      for (int nI = 0; nI < nNode; nI++)
      {
         for (int nChild : aNode[static_cast<size_t> (nI)].aChild)
         {
            if (nChild >= 0  &&  nChild < nNode)
               aParent[static_cast<size_t> (nChild)] = nI;
         }
      }
   }

   void Node_Globals (const std::vector<DEP::GLTF_NODE>& aNode, std::vector<MAT4>& aGlobal)
   {
      const int nNode = static_cast<int> (aNode.size ());
      std::vector<int> aParent;
      Node_Parents (aNode, aParent);

      aGlobal.assign (static_cast<size_t> (nNode), Mat4_Identity ());
      std::vector<uint8_t> aDone (static_cast<size_t> (nNode), 0);

      std::vector<int> aStack;
      aStack.reserve (static_cast<size_t> (nNode));

      for (int nStart = 0; nStart < nNode; nStart++)
      {
         aStack.clear ();
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
               aGlobal[static_cast<size_t> (nNodeIx)] = Mat4_Multiply (aGlobal[static_cast<size_t> (nParent)], aNode[static_cast<size_t> (nNodeIx)].transform);
            else
               aGlobal[static_cast<size_t> (nNodeIx)] = aNode[static_cast<size_t> (nNodeIx)].transform;
            aDone[static_cast<size_t> (nNodeIx)] = 1;
         }
      }
   }

   void Node_SetRotation (DEP::GLTF_NODE& node, QUATD qR)
   {
      VEC3D vT;
      QUATD qOld;
      VEC3D vS;
      Mat4_Decompose (node.transform, vT, qOld, vS);
      node.transform = Mat4_FromTRS (vT, qR, vS);
   }

   void Constraint_ApplyOne (std::vector<DEP::GLTF_NODE>& aNode, const DEP::GLTF_CONSTRAINT& constraint, const std::vector<MAT4>& aRest, const std::vector<MAT4>& aGlobal, const std::vector<int>& aParent)
   {
      const int nDest   = constraint.nNode;
      const int nSource = constraint.nSource;
      const int nNode   = static_cast<int> (aNode.size ());

      if (nDest >= 0  &&  nDest < nNode  &&  nSource >= 0  &&  nSource < nNode)
      {
      VEC3D vDstT;
      QUATD qDstRest;
      VEC3D vDstS;
      Mat4_Decompose (aRest[static_cast<size_t> (nDest)], vDstT, qDstRest, vDstS);

      VEC3D vSrcRestT;
      QUATD qSrcRest;
      VEC3D vSrcRestS;
      Mat4_Decompose (aRest[static_cast<size_t> (nSource)], vSrcRestT, qSrcRest, vSrcRestS);

      VEC3D vSrcT;
      QUATD qSrcNow;
      VEC3D vSrcS;
      Mat4_Decompose (aNode[static_cast<size_t> (nSource)].transform, vSrcT, qSrcNow, vSrcS);

      QUATD qOut = qDstRest;
      double dWeight = constraint.dWeight;
      if (dWeight < 0.0)
         dWeight = 0.0;
      if (dWeight > 1.0)
         dWeight = 1.0;

      if (constraint.eKind == DEP::GLTF_CONSTRAINT::kROTATION)
      {
         QUATD qDelta = Quat_Mul (Quat_Conjugate (qSrcRest), qSrcNow);
         qOut = Quat_Mul (qDstRest, qDelta);
      }
      else if (constraint.eKind == DEP::GLTF_CONSTRAINT::kROLL)
      {
         QUATD qDelta = Quat_Mul (Quat_Conjugate (qSrcRest), qSrcNow);
         VEC3D vAxis  = Roll_Vector (constraint.nAxis);
         VEC3D vTwist = Quat_Rotate (qDelta, vAxis);
         QUATD qSwing = Quat_FromTo (vAxis, vTwist);
         QUATD qTwist = Quat_Mul (Quat_Conjugate (qSwing), qDelta);
         qOut = Quat_Mul (qDstRest, qTwist);
      }
      else if (constraint.eKind == DEP::GLTF_CONSTRAINT::kAIM)
      {
         const MAT4& matDst = aGlobal[static_cast<size_t> (nDest)];
         const MAT4& matSrc = aGlobal[static_cast<size_t> (nSource)];
         VEC3D vDstPos = { matDst.d[12], matDst.d[13], matDst.d[14] };
         VEC3D vSrcPos = { matSrc.d[12], matSrc.d[13], matSrc.d[14] };
         VEC3D vTo     = Vec_Sub (vSrcPos, vDstPos);
         if (Vec_Length (vTo) > 1.0e-8)
         {
            VEC3D vDstWT;
            QUATD qDstWorld;
            VEC3D vDstWS;
            Mat4_Decompose (matDst, vDstWT, qDstWorld, vDstWS);

            VEC3D vFrom = Quat_Rotate (qDstWorld, Aim_Vector (constraint.nAxis));
            QUATD qTurn = Quat_FromTo (vFrom, vTo);
            QUATD qWorld = Quat_Mul (qTurn, qDstWorld);

            QUATD qParent = Quat_Identity ();
            const int nParent = aParent[static_cast<size_t> (nDest)];
            if (nParent >= 0  &&  nParent < nNode)
            {
               VEC3D vPT;
               VEC3D vPS;
               Mat4_Decompose (aGlobal[static_cast<size_t> (nParent)], vPT, qParent, vPS);
            }
            qOut = Quat_Mul (Quat_Conjugate (qParent), qWorld);
         }
      }

      qOut = Quat_Slerp (qDstRest, qOut, dWeight);
      Node_SetRotation (aNode[static_cast<size_t> (nDest)], qOut);
      }
   }

   void Constraint_ApplyNodes (std::vector<DEP::GLTF_NODE>& aNode, const std::vector<DEP::GLTF_CONSTRAINT>& aConstraint, const std::vector<MAT4>& aRest)
   {
      if (!aConstraint.empty ()  &&  aRest.size () == aNode.size ())
      {
         std::vector<int> aParent;
         Node_Parents (aNode, aParent);

         const int nPass = 2;

         for (int nI = 0; nI < nPass; nI++)
         {
            std::vector<MAT4> aGlobal;
            Node_Globals (aNode, aGlobal);
            for (const DEP::GLTF_CONSTRAINT& constraint : aConstraint)
               Constraint_ApplyOne (aNode, constraint, aRest, aGlobal, aParent);
         }
      }
   }

   void Constraint_Apply (DEP::GLTF_MODEL& model)
   {
      std::vector<MAT4> aRest;
      aRest.reserve (model.aNode.size ());
      for (const DEP::GLTF_NODE& node : model.aNode)
         aRest.push_back (node.transform);
      Constraint_ApplyNodes (model.aNode, model.aConstraint, aRest);
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

   void Palette_Pack (const std::vector<MAT4>& aPalette, std::vector<float>& aOut)
   {
      size_t nBone = aPalette.size ();
      if (nBone > 255)
         nBone = 255;

      aOut.resize (nBone * 16);
      for (size_t nI = 0; nI < nBone; nI++)
      {
         for (int nK = 0; nK < 16; nK++)
            aOut[nI * 16 + static_cast<size_t> (nK)] = static_cast<float> (aPalette[nI].d[nK]);
      }
   }

   void Mesh_Emit (GLTF_RENDER_MODEL& out, int nMesh, const MAT4& matWorld, const MAT4& matInstance, int nSkin)
   {
      const DEP::GLTF_MESH& mesh = out.model.aMesh[nMesh];
      for (const DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
      {
         if (prim.aPosition.empty ())
            continue;
         if (Blend_IsInvisible (out, prim.nMaterial))
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

         const bool bSkinned = nSkin >= 0
            &&  nSkin < static_cast<int> (out.aBonePalette.size ())
            &&  !out.aBonePalette[static_cast<size_t> (nSkin)].empty ()
            &&  !prim.aJoint.empty ()
            &&  prim.aJoint.size () == static_cast<size_t> (data.uCount_Vertex) * 4
            &&  prim.aWeight.size () == prim.aJoint.size ();

         const MAT4& matDraw = bSkinned ? matInstance : matWorld;
         for (int n = 0; n < 16; n++)
            data.mWorld.f[n] = static_cast<float> (matDraw.d[n]);

         if (bSkinned)
         {
            data.nSkin        = nSkin;
            data.puJoint      = prim.aJoint.data ();
            data.pfWeight     = prim.aWeight.data ();
            data.pfBoneMatrix = out.aBonePalette[static_cast<size_t> (nSkin)].data ();
            data.uCount_Bone  = static_cast<uint32_t> (out.aBonePalette[static_cast<size_t> (nSkin)].size () / 16);
         }
         if (!prim.aTexCoord.empty ())
            data.pfTexCoord = prim.aTexCoord.data ();
         if (!prim.aTexCoord1.empty ())
            data.pfTexCoord1 = prim.aTexCoord1.data ();
         if (!prim.aTangent.empty ())
            data.pfTangent = prim.aTangent.data ();
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
            data.fNormalScale     = mat.dNormalScale;
            data.fOcclusionStrength = mat.dOcclusionStrength;
            data.bUnlit           = mat.bUnlit;
            data.bDoubleSided     = mat.bDoubleSided;
            data.eAlpha           = mat.eAlpha;
            data.fAlphaCutoff     = mat.dAlphaCutoff;

            int nTex = mat.nBaseColorTexture;
            if (Texture_Decoded (out, nTex))
            {
               const DEP::GLTF_TEXTURE& tex = out.model.aTexture[static_cast<size_t> (nTex)];
               data.pbTexturePixels = out.aTexturePixel[nTex].data ();
               data.dimTexture.nW = out.aTextureWidth[nTex];
               data.dimTexture.nH = out.aTextureHeight[nTex];
               data.eTextureWrapS = tex.eWrapS;
               data.eTextureWrapT = tex.eWrapT;
               data.eTextureFilter = tex.eMag;
               data.nTextureTexCoord = mat.uvBaseColor.nTexCoord;
               UvMatrix_FromKhr (mat.uvBaseColor, data.aTextureUvMatrix);
            }

            const int nEmissive = mat.nEmissiveTexture;
            if (Texture_Decoded (out, nEmissive))
            {
               const DEP::GLTF_TEXTURE& tex = out.model.aTexture[static_cast<size_t> (nEmissive)];
               data.pbEmissivePixels = out.aTexturePixel[nEmissive].data ();
               data.dimEmissive.nW = out.aTextureWidth[nEmissive];
               data.dimEmissive.nH = out.aTextureHeight[nEmissive];
               data.eEmissiveWrapS = tex.eWrapS;
               data.eEmissiveWrapT = tex.eWrapT;
               data.eEmissiveFilter = tex.eMag;
               data.nEmissiveTexCoord = mat.uvEmissive.nTexCoord;
               UvMatrix_FromKhr (mat.uvEmissive, data.aEmissiveUvMatrix);
            }
            else if (nEmissive >= 0)
            {
               data.rgbEmissive.fR = 0.0f;
               data.rgbEmissive.fG = 0.0f;
               data.rgbEmissive.fB = 0.0f;
            }

            Mesh_BindMap (data.mapMetallicRoughness, out, mat.nMetallicRoughnessTexture, mat.uvMetallicRoughness);
            Mesh_BindMap (data.mapNormal, out, mat.nNormalTexture, mat.uvNormal);
            Mesh_BindMap (data.mapOcclusion, out, mat.nOcclusionTexture, mat.uvOcclusion);
         }

         out.aMesh.push_back (data);
      }
   }

   bool Mesh_SkinnedCompatible (const MESH_DATA& a, const MESH_DATA& b)
   {
      bool bOk = false;

      if (a.nSkin >= 0  &&  a.nSkin == b.nSkin
       &&  a.puJoint  &&  b.puJoint
       &&  a.pfWeight  &&  b.pfWeight
       &&  a.uCount_Vertex > 0  &&  b.uCount_Vertex > 0
       &&  ((a.pfNormal == nullptr) == (b.pfNormal == nullptr))
       &&  ((a.pfTexCoord == nullptr) == (b.pfTexCoord == nullptr))
       &&  ((a.pfTexCoord1 == nullptr) == (b.pfTexCoord1 == nullptr))
       &&  ((a.pfTangent == nullptr) == (b.pfTangent == nullptr))
       &&  a.pbTexturePixels == b.pbTexturePixels
       &&  a.dimTexture.nW == b.dimTexture.nW
       &&  a.dimTexture.nH == b.dimTexture.nH
       &&  a.eTextureWrapS == b.eTextureWrapS
       &&  a.eTextureWrapT == b.eTextureWrapT
       &&  a.eTextureFilter == b.eTextureFilter
       &&  a.nTextureTexCoord == b.nTextureTexCoord
       &&  UvMatrix_Equal (a.aTextureUvMatrix, b.aTextureUvMatrix)
       &&  a.pbEmissivePixels == b.pbEmissivePixels
       &&  a.dimEmissive.nW == b.dimEmissive.nW
       &&  a.dimEmissive.nH == b.dimEmissive.nH
       &&  a.eEmissiveWrapS == b.eEmissiveWrapS
       &&  a.eEmissiveWrapT == b.eEmissiveWrapT
       &&  a.eEmissiveFilter == b.eEmissiveFilter
       &&  a.nEmissiveTexCoord == b.nEmissiveTexCoord
       &&  UvMatrix_Equal (a.aEmissiveUvMatrix, b.aEmissiveUvMatrix)
       &&  Mesh_MapEqual (a.mapMetallicRoughness, b.mapMetallicRoughness)
       &&  Mesh_MapEqual (a.mapNormal, b.mapNormal)
       &&  Mesh_MapEqual (a.mapOcclusion, b.mapOcclusion)
       &&  a.rgbaBaseColor.fR == b.rgbaBaseColor.fR
       &&  a.rgbaBaseColor.fG == b.rgbaBaseColor.fG
       &&  a.rgbaBaseColor.fB == b.rgbaBaseColor.fB
       &&  a.rgbaBaseColor.fA == b.rgbaBaseColor.fA
       &&  a.fMetallic == b.fMetallic
       &&  a.fRoughness == b.fRoughness
       &&  a.fNormalScale == b.fNormalScale
       &&  a.fOcclusionStrength == b.fOcclusionStrength
       &&  a.rgbEmissive.fR == b.rgbEmissive.fR
       &&  a.rgbEmissive.fG == b.rgbEmissive.fG
       &&  a.rgbEmissive.fB == b.rgbEmissive.fB
       &&  a.bUnlit == b.bUnlit
       &&  a.bDoubleSided == b.bDoubleSided
       &&  a.eAlpha == b.eAlpha
       &&  a.fAlphaCutoff == b.fAlphaCutoff)
         bOk = true;

      return bOk;
   }

   void Mesh_BoundExpand (MESH_DATA& out, const MESH_DATA& src)
   {
      if (src.bBound)
      {
         if (!out.bBound)
         {
            out.aBoundMin[0] = src.aBoundMin[0];
            out.aBoundMin[1] = src.aBoundMin[1];
            out.aBoundMin[2] = src.aBoundMin[2];
            out.aBoundMax[0] = src.aBoundMax[0];
            out.aBoundMax[1] = src.aBoundMax[1];
            out.aBoundMax[2] = src.aBoundMax[2];
            out.bBound       = true;
         }
         else
         {
            if (src.aBoundMin[0] < out.aBoundMin[0]) out.aBoundMin[0] = src.aBoundMin[0];
            if (src.aBoundMin[1] < out.aBoundMin[1]) out.aBoundMin[1] = src.aBoundMin[1];
            if (src.aBoundMin[2] < out.aBoundMin[2]) out.aBoundMin[2] = src.aBoundMin[2];
            if (src.aBoundMax[0] > out.aBoundMax[0]) out.aBoundMax[0] = src.aBoundMax[0];
            if (src.aBoundMax[1] > out.aBoundMax[1]) out.aBoundMax[1] = src.aBoundMax[1];
            if (src.aBoundMax[2] > out.aBoundMax[2]) out.aBoundMax[2] = src.aBoundMax[2];
         }
      }
   }

   MESH_DATA Mesh_ConcatDraws (GLTF_RENDER_MODEL& out, const std::vector<size_t>& aGroup)
   {
      MESH_DATA                 data;
      GLTF_RENDER_MODEL::MESH_STREAM stream;
      bool                      bAnyIndex = false;

      if (!aGroup.empty ())
      {
         data = out.aMesh[aGroup[0]];

         for (size_t nG : aGroup)
         {
            if (out.aMesh[nG].puIndex  &&  out.aMesh[nG].uCount_Index > 0)
               bAnyIndex = true;
         }

         data.pfPosition    = nullptr;
         data.pfNormal      = nullptr;
         data.pfTexCoord    = nullptr;
         data.pfTexCoord1   = nullptr;
         data.pfTangent     = nullptr;
         data.puJoint       = nullptr;
         data.pfWeight      = nullptr;
         data.puIndex       = nullptr;
         data.uCount_Vertex = 0;
         data.uCount_Index  = 0;
         data.bBound        = false;

         for (size_t nG : aGroup)
         {
            const MESH_DATA& src = out.aMesh[nG];
            const uint32_t nVertexBase = static_cast<uint32_t> (stream.aPosition.size () / 3);
            const uint32_t nVertex     = src.uCount_Vertex;

            stream.aPosition.insert (stream.aPosition.end (), src.pfPosition, src.pfPosition + static_cast<size_t> (nVertex) * 3);
            if (src.pfNormal)
               stream.aNormal.insert (stream.aNormal.end (), src.pfNormal, src.pfNormal + static_cast<size_t> (nVertex) * 3);
            if (src.pfTexCoord)
               stream.aTexCoord.insert (stream.aTexCoord.end (), src.pfTexCoord, src.pfTexCoord + static_cast<size_t> (nVertex) * 2);
            if (src.pfTexCoord1)
               stream.aTexCoord1.insert (stream.aTexCoord1.end (), src.pfTexCoord1, src.pfTexCoord1 + static_cast<size_t> (nVertex) * 2);
            if (src.pfTangent)
               stream.aTangent.insert (stream.aTangent.end (), src.pfTangent, src.pfTangent + static_cast<size_t> (nVertex) * 4);
            if (src.puJoint)
               stream.aJoint.insert (stream.aJoint.end (), src.puJoint, src.puJoint + static_cast<size_t> (nVertex) * 4);
            if (src.pfWeight)
               stream.aWeight.insert (stream.aWeight.end (), src.pfWeight, src.pfWeight + static_cast<size_t> (nVertex) * 4);

            if (bAnyIndex)
            {
               if (src.puIndex  &&  src.uCount_Index > 0)
               {
                  for (uint32_t nI = 0; nI < src.uCount_Index; nI++)
                     stream.aIndex.push_back (src.puIndex[nI] + nVertexBase);
               }
               else
               {
                  for (uint32_t n = 0; n < nVertex; n++)
                     stream.aIndex.push_back (n + nVertexBase);
               }
            }

            Mesh_BoundExpand (data, src);
         }

         out.aMerged.push_back (std::move (stream));
         GLTF_RENDER_MODEL::MESH_STREAM& live = out.aMerged.back ();
         data.uCount_Vertex = static_cast<uint32_t> (live.aPosition.size () / 3);
         data.pfPosition    = live.aPosition.data ();
         if (!live.aNormal.empty ())
            data.pfNormal = live.aNormal.data ();
         if (!live.aTexCoord.empty ())
            data.pfTexCoord = live.aTexCoord.data ();
         if (!live.aTexCoord1.empty ())
            data.pfTexCoord1 = live.aTexCoord1.data ();
         if (!live.aTangent.empty ())
            data.pfTangent = live.aTangent.data ();
         if (!live.aJoint.empty ())
            data.puJoint = live.aJoint.data ();
         if (!live.aWeight.empty ())
            data.pfWeight = live.aWeight.data ();
         if (!live.aIndex.empty ())
         {
            data.puIndex      = live.aIndex.data ();
            data.uCount_Index = static_cast<uint32_t> (live.aIndex.size ());
         }
      }

      return data;
   }

   void Mesh_MergeSkinnedDraws (GLTF_RENDER_MODEL& out)
   {
      const size_t nCount = out.aMesh.size ();
      if (nCount >= 2)
      {
         std::vector<MESH_DATA> aNext;
         std::vector<uint8_t>   aUsed (nCount, 0);

         out.aMerged.clear ();
         out.aMerged.reserve (nCount);
         aNext.reserve (nCount);

         for (size_t nI = 0; nI < nCount; nI++)
         {
            if (!aUsed[nI])
            {
               if (out.aMesh[nI].nSkin < 0  ||  !out.aMesh[nI].puJoint  ||  !out.aMesh[nI].pfWeight)
               {
                  aNext.push_back (out.aMesh[nI]);
                  aUsed[nI] = 1;
               }
               else
               {
                  std::vector<size_t> aGroup;
                  aGroup.push_back (nI);
                  for (size_t nJ = nI + 1; nJ < nCount; nJ++)
                  {
                     if (!aUsed[nJ]  &&  Mesh_SkinnedCompatible (out.aMesh[nI], out.aMesh[nJ]))
                        aGroup.push_back (nJ);
                  }

                  if (aGroup.size () == 1)
                     aNext.push_back (out.aMesh[nI]);
                  else
                     aNext.push_back (Mesh_ConcatDraws (out, aGroup));

                  for (size_t nG : aGroup)
                     aUsed[nG] = 1;
               }
            }
         }

         out.aMesh = std::move (aNext);
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

   void Node_Walk (GLTF_RENDER_MODEL& out, int nNode, const MAT4& matParent, const MAT4& matInstance)
   {
      if (nNode < 0  ||  nNode >= static_cast<int> (out.model.aNode.size ()))
         return;

      const DEP::GLTF_NODE& node = out.model.aNode[nNode];
      MAT4 matWorld = Mat4_Multiply (matParent, node.transform);

      if (node.nMesh >= 0  &&  node.nMesh < static_cast<int> (out.model.aMesh.size ()))
      {
         // A node with nSkin poses its mesh in joint space. Rest-pose verts stay
         // in model space; GPU skinning applies aBonePalette. mWorld is only
         // matInstance (Y-up convert).
         Mesh_Emit (out, node.nMesh, matWorld, matInstance, node.nSkin);
      }

      for (int nChild : node.aChild)
         Node_Walk (out, nChild, matWorld, matInstance);
   }

   void TexCoord_FlipV_Unlit (DEP::GLTF_MODEL& model)
   {
      for (DEP::GLTF_MESH& mesh : model.aMesh)
      {
         for (DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
         {
            bool bUnlit = false;
            if (prim.nMaterial >= 0  &&  prim.nMaterial < static_cast<int> (model.aMaterial.size ()))
               bUnlit = model.aMaterial[static_cast<size_t> (prim.nMaterial)].bUnlit;
            if (bUnlit)
            {
               const size_t nUVCount = prim.aTexCoord.size () / 2;
               for (size_t i = 0; i < nUVCount; i++)
                  prim.aTexCoord[i * 2 + 1] = 1.0f - prim.aTexCoord[i * 2 + 1];
               const size_t nUV1Count = prim.aTexCoord1.size () / 2;
               for (size_t i = 0; i < nUV1Count; i++)
                  prim.aTexCoord1[i * 2 + 1] = 1.0f - prim.aTexCoord1[i * 2 + 1];
            }
         }
      }
   }

   bool Texture_Decoded (const GLTF_RENDER_MODEL& out, int nTex)
   {
      bool bDecoded = false;

      if (nTex >= 0  &&  nTex < static_cast<int> (out.aTexturePixel.size ())
       &&  out.aTextureWidth[nTex] > 0  &&  out.aTextureHeight[nTex] > 0
       &&  out.aTexturePixel[static_cast<size_t> (nTex)].size ()
             == static_cast<size_t> (out.aTextureWidth[nTex]) * static_cast<size_t> (out.aTextureHeight[nTex]) * 4)
         bDecoded = true;

      return bDecoded;
   }

   void Texture_DownscaleMax (int& nWidth, int& nHeight, std::vector<uint8_t>& aPixel, int nMax)
   {
      while ((nWidth > nMax  ||  nHeight > nMax)  &&  nWidth > 0  &&  nHeight > 0
          &&  aPixel.size () == static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4)
      {
         const int nW2 = (nWidth  > 1) ? (nWidth  / 2) : 1;
         const int nH2 = (nHeight > 1) ? (nHeight / 2) : 1;
         std::vector<uint8_t> aOut (static_cast<size_t> (nW2) * static_cast<size_t> (nH2) * 4);

         for (int nY = 0; nY < nH2; nY++)
         {
            const int nY0 = nY * 2;
            int       nY1 = nY0 + 1;
            if (nY1 >= nHeight)
               nY1 = nY0;

            for (int nX = 0; nX < nW2; nX++)
            {
               const int nX0 = nX * 2;
               int       nX1 = nX0 + 1;
               if (nX1 >= nWidth)
                  nX1 = nX0;

               const size_t n00 = (static_cast<size_t> (nY0) * static_cast<size_t> (nWidth) + static_cast<size_t> (nX0)) * 4;
               const size_t n10 = (static_cast<size_t> (nY0) * static_cast<size_t> (nWidth) + static_cast<size_t> (nX1)) * 4;
               const size_t n01 = (static_cast<size_t> (nY1) * static_cast<size_t> (nWidth) + static_cast<size_t> (nX0)) * 4;
               const size_t n11 = (static_cast<size_t> (nY1) * static_cast<size_t> (nWidth) + static_cast<size_t> (nX1)) * 4;
               const size_t nD  = (static_cast<size_t> (nY)  * static_cast<size_t> (nW2)    + static_cast<size_t> (nX))  * 4;

               for (int nC = 0; nC < 4; nC++)
               {
                  const unsigned nSum = static_cast<unsigned> (aPixel[n00 + nC]) + aPixel[n10 + nC] + aPixel[n01 + nC] + aPixel[n11 + nC];
                  aOut[nD + nC] = static_cast<uint8_t> (nSum / 4);
               }
            }
         }

         aPixel  = std::move (aOut);
         nWidth  = nW2;
         nHeight = nH2;
      }
   }

   // UniVRM often leaves cutout decals marked OPAQUE (eyeline, hair cards,
   // face overlays). 3ds Max / Sketchfab foliage and antenna cards often author
   // BLEND on a binary-alpha PNG. The PNG still carries a real alpha channel;
   // RGB in the discarded texels is typically black, which draws as solid
   // black (OPAQUE) or a ghostly overlay (BLEND with no depth write) when
   // Halogen does not mask. Promote both to MASK when the albedo is a hard
   // cutout. BLEND with a large mid-alpha band (glass, soft smoke) stays BLEND.
   void Alpha_PromoteFromTexture (GLTF_RENDER_MODEL& out)
   {
      for (size_t nMat = 0; nMat < out.model.aMaterial.size (); nMat++)
      {
         DEP::GLTF_MATERIAL& mat = out.model.aMaterial[nMat];
         if (mat.eAlpha == DEP::GLTF_MATERIAL::kOPAQUE
          ||  mat.eAlpha == DEP::GLTF_MATERIAL::kBLEND)
         {
            const int nTex = mat.nBaseColorTexture;
            if (nTex >= 0  &&  nTex < static_cast<int> (out.aTexturePixel.size ())
             &&  out.aTextureWidth[nTex] > 0  &&  out.aTextureHeight[nTex] > 0)
            {
               const std::vector<uint8_t>& aPixel = out.aTexturePixel[static_cast<size_t> (nTex)];
               const size_t nCount = aPixel.size () / 4;
               if (aPixel.size () == static_cast<size_t> (out.aTextureWidth[nTex]) * static_cast<size_t> (out.aTextureHeight[nTex]) * 4
                &&  nCount > 0)
               {
                  bool   bLow  = false;
                  bool   bHigh = false;
                  size_t nMid  = 0;
                  for (size_t nI = 0; nI < nCount; nI++)
                  {
                     const uint8_t nA = aPixel[nI * 4 + 3];
                     if (nA < 128)
                        bLow = true;
                     else if (nA >= 200)
                        bHigh = true;
                     else
                        nMid++;
                     if (bLow  &&  bHigh  &&  mat.eAlpha == DEP::GLTF_MATERIAL::kOPAQUE)
                        break;
                  }
                  if (bLow  &&  bHigh)
                  {
                     if (mat.eAlpha == DEP::GLTF_MATERIAL::kOPAQUE)
                        mat.eAlpha = DEP::GLTF_MATERIAL::kMASK;
                     else if (nMid * 10 < nCount)
                        mat.eAlpha = DEP::GLTF_MATERIAL::kMASK;
                  }
               }
            }
         }
      }
   }

   // Sketchfab / 3ds Max often author a second hull as BLEND with RGB near
   // white and alpha 0 on every texel (or baseColorFactor alpha 0). Filament
   // "transparent" blending expects premultiplied RGB, so that overlay adds
   // full lighting -- a white wash over the real albedo. Do not emit it.
   // Soft-alpha glass (factor or texels with alpha > 0) still draws.
   bool Blend_IsInvisible (const GLTF_RENDER_MODEL& out, int nMat)
   {
      bool bInvisible = false;

      if (nMat >= 0  &&  nMat < static_cast<int> (out.model.aMaterial.size ()))
      {
         const DEP::GLTF_MATERIAL& mat = out.model.aMaterial[static_cast<size_t> (nMat)];
         if (mat.eAlpha == DEP::GLTF_MATERIAL::kBLEND)
         {
            if (mat.baseColor[3] <= 0.0f)
               bInvisible = true;
            else
            {
               const uint8_t* pPixel = nullptr;
               size_t         nBytes = 0;
               if (static_cast<size_t> (nMat) < out.aMaterialPixel.size ()
                &&  !out.aMaterialPixel[static_cast<size_t> (nMat)].empty ())
               {
                  pPixel = out.aMaterialPixel[static_cast<size_t> (nMat)].data ();
                  nBytes = out.aMaterialPixel[static_cast<size_t> (nMat)].size ();
               }
               else if (Texture_Decoded (out, mat.nBaseColorTexture))
               {
                  pPixel = out.aTexturePixel[static_cast<size_t> (mat.nBaseColorTexture)].data ();
                  nBytes = out.aTexturePixel[static_cast<size_t> (mat.nBaseColorTexture)].size ();
               }

               if (pPixel  &&  nBytes >= 4)
               {
                  bool bAny = false;
                  for (size_t nI = 3; nI < nBytes; nI += 4)
                  {
                     if (pPixel[nI] != 0)
                     {
                        bAny = true;
                        break;
                     }
                  }
                  if (!bAny)
                     bInvisible = true;
               }
            }
         }
      }

      return bInvisible;
   }

   void Node_Compose (DEP::GLTF_NODE& node)
   {
      VEC3D vT = { node.aTranslation[0], node.aTranslation[1], node.aTranslation[2] };
      QUATD qR = { node.aRotation[0], node.aRotation[1], node.aRotation[2], node.aRotation[3] };
      VEC3D vS = { node.aScale[0], node.aScale[1], node.aScale[2] };
      node.transform = Mat4_FromTRS (vT, qR, vS);
   }

   void Rest_FromTrs (const std::vector<DEP::GLTF_NODE>& aNode, std::vector<MAT4>& aRest)
   {
      aRest.resize (aNode.size ());
      for (size_t nI = 0; nI < aNode.size (); nI++)
      {
         const DEP::GLTF_NODE& node = aNode[nI];
         VEC3D vT = { node.aTranslation[0], node.aTranslation[1], node.aTranslation[2] };
         QUATD qR = { node.aRotation[0], node.aRotation[1], node.aRotation[2], node.aRotation[3] };
         VEC3D vS = { node.aScale[0], node.aScale[1], node.aScale[2] };
         aRest[nI] = Mat4_FromTRS (vT, qR, vS);
      }
   }

   void Pose_Reset (const DEP::GLTF_MODEL& model, const std::vector<MAT4>& aRest, std::vector<DEP::GLTF_NODE>& aNode)
   {
      if (aNode.size () != model.aNode.size ())
         aNode = model.aNode;
      else
      {
         for (size_t nI = 0; nI < aNode.size (); nI++)
         {
            std::memcpy (aNode[nI].aTranslation, model.aNode[nI].aTranslation, sizeof (aNode[nI].aTranslation));
            std::memcpy (aNode[nI].aRotation,    model.aNode[nI].aRotation,    sizeof (aNode[nI].aRotation));
            std::memcpy (aNode[nI].aScale,       model.aNode[nI].aScale,       sizeof (aNode[nI].aScale));
         }
      }

      if (aRest.size () == aNode.size ())
      {
         for (size_t nI = 0; nI < aNode.size (); nI++)
            aNode[nI].transform = aRest[nI];
      }
      else
      {
         for (DEP::GLTF_NODE& node : aNode)
            Node_Compose (node);
      }
   }

   void Channel_Span (const std::vector<float>& aTime, double dTime, size_t& n0, size_t& n1, double& dU, double& dDt)
   {
      n0 = 0;
      n1 = 0;
      dU = 0.0;
      dDt = 0.0;

      if (aTime.size () == 1)
      {
         n0 = 0;
         n1 = 0;
      }
      else if (!aTime.empty ())
      {
         if (dTime <= aTime[0])
         {
            n0 = 0;
            n1 = 0;
         }
         else if (dTime >= aTime.back ())
         {
            n0 = aTime.size () - 1;
            n1 = n0;
         }
         else
         {
            while (n0 + 1 < aTime.size ()  &&  aTime[n0 + 1] < dTime)
               n0++;
            n1 = n0 + 1;
            dDt = aTime[n1] - aTime[n0];
            if (dDt > 1.0e-12)
               dU = (dTime - aTime[n0]) / dDt;
         }
      }
   }

   void Channel_Read (const DEP::GLTF_CHANNEL& channel, size_t nKey, int nStride, int nSlot, int nComp, double* aOut)
   {
      const size_t nBase = nKey * static_cast<size_t> (nStride) + static_cast<size_t> (nSlot) * static_cast<size_t> (nComp);
      for (int nI = 0; nI < nComp; nI++)
      {
         double dV = 0.0;
         const size_t nIx = nBase + static_cast<size_t> (nI);
         if (nIx < channel.aValue.size ())
            dV = channel.aValue[nIx];
         aOut[nI] = dV;
      }
   }

   void Channel_Cubic (const DEP::GLTF_CHANNEL& channel, size_t n0, size_t n1, double dU, double dDt, int nComp, double* aOut)
   {
      double aV0[4] = {};
      double aB0[4] = {};
      double aV1[4] = {};
      double aA1[4] = {};
      Channel_Read (channel, n0, nComp * 3, 1, nComp, aV0);
      Channel_Read (channel, n0, nComp * 3, 2, nComp, aB0);
      Channel_Read (channel, n1, nComp * 3, 1, nComp, aV1);
      Channel_Read (channel, n1, nComp * 3, 0, nComp, aA1);

      const double dT2 = dU * dU;
      const double dT3 = dT2 * dU;
      const double dC0 =  2.0 * dT3 - 3.0 * dT2 + 1.0;
      const double dC1 =      dT3 - 2.0 * dT2 + dU;
      const double dC2 = -2.0 * dT3 + 3.0 * dT2;
      const double dC3 =      dT3 -       dT2;

      for (int nI = 0; nI < nComp; nI++)
         aOut[nI] = dC0 * aV0[nI] + dC1 * dDt * aB0[nI] + dC2 * aV1[nI] + dC3 * dDt * aA1[nI];
   }

   void Channel_Sample (const DEP::GLTF_CHANNEL& channel, double dTime, int nComp, double* aOut)
   {
      size_t n0 = 0;
      size_t n1 = 0;
      double dU = 0.0;
      double dDt = 0.0;
      Channel_Span (channel.aTime, dTime, n0, n1, dU, dDt);

      const int nStride = (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC) ? nComp * 3 : nComp;
      const int nSlot   = (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC) ? 1 : 0;

      if (n0 == n1  ||  channel.eInterp == DEP::GLTF_CHANNEL::kSTEP)
         Channel_Read (channel, n0, nStride, nSlot, nComp, aOut);
      else if (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC)
         Channel_Cubic (channel, n0, n1, dU, dDt, nComp, aOut);
      else
      {
         double aA[4] = {};
         double aB[4] = {};
         Channel_Read (channel, n0, nStride, nSlot, nComp, aA);
         Channel_Read (channel, n1, nStride, nSlot, nComp, aB);
         if (nComp == 4)
         {
            QUATD qA = { aA[0], aA[1], aA[2], aA[3] };
            QUATD qB = { aB[0], aB[1], aB[2], aB[3] };
            QUATD qR = Quat_Slerp (qA, qB, dU);
            aOut[0] = qR.dX;
            aOut[1] = qR.dY;
            aOut[2] = qR.dZ;
            aOut[3] = qR.dW;
         }
         else
         {
            for (int nI = 0; nI < nComp; nI++)
               aOut[nI] = aA[nI] + (aB[nI] - aA[nI]) * dU;
         }
      }

      if (nComp == 4)
      {
         QUATD qR = { aOut[0], aOut[1], aOut[2], aOut[3] };
         qR = Quat_Normalize (qR);
         aOut[0] = qR.dX;
         aOut[1] = qR.dY;
         aOut[2] = qR.dZ;
         aOut[3] = qR.dW;
      }
   }

   void Animation_Apply (const DEP::GLTF_ANIMATION& anim, double dTime, std::vector<DEP::GLTF_NODE>& aNode)
   {
      for (const DEP::GLTF_CHANNEL& channel : anim.aChannel)
      {
         if (channel.nNode < 0  ||  channel.nNode >= static_cast<int> (aNode.size ()))
            continue;

         DEP::GLTF_NODE& node = aNode[static_cast<size_t> (channel.nNode)];
         if (channel.ePath == DEP::GLTF_CHANNEL::kROTATION)
         {
            double aV[4] = {};
            Channel_Sample (channel, dTime, 4, aV);
            node.aRotation[0] = aV[0];
            node.aRotation[1] = aV[1];
            node.aRotation[2] = aV[2];
            node.aRotation[3] = aV[3];
         }
         else
         {
            double aV[4] = {};
            Channel_Sample (channel, dTime, 3, aV);
            if (channel.ePath == DEP::GLTF_CHANNEL::kSCALE)
            {
               node.aScale[0] = aV[0];
               node.aScale[1] = aV[1];
               node.aScale[2] = aV[2];
            }
            else
            {
               node.aTranslation[0] = aV[0];
               node.aTranslation[1] = aV[1];
               node.aTranslation[2] = aV[2];
            }
         }
         Node_Compose (node);
      }
   }

   int Humanoid_Node (const DEP::GLTF_MODEL& model, const std::string& sName)
   {
      int nNode = -1;
      for (const DEP::GLTF_HUMANOID& bone : model.aHumanoid)
      {
         if (bone.sName == sName)
         {
            nNode = bone.nNode;
            break;
         }
      }
      return nNode;
   }

   std::string Humanoid_Name (const DEP::GLTF_MODEL& model, int nNode)
   {
      std::string sName;
      for (const DEP::GLTF_HUMANOID& bone : model.aHumanoid)
      {
         if (bone.nNode == nNode)
         {
            sName = bone.sName;
            break;
         }
      }
      return sName;
   }

   QUATD Quat_Local (const DEP::GLTF_NODE& node)
   {
      QUATD q = { node.aRotation[0], node.aRotation[1], node.aRotation[2], node.aRotation[3] };
      return Quat_Normalize (q);
   }

   void Rest_World (const DEP::GLTF_MODEL& model, std::vector<DEP::GLTF_NODE>& aNode, std::vector<MAT4>& aGlobal, std::vector<int>& aParent)
   {
      aNode = model.aNode;
      for (DEP::GLTF_NODE& node : aNode)
         Node_Compose (node);
      Node_Parents (aNode, aParent);
      Node_Globals (aNode, aGlobal);
   }

   // VRMC_vrm_animation pose compatibility: NormalizedLocalRotation =
   // W * L^-1 * A.local * W^-1, then dest local = L * W^-1 * N * W.
   QUATD Retarget_Rotation (QUATD qA, QUATD qLsrc, QUATD qWsrc, QUATD qLdst, QUATD qWdst)
   {
      QUATD qN = Quat_Mul (Quat_Mul (Quat_Mul (qWsrc, Quat_Conjugate (qLsrc)), qA), Quat_Conjugate (qWsrc));
      QUATD qB = Quat_Mul (Quat_Mul (Quat_Mul (qLdst, Quat_Conjugate (qWdst)), qN), qWdst);
      return Quat_Normalize (qB);
   }

   VEC3D Retarget_HipsT (VEC3D vLocal, const MAT4& matParentSrc, const MAT4& matParentDstInv, double dScale)
   {
      VEC3D vWorld = Mat4_MulPoint (matParentSrc, vLocal);
      vWorld.dX *= dScale;
      vWorld.dY *= dScale;
      vWorld.dZ *= dScale;
      return Mat4_MulPoint (matParentDstInv, vWorld);
   }

   void Channel_RetargetRotation (DEP::GLTF_CHANNEL& channel, QUATD qLsrc, QUATD qWsrc, QUATD qLdst, QUATD qWdst)
   {
      const int nComp   = 4;
      const int nStride = (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC) ? nComp * 3 : nComp;
      const int nSlot   = (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC) ? 3 : 1;
      for (size_t nKey = 0; nKey < channel.aTime.size (); nKey++)
      {
         for (int nI = 0; nI < nSlot; nI++)
         {
            const size_t nBase = nKey * static_cast<size_t> (nStride) + static_cast<size_t> (nI) * static_cast<size_t> (nComp);
            if (nBase + 3 < channel.aValue.size ())
            {
               QUATD qA = { channel.aValue[nBase], channel.aValue[nBase + 1], channel.aValue[nBase + 2], channel.aValue[nBase + 3] };
               QUATD qB = Retarget_Rotation (qA, qLsrc, qWsrc, qLdst, qWdst);
               channel.aValue[nBase]     = static_cast<float> (qB.dX);
               channel.aValue[nBase + 1] = static_cast<float> (qB.dY);
               channel.aValue[nBase + 2] = static_cast<float> (qB.dZ);
               channel.aValue[nBase + 3] = static_cast<float> (qB.dW);
            }
         }
      }
   }

   void Channel_RetargetHipsT (DEP::GLTF_CHANNEL& channel, const MAT4& matParentSrc, const MAT4& matParentDstInv, double dScale)
   {
      const int nComp   = 3;
      const int nStride = (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC) ? nComp * 3 : nComp;
      const int nSlot   = (channel.eInterp == DEP::GLTF_CHANNEL::kCUBIC) ? 3 : 1;
      for (size_t nKey = 0; nKey < channel.aTime.size (); nKey++)
      {
         for (int nI = 0; nI < nSlot; nI++)
         {
            const size_t nBase = nKey * static_cast<size_t> (nStride) + static_cast<size_t> (nI) * static_cast<size_t> (nComp);
            if (nBase + 2 < channel.aValue.size ())
            {
               VEC3D vA = { channel.aValue[nBase], channel.aValue[nBase + 1], channel.aValue[nBase + 2] };
               VEC3D vB = Retarget_HipsT (vA, matParentSrc, matParentDstInv, dScale);
               channel.aValue[nBase]     = static_cast<float> (vB.dX);
               channel.aValue[nBase + 1] = static_cast<float> (vB.dY);
               channel.aValue[nBase + 2] = static_cast<float> (vB.dZ);
            }
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

   std::vector<char> abNeed (nTexture, 0);
   for (const DEP::GLTF_MATERIAL& mat : out.model.aMaterial)
   {
      if (mat.nBaseColorTexture >= 0  &&  mat.nBaseColorTexture < static_cast<int> (nTexture))
         abNeed[static_cast<size_t> (mat.nBaseColorTexture)] = 1;
      if (mat.nEmissiveTexture >= 0  &&  mat.nEmissiveTexture < static_cast<int> (nTexture))
         abNeed[static_cast<size_t> (mat.nEmissiveTexture)] = 1;
      if (mat.nMetallicRoughnessTexture >= 0  &&  mat.nMetallicRoughnessTexture < static_cast<int> (nTexture))
         abNeed[static_cast<size_t> (mat.nMetallicRoughnessTexture)] = 1;
      if (mat.nNormalTexture >= 0  &&  mat.nNormalTexture < static_cast<int> (nTexture))
         abNeed[static_cast<size_t> (mat.nNormalTexture)] = 1;
      if (mat.nOcclusionTexture >= 0  &&  mat.nOcclusionTexture < static_cast<int> (nTexture))
         abNeed[static_cast<size_t> (mat.nOcclusionTexture)] = 1;
   }
   for (size_t i = 0; i < nTexture; i++)
   {
      if (abNeed[i])
      {
         IMAGE::Decode (out.model.aTexture[i].aEncoded, out.aTextureWidth[i], out.aTextureHeight[i], out.aTexturePixel[i]);
         Texture_DownscaleMax (out.aTextureWidth[i], out.aTextureHeight[i], out.aTexturePixel[i], 1024);
      }
   }

   Alpha_PromoteFromTexture (out);

   // physicallyBased.mat is compiled with flipUV false (gltfio). Unlit keeps
   // Filament's default flip, so only unlit primitives need the CPU V-flip.
   TexCoord_FlipV_Unlit (out.model);

   // Concatenate same-material primitives within each mesh before emit so
   // kit-style glTFs become one ANARI surface per material. Must run on the
   // CPU model (not the flattened draw list) so two nodes that instance the
   // same rigid mesh still share vertex pointers. Skinned same-material
   // primitives on different meshes are concatenated after emit.
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

   Constraint_Apply (out.model);
   Rest_FromTrs (out.model.aNode, out.aRest);

   std::vector<MAT4> aGlobal;
   Node_Globals (out.model.aNode, aGlobal);

   out.aBonePalette.resize (out.model.aSkin.size ());
   for (size_t nSkin = 0; nSkin < out.model.aSkin.size (); nSkin++)
   {
      std::vector<MAT4> aPalette;
      Skin_Palette (out.model.aSkin[nSkin], aGlobal, aPalette);
      Palette_Pack (aPalette, out.aBonePalette[nSkin]);
   }

   for (int nRoot : out.model.aRoot)
      Node_Walk (out, nRoot, matRoot, matRoot);

   Mesh_MergeSkinnedDraws (out);
   Bounds_Compute (out);

   return !out.aMesh.empty ();
}

bool SNEEZE::Gltf_Render_Model_Pose (const GLTF_RENDER_MODEL& render, uint32_t nClip, double dTime, std::vector<std::vector<float>>& aPalette)
{
   bool bResult = false;

   const DEP::GLTF_MODEL& model = render.model;
   if (nClip < model.aAnimation.size ())
      bResult = Gltf_Render_Model_Pose (render, model.aAnimation[nClip], dTime, aPalette);

   return bResult;
}

bool SNEEZE::Gltf_Render_Model_Pose (const GLTF_RENDER_MODEL& render, const DEP::GLTF_ANIMATION& anim, double dTime, std::vector<std::vector<float>>& aPalette)
{
   std::vector<DEP::GLTF_NODE> aNode;
   return Gltf_Render_Model_Pose (render, anim, dTime, aNode, aPalette);
}

bool SNEEZE::Gltf_Render_Model_Pose (const GLTF_RENDER_MODEL& render, const DEP::GLTF_ANIMATION& anim, double dTime, std::vector<DEP::GLTF_NODE>& aNode, std::vector<std::vector<float>>& aPalette)
{
   bool bResult = false;

   const DEP::GLTF_MODEL& model = render.model;
   if (!model.aSkin.empty ())
   {
      Pose_Reset (model, render.aRest, aNode);

      const std::vector<MAT4>* pRest = &render.aRest;
      std::vector<MAT4> aRestLocal;
      if (pRest->size () != aNode.size ())
      {
         Rest_FromTrs (aNode, aRestLocal);
         pRest = &aRestLocal;
      }

      Animation_Apply (anim, dTime, aNode);
      Constraint_ApplyNodes (aNode, model.aConstraint, *pRest);

      std::vector<MAT4> aGlobal;
      Node_Globals (aNode, aGlobal);

      aPalette.resize (model.aSkin.size ());
      for (size_t nSkin = 0; nSkin < model.aSkin.size (); nSkin++)
      {
         std::vector<MAT4> aPacked;
         Skin_Palette (model.aSkin[nSkin], aGlobal, aPacked);
         Palette_Pack (aPacked, aPalette[nSkin]);
      }
      bResult = true;
   }

   return bResult;
}

bool SNEEZE::Gltf_Vrma_Retarget (const DEP::GLTF_MODEL& modelSrc, const DEP::GLTF_MODEL& modelDst, DEP::GLTF_ANIMATION& animOut)
{
   bool bResult = false;

   animOut = DEP::GLTF_ANIMATION ();

   if (!modelSrc.aAnimation.empty ()  &&  !modelSrc.aHumanoid.empty ()  &&  !modelDst.aHumanoid.empty ())
   {
      const DEP::GLTF_ANIMATION& animSrc = modelSrc.aAnimation[0];
      std::vector<DEP::GLTF_NODE> aNodeSrc;
      std::vector<DEP::GLTF_NODE> aNodeDst;
      std::vector<MAT4> aGlobalSrc;
      std::vector<MAT4> aGlobalDst;
      std::vector<int>  aParentSrc;
      std::vector<int>  aParentDst;
      Rest_World (modelSrc, aNodeSrc, aGlobalSrc, aParentSrc);
      Rest_World (modelDst, aNodeDst, aGlobalDst, aParentDst);

      const int nHipsSrc = Humanoid_Node (modelSrc, "hips");
      const int nHipsDst = Humanoid_Node (modelDst, "hips");
      double dScale = 1.0;
      if (nHipsSrc >= 0  &&  nHipsSrc < static_cast<int> (aGlobalSrc.size ())
       &&  nHipsDst >= 0  &&  nHipsDst < static_cast<int> (aGlobalDst.size ()))
      {
         const double dYsrc = aGlobalSrc[static_cast<size_t> (nHipsSrc)].d[13];
         const double dYdst = aGlobalDst[static_cast<size_t> (nHipsDst)].d[13];
         if (std::fabs (dYsrc) > 1.0e-3)
            dScale = dYdst / dYsrc;
      }

      MAT4 matHipsParentSrc    = Mat4_Identity ();
      MAT4 matHipsParentDstInv = Mat4_Identity ();
      if (nHipsSrc >= 0  &&  nHipsSrc < static_cast<int> (aParentSrc.size ()))
      {
         const int nParent = aParentSrc[static_cast<size_t> (nHipsSrc)];
         if (nParent >= 0  &&  nParent < static_cast<int> (aGlobalSrc.size ()))
            matHipsParentSrc = aGlobalSrc[static_cast<size_t> (nParent)];
      }
      if (nHipsDst >= 0  &&  nHipsDst < static_cast<int> (aParentDst.size ()))
      {
         const int nParent = aParentDst[static_cast<size_t> (nHipsDst)];
         if (nParent >= 0  &&  nParent < static_cast<int> (aGlobalDst.size ()))
            Mat4_Inverse (aGlobalDst[static_cast<size_t> (nParent)], matHipsParentDstInv);
      }

      animOut.sName     = animSrc.sName;
      animOut.dDuration = animSrc.dDuration;

      const int nNodeSrc = static_cast<int> (aNodeSrc.size ());
      const int nNodeDst = static_cast<int> (aNodeDst.size ());

      for (const DEP::GLTF_CHANNEL& channelSrc : animSrc.aChannel)
      {
         if (channelSrc.nNode < 0  ||  channelSrc.nNode >= nNodeSrc)
            continue;
         if (channelSrc.ePath == DEP::GLTF_CHANNEL::kSCALE)
            continue;

         const std::string sBone = Humanoid_Name (modelSrc, channelSrc.nNode);
         if (sBone.empty ()  ||  sBone == "leftEye"  ||  sBone == "rightEye")
            continue;

         const int nDst = Humanoid_Node (modelDst, sBone);
         if (nDst < 0  ||  nDst >= nNodeDst)
            continue;

         if (channelSrc.ePath == DEP::GLTF_CHANNEL::kTRANSLATION  &&  sBone != "hips")
            continue;

         DEP::GLTF_CHANNEL channel = channelSrc;
         channel.nNode = nDst;

         if (channel.ePath == DEP::GLTF_CHANNEL::kROTATION)
         {
            const QUATD qLsrc = Quat_Local (aNodeSrc[static_cast<size_t> (channelSrc.nNode)]);
            const QUATD qWsrc = Quat_FromMatrix (aGlobalSrc[static_cast<size_t> (channelSrc.nNode)]);
            const QUATD qLdst = Quat_Local (aNodeDst[static_cast<size_t> (nDst)]);
            const QUATD qWdst = Quat_FromMatrix (aGlobalDst[static_cast<size_t> (nDst)]);
            Channel_RetargetRotation (channel, qLsrc, qWsrc, qLdst, qWdst);
         }
         else
            Channel_RetargetHipsT (channel, matHipsParentSrc, matHipsParentDstInv, dScale);

         animOut.aChannel.push_back (std::move (channel));
      }

      bResult = !animOut.aChannel.empty ();
      if (!bResult)
         animOut = DEP::GLTF_ANIMATION ();
   }

   return bResult;
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
