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
#include "gltf/Gltf.h"
#include "context/viewport/Viewport.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifndef SNEEZE_TEST_DATA_DIR
#define SNEEZE_TEST_DATA_DIR "tests/data"
#endif

static int nPassed = 0;
static int nFailed = 0;

static void Check (bool bCondition, const char* szName)
{
   if (bCondition)
   {
      std::printf ("  PASS: %s\n", szName);
      nPassed++;
   }
   else
   {
      std::printf ("  FAIL: %s\n", szName);
      nFailed++;
   }
}

static bool ReadFile (const std::string& sPath, std::vector<uint8_t>& aBytes)
{
   bool bResult = false;

   std::ifstream file (sPath, std::ios::binary | std::ios::ate);
   if (file.is_open ())
   {
      std::streamsize nSize = file.tellg ();
      file.seekg (0, std::ios::beg);

      aBytes.resize (static_cast<size_t> (nSize));
      if (nSize > 0  &&  file.read (reinterpret_cast<char*> (aBytes.data ()), nSize))
         bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Test 1: Reject empty input
// ---------------------------------------------------------------------------
static void TestEmptyInput ()
{
   std::printf ("\n[Test 1] Reject empty input\n");

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;

   bool bOk = SNEEZE::DEP::GLTF::Load (nullptr, 0, model, sError);
   Check (!bOk, "Empty data rejected");
   Check (!sError.empty (), "Error string populated");
}

// ---------------------------------------------------------------------------
// Test 2: Reject garbage bytes
// ---------------------------------------------------------------------------
static void TestGarbageInput ()
{
   std::printf ("\n[Test 2] Reject garbage bytes\n");

   std::vector<uint8_t> aGarbage = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, };

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;

   bool bOk = SNEEZE::DEP::GLTF::Load (aGarbage.data (), aGarbage.size (), model, sError);
   Check (!bOk, "Garbage data rejected");
}

// ---------------------------------------------------------------------------
// Test 3: Load a real GLB and validate the mapped model
// ---------------------------------------------------------------------------
static void TestLoadGlb ()
{
   std::printf ("\n[Test 3] Load a real GLB\n");

   std::string sPath = std::string (SNEEZE_TEST_DATA_DIR) + "/model.glb";

   std::vector<uint8_t> aBytes;
   bool bRead = ReadFile (sPath, aBytes);
   Check (bRead, "Sample GLB read from disk");
   if (!bRead)
   {
      std::printf ("    (expected at %s)\n", sPath.c_str ());
      return;
   }

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;

   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "GLB parsed and mapped");
   if (!bOk)
   {
      std::printf ("    error: %s\n", sError.c_str ());
      return;
   }

   std::printf ("    meshes=%zu materials=%zu textures=%zu nodes=%zu roots=%zu\n",
      model.aMesh.size (), model.aMaterial.size (), model.aTexture.size (),
      model.aNode.size (), model.aRoot.size ());

   Check (!model.aMesh.empty (), "Model has at least one mesh");
   Check (!model.aNode.empty (), "Model has at least one node");
   Check (!model.aRoot.empty (), "Default scene has at least one root node");

   size_t nPrimitives = 0;
   size_t nPositions  = 0;
   size_t nIndices    = 0;
   bool   bPosTriples = true;
   bool   bUvPairs    = true;
   bool   bNormalsMatchPositions = true;

   for (const SNEEZE::DEP::GLTF_MESH& mesh : model.aMesh)
   {
      for (const SNEEZE::DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
      {
         nPrimitives++;
         nPositions += prim.aPosition.size () / 3;
         nIndices   += prim.aIndex.size ();

         if (prim.aPosition.size () % 3 != 0)
            bPosTriples = false;
         if (prim.aTexCoord.size () % 2 != 0)
            bUvPairs = false;
         if (!prim.aNormal.empty ()  &&  prim.aNormal.size () != prim.aPosition.size ())
            bNormalsMatchPositions = false;
      }
   }

   std::printf ("    primitives=%zu vertices=%zu indices=%zu\n", nPrimitives, nPositions, nIndices);

   Check (nPrimitives > 0, "Model has at least one primitive");
   Check (nPositions > 0, "Primitives carry vertex positions");
   Check (bPosTriples, "Position stream is a multiple of 3");
   Check (bUvPairs, "TexCoord stream is a multiple of 2");
   Check (bNormalsMatchPositions, "Normal stream length matches position stream");

   bool bMaterialRefsValid = true;
   bool bTextureRefsValid  = true;
   for (const SNEEZE::DEP::GLTF_MESH& mesh : model.aMesh)
      for (const SNEEZE::DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
         if (prim.nMaterial >= static_cast<int> (model.aMaterial.size ()))
            bMaterialRefsValid = false;

   for (const SNEEZE::DEP::GLTF_MATERIAL& material : model.aMaterial)
      if (material.nBaseColorTexture >= static_cast<int> (model.aTexture.size ()))
         bTextureRefsValid = false;

   Check (bMaterialRefsValid, "Primitive material indices in range");
   Check (bTextureRefsValid, "Material texture indices in range");

   bool bMeshRefsValid  = true;
   bool bChildRefsValid = true;
   for (const SNEEZE::DEP::GLTF_NODE& node : model.aNode)
   {
      if (node.nMesh >= static_cast<int> (model.aMesh.size ()))
         bMeshRefsValid = false;
      for (int nChild : node.aChild)
         if (nChild < 0  ||  nChild >= static_cast<int> (model.aNode.size ()))
            bChildRefsValid = false;
   }

   Check (bMeshRefsValid, "Node mesh indices in range");
   Check (bChildRefsValid, "Node child indices in range");

   bool bSkinRefsValid = true;
   for (const SNEEZE::DEP::GLTF_NODE& node : model.aNode)
      if (node.nSkin >= static_cast<int> (model.aSkin.size ()))
         bSkinRefsValid = false;
   for (const SNEEZE::DEP::GLTF_SKIN& skin : model.aSkin)
   {
      if (skin.aInverseBind.size () != skin.aJoint.size ())
         bSkinRefsValid = false;
      for (int nJoint : skin.aJoint)
         if (nJoint < 0  ||  nJoint >= static_cast<int> (model.aNode.size ()))
            bSkinRefsValid = false;
   }
   Check (bSkinRefsValid, "Skin joint indices and IBM counts in range");
}

// ---------------------------------------------------------------------------
// Test 4: Flatten + decode into a renderer-ready draw list
// ---------------------------------------------------------------------------
static void TestBuildRenderModel ()
{
   std::printf ("\n[Test 4] Build render model (flatten + decode)\n");

   std::string sPath = std::string (SNEEZE_TEST_DATA_DIR) + "/model.glb";

   std::vector<uint8_t> aBytes;
   if (!ReadFile (sPath, aBytes))
   {
      Check (false, "Sample GLB read from disk");
      return;
   }

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   if (!SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError))
   {
      Check (false, "GLB parsed and mapped");
      return;
   }

   size_t nTextures = model.aTexture.size ();

   MAT4 matIdentity =
   { {
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0,
   } };

   SNEEZE::GLTF_RENDER_MODEL render;
   bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), matIdentity, render);

   Check (bBuilt, "Render model built");
   Check (!render.aMesh.empty (), "Render model has drawable meshes");

   std::printf ("    draws=%zu textures=%zu\n", render.aMesh.size (), render.aTexturePixel.size ());

   bool bPositionsPresent = true;
   bool bTransformsFinite = true;
   bool bIndicesAligned   = true;
   for (const SNEEZE::MESH_DATA& mesh : render.aMesh)
   {
      if (!mesh.pfPosition  ||  mesh.uCount_Vertex == 0)
         bPositionsPresent = false;
      for (int n = 0; n < 16; n++)
         if (!std::isfinite (mesh.mWorld.f[n]))
            bTransformsFinite = false;
      if (mesh.puIndex  &&  mesh.uCount_Index % 3 != 0)
         bIndicesAligned = false;
   }

   Check (bPositionsPresent, "Every draw carries positions");
   Check (bTransformsFinite, "Every draw transform is finite");
   Check (bIndicesAligned, "Index counts are triangle-aligned");

   bool bAnyTextureDecoded = false;
   for (size_t i = 0; i < render.aTextureWidth.size (); i++)
      if (render.aTextureWidth[i] > 0  &&  render.aTextureHeight[i] > 0)
         bAnyTextureDecoded = true;

   bool bAnyTextured = false;
   for (const SNEEZE::MESH_DATA& mesh : render.aMesh)
      if (mesh.pbTexturePixels  &&  mesh.dimTexture.nW > 0  &&  mesh.dimTexture.nH > 0)
         bAnyTextured = true;

   if (nTextures > 0)
   {
      Check (bAnyTextureDecoded, "At least one base-color texture decoded to RGBA8");
      Check (bAnyTextured, "At least one draw references a decoded texture");
   }

   bool bUvValuesInRange = true;
   size_t nUvMeshCount   = 0;
   for (const SNEEZE::MESH_DATA& mesh : render.aMesh)
   {
      if (mesh.pfTexCoord)
      {
         nUvMeshCount++;
         uint32_t nCheck = std::min (mesh.uCount_Vertex, static_cast<uint32_t> (8));
         for (uint32_t v = 0; v < nCheck; v++)
         {
            float fV = mesh.pfTexCoord[v * 2 + 1];
            if (fV < 0.0f  ||  fV > 1.0f)
               bUvValuesInRange = false;
         }
      }
   }
   if (nUvMeshCount > 0)
   {
      Check (true, "UV-carrying draws have pfTexCoord");
      Check (bUvValuesInRange, "Flipped V values are in [0, 1]");
   }
}

