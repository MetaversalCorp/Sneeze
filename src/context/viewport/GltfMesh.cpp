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

   void Node_Parents (const DEP::GLTF_MODEL& model, std::vector<int>& aParent)
   {
      const int nNode = static_cast<int> (model.aNode.size ());
      aParent.assign (static_cast<size_t> (nNode), -1);
      for (int nI = 0; nI < nNode; nI++)
      {
         for (int nChild : model.aNode[static_cast<size_t> (nI)].aChild)
         {
            if (nChild >= 0  &&  nChild < nNode)
               aParent[static_cast<size_t> (nChild)] = nI;
         }
      }
   }

   void Node_Globals (const DEP::GLTF_MODEL& model, std::vector<MAT4>& aGlobal)
   {
      const int nNode = static_cast<int> (model.aNode.size ());
      std::vector<int> aParent;
      Node_Parents (model, aParent);

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

   void Node_SetRotation (DEP::GLTF_NODE& node, QUATD qR)
   {
      VEC3D vT;
      QUATD qOld;
      VEC3D vS;
      Mat4_Decompose (node.transform, vT, qOld, vS);
      node.transform = Mat4_FromTRS (vT, qR, vS);
   }

   void Constraint_ApplyOne (DEP::GLTF_MODEL& model, const DEP::GLTF_CONSTRAINT& constraint, const std::vector<MAT4>& aRest, const std::vector<MAT4>& aGlobal, const std::vector<int>& aParent)
   {
      const int nDest   = constraint.nNode;
      const int nSource = constraint.nSource;
      const int nNode   = static_cast<int> (model.aNode.size ());

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
      Mat4_Decompose (model.aNode[static_cast<size_t> (nSource)].transform, vSrcT, qSrcNow, vSrcS);

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
      Node_SetRotation (model.aNode[static_cast<size_t> (nDest)], qOut);
      }
   }

   void Constraint_Apply (DEP::GLTF_MODEL& model)
   {
      if (!model.aConstraint.empty ())
      {
         std::vector<MAT4> aRest;
         aRest.reserve (model.aNode.size ());
         for (const DEP::GLTF_NODE& node : model.aNode)
            aRest.push_back (node.transform);

         std::vector<int> aParent;
         Node_Parents (model, aParent);

         int nPass = static_cast<int> (model.aConstraint.size ()) + 1;
         if (nPass > 16)
            nPass = 16;

         for (int nI = 0; nI < nPass; nI++)
         {
            std::vector<MAT4> aGlobal;
            Node_Globals (model, aGlobal);
            for (const DEP::GLTF_CONSTRAINT& constraint : model.aConstraint)
               Constraint_ApplyOne (model, constraint, aRest, aGlobal, aParent);
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
            data.bUnlit           = mat.bUnlit;

            int nTex = mat.nBaseColorTexture;
            if (nTex >= 0  &&  nTex < static_cast<int> (out.aTexturePixel.size ())  &&  out.aTextureWidth[nTex] > 0  &&  out.aTextureHeight[nTex] > 0)
            {
               const int nMat = prim.nMaterial;
               if (nMat >= 0  &&  nMat < static_cast<int> (out.aMaterialPixel.size ())  &&  !out.aMaterialPixel[static_cast<size_t> (nMat)].empty ())
                  data.pbTexturePixels = out.aMaterialPixel[static_cast<size_t> (nMat)].data ();
               else
                  data.pbTexturePixels = out.aTexturePixel[nTex].data ();
               data.dimTexture.nW = out.aTextureWidth[nTex];
               data.dimTexture.nH = out.aTextureHeight[nTex];
               data.rgbaBaseColor.fR = 1.0f;
               data.rgbaBaseColor.fG = 1.0f;
               data.rgbaBaseColor.fB = 1.0f;
               data.rgbaBaseColor.fA = 1.0f;
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

   float Srgb_ToLinear (float f)
   {
      float fOut = 0.0f;
      if (f <= 0.04045f)
         fOut = f / 12.92f;
      else
         fOut = std::pow ((f + 0.055f) / 1.055f, 2.4f);
      return fOut;
   }

   uint8_t Linear_ToU8 (float f)
   {
      if (f < 0.0f)
         f = 0.0f;
      if (f > 1.0f)
         f = 1.0f;
      return static_cast<uint8_t> (f * 255.0f + 0.5f);
   }

   bool Factor_IsWhite (const float aFactor[4])
   {
      bool bWhite = true;
      for (int nI = 0; nI < 4; nI++)
      {
         if (std::fabs (aFactor[nI] - 1.0f) > 1.0e-5f)
            bWhite = false;
      }
      return bWhite;
   }

   void Texture_SrgbToLinear (std::vector<uint8_t>& aPixel)
   {
      const size_t nCount = aPixel.size () / 4;
      for (size_t nI = 0; nI < nCount; nI++)
      {
         float fR = Srgb_ToLinear (aPixel[nI * 4 + 0] / 255.0f);
         float fG = Srgb_ToLinear (aPixel[nI * 4 + 1] / 255.0f);
         float fB = Srgb_ToLinear (aPixel[nI * 4 + 2] / 255.0f);
         aPixel[nI * 4 + 0] = Linear_ToU8 (fR);
         aPixel[nI * 4 + 1] = Linear_ToU8 (fG);
         aPixel[nI * 4 + 2] = Linear_ToU8 (fB);
      }
   }

   void Texture_TintLinear (std::vector<uint8_t>& aPixel, const float aFactor[4])
   {
      const size_t nCount = aPixel.size () / 4;
      for (size_t nI = 0; nI < nCount; nI++)
      {
         float fR = (aPixel[nI * 4 + 0] / 255.0f) * aFactor[0];
         float fG = (aPixel[nI * 4 + 1] / 255.0f) * aFactor[1];
         float fB = (aPixel[nI * 4 + 2] / 255.0f) * aFactor[2];
         float fA = (aPixel[nI * 4 + 3] / 255.0f) * aFactor[3];
         aPixel[nI * 4 + 0] = Linear_ToU8 (fR);
         aPixel[nI * 4 + 1] = Linear_ToU8 (fG);
         aPixel[nI * 4 + 2] = Linear_ToU8 (fB);
         aPixel[nI * 4 + 3] = Linear_ToU8 (fA);
      }
   }

   void Albedo_Prepare (GLTF_RENDER_MODEL& out)
   {
      const size_t nTexture = out.aTexturePixel.size ();
      std::vector<uint8_t> aLinear (nTexture, 0);

      out.aMaterialPixel.assign (out.model.aMaterial.size (), std::vector<uint8_t> ());

      for (size_t nMat = 0; nMat < out.model.aMaterial.size (); nMat++)
      {
         const DEP::GLTF_MATERIAL& mat = out.model.aMaterial[nMat];
         const int nTex = mat.nBaseColorTexture;
         if (nTex >= 0  &&  nTex < static_cast<int> (nTexture)
          &&  out.aTextureWidth[nTex] > 0  &&  out.aTextureHeight[nTex] > 0
          &&  out.aTexturePixel[static_cast<size_t> (nTex)].size ()
                == static_cast<size_t> (out.aTextureWidth[nTex]) * static_cast<size_t> (out.aTextureHeight[nTex]) * 4)
         {
            if (aLinear[static_cast<size_t> (nTex)] == 0)
            {
               Texture_SrgbToLinear (out.aTexturePixel[static_cast<size_t> (nTex)]);
               aLinear[static_cast<size_t> (nTex)] = 1;
            }

            if (!Factor_IsWhite (mat.baseColor))
            {
               out.aMaterialPixel[nMat] = out.aTexturePixel[static_cast<size_t> (nTex)];
               Texture_TintLinear (out.aMaterialPixel[nMat], mat.baseColor);
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

   for (size_t i = 0; i < nTexture; i++)
      IMAGE::Decode (out.model.aTexture[i].aEncoded, out.aTextureWidth[i], out.aTextureHeight[i], out.aTexturePixel[i]);

   // Halogen's image2D sampler uploads UFIXED8 as Filament RGBA8 (linear).
   // Decode PNG/JPEG as sRGB, convert RGB to linear, and bake baseColorFactor
   // into a per-material copy when the factor is not white.
   Albedo_Prepare (out);

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

   Constraint_Apply (out.model);

   std::vector<MAT4> aGlobal;
   Node_Globals (out.model, aGlobal);

   out.aBonePalette.resize (out.model.aSkin.size ());
   for (size_t nSkin = 0; nSkin < out.model.aSkin.size (); nSkin++)
   {
      std::vector<MAT4> aPalette;
      Skin_Palette (out.model.aSkin[nSkin], aGlobal, aPalette);
      Palette_Pack (aPalette, out.aBonePalette[nSkin]);
   }

   for (int nRoot : out.model.aRoot)
      Node_Walk (out, nRoot, matRoot, matRoot);

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
