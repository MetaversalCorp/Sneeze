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

#include "gltf/Gltf.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/math.hpp>

#include <cstddef>

using namespace SNEEZE::DEP;

namespace
{
   // Copies one vertex attribute accessor into a flat float stream, N components
   // per element. fastgltf converts component types and de-normalizes for us.
   template <typename VEC>
   void Stream_Read (const fastgltf::Asset& asset, const fastgltf::Accessor& accessor, std::vector<float>& aOut, int nComponents)
   {
      aOut.reserve (accessor.count * static_cast<size_t> (nComponents));
      fastgltf::iterateAccessor<VEC> (asset, accessor,
         [&] (VEC value)
         {
            for (int n = 0; n < nComponents; ++n)
               aOut.push_back (static_cast<float> (value[n]));
         });
   }

   void Primitive_Map (const fastgltf::Asset& asset, const fastgltf::Primitive& prim, GLTF_PRIMITIVE& out)
   {
      auto itPosition = prim.findAttribute ("POSITION");
      if (itPosition != prim.attributes.cend ())
         Stream_Read<fastgltf::math::fvec3> (asset, asset.accessors[itPosition->accessorIndex], out.aPosition, 3);

      auto itNormal = prim.findAttribute ("NORMAL");
      if (itNormal != prim.attributes.cend ())
         Stream_Read<fastgltf::math::fvec3> (asset, asset.accessors[itNormal->accessorIndex], out.aNormal, 3);

      auto itTexCoord = prim.findAttribute ("TEXCOORD_0");
      if (itTexCoord != prim.attributes.cend ())
         Stream_Read<fastgltf::math::fvec2> (asset, asset.accessors[itTexCoord->accessorIndex], out.aTexCoord, 2);

      if (prim.indicesAccessor.has_value ())
      {
         const fastgltf::Accessor& accessor = asset.accessors[*prim.indicesAccessor];
         out.aIndex.reserve (accessor.count);
         fastgltf::iterateAccessor<std::uint32_t> (asset, accessor,
            [&] (std::uint32_t nIndex)
            {
               out.aIndex.push_back (nIndex);
            });
      }

      out.nMaterial = prim.materialIndex.has_value () ? static_cast<int> (*prim.materialIndex) : -1;
   }

   void Meshes_Map (const fastgltf::Asset& asset, GLTF_MODEL& model)
   {
      model.aMesh.reserve (asset.meshes.size ());
      for (const fastgltf::Mesh& mesh : asset.meshes)
      {
         GLTF_MESH meshOut;
         meshOut.aPrimitive.reserve (mesh.primitives.size ());
         for (const fastgltf::Primitive& prim : mesh.primitives)
         {
            GLTF_PRIMITIVE primOut;
            Primitive_Map (asset, prim, primOut);
            meshOut.aPrimitive.push_back (std::move (primOut));
         }
         model.aMesh.push_back (std::move (meshOut));
      }
   }

   void Materials_Map (const fastgltf::Asset& asset, GLTF_MODEL& model)
   {
      model.aMaterial.reserve (asset.materials.size ());
      for (const fastgltf::Material& material : asset.materials)
      {
         GLTF_MATERIAL materialOut;
         materialOut.baseColor[0]      = static_cast<float> (material.pbrData.baseColorFactor[0]);
         materialOut.baseColor[1]      = static_cast<float> (material.pbrData.baseColorFactor[1]);
         materialOut.baseColor[2]      = static_cast<float> (material.pbrData.baseColorFactor[2]);
         materialOut.baseColor[3]      = static_cast<float> (material.pbrData.baseColorFactor[3]);
         materialOut.dMetallic         = static_cast<float> (material.pbrData.metallicFactor);
         materialOut.dRoughness        = static_cast<float> (material.pbrData.roughnessFactor);
         materialOut.emissive[0]       = static_cast<float> (material.emissiveFactor[0]);
         materialOut.emissive[1]       = static_cast<float> (material.emissiveFactor[1]);
         materialOut.emissive[2]       = static_cast<float> (material.emissiveFactor[2]);
         materialOut.nBaseColorTexture = material.pbrData.baseColorTexture.has_value ()
            ? static_cast<int> ((*material.pbrData.baseColorTexture).textureIndex)
            : -1;
         model.aMaterial.push_back (materialOut);
      }
   }