// ---------------------------------------------------------------------------
// Test 5: KHR_draco_mesh_compression REQUIRED GLB (doughnut1Mb.glb)
// ---------------------------------------------------------------------------
static void TestDracoGlb ()
{
   std::printf ("\n[Test 5] Load Draco-required GLB if present\n");

   const char* szPath = std::getenv ("SNEEZE_DRACO_GLB");
   if (!szPath)
   {
      std::printf ("    skipped (set SNEEZE_DRACO_GLB to a .glb path)\n");
      return;
   }

   std::string sPath = szPath;
   std::vector<uint8_t> aBytes;
   if (!ReadFile (sPath, aBytes))
   {
      std::printf ("    skipped (not at %s)\n", sPath.c_str ());
      return;
   }

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "Draco GLB parsed and mapped");
   if (!bOk)
   {
      std::printf ("    error: %s\n", sError.c_str ());
      return;
   }

   uint32_t nVertex = 0;
   uint32_t nIndex  = 0;
   bool     bPos    = true;
   for (const SNEEZE::DEP::GLTF_MESH& mesh : model.aMesh)
   {
      for (const SNEEZE::DEP::GLTF_PRIMITIVE& prim : mesh.aPrimitive)
      {
         if (prim.aPosition.empty ())
            bPos = false;
         nVertex += static_cast<uint32_t> (prim.aPosition.size () / 3);
         nIndex  += static_cast<uint32_t> (prim.aIndex.size ());
      }
   }

   std::printf ("    meshes=%zu vertices=%u triangles=%u\n",
      model.aMesh.size (), nVertex, nIndex / 3);

   Check (model.aMesh.size () == 10, "Draco doughnut has 10 meshes");
   Check (bPos, "Every Draco primitive has positions");
   Check (nIndex / 3 >= 900000, "Decoded triangle count is about 1M");
}

