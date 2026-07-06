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
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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
      if (!mesh.pPosition  ||  mesh.nVertexCount == 0)
         bPositionsPresent = false;
      for (int n = 0; n < 16; n++)
         if (!std::isfinite (mesh.m16[n]))
            bTransformsFinite = false;
      if (mesh.pIndex  &&  mesh.nIndexCount % 3 != 0)
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
      if (mesh.pTexturePixels  &&  mesh.nTextureWidth > 0  &&  mesh.nTextureHeight > 0)
         bAnyTextured = true;

   if (nTextures > 0)
   {
      Check (bAnyTextureDecoded, "At least one base-color texture decoded to RGBA8");
      Check (bAnyTextured, "At least one draw references a decoded texture");
   }
}

// ---------------------------------------------------------------------------
// Test 5: GLTF_MODEL_CACHE shares one build across concurrent loaders
// ---------------------------------------------------------------------------

static void Glb_Append_U32 (std::vector<uint8_t>& aOut, uint32_t nValue)
{
   aOut.push_back (static_cast<uint8_t> (nValue         & 0xFF));
   aOut.push_back (static_cast<uint8_t> ((nValue >>  8) & 0xFF));
   aOut.push_back (static_cast<uint8_t> ((nValue >> 16) & 0xFF));
   aOut.push_back (static_cast<uint8_t> ((nValue >> 24) & 0xFF));
}

// Assembles a minimal valid GLB in memory -- one triangle, one node, one scene
// -- so cache tests need no fixture on disk.
static void Glb_Build_Triangle (std::vector<uint8_t>& aOut)
{
   const char szJson[] = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                         R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
                         R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0.0,0.0,0.0],"max":[1.0,1.0,0.0]},)"
                         R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}],)"
                         R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12}],)"
                         R"("buffers":[{"byteLength":48}]})";

   std::vector<uint8_t> aJson (szJson, szJson + sizeof (szJson) - 1);
   while (aJson.size () % 4 != 0)
      aJson.push_back (' ');

   const float    aVertex[9] = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, };
   const uint32_t aIndex[3]  = { 0, 1, 2, };

   std::vector<uint8_t> aBin (48);
   std::memcpy (aBin.data (),      aVertex, 36);
   std::memcpy (aBin.data () + 36, aIndex,  12);

   aOut.clear ();
   Glb_Append_U32 (aOut, 0x46546C67);   // "glTF" magic
   Glb_Append_U32 (aOut, 2);            // container version
   Glb_Append_U32 (aOut, static_cast<uint32_t> (12 + 8 + aJson.size () + 8 + aBin.size ()));
   Glb_Append_U32 (aOut, static_cast<uint32_t> (aJson.size ()));
   Glb_Append_U32 (aOut, 0x4E4F534A);   // "JSON" chunk
   aOut.insert (aOut.end (), aJson.begin (), aJson.end ());
   Glb_Append_U32 (aOut, static_cast<uint32_t> (aBin.size ()));
   Glb_Append_U32 (aOut, 0x004E4942);   // "BIN" chunk
   aOut.insert (aOut.end (), aBin.begin (), aBin.end ());
}

static void TestModelCache ()
{
   std::printf ("\n[Test 5] Model cache shares one build\n");

   std::vector<uint8_t> aGlb;
   Glb_Build_Triangle (aGlb);

   {
      SNEEZE::DEP::GLTF_MODEL model;
      std::string sError;

      bool bOk = SNEEZE::DEP::GLTF::Load (aGlb.data (), aGlb.size (), model, sError);
      Check (bOk, "In-memory triangle GLB parses");
      if (!bOk)
      {
         std::printf ("    (%s)\n", sError.c_str ());
         return;
      }
   }

   SNEEZE::GLTF_MODEL_CACHE cache;

   const std::string sUrl = "https://example.com/model.glb";

   // Hammer one URL from many threads at once: the cache must coalesce to a
   // single build, and every caller must receive that same shared instance.
   const int nThread = 8;
   std::vector<std::shared_ptr<const SNEEZE::GLTF_RENDER_MODEL>> apModel (nThread);
   std::vector<std::thread> aThread;
   for (int n = 0; n < nThread; n++)
      aThread.emplace_back ([&cache, &apModel, &aGlb, &sUrl, n] ()
      {
         apModel[n] = cache.Model_Load (sUrl, aGlb);
      });
   for (std::thread& th : aThread)
      th.join ();

   bool bAllLoaded = true;
   bool bAllShared = true;
   for (int n = 0; n < nThread; n++)
   {
      if (!apModel[n])
         bAllLoaded = false;
      else if (apModel[n] != apModel[0])
         bAllShared = false;
   }

   Check (bAllLoaded, "Every concurrent caller received a model");
   Check (bAllShared, "All callers share one model instance");

   std::shared_ptr<const SNEEZE::GLTF_RENDER_MODEL> pHeld = apModel[0];
   Check (cache.Model_Find (sUrl) == pHeld, "Find returns the live shared instance");

   std::shared_ptr<const SNEEZE::GLTF_RENDER_MODEL> pOther = cache.Model_Load ("https://example.com/other.glb", aGlb);
   Check (pOther  &&  pOther != pHeld, "Different URL builds a distinct model");

   // Entries are weak: once the last holder releases, the entry expires and a
   // later request rebuilds rather than resurrecting freed storage.
   apModel.clear ();
   pOther.reset ();
   pHeld.reset ();
   Check (cache.Model_Find (sUrl) == nullptr, "Entry expires when the last holder releases");

   std::shared_ptr<const SNEEZE::GLTF_RENDER_MODEL> pRebuilt = cache.Model_Load (sUrl, aGlb);
   Check (pRebuilt != nullptr, "Expired entry rebuilds on demand");
}

// ---------------------------------------------------------------------------

int RunGltfTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== glTF Loader Test Suite ===\n");

   TestEmptyInput ();
   TestGarbageInput ();
   TestLoadGlb ();
   TestBuildRenderModel ();
   TestModelCache ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