   void Textures_Map (const fastgltf::Asset& asset, GLTF_MODEL& model)
   {
      const fastgltf::DefaultBufferDataAdapter adapter {};

      model.aTexture.reserve (asset.textures.size ());
      for (const fastgltf::Texture& texture : asset.textures)
      {
         GLTF_TEXTURE textureOut;
         if (texture.imageIndex.has_value ())
         {
            const fastgltf::Image& image = asset.images[*texture.imageIndex];
            std::visit (fastgltf::visitor
            {
               [&] (const auto&) {},
               [&] (const fastgltf::sources::BufferView& view)
               {
                  auto bytes = adapter (asset, view.bufferViewIndex);
                  const uint8_t* pBytes = reinterpret_cast<const uint8_t*> (bytes.data ());
                  textureOut.aEncoded.assign (pBytes, pBytes + bytes.size ());
               },
               [&] (const fastgltf::sources::Array& array)
               {
                  const uint8_t* pBytes = reinterpret_cast<const uint8_t*> (array.bytes.data ());
                  textureOut.aEncoded.assign (pBytes, pBytes + array.bytes.size_bytes ());
               },
               [&] (const fastgltf::sources::Vector& vector)
               {
                  const uint8_t* pBytes = reinterpret_cast<const uint8_t*> (vector.bytes.data ());
                  textureOut.aEncoded.assign (pBytes, pBytes + vector.bytes.size ());
               },
            }, image.data);
         }
         model.aTexture.push_back (std::move (textureOut));
      }
   }

   void Nodes_Map (const fastgltf::Asset& asset, GLTF_MODEL& model)
   {
      model.aNode.reserve (asset.nodes.size ());
      for (const fastgltf::Node& node : asset.nodes)
      {
         GLTF_NODE nodeOut;

         fastgltf::math::fmat4x4 matrix = fastgltf::getTransformMatrix (node);
         for (int nColumn = 0; nColumn < 4; ++nColumn)
            for (int nRow = 0; nRow < 4; ++nRow)
               nodeOut.transform.d[nColumn * 4 + nRow] = matrix[nColumn][nRow];

         nodeOut.nMesh = node.meshIndex.has_value () ? static_cast<int> (*node.meshIndex) : -1;

         nodeOut.aChild.reserve (node.children.size ());
         for (std::size_t nChild : node.children)
            nodeOut.aChild.push_back (static_cast<int> (nChild));

         model.aNode.push_back (std::move (nodeOut));
      }

      size_t nScene = asset.defaultScene.has_value () ? *asset.defaultScene : 0;
      if (nScene < asset.scenes.size ())
      {
         const fastgltf::Scene& scene = asset.scenes[nScene];
         model.aRoot.reserve (scene.nodeIndices.size ());
         for (std::size_t nRoot : scene.nodeIndices)
            model.aRoot.push_back (static_cast<int> (nRoot));
      }
   }
}

GLTF::GLTF (ENGINE* pEngine)
   : m_pEngine (pEngine)
   , m_bInitialized (false)
{
}

GLTF::~GLTF ()
{
   m_bInitialized = false;
}

bool GLTF::Initialize ()
{
   m_bInitialized = true;
   m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "GLTF",
      "glTF loader initialized (fastgltf)");
   return true;
}

bool GLTF::Load (const uint8_t* pData, size_t nLen, GLTF_MODEL& model, std::string& sError)
{
   bool bResult = false;

   model = GLTF_MODEL ();
   sError.clear ();

   if (pData != nullptr  &&  nLen > 0)
   {
      auto expBuffer = fastgltf::GltfDataBuffer::FromBytes (reinterpret_cast<const std::byte*> (pData), nLen);
      if (expBuffer)
      {
         // Enable the extensions we accept. KHR_mesh_quantization is the important
         // one: assets optimized by glTF-Transform quantize vertex attributes
         // (SHORT/BYTE positions/normals, USHORT texcoords) and mark the extension
         // as REQUIRED, so fastgltf rejects the whole file unless it is enabled
         // here. iterateAccessor (Stream_Read) already de-quantizes to float, so
         // enabling the flag is all that's needed to load such meshes. The two
         // material extensions are only ever listed as "used" (never required), so
         // enabling them is harmless and future-proofs against a file marking them
         // required.
         fastgltf::Parser pParser (fastgltf::Extensions::KHR_mesh_quantization
                                 | fastgltf::Extensions::KHR_materials_emissive_strength
                                 | fastgltf::Extensions::KHR_materials_clearcoat);
         auto expAsset = pParser.loadGltf (expBuffer.get (), std::filesystem::path (), fastgltf::Options::None);
         if (expAsset)
         {
            const fastgltf::Asset& asset = expAsset.get ();

            Materials_Map (asset, model);
            Textures_Map (asset, model);
            Meshes_Map (asset, model);
            Nodes_Map (asset, model);

            bResult = true;
         }
         else
         {
            sError = std::string (fastgltf::getErrorMessage (expAsset.error ()));
         }
      }
      else
      {
         sError = std::string (fastgltf::getErrorMessage (expBuffer.error ()));
      }
   }
   else
   {
      sError = "empty glTF data";
   }

   return bResult;
}