// ---------------------------------------------------------------------------
// Test 6: Same-material primitives on one mesh concatenate into one draw
// ---------------------------------------------------------------------------
static MAT4 Mat4_Identity ()
{
   MAT4 mat =
   { {
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0,
   } };
   return mat;
}

static SNEEZE::DEP::GLTF_PRIMITIVE Prim_Triangle (int nMaterial, float fX)
{
   SNEEZE::DEP::GLTF_PRIMITIVE prim;
   prim.nMaterial = nMaterial;
   prim.aPosition = { fX, 0.0f, 0.0f,  fX + 1.0f, 0.0f, 0.0f,  fX, 1.0f, 0.0f };
   prim.aNormal   = { 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f };
   prim.aIndex    = { 0, 1, 2 };
   prim.bBound    = true;
   prim.aBoundMin[0] = fX;
   prim.aBoundMin[1] = 0.0f;
   prim.aBoundMin[2] = 0.0f;
   prim.aBoundMax[0] = fX + 1.0f;
   prim.aBoundMax[1] = 1.0f;
   prim.aBoundMax[2] = 0.0f;
   return prim;
}

static void TestMergeSameMaterial ()
{
   std::printf ("\n[Test 6] Merge same-material primitives within a mesh\n");

   MAT4 matIdentity = Mat4_Identity ();

   {
      SNEEZE::DEP::GLTF_MODEL model;
      SNEEZE::DEP::GLTF_MESH mesh;
      mesh.aPrimitive.push_back (Prim_Triangle (0, 0.0f));
      mesh.aPrimitive.push_back (Prim_Triangle (0, 2.0f));
      model.aMesh.push_back (std::move (mesh));
      model.aMaterial.push_back (SNEEZE::DEP::GLTF_MATERIAL ());

      SNEEZE::DEP::GLTF_NODE node;
      node.transform = matIdentity;
      node.nMesh     = 0;
      model.aNode.push_back (node);
      model.aRoot.push_back (0);

      SNEEZE::GLTF_RENDER_MODEL render;
      bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), matIdentity, render);
      Check (bBuilt, "Same-material pair built");
      Check (render.aMesh.size () == 1, "Same-material primitives become one draw");
      if (render.aMesh.size () == 1)
      {
         Check (render.aMesh[0].uCount_Vertex == 6, "Merged draw has 6 vertices");
         Check (render.aMesh[0].uCount_Index == 6, "Merged draw has 6 indices");
         Check (render.aMesh[0].puIndex != nullptr
             && render.aMesh[0].puIndex[3] == 3
             && render.aMesh[0].puIndex[4] == 4
             && render.aMesh[0].puIndex[5] == 5, "Second primitive indices are offset by 3");
         Check (render.aMesh[0].pfPosition != nullptr
             && render.aMesh[0].pfPosition[9] == 2.0f, "Second primitive positions are concatenated");
         Check (render.aMesh[0].bBound
             && render.aMesh[0].aBoundMin[0] == 0.0f
             && render.aMesh[0].aBoundMax[0] == 3.0f, "Merged AABB unions both primitives");
      }
   }

   {
      SNEEZE::DEP::GLTF_MODEL model;
      SNEEZE::DEP::GLTF_MESH mesh;
      mesh.aPrimitive.push_back (Prim_Triangle (0, 0.0f));
      mesh.aPrimitive.push_back (Prim_Triangle (1, 2.0f));
      model.aMesh.push_back (std::move (mesh));
      model.aMaterial.push_back (SNEEZE::DEP::GLTF_MATERIAL ());
      model.aMaterial.push_back (SNEEZE::DEP::GLTF_MATERIAL ());

      SNEEZE::DEP::GLTF_NODE node;
      node.transform = matIdentity;
      node.nMesh     = 0;
      model.aNode.push_back (node);
      model.aRoot.push_back (0);

      SNEEZE::GLTF_RENDER_MODEL render;
      SNEEZE::Gltf_Render_Model_Build (std::move (model), matIdentity, render);
      Check (render.aMesh.size () == 2, "Different materials stay two draws");
   }

   {
      SNEEZE::DEP::GLTF_MODEL model;
      SNEEZE::DEP::GLTF_MESH meshA;
      SNEEZE::DEP::GLTF_MESH meshB;
      meshA.aPrimitive.push_back (Prim_Triangle (0, 0.0f));
      meshB.aPrimitive.push_back (Prim_Triangle (0, 2.0f));
      model.aMesh.push_back (std::move (meshA));
      model.aMesh.push_back (std::move (meshB));
      model.aMaterial.push_back (SNEEZE::DEP::GLTF_MATERIAL ());

      SNEEZE::DEP::GLTF_NODE nodeA;
      nodeA.transform = matIdentity;
      nodeA.nMesh     = 0;
      SNEEZE::DEP::GLTF_NODE nodeB;
      nodeB.transform = matIdentity;
      nodeB.nMesh     = 1;
      model.aNode.push_back (nodeA);
      model.aNode.push_back (nodeB);
      model.aRoot.push_back (0);
      model.aRoot.push_back (1);

      SNEEZE::GLTF_RENDER_MODEL render;
      SNEEZE::Gltf_Render_Model_Build (std::move (model), matIdentity, render);
      Check (render.aMesh.size () == 2, "Same material on different meshes stays two draws");
   }
}

