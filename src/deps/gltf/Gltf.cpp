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
#include <meshoptimizer.h>
#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>

#include <cstddef>
#include <cstring>
#include <memory>

using namespace SNEEZE::DEP;

namespace
{
   struct BUFFER_ADAPTER
   {
      const std::vector<std::vector<std::byte>>* pDecompressed = nullptr;
      fastgltf::DefaultBufferDataAdapter         Default;

      auto operator() (const fastgltf::Asset& asset, const std::size_t nBufferView) const
      {
         fastgltf::span<const std::byte> Bytes;

         if (pDecompressed  &&  nBufferView < pDecompressed->size ()  &&  !(*pDecompressed)[nBufferView].empty ())
         {
            const std::vector<std::byte>& aView = (*pDecompressed)[nBufferView];
            Bytes = fastgltf::span<const std::byte> (aView.data (), aView.size ());
         }
         else
            Bytes = Default (asset, nBufferView);

         return Bytes;
      }
   };

   bool Buffer_Bytes (const fastgltf::Asset& asset, size_t nBuffer, size_t nOffset, size_t nLength, const std::byte*& pOut, size_t& nOut)
   {
      bool bResult = false;

      pOut = nullptr;
      nOut = 0;

      if (nBuffer < asset.buffers.size ())
      {
         std::visit (fastgltf::visitor
         {
            [] (const auto&) {},
            [&] (const fastgltf::sources::Array& array)
            {
               if (nOffset + nLength <= array.bytes.size_bytes ())
               {
                  pOut = array.bytes.data () + nOffset;
                  nOut = nLength;
                  bResult = true;
               }
            },
            [&] (const fastgltf::sources::Vector& vector)
            {
               if (nOffset + nLength <= vector.bytes.size ())
               {
                  pOut = vector.bytes.data () + nOffset;
                  nOut = nLength;
                  bResult = true;
               }
            },
            [&] (const fastgltf::sources::ByteView& view)
            {
               if (nOffset + nLength <= view.bytes.size ())
               {
                  pOut = view.bytes.data () + nOffset;
                  nOut = nLength;
                  bResult = true;
               }
            },
         }, asset.buffers[nBuffer].data);
      }

      return bResult;
   }

   bool Meshopt_Decompress (const fastgltf::Asset& asset, std::vector<std::vector<std::byte>>& aDecompressed, std::string& sError)
   {
      bool bResult = true;

      aDecompressed.assign (asset.bufferViews.size (), std::vector<std::byte> ());

      for (size_t nView = 0; bResult  &&  nView < asset.bufferViews.size (); nView++)
      {
         const fastgltf::BufferView& view = asset.bufferViews[nView];
         if (view.meshoptCompression)
         {
            const fastgltf::CompressedBufferView& compression = *view.meshoptCompression;
            const std::byte* pSource = nullptr;
            size_t           nSource = 0;

            if (!Buffer_Bytes (asset, compression.bufferIndex, compression.byteOffset, compression.byteLength, pSource, nSource))
            {
               sError = "EXT_meshopt_compression: compressed buffer view is out of range";
               bResult = false;
            }
            else
            {
               const size_t nStride = compression.byteStride;
               const size_t nCount  = compression.count;
               std::vector<std::byte>& aOut = aDecompressed[nView];
               aOut.resize (nCount * nStride);

               int nCode = 0;
               if (compression.mode == fastgltf::MeshoptCompressionMode::Attributes)
                  nCode = meshopt_decodeVertexBuffer (aOut.data (), nCount, nStride, reinterpret_cast<const unsigned char*> (pSource), nSource);
               else if (compression.mode == fastgltf::MeshoptCompressionMode::Triangles)
                  nCode = meshopt_decodeIndexBuffer (aOut.data (), nCount, nStride, reinterpret_cast<const unsigned char*> (pSource), nSource);
               else
                  nCode = meshopt_decodeIndexSequence (aOut.data (), nCount, nStride, reinterpret_cast<const unsigned char*> (pSource), nSource);

               if (nCode != 0)
               {
                  sError = "EXT_meshopt_compression: decode failed";
                  bResult = false;
               }
               else if (compression.filter == fastgltf::MeshoptCompressionFilter::Octahedral)
                  meshopt_decodeFilterOct (aOut.data (), nCount, nStride);
               else if (compression.filter == fastgltf::MeshoptCompressionFilter::Quaternion)
                  meshopt_decodeFilterQuat (aOut.data (), nCount, nStride);
               else if (compression.filter == fastgltf::MeshoptCompressionFilter::Exponential)
                  meshopt_decodeFilterExp (aOut.data (), nCount, nStride);
            }
         }
      }

      if (!bResult)
         aDecompressed.clear ();

      return bResult;
   }

