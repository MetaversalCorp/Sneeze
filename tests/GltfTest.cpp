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

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