static MAT4 Mat4_Translate (double dX, double dY, double dZ)
{
   MAT4 mat = Mat4_Identity ();
   mat.d[12] = dX;
   mat.d[13] = dY;
   mat.d[14] = dZ;
   return mat;
}

static void TestSkinBindPose ()
{
   std::printf ("\n[Test 7] GPU-skin a rigged triangle: rest verts + bind palette\n");

   SNEEZE::DEP::GLTF_MODEL model;
   SNEEZE::DEP::GLTF_MESH mesh;
   SNEEZE::DEP::GLTF_PRIMITIVE prim = Prim_Triangle (0, 1.0f);
   prim.aJoint  = { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
   prim.aWeight = { 1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f };
   mesh.aPrimitive.push_back (std::move (prim));
   model.aMesh.push_back (std::move (mesh));
   model.aMaterial.push_back (SNEEZE::DEP::GLTF_MATERIAL ());

   SNEEZE::DEP::GLTF_NODE nodeMesh;
   nodeMesh.transform = Mat4_Translate (100.0, 0.0, 0.0);
   nodeMesh.nMesh     = 0;
   nodeMesh.nSkin     = 0;
   SNEEZE::DEP::GLTF_NODE nodeJoint;
   nodeJoint.transform = Mat4_Translate (0.0, 2.0, 0.0);

   model.aNode.push_back (nodeMesh);
   model.aNode.push_back (nodeJoint);
   model.aRoot.push_back (0);
   model.aRoot.push_back (1);

   SNEEZE::DEP::GLTF_SKIN skin;
   skin.aJoint.push_back (1);
   skin.aInverseBind.push_back (Mat4_Identity ());
   model.aSkin.push_back (std::move (skin));

   SNEEZE::GLTF_RENDER_MODEL render;
   bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), Mat4_Identity (), render);
   Check (bBuilt, "Skinned triangle built");
   Check (render.aMesh.size () == 1, "Skinned model emits one draw");
   Check (render.aBonePalette.size () == 1  &&  render.aBonePalette[0].size () == 16, "One bind-pose bone matrix was packed");

   if (render.aMesh.size () == 1)
   {
      const SNEEZE::MESH_DATA& Mesh_Data = render.aMesh[0];
      Check (Mesh_Data.nSkin == 0, "Draw is tagged with skin 0");
      Check (Mesh_Data.puJoint != nullptr  &&  Mesh_Data.pfWeight != nullptr, "Joints and weights stay on the rest-pose mesh");
      Check (Mesh_Data.uCount_Bone == 1  &&  Mesh_Data.pfBoneMatrix != nullptr, "Draw borrows the packed palette");
      if (Mesh_Data.pfBoneMatrix)
         Check (std::fabs (Mesh_Data.pfBoneMatrix[13] - 2.0f) < 1.0e-5f, "Joint +2Y lives in the palette, not the vertices");
      if (Mesh_Data.pfPosition)
      {
         const float* pfP = Mesh_Data.pfPosition;
         Check (std::fabs (pfP[0] - 1.0f) < 1.0e-5f
             && std::fabs (pfP[1] - 0.0f) < 1.0e-5f
             && std::fabs (pfP[2] - 0.0f) < 1.0e-5f, "Vertex 0 stays at rest in glTF space");
         Check (std::fabs (pfP[3] - 2.0f) < 1.0e-5f
             && std::fabs (pfP[4] - 0.0f) < 1.0e-5f, "Vertex 1 stays at rest in glTF space");
         Check (std::fabs (pfP[6] - 1.0f) < 1.0e-5f
             && std::fabs (pfP[7] - 1.0f) < 1.0e-5f, "Vertex 2 stays at rest in glTF space");
      }
      Check (std::fabs (Mesh_Data.mWorld.f[12]) < 1.0e-5f, "Mesh-node translation is not baked into mWorld");
   }
}