   // Copies one vertex attribute accessor into a flat float stream, N components
   // per element. fastgltf converts component types and de-normalizes for us.
   template <typename VEC, typename ADAPTER>
   void Stream_Read (const fastgltf::Asset& asset, const fastgltf::Accessor& accessor, std::vector<float>& aOut, int nComponents, const ADAPTER& adapter)
   {
      const size_t nFloat = accessor.count * static_cast<size_t> (nComponents);
      aOut.resize (nFloat);

      size_t nWrite = 0;
      fastgltf::iterateAccessor<VEC> (asset, accessor,
         [&] (VEC value)
         {
            for (int n = 0; n < nComponents; ++n)
            {
               if (nWrite < nFloat)
                  aOut[nWrite++] = static_cast<float> (value[n]);
            }
         }, adapter);
   }

   bool Draco_FillAttribute (const draco::PointAttribute* pAttr, uint32_t nPoint, int nComponent, std::vector<float>& aOut)
   {
      bool bResult = false;

      if (pAttr  &&  nPoint > 0  &&  pAttr->num_components () >= nComponent)
      {
         aOut.assign (static_cast<size_t> (nPoint) * static_cast<size_t> (nComponent), 0.0f);
         bResult = true;

         for (uint32_t nPointIx = 0; bResult  &&  nPointIx < nPoint; nPointIx++)
         {
            float aValue[4] = {};
            if (!pAttr->ConvertValue (pAttr->mapped_index (draco::PointIndex (nPointIx)), static_cast<int8_t> (nComponent), aValue))
               bResult = false;
            else
            {
               for (int nComp = 0; nComp < nComponent; nComp++)
                  aOut[static_cast<size_t> (nPointIx) * static_cast<size_t> (nComponent) + static_cast<size_t> (nComp)] = aValue[nComp];
            }
         }

         if (!bResult)
            aOut.clear ();
      }

      return bResult;
   }

   bool Draco_Map (const fastgltf::Asset& asset, const fastgltf::Primitive& prim, GLTF_PRIMITIVE& out, std::string& sError)
   {
      bool bResult = false;

      const fastgltf::DracoCompressedPrimitive& Compression = *prim.dracoCompression;
      if (Compression.bufferView >= asset.bufferViews.size ())
         sError = "KHR_draco_mesh_compression: buffer view is out of range";
      else
      {
         const fastgltf::BufferView& View = asset.bufferViews[Compression.bufferView];
         const std::byte* pSource = nullptr;
         size_t           nSource = 0;

         if (!Buffer_Bytes (asset, View.bufferIndex, View.byteOffset, View.byteLength, pSource, nSource))
            sError = "KHR_draco_mesh_compression: compressed buffer view is out of range";
         else
         {
            draco::DecoderBuffer Buffer;
            Buffer.Init (reinterpret_cast<const char*> (pSource), nSource);

            draco::Decoder Decoder;
            auto MeshOr = Decoder.DecodeMeshFromBuffer (&Buffer);
            if (!MeshOr.ok ())
               sError = "KHR_draco_mesh_compression: decode failed";
            else
            {
               std::unique_ptr<draco::Mesh> pMesh = std::move (MeshOr).value ();
               const uint32_t nPoint = pMesh->num_points ();

               auto itPositionId = Compression.findAttribute ("POSITION");
               if (itPositionId == Compression.attributes.cend ())
                  sError = "KHR_draco_mesh_compression: POSITION unique id is missing";
               else
               {
                  const draco::PointAttribute* pPosition = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itPositionId->accessorIndex));
                  if (!Draco_FillAttribute (pPosition, nPoint, 3, out.aPosition))
                     sError = "KHR_draco_mesh_compression: POSITION decode failed";
                  else
                  {
                     auto itNormalId = Compression.findAttribute ("NORMAL");
                     if (itNormalId != Compression.attributes.cend ())
                     {
                        const draco::PointAttribute* pNormal = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itNormalId->accessorIndex));
                        Draco_FillAttribute (pNormal, nPoint, 3, out.aNormal);
                     }

                     auto itTexCoordId = Compression.findAttribute ("TEXCOORD_0");
                     if (itTexCoordId != Compression.attributes.cend ())
                     {
                        const draco::PointAttribute* pTexCoord = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itTexCoordId->accessorIndex));
                        Draco_FillAttribute (pTexCoord, nPoint, 2, out.aTexCoord);
                     }