static void WriteU32LE (std::vector<uint8_t>& aOut, uint32_t n)
{
   aOut.push_back (static_cast<uint8_t> (n));
   aOut.push_back (static_cast<uint8_t> (n >> 8));
   aOut.push_back (static_cast<uint8_t> (n >> 16));
   aOut.push_back (static_cast<uint8_t> (n >> 24));
}

static void WriteF32LE (std::vector<uint8_t>& aOut, float f)
{
   uint32_t n = 0;
   std::memcpy (&n, &f, sizeof (n));
   WriteU32LE (aOut, n);
}

static void PackGlb (const std::string& sJson, const uint8_t* pBin, size_t nBin, std::vector<uint8_t>& aOut)
{
   std::string sChunk = sJson;
   while ((sChunk.size () % 4) != 0)
      sChunk.push_back (' ');

   const uint32_t nJson = static_cast<uint32_t> (sChunk.size ());
   const uint32_t nBinPad = static_cast<uint32_t> ((4 - (nBin % 4)) % 4);
   const uint32_t nBinChunk = static_cast<uint32_t> (nBin) + nBinPad;
   const uint32_t nTotal = 12 + 8 + nJson + 8 + nBinChunk;

   aOut.clear ();
   aOut.push_back ('g');
   aOut.push_back ('l');
   aOut.push_back ('T');
   aOut.push_back ('F');
   WriteU32LE (aOut, 2);
   WriteU32LE (aOut, nTotal);
   WriteU32LE (aOut, nJson);
   WriteU32LE (aOut, 0x4E4F534A);
   aOut.insert (aOut.end (), sChunk.begin (), sChunk.end ());
   WriteU32LE (aOut, nBinChunk);
   WriteU32LE (aOut, 0x004E4942);
   if (pBin  &&  nBin > 0)
      aOut.insert (aOut.end (), pBin, pBin + nBin);
   for (uint32_t n = 0; n < nBinPad; n++)
      aOut.push_back (0);
}

static void TestVrm10Required ()
{
   std::printf ("\n[Test 8] Load a VRM 1.0 GLB with required VRMC extensions\n");

   const float aPos[9] = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
   const char* szJson =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]},"
      "\"extensions\":{\"VRMC_materials_mtoon\":{\"specVersion\":\"1.0\",\"shadeColorFactor\":[0.25,0.5,0.75]}}}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
      "\"max\":[1.0,1.0,0.0],\"min\":[0.0,0.0,0.0]}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0}],"
      "\"buffers\":[{\"byteLength\":36}],"
      "\"extensionsUsed\":[\"VRMC_vrm\",\"VRMC_materials_mtoon\"],"
      "\"extensionsRequired\":[\"VRMC_vrm\",\"VRMC_materials_mtoon\"],"
      "\"extensions\":{\"VRMC_vrm\":{\"specVersion\":\"1.0\"}}"
      "}";

   std::vector<uint8_t> aBytes;
   PackGlb (szJson, reinterpret_cast<const uint8_t*> (aPos), sizeof (aPos), aBytes);

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "VRM 1.0 GLB with required VRMC extensions parsed");
   if (!bOk)
      std::printf ("    error: %s\n", sError.c_str ());
   Check (model.aMesh.size () == 1  &&  !model.aMesh[0].aPrimitive.empty (), "VRM mesh was mapped");
   if (!model.aMesh.empty ()  &&  !model.aMesh[0].aPrimitive.empty ())
      Check (model.aMesh[0].aPrimitive[0].aPosition.size () == 9, "VRM triangle positions were read");
   Check (model.aMaterial.size () == 1  &&  !model.aMaterial[0].bUnlit, "MToon is a lit dielectric, not Halogen unlit");
   if (!model.aMaterial.empty ())
   {
      Check (model.aMaterial[0].dMetallic == 0.0f, "MToon material is a dielectric");
      Check (std::fabs (model.aMaterial[0].shadeColor[0] - 0.25f) < 1.0e-5f
          && std::fabs (model.aMaterial[0].shadeColor[1] - 0.50f) < 1.0e-5f
          && std::fabs (model.aMaterial[0].shadeColor[2] - 0.75f) < 1.0e-5f, "MToon shadeColorFactor was mapped");
   }

   SNEEZE::GLTF_RENDER_MODEL render;
   bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), Mat4_Identity (), render);
   Check (bBuilt  &&  render.aMesh.size () == 1, "VRM render model built");
   if (render.aMesh.size () == 1)
      Check (!render.aMesh[0].bUnlit, "MToon draws as lit physicallyBased");
}

static void TestVrmNodeConstraint ()
{
   std::printf ("\n[Test 9] Apply VRMC_node_constraint at bind\n");

   const float aPos[9] = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
   const char* szJson =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0,1,2]}],"
      "\"nodes\":["
      "{\"mesh\":0,\"translation\":[0,0,0]},"
      "{\"translation\":[0,1,0],\"extensions\":{\"VRMC_node_constraint\":{\"specVersion\":\"1.0\","
      "\"constraint\":{\"aim\":{\"source\":0,\"aimAxis\":\"PositiveY\",\"weight\":1.0}}}}},"
      "{\"translation\":[1,0,0],\"extensions\":{\"VRMC_node_constraint\":{\"specVersion\":\"1.0\","
      "\"constraint\":{\"rotation\":{\"source\":0,\"weight\":1.0}}}}}"
      "],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
      "\"max\":[1.0,1.0,0.0],\"min\":[0.0,0.0,0.0]}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0}],"
      "\"buffers\":[{\"byteLength\":36}],"
      "\"extensionsUsed\":[\"VRMC_node_constraint\"],"
      "\"extensionsRequired\":[\"VRMC_node_constraint\"]"
      "}";

   std::vector<uint8_t> aBytes;
   PackGlb (szJson, reinterpret_cast<const uint8_t*> (aPos), sizeof (aPos), aBytes);

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "VRM constraint GLB parsed");
   if (!bOk)
      std::printf ("    error: %s\n", sError.c_str ());
   Check (model.aConstraint.size () == 2, "Two node constraints were mapped");
   if (model.aConstraint.size () >= 2)
   {
      Check (model.aConstraint[0].eKind == SNEEZE::DEP::GLTF_CONSTRAINT::kAIM
          && model.aConstraint[0].nNode == 1
          && model.aConstraint[0].nSource == 0
          && model.aConstraint[0].nAxis == 2, "Aim constraint: dest 1, source 0, +Y");
      Check (model.aConstraint[1].eKind == SNEEZE::DEP::GLTF_CONSTRAINT::kROTATION
          && model.aConstraint[1].nNode == 2
          && model.aConstraint[1].nSource == 0, "Rotation constraint: dest 2, source 0");
   }

   SNEEZE::GLTF_RENDER_MODEL render;
   bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), Mat4_Identity (), render);
   Check (bBuilt, "Constraint model built");
   Check (render.model.aNode.size () == 3, "Three nodes survive the build");
   if (render.model.aNode.size () >= 3)
   {
      const MAT4& matAim = render.model.aNode[1].transform;
      Check (std::fabs (matAim.d[13] - 1.0) < 1.0e-5, "Aim dest keeps its translation");
      Check (std::fabs (matAim.d[4])        < 1.0e-4
          && std::fabs (matAim.d[5]  + 1.0) < 1.0e-4
          && std::fabs (matAim.d[6])        < 1.0e-4, "Aim +Y points at the source");

      const MAT4& matRot = render.model.aNode[2].transform;
      Check (std::fabs (matRot.d[12] - 1.0) < 1.0e-5, "Rotation dest keeps its translation");
      Check (std::fabs (matRot.d[0]  - 1.0) < 1.0e-4
          && std::fabs (matRot.d[5]  - 1.0) < 1.0e-4
          && std::fabs (matRot.d[10] - 1.0) < 1.0e-4, "Rotation constraint is a no-op at bind");
   }
}

static void TestMaterialAlphaMode ()
{
   std::printf ("\n[Test 10] Map glTF and VRM alpha modes\n");

   const float aPos[9] = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
   const char* szJson =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"materials\":["
      "{\"alphaMode\":\"MASK\",\"alphaCutoff\":0.4},"
      "{\"alphaMode\":\"BLEND\"},"
      "{\"alphaMode\":\"OPAQUE\",\"extensions\":{\"VRMC_materials_mtoon\":{\"specVersion\":\"1.0\",\"transparentWithZWrite\":true}}},"
      "{\"alphaMode\":\"OPAQUE\"}"
      "],"
      "\"meshes\":[{\"primitives\":["
      "{\"attributes\":{\"POSITION\":0},\"material\":0},"
      "{\"attributes\":{\"POSITION\":0},\"material\":1},"
      "{\"attributes\":{\"POSITION\":0},\"material\":2},"
      "{\"attributes\":{\"POSITION\":0},\"material\":3}"
      "]}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
      "\"max\":[1.0,1.0,0.0],\"min\":[0.0,0.0,0.0]}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0}],"
      "\"buffers\":[{\"byteLength\":36}],"
      "\"extensions\":{\"VRM\":{\"materialProperties\":[{},{},{},"
      "{\"floatProperties\":{\"_BlendMode\":1,\"_Cutoff\":0.3}}]}},"
      "\"extensionsUsed\":[\"VRMC_materials_mtoon\",\"VRM\"]"
      "}";

   std::vector<uint8_t> aBytes;
   PackGlb (szJson, reinterpret_cast<const uint8_t*> (aPos), sizeof (aPos), aBytes);

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "Alpha-mode GLB parsed");
   if (!bOk)
      std::printf ("    error: %s\n", sError.c_str ());
   Check (model.aMaterial.size () == 4, "Four materials were mapped");
   if (model.aMaterial.size () == 4)
   {
      Check (model.aMaterial[0].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kMASK
          && std::fabs (model.aMaterial[0].dAlphaCutoff - 0.4f) < 1.0e-5f, "glTF MASK plus cutoff");
      Check (model.aMaterial[1].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kBLEND, "glTF BLEND");
      Check (model.aMaterial[2].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kBLEND, "MToon transparentWithZWrite becomes BLEND");
      Check (model.aMaterial[3].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kMASK
          && std::fabs (model.aMaterial[3].dAlphaCutoff - 0.3f) < 1.0e-5f, "VRM 0 _BlendMode Cutout becomes MASK");
   }
}