                     const uint32_t nIndex = pMesh->num_faces () * 3;
                     out.aIndex.resize (nIndex);
                     uint32_t nWrite = 0;
                     for (uint32_t nFace = 0; nFace < pMesh->num_faces (); nFace++)
                     {
                        const draco::Mesh::Face Face = pMesh->face (draco::FaceIndex (nFace));
                        out.aIndex[nWrite++] = Face[0].value ();
                        out.aIndex[nWrite++] = Face[1].value ();
                        out.aIndex[nWrite++] = Face[2].value ();
                     }

                     bResult = true;
                  }
               }
            }
         }
      }

      return bResult;
   }

   void Bound_FromPosition (GLTF_PRIMITIVE& out)
   {
      const size_t nVertex = out.aPosition.size () / 3;
      if (nVertex > 0)
      {
         out.aBoundMin[0] = out.aPosition[0];
         out.aBoundMin[1] = out.aPosition[1];
         out.aBoundMin[2] = out.aPosition[2];
         out.aBoundMax[0] = out.aBoundMin[0];
         out.aBoundMax[1] = out.aBoundMin[1];
         out.aBoundMax[2] = out.aBoundMin[2];

         for (size_t nVertexIx = 1; nVertexIx < nVertex; nVertexIx++)
         {
            const float fX = out.aPosition[nVertexIx * 3 + 0];
            const float fY = out.aPosition[nVertexIx * 3 + 1];
            const float fZ = out.aPosition[nVertexIx * 3 + 2];
            if (fX < out.aBoundMin[0]) out.aBoundMin[0] = fX;
            if (fY < out.aBoundMin[1]) out.aBoundMin[1] = fY;
            if (fZ < out.aBoundMin[2]) out.aBoundMin[2] = fZ;
            if (fX > out.aBoundMax[0]) out.aBoundMax[0] = fX;
            if (fY > out.aBoundMax[1]) out.aBoundMax[1] = fY;
            if (fZ > out.aBoundMax[2]) out.aBoundMax[2] = fZ;
         }

         out.bBound = true;
      }
   }

   template <typename ADAPTER>
   bool Primitive_Map (const fastgltf::Asset& asset, const fastgltf::Primitive& prim, GLTF_PRIMITIVE& out, const ADAPTER& adapter, std::string& sError)
   {
      bool bResult = true;

      if (prim.type == fastgltf::PrimitiveType::Triangles)
      {
         if (prim.dracoCompression)
            bResult = Draco_Map (asset, prim, out, sError);
         else
         {
            auto itPosition = prim.findAttribute ("POSITION");
            if (itPosition != prim.attributes.cend ())
               Stream_Read<fastgltf::math::fvec3> (asset, asset.accessors[itPosition->accessorIndex], out.aPosition, 3, adapter);

            auto itNormal = prim.findAttribute ("NORMAL");
            if (itNormal != prim.attributes.cend ())
               Stream_Read<fastgltf::math::fvec3> (asset, asset.accessors[itNormal->accessorIndex], out.aNormal, 3, adapter);

            auto itTexCoord = prim.findAttribute ("TEXCOORD_0");
            if (itTexCoord != prim.attributes.cend ())
               Stream_Read<fastgltf::math::fvec2> (asset, asset.accessors[itTexCoord->accessorIndex], out.aTexCoord, 2, adapter);

            if (prim.indicesAccessor.has_value ())
            {
               const fastgltf::Accessor& accessor = asset.accessors[*prim.indicesAccessor];
               out.aIndex.resize (accessor.count);
               size_t nWrite = 0;
               fastgltf::iterateAccessor<std::uint32_t> (asset, accessor,
                  [&] (std::uint32_t nIndex)
                  {
                     if (nWrite < out.aIndex.size ())
                        out.aIndex[nWrite++] = nIndex;
                  }, adapter);
            }
         }

         if (bResult)
         {
            out.nMaterial = prim.materialIndex.has_value () ? static_cast<int> (*prim.materialIndex) : -1;
            Bound_FromPosition (out);
         }
      }

      return bResult;
   }

   template <typename ADAPTER>
   bool Meshes_Map (const fastgltf::Asset& asset, GLTF_MODEL& model, const ADAPTER& adapter, std::string& sError)
   {
      bool bResult = true;

      model.aMesh.reserve (asset.meshes.size ());
      for (const fastgltf::Mesh& mesh : asset.meshes)
      {
         if (!bResult)
            break;

         GLTF_MESH meshOut;
         meshOut.aPrimitive.reserve (mesh.primitives.size ());
         for (const fastgltf::Primitive& prim : mesh.primitives)
         {
            if (!bResult)
               break;

            GLTF_PRIMITIVE primOut;
            if (!Primitive_Map (asset, prim, primOut, adapter, sError))
               bResult = false;
            else
               meshOut.aPrimitive.push_back (std::move (primOut));
         }
         if (bResult)
            model.aMesh.push_back (std::move (meshOut));
      }

      return bResult;
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
         materialOut.bDoubleSided      = material.doubleSided;
         // alphaMode: 0=opaque, 1=mask, 2=blend (Halogen physicallyBased)
         switch (material.alphaMode)
         {
            case fastgltf::AlphaMode::Mask:  materialOut.nAlphaMode = 1; break;
            case fastgltf::AlphaMode::Blend: materialOut.nAlphaMode = 2; break;
            default:                         materialOut.nAlphaMode = 0; break;
         }
         materialOut.fAlphaCutoff = static_cast<float> (material.alphaCutoff);
         model.aMaterial.push_back (materialOut);
      }
   }

   template <typename ADAPTER>
   void Textures_Map (const fastgltf::Asset& asset, GLTF_MODEL& model, const ADAPTER& adapter)
   {
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
         // enabling the flag is all that's needed to load such meshes. Draco and
         // meshopt are also commonly REQUIRED on large GLBs; those need a decode
         // pass (Draco_Map / Meshopt_Decompress), not just the parser flag.
         fastgltf::Parser pParser (fastgltf::Extensions::KHR_mesh_quantization
                                 | fastgltf::Extensions::KHR_materials_emissive_strength
                                 | fastgltf::Extensions::KHR_materials_clearcoat
                                 | fastgltf::Extensions::KHR_texture_transform
                                 | fastgltf::Extensions::KHR_materials_unlit
                                 | fastgltf::Extensions::KHR_texture_basisu
                                 | fastgltf::Extensions::EXT_texture_webp
                                 | fastgltf::Extensions::EXT_meshopt_compression
                                 | fastgltf::Extensions::KHR_draco_mesh_compression);
         auto expAsset = pParser.loadGltf (expBuffer.get (), std::filesystem::path (), fastgltf::Options::None);
         if (expAsset)
         {
            const fastgltf::Asset& asset = expAsset.get ();
            std::vector<std::vector<std::byte>> aDecompressed;

            if (Meshopt_Decompress (asset, aDecompressed, sError))
            {
               BUFFER_ADAPTER adapter;
               adapter.pDecompressed = aDecompressed.empty () ? nullptr : &aDecompressed;

               Materials_Map (asset, model);
               Textures_Map (asset, model, adapter);
               if (Meshes_Map (asset, model, adapter, sError))
               {
                  Nodes_Map (asset, model);
                  bResult = true;
               }
            }
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