static void TestOpaqueTextureAlphaPromotes ()
{
   std::printf ("\n[Test 11] OPAQUE albedo with cutout alpha becomes MASK\n");

   const float aPos[9] = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
   static const uint8_t aPng[] =
   {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xF4, 0x22, 0x7F,
      0x8A, 0x00, 0x00, 0x00, 0x0E, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x60, 0x00, 0x82, 0xFF,
      0x40, 0x04, 0x00, 0x05, 0x04, 0x01, 0xFF, 0x7F, 0x05, 0x6D, 0x50, 0x00, 0x00, 0x00, 0x00, 0x49,
      0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
   };

   std::vector<uint8_t> aBin (reinterpret_cast<const uint8_t*> (aPos), reinterpret_cast<const uint8_t*> (aPos) + sizeof (aPos));
   aBin.insert (aBin.end (), aPng, aPng + sizeof (aPng));

   const char* szJson =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/png\"}],"
      "\"textures\":[{\"source\":0}],"
      "\"materials\":[{\"alphaMode\":\"OPAQUE\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
      "\"max\":[1.0,1.0,0.0],\"min\":[0.0,0.0,0.0]}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":71}"
      "],"
      "\"buffers\":[{\"byteLength\":107}]"
      "}";

   std::vector<uint8_t> aBytes;
   PackGlb (szJson, aBin.data (), aBin.size (), aBytes);

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "OPAQUE cutout GLB parsed");
   if (!bOk)
      std::printf ("    error: %s\n", sError.c_str ());
   Check (!model.aMaterial.empty ()  &&  model.aMaterial[0].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kOPAQUE, "Loader keeps authored OPAQUE");
   Check (!model.aTexture.empty ()  &&  !model.aTexture[0].aEncoded.empty (), "PNG bytes were mapped");

   SNEEZE::GLTF_RENDER_MODEL render;
   bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), Mat4_Identity (), render);
   Check (bBuilt  &&  !render.aMesh.empty (), "Cutout render model built");
   if (!render.model.aMaterial.empty ())
      Check (render.model.aMaterial[0].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kMASK, "Decoded cutout alpha promotes OPAQUE to MASK");
   if (!render.aMesh.empty ())
      Check (render.aMesh[0].eAlpha == SNEEZE::DEP::GLTF_MATERIAL::kMASK, "Draw list carries MASK");
}

static void TestAnimationClip ()
{
   std::printf ("\n[Test 12] Load a glTF animation clip (node TRS)\n");

   const char* szJson =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"translation\":[0,0,0]}],"
      "\"animations\":[{\"name\":\"slide\",\"samplers\":[{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"max\":[1.0],\"min\":[0.0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}"
      "],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24}"
      "],"
      "\"buffers\":[{\"byteLength\":32}]"
      "}";

   std::vector<uint8_t> aBin;
   WriteF32LE (aBin, 0.0f);
   WriteF32LE (aBin, 1.0f);
   WriteF32LE (aBin, 0.0f);
   WriteF32LE (aBin, 0.0f);
   WriteF32LE (aBin, 0.0f);
   WriteF32LE (aBin, 2.0f);
   WriteF32LE (aBin, 0.0f);
   WriteF32LE (aBin, 0.0f);

   std::vector<uint8_t> aBytes;
   PackGlb (szJson, aBin.data (), aBin.size (), aBytes);

   SNEEZE::DEP::GLTF_MODEL model;
   std::string sError;
   bool bOk = SNEEZE::DEP::GLTF::Load (aBytes.data (), aBytes.size (), model, sError);
   Check (bOk, "GLB with a translation clip parsed");
   if (!bOk)
      std::printf ("    error: %s\n", sError.c_str ());
   Check (model.aAnimation.size () == 1, "One animation was mapped");
   Check (!model.aNode.empty (), "Rest node was mapped");
   if (!model.aNode.empty ())
   {
      Check (std::fabs (model.aNode[0].aTranslation[0]) < 1.0e-5
          &&  std::fabs (model.aNode[0].aTranslation[1]) < 1.0e-5
          &&  std::fabs (model.aNode[0].aTranslation[2]) < 1.0e-5, "Node rest translation is authored TRS");
   }
   if (model.aAnimation.size () == 1)
   {
      const SNEEZE::DEP::GLTF_ANIMATION& anim = model.aAnimation[0];
      Check (anim.sName == "slide", "Clip name was mapped");
      Check (std::fabs (anim.dDuration - 1.0) < 1.0e-5, "Clip duration is the last input time");
      Check (anim.aChannel.size () == 1, "One translation channel was mapped");
      if (anim.aChannel.size () == 1)
      {
         const SNEEZE::DEP::GLTF_CHANNEL& channel = anim.aChannel[0];
         Check (channel.nNode == 0  &&  channel.ePath == SNEEZE::DEP::GLTF_CHANNEL::kTRANSLATION, "Channel targets node 0 translation");
         Check (channel.eInterp == SNEEZE::DEP::GLTF_CHANNEL::kLINEAR, "Channel interpolation is LINEAR");
         Check (channel.aTime.size () == 2  &&  std::fabs (channel.aTime[0]) < 1.0e-5f  &&  std::fabs (channel.aTime[1] - 1.0f) < 1.0e-5f, "Sampler times are 0 and 1");
         Check (channel.aValue.size () == 6, "Two VEC3 keys were mapped");
         if (channel.aValue.size () == 6)
         {
            Check (std::fabs (channel.aValue[0]) < 1.0e-5f
                &&  std::fabs (channel.aValue[1]) < 1.0e-5f
                &&  std::fabs (channel.aValue[2]) < 1.0e-5f, "First key is (0,0,0)");
            Check (std::fabs (channel.aValue[3] - 2.0f) < 1.0e-5f
                &&  std::fabs (channel.aValue[4]) < 1.0e-5f
                &&  std::fabs (channel.aValue[5]) < 1.0e-5f, "Second key is (2,0,0)");
         }
      }
   }
}

static void TestAnimationPose ()
{
   std::printf ("\n[Test 13] Sample a clip into a live bone palette\n");

   SNEEZE::DEP::GLTF_MODEL model;
   SNEEZE::DEP::GLTF_MESH mesh;
   SNEEZE::DEP::GLTF_PRIMITIVE prim = Prim_Triangle (0, 1.0f);
   prim.aJoint  = { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
   prim.aWeight = { 1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f };
   mesh.aPrimitive.push_back (std::move (prim));
   model.aMesh.push_back (std::move (mesh));
   model.aMaterial.push_back (SNEEZE::DEP::GLTF_MATERIAL ());

   SNEEZE::DEP::GLTF_NODE nodeMesh;
   nodeMesh.aTranslation[0] = 100.0;
   nodeMesh.transform       = Mat4_Translate (100.0, 0.0, 0.0);
   nodeMesh.nMesh           = 0;
   nodeMesh.nSkin           = 0;
   SNEEZE::DEP::GLTF_NODE nodeJoint;
   nodeJoint.aTranslation[1] = 2.0;
   nodeJoint.transform       = Mat4_Translate (0.0, 2.0, 0.0);

   model.aNode.push_back (nodeMesh);
   model.aNode.push_back (nodeJoint);
   model.aRoot.push_back (0);
   model.aRoot.push_back (1);

   SNEEZE::DEP::GLTF_SKIN skin;
   skin.aJoint.push_back (1);
   skin.aInverseBind.push_back (Mat4_Identity ());
   model.aSkin.push_back (std::move (skin));

   SNEEZE::DEP::GLTF_CHANNEL channel;
   channel.nNode   = 1;
   channel.ePath   = SNEEZE::DEP::GLTF_CHANNEL::kTRANSLATION;
   channel.eInterp = SNEEZE::DEP::GLTF_CHANNEL::kLINEAR;
   channel.aTime   = { 0.0f, 1.0f };
   channel.aValue  = { 0.0f, 2.0f, 0.0f,  0.0f, 5.0f, 0.0f };
   SNEEZE::DEP::GLTF_ANIMATION anim;
   anim.sName     = "lift";
   anim.dDuration = 1.0;
   anim.aChannel.push_back (std::move (channel));
   model.aAnimation.push_back (std::move (anim));

   SNEEZE::GLTF_RENDER_MODEL render;
   bool bBuilt = SNEEZE::Gltf_Render_Model_Build (std::move (model), Mat4_Identity (), render);
   Check (bBuilt, "Skinned triangle with a clip built");
   Check (render.aBonePalette.size () == 1  &&  render.aBonePalette[0].size () == 16, "Bind palette was packed");
   if (!render.aBonePalette.empty ()  &&  render.aBonePalette[0].size () >= 14)
      Check (std::fabs (render.aBonePalette[0][13] - 2.0f) < 1.0e-5f, "Bind palette keeps the joint at +2Y");

   std::vector<std::vector<float>> aPalette;
   bool bT0 = SNEEZE::Gltf_Render_Model_Pose (render, 0, 0.0, aPalette);
   Check (bT0  &&  aPalette.size () == 1  &&  aPalette[0].size () == 16, "Pose at t=0 packed one palette");
   if (!aPalette.empty ()  &&  aPalette[0].size () >= 14)
      Check (std::fabs (aPalette[0][13] - 2.0f) < 1.0e-5f, "t=0 samples the first translation key");

   bool bT1 = SNEEZE::Gltf_Render_Model_Pose (render, 0, 1.0, aPalette);
   Check (bT1, "Pose at t=1 packed a palette");
   if (!aPalette.empty ()  &&  aPalette[0].size () >= 14)
      Check (std::fabs (aPalette[0][13] - 5.0f) < 1.0e-5f, "t=1 samples the last translation key");

   if (!render.aBonePalette.empty ()  &&  render.aBonePalette[0].size () >= 14)
      Check (std::fabs (render.aBonePalette[0][13] - 2.0f) < 1.0e-5f, "Shared bind palette was not mutated");
}

// ---------------------------------------------------------------------------

int RunGltfTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== glTF Loader Test Suite ===\n");

   TestEmptyInput ();
   TestGarbageInput ();
   TestLoadGlb ();
   TestBuildRenderModel ();
   TestDracoGlb ();
   TestMergeSameMaterial ();
   TestSkinBindPose ();
   TestVrm10Required ();
   TestVrmNodeConstraint ();
   TestMaterialAlphaMode ();
   TestOpaqueTextureAlphaPromotes ();
   TestAnimationClip ();
   TestAnimationPose ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
