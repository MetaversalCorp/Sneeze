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
#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
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

   bool Draco_FillJoints (const draco::PointAttribute* pAttr, uint32_t nPoint, std::vector<uint16_t>& aOut)
   {
      bool bResult = false;

      if (pAttr  &&  nPoint > 0  &&  pAttr->num_components () >= 4)
      {
         aOut.assign (static_cast<size_t> (nPoint) * 4, 0);
         bResult = true;

         for (uint32_t nPointIx = 0; bResult  &&  nPointIx < nPoint; nPointIx++)
         {
            float aValue[4] = {};
            if (!pAttr->ConvertValue (pAttr->mapped_index (draco::PointIndex (nPointIx)), 4, aValue))
               bResult = false;
            else
            {
               for (int nComp = 0; nComp < 4; nComp++)
               {
                  float fJoint = aValue[nComp];
                  if (fJoint < 0.0f)
                     fJoint = 0.0f;
                  aOut[static_cast<size_t> (nPointIx) * 4 + static_cast<size_t> (nComp)] =
                     static_cast<uint16_t> (fJoint + 0.5f);
               }
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

                     auto itTexCoord1Id = Compression.findAttribute ("TEXCOORD_1");
                     if (itTexCoord1Id != Compression.attributes.cend ())
                     {
                        const draco::PointAttribute* pTexCoord1 = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itTexCoord1Id->accessorIndex));
                        Draco_FillAttribute (pTexCoord1, nPoint, 2, out.aTexCoord1);
                     }

                     auto itTangentId = Compression.findAttribute ("TANGENT");
                     if (itTangentId != Compression.attributes.cend ())
                     {
                        const draco::PointAttribute* pTangent = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itTangentId->accessorIndex));
                        Draco_FillAttribute (pTangent, nPoint, 4, out.aTangent);
                     }

                     auto itJointId = Compression.findAttribute ("JOINTS_0");
                     if (itJointId != Compression.attributes.cend ())
                     {
                        const draco::PointAttribute* pJoint = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itJointId->accessorIndex));
                        Draco_FillJoints (pJoint, nPoint, out.aJoint);
                     }

                     auto itWeightId = Compression.findAttribute ("WEIGHTS_0");
                     if (itWeightId != Compression.attributes.cend ())
                     {
                        const draco::PointAttribute* pWeight = pMesh->GetAttributeByUniqueId (static_cast<uint32_t> (itWeightId->accessorIndex));
                        Draco_FillAttribute (pWeight, nPoint, 4, out.aWeight);
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
   void SkinAttributes_Read (const fastgltf::Asset& asset, const fastgltf::Primitive& prim, GLTF_PRIMITIVE& out, const ADAPTER& adapter)
   {
      // Draco_Map already fills JOINTS_0 / WEIGHTS_0 when they live in the
      // compressed blob. Those accessors often have no buffer view; iterating
      // them would overwrite the decoded streams with zeros.
      if (out.aJoint.empty ())
      {
         auto itJoint = prim.findAttribute ("JOINTS_0");
         if (itJoint != prim.attributes.cend ())
         {
            const fastgltf::Accessor& accessor = asset.accessors[itJoint->accessorIndex];
            out.aJoint.resize (accessor.count * 4);
            size_t nWrite = 0;
            if (accessor.componentType == fastgltf::ComponentType::UnsignedByte)
            {
               fastgltf::iterateAccessor<fastgltf::math::u8vec4> (asset, accessor,
                  [&] (fastgltf::math::u8vec4 value)
                  {
                     if (nWrite + 4 <= out.aJoint.size ())
                     {
                        out.aJoint[nWrite++] = value[0];
                        out.aJoint[nWrite++] = value[1];
                        out.aJoint[nWrite++] = value[2];
                        out.aJoint[nWrite++] = value[3];
                     }
                  }, adapter);
            }
            else
            {
               fastgltf::iterateAccessor<fastgltf::math::u16vec4> (asset, accessor,
                  [&] (fastgltf::math::u16vec4 value)
                  {
                     if (nWrite + 4 <= out.aJoint.size ())
                     {
                        out.aJoint[nWrite++] = value[0];
                        out.aJoint[nWrite++] = value[1];
                        out.aJoint[nWrite++] = value[2];
                        out.aJoint[nWrite++] = value[3];
                     }
                  }, adapter);
            }
         }
      }

      if (out.aWeight.empty ())
      {
         auto itWeight = prim.findAttribute ("WEIGHTS_0");
         if (itWeight != prim.attributes.cend ())
            Stream_Read<fastgltf::math::fvec4> (asset, asset.accessors[itWeight->accessorIndex], out.aWeight, 4, adapter);
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

            auto itTexCoord1 = prim.findAttribute ("TEXCOORD_1");
            if (itTexCoord1 != prim.attributes.cend ())
               Stream_Read<fastgltf::math::fvec2> (asset, asset.accessors[itTexCoord1->accessorIndex], out.aTexCoord1, 2, adapter);

            auto itTangent = prim.findAttribute ("TANGENT");
            if (itTangent != prim.attributes.cend ())
               Stream_Read<fastgltf::math::fvec4> (asset, asset.accessors[itTangent->accessorIndex], out.aTangent, 4, adapter);

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
            SkinAttributes_Read (asset, prim, out, adapter);

         if (bResult)
         {
            const size_t nVertex = out.aPosition.size () / 3;
            if (out.aJoint.size () != nVertex * 4  ||  out.aWeight.size () != nVertex * 4)
            {
               out.aJoint.clear ();
               out.aWeight.clear ();
            }
            if (out.aTexCoord1.size () != nVertex * 2)
               out.aTexCoord1.clear ();
            if (out.aTangent.size () != nVertex * 4)
               out.aTangent.clear ();

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

   void UvTransform_Apply (const fastgltf::TextureInfo& info, GLTF_UVX& uv)
   {
      size_t nCoord = info.texCoordIndex;
      if (info.transform)
      {
         if (info.transform->texCoordIndex.has_value ())
            nCoord = *info.transform->texCoordIndex;
         uv.dOffset[0] = static_cast<float> (info.transform->uvOffset[0]);
         uv.dOffset[1] = static_cast<float> (info.transform->uvOffset[1]);
         uv.dRotation  = static_cast<float> (info.transform->rotation);
         uv.dScale[0]  = static_cast<float> (info.transform->uvScale[0]);
         uv.dScale[1]  = static_cast<float> (info.transform->uvScale[1]);
      }
      uv.nTexCoord = (nCoord >= 1) ? 1 : 0;
   }

   template <typename T>
   void UvTransform_Map (const T& info, GLTF_UVX& uv)
   {
      if (info.has_value ())
         UvTransform_Apply (*info, uv);
   }

   void Materials_Map (const fastgltf::Asset& asset, GLTF_MODEL& model)
   {
      bool bMtoon = false;
      for (const auto& sExt : asset.extensionsUsed)
      {
         if (sExt == "VRMC_materials_mtoon")
            bMtoon = true;
      }

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
         const float fEmissiveStrength = static_cast<float> (material.emissiveStrength);
         materialOut.emissive[0]       = static_cast<float> (material.emissiveFactor[0]) * fEmissiveStrength;
         materialOut.emissive[1]       = static_cast<float> (material.emissiveFactor[1]) * fEmissiveStrength;
         materialOut.emissive[2]       = static_cast<float> (material.emissiveFactor[2]) * fEmissiveStrength;
         materialOut.nBaseColorTexture = material.pbrData.baseColorTexture.has_value ()
            ? static_cast<int> ((*material.pbrData.baseColorTexture).textureIndex)
            : -1;
         materialOut.nEmissiveTexture  = material.emissiveTexture.has_value ()
            ? static_cast<int> ((*material.emissiveTexture).textureIndex)
            : -1;
         materialOut.nMetallicRoughnessTexture = material.pbrData.metallicRoughnessTexture.has_value ()
            ? static_cast<int> ((*material.pbrData.metallicRoughnessTexture).textureIndex)
            : -1;
         materialOut.nNormalTexture = material.normalTexture.has_value ()
            ? static_cast<int> ((*material.normalTexture).textureIndex)
            : -1;
         materialOut.nOcclusionTexture = material.occlusionTexture.has_value ()
            ? static_cast<int> ((*material.occlusionTexture).textureIndex)
            : -1;
         if (material.normalTexture.has_value ())
            materialOut.dNormalScale = static_cast<float> ((*material.normalTexture).scale);
         if (material.occlusionTexture.has_value ())
            materialOut.dOcclusionStrength = static_cast<float> ((*material.occlusionTexture).strength);

         UvTransform_Map (material.pbrData.baseColorTexture, materialOut.uvBaseColor);
         UvTransform_Map (material.emissiveTexture, materialOut.uvEmissive);
         UvTransform_Map (material.pbrData.metallicRoughnessTexture, materialOut.uvMetallicRoughness);
         UvTransform_Map (material.normalTexture, materialOut.uvNormal);
         UvTransform_Map (material.occlusionTexture, materialOut.uvOcclusion);

         // VRM 1.0 MToon stamps KHR_materials_unlit as a fallback for viewers
         // that do not know MToon. UniVRM also leaves metallicFactor at 1.
         // Treat MToon as a dielectric here; Vrm_Extras_Map clears bUnlit
         // per material when VRMC_materials_mtoon is present.
         materialOut.bUnlit = material.unlit;
         if (material.unlit  ||  bMtoon)
            materialOut.dMetallic = 0.0f;

         materialOut.bDoubleSided = material.doubleSided;

         materialOut.eAlpha = GLTF_MATERIAL::kOPAQUE;
         if (material.alphaMode == fastgltf::AlphaMode::Mask)
            materialOut.eAlpha = GLTF_MATERIAL::kMASK;
         else if (material.alphaMode == fastgltf::AlphaMode::Blend)
            materialOut.eAlpha = GLTF_MATERIAL::kBLEND;
         materialOut.dAlphaCutoff = static_cast<float> (material.alphaCutoff);

         model.aMaterial.push_back (materialOut);
      }
   }

   GLTF_TEXTURE::eWRAP Wrap_Map (fastgltf::Wrap wrap)
   {
      GLTF_TEXTURE::eWRAP e = GLTF_TEXTURE::kREPEAT;

      if (wrap == fastgltf::Wrap::ClampToEdge)
         e = GLTF_TEXTURE::kCLAMP;
      else if (wrap == fastgltf::Wrap::MirroredRepeat)
         e = GLTF_TEXTURE::kMIRROR;

      return e;
   }

   GLTF_TEXTURE::eFILTER Filter_Map (fastgltf::Filter filter)
   {
      GLTF_TEXTURE::eFILTER e = GLTF_TEXTURE::kLINEAR;

      if (filter == fastgltf::Filter::Nearest
       ||  filter == fastgltf::Filter::NearestMipMapNearest)
         e = GLTF_TEXTURE::kNEAREST;

      return e;
   }

   template <typename ADAPTER>
   void Textures_Map (const fastgltf::Asset& asset, GLTF_MODEL& model, const ADAPTER& adapter)
   {
      model.aTexture.reserve (asset.textures.size ());
      for (const fastgltf::Texture& texture : asset.textures)
      {
         GLTF_TEXTURE textureOut;

         if (texture.samplerIndex.has_value ()  &&  *texture.samplerIndex < asset.samplers.size ())
         {
            const fastgltf::Sampler& sampler = asset.samplers[*texture.samplerIndex];
            textureOut.eWrapS = Wrap_Map (sampler.wrapS);
            textureOut.eWrapT = Wrap_Map (sampler.wrapT);
            if (sampler.magFilter.has_value ())
               textureOut.eMag = Filter_Map (*sampler.magFilter);
            if (sampler.minFilter.has_value ())
               textureOut.eMin = Filter_Map (*sampler.minFilter);
         }

         // Standard textures name their image via imageIndex. EXT_texture_webp
         // (like KHR_texture_basisu) instead files the image under its own
         // extension index and leaves imageIndex empty; fall back to the WebP
         // image so WebP-only assets (common output of glTF-Transform) still
         // yield encoded bytes. IMAGE::Decode routes the WebP bytes to libwebp.
         auto nImage = texture.imageIndex;
         if (!nImage.has_value ())
            nImage = texture.webpImageIndex;

         if (nImage.has_value ())
         {
            const fastgltf::Image& image = asset.images[*nImage];
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

   void Node_Rest (const fastgltf::Node& node, GLTF_NODE& nodeOut)
   {
      nodeOut.aTranslation[0] = 0.0;
      nodeOut.aTranslation[1] = 0.0;
      nodeOut.aTranslation[2] = 0.0;
      nodeOut.aRotation[0]    = 0.0;
      nodeOut.aRotation[1]    = 0.0;
      nodeOut.aRotation[2]    = 0.0;
      nodeOut.aRotation[3]    = 1.0;
      nodeOut.aScale[0]       = 1.0;
      nodeOut.aScale[1]       = 1.0;
      nodeOut.aScale[2]       = 1.0;

      std::visit (fastgltf::visitor
      {
         [&] (const fastgltf::TRS& trs)
         {
            nodeOut.aTranslation[0] = trs.translation[0];
            nodeOut.aTranslation[1] = trs.translation[1];
            nodeOut.aTranslation[2] = trs.translation[2];
            nodeOut.aRotation[0]    = trs.rotation[0];
            nodeOut.aRotation[1]    = trs.rotation[1];
            nodeOut.aRotation[2]    = trs.rotation[2];
            nodeOut.aRotation[3]    = trs.rotation[3];
            nodeOut.aScale[0]       = trs.scale[0];
            nodeOut.aScale[1]       = trs.scale[1];
            nodeOut.aScale[2]       = trs.scale[2];
         },
         [&] (const fastgltf::math::fmat4x4& matrix)
         {
            nodeOut.aTranslation[0] = matrix[3][0];
            nodeOut.aTranslation[1] = matrix[3][1];
            nodeOut.aTranslation[2] = matrix[3][2];
            auto ColumnLen = [] (const fastgltf::math::fmat4x4& mat, int nCol) -> double
            {
               double dX = mat[nCol][0];
               double dY = mat[nCol][1];
               double dZ = mat[nCol][2];
               return std::sqrt (dX * dX + dY * dY + dZ * dZ);
            };
            nodeOut.aScale[0] = ColumnLen (matrix, 0);
            nodeOut.aScale[1] = ColumnLen (matrix, 1);
            nodeOut.aScale[2] = ColumnLen (matrix, 2);
         },
      }, node.transform);
   }

   void Nodes_Map (const fastgltf::Asset& asset, GLTF_MODEL& model)
   {
      model.aNode.reserve (asset.nodes.size ());
      for (const fastgltf::Node& node : asset.nodes)
      {
         GLTF_NODE nodeOut;

         Node_Rest (node, nodeOut);

         fastgltf::math::fmat4x4 matrix = fastgltf::getTransformMatrix (node);
         for (int nColumn = 0; nColumn < 4; ++nColumn)
            for (int nRow = 0; nRow < 4; ++nRow)
               nodeOut.transform.d[nColumn * 4 + nRow] = matrix[nColumn][nRow];

         nodeOut.nMesh = node.meshIndex.has_value () ? static_cast<int> (*node.meshIndex) : -1;
         nodeOut.nSkin = node.skinIndex.has_value () ? static_cast<int> (*node.skinIndex) : -1;

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

   MAT4 Mat4_Identity ()
   {
      MAT4 mat = {};
      mat.d[0]  = 1.0;
      mat.d[5]  = 1.0;
      mat.d[10] = 1.0;
      mat.d[15] = 1.0;
      return mat;
   }

   template <typename ADAPTER>
   void Skins_Map (const fastgltf::Asset& asset, GLTF_MODEL& model, const ADAPTER& adapter)
   {
      model.aSkin.reserve (asset.skins.size ());
      for (const fastgltf::Skin& skin : asset.skins)
      {
         GLTF_SKIN skinOut;
         skinOut.nSkeleton = skin.skeleton.has_value () ? static_cast<int> (*skin.skeleton) : -1;
         skinOut.aJoint.reserve (skin.joints.size ());
         for (std::size_t nJoint : skin.joints)
            skinOut.aJoint.push_back (static_cast<int> (nJoint));

         const MAT4 matIdentity = Mat4_Identity ();
         skinOut.aInverseBind.assign (skinOut.aJoint.size (), matIdentity);

         if (skin.inverseBindMatrices.has_value ())
         {
            const fastgltf::Accessor& accessor = asset.accessors[*skin.inverseBindMatrices];
            size_t nWrite = 0;
            fastgltf::iterateAccessor<fastgltf::math::fmat4x4> (asset, accessor,
               [&] (fastgltf::math::fmat4x4 matrix)
               {
                  if (nWrite < skinOut.aInverseBind.size ())
                  {
                     MAT4& matIbm = skinOut.aInverseBind[nWrite];
                     for (int nColumn = 0; nColumn < 4; ++nColumn)
                        for (int nRow = 0; nRow < 4; ++nRow)
                           matIbm.d[nColumn * 4 + nRow] = matrix[nColumn][nRow];
                     nWrite++;
                  }
               }, adapter);
         }

         model.aSkin.push_back (std::move (skinOut));
      }
   }

   template <typename ADAPTER>
   void Accessor_Floats (const fastgltf::Asset& asset, const fastgltf::Accessor& accessor, int nComp, std::vector<float>& aOut, const ADAPTER& adapter)
   {
      aOut.clear ();
      aOut.reserve (accessor.count * static_cast<size_t> (nComp));

      if (nComp == 1)
      {
         fastgltf::iterateAccessor<float> (asset, accessor,
            [&] (float fValue)
            {
               aOut.push_back (fValue);
            }, adapter);
      }
      else if (nComp == 3)
      {
         fastgltf::iterateAccessor<fastgltf::math::fvec3> (asset, accessor,
            [&] (fastgltf::math::fvec3 v)
            {
               aOut.push_back (v[0]);
               aOut.push_back (v[1]);
               aOut.push_back (v[2]);
            }, adapter);
      }
      else if (nComp == 4)
      {
         fastgltf::iterateAccessor<fastgltf::math::fvec4> (asset, accessor,
            [&] (fastgltf::math::fvec4 v)
            {
               aOut.push_back (v[0]);
               aOut.push_back (v[1]);
               aOut.push_back (v[2]);
               aOut.push_back (v[3]);
            }, adapter);
      }
   }

   template <typename ADAPTER>
   void Animations_Map (const fastgltf::Asset& asset, GLTF_MODEL& model, const ADAPTER& adapter)
   {
      model.aAnimation.reserve (asset.animations.size ());
      for (const fastgltf::Animation& anim : asset.animations)
      {
         GLTF_ANIMATION animOut;
         animOut.sName = std::string (anim.name);

         for (const fastgltf::AnimationChannel& channel : anim.channels)
         {
            if (!channel.nodeIndex.has_value ())
               continue;
            if (channel.path == fastgltf::AnimationPath::Weights)
               continue;
            if (channel.samplerIndex >= anim.samplers.size ())
               continue;

            const fastgltf::AnimationSampler& sampler = anim.samplers[channel.samplerIndex];
            if (sampler.inputAccessor >= asset.accessors.size ()  ||  sampler.outputAccessor >= asset.accessors.size ())
               continue;

            GLTF_CHANNEL channelOut;
            channelOut.nNode = static_cast<int> (*channel.nodeIndex);
            if (channel.path == fastgltf::AnimationPath::Rotation)
               channelOut.ePath = GLTF_CHANNEL::kROTATION;
            else if (channel.path == fastgltf::AnimationPath::Scale)
               channelOut.ePath = GLTF_CHANNEL::kSCALE;
            else
               channelOut.ePath = GLTF_CHANNEL::kTRANSLATION;

            if (sampler.interpolation == fastgltf::AnimationInterpolation::Step)
               channelOut.eInterp = GLTF_CHANNEL::kSTEP;
            else if (sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline)
               channelOut.eInterp = GLTF_CHANNEL::kCUBIC;
            else
               channelOut.eInterp = GLTF_CHANNEL::kLINEAR;

            const int nComp = (channelOut.ePath == GLTF_CHANNEL::kROTATION) ? 4 : 3;
            Accessor_Floats (asset, asset.accessors[sampler.inputAccessor], 1, channelOut.aTime, adapter);
            Accessor_Floats (asset, asset.accessors[sampler.outputAccessor], nComp, channelOut.aValue, adapter);

            if (!channelOut.aTime.empty ())
            {
               const double dEnd = channelOut.aTime.back ();
               if (dEnd > animOut.dDuration)
                  animOut.dDuration = dEnd;
            }

            animOut.aChannel.push_back (std::move (channelOut));
         }

         model.aAnimation.push_back (std::move (animOut));
      }
   }

   // VRM 1.0 (.vrm) is a GLB that lists VRMC_* in extensionsRequired. fastgltf
   // has no VRMC parsers and rejects the whole file as UnknownRequiredExtension.
   // Those extensions are avatar extras (humanoid, MToon, spring bones) -- the
   // mesh/skin/embedded images are ordinary glTF. Drop only the VRMC_*/VRM
   // names from extensionsRequired so the rest of the asset loads.
   bool Extension_IsVrm (const std::string& sName)
   {
      bool bVrm = (sName == "VRM");
      if (!bVrm  &&  sName.size () >= 5)
         bVrm = (sName.compare (0, 5, "VRMC_") == 0);
      return bVrm;
   }

   bool Json_HasVrmc (const char* pJson, size_t nJson)
   {
      bool bHas = false;
      if (pJson  &&  nJson >= 5)
      {
         for (size_t nI = 0; !bHas  &&  nI + 5 <= nJson; nI++)
         {
            if (pJson[nI] == 'V'  &&  pJson[nI + 1] == 'R'  &&  pJson[nI + 2] == 'M'
             &&  pJson[nI + 3] == 'C'  &&  pJson[nI + 4] == '_')
               bHas = true;
         }
      }
      return bHas;
   }

   uint32_t U32LE_Read (const uint8_t* p)
   {
      return static_cast<uint32_t> (p[0])
           | (static_cast<uint32_t> (p[1]) << 8)
           | (static_cast<uint32_t> (p[2]) << 16)
           | (static_cast<uint32_t> (p[3]) << 24);
   }

   void U32LE_Write (uint8_t* p, uint32_t n)
   {
      p[0] = static_cast<uint8_t> (n);
      p[1] = static_cast<uint8_t> (n >> 8);
      p[2] = static_cast<uint8_t> (n >> 16);
      p[3] = static_cast<uint8_t> (n >> 24);
   }

   bool Json_StripVrmRequired (nlohmann::json& j)
   {
      bool bChanged = false;

      if (j.contains ("extensionsRequired")  &&  j["extensionsRequired"].is_array ())
      {
         nlohmann::json aKeep = nlohmann::json::array ();
         for (const nlohmann::json& Item : j["extensionsRequired"])
         {
            bool bDrop = false;
            if (Item.is_string ())
               bDrop = Extension_IsVrm (Item.get<std::string> ());
            if (bDrop)
               bChanged = true;
            else
               aKeep.push_back (Item);
         }
         if (bChanged)
            j["extensionsRequired"] = std::move (aKeep);
      }

      return bChanged;
   }

   bool Json_AllowVrm (std::string& sJson)
   {
      bool bChanged = false;

      try
      {
         nlohmann::json j = nlohmann::json::parse (sJson);
         if (Json_StripVrmRequired (j))
         {
            sJson = j.dump ();
            bChanged = true;
         }
      }
      catch (...)
      {
      }

      return bChanged;
   }

   void Glb_Pack (const std::string& sJson, const uint8_t* pRest, size_t nRest, std::vector<uint8_t>& aOut)
   {
      std::string sChunk = sJson;
      while ((sChunk.size () % 4) != 0)
         sChunk.push_back (' ');

      const uint32_t nJson  = static_cast<uint32_t> (sChunk.size ());
      const uint32_t nTotal = 12 + 8 + nJson + static_cast<uint32_t> (nRest);
      aOut.assign (static_cast<size_t> (nTotal), 0);

      aOut[0] = 'g';
      aOut[1] = 'l';
      aOut[2] = 'T';
      aOut[3] = 'F';
      U32LE_Write (aOut.data () + 4, 2);
      U32LE_Write (aOut.data () + 8, nTotal);
      U32LE_Write (aOut.data () + 12, nJson);
      U32LE_Write (aOut.data () + 16, 0x4E4F534A);
      std::memcpy (aOut.data () + 20, sChunk.data (), sChunk.size ());
      if (pRest  &&  nRest > 0)
         std::memcpy (aOut.data () + 20 + sChunk.size (), pRest, nRest);
   }

   bool Json_FromBytes (const uint8_t* pData, size_t nLen, std::string& sJson)
   {
      bool bOk = false;

      sJson.clear ();

      if (pData  &&  nLen >= 20
       &&  pData[0] == 'g'  &&  pData[1] == 'l'  &&  pData[2] == 'T'  &&  pData[3] == 'F')
      {
         const uint32_t nVersion  = U32LE_Read (pData + 4);
         const uint32_t nJsonLen  = U32LE_Read (pData + 12);
         const uint32_t nJsonType = U32LE_Read (pData + 16);
         if (nVersion == 2
          &&  nJsonType == 0x4E4F534A
          &&  20 + nJsonLen <= nLen)
         {
            sJson.assign (reinterpret_cast<const char*> (pData + 20), nJsonLen);
            bOk = true;
         }
      }
      else if (pData  &&  nLen > 0)
      {
         size_t nFirst = 0;
         while (nFirst < nLen  &&  std::isspace (static_cast<unsigned char> (pData[nFirst])))
            nFirst++;
         if (nFirst < nLen  &&  pData[nFirst] == '{')
         {
            sJson.assign (reinterpret_cast<const char*> (pData), nLen);
            bOk = true;
         }
      }

      return bOk;
   }

   int Json_Int (const nlohmann::json& j, const char* szKey, int nDefault)
   {
      int n = nDefault;

      if (j.contains (szKey)  &&  j[szKey].is_number_integer ())
         n = j[szKey].get<int> ();
      else if (j.contains (szKey)  &&  j[szKey].is_number_unsigned ())
         n = static_cast<int> (j[szKey].get<unsigned> ());
      else if (j.contains (szKey)  &&  j[szKey].is_number ())
         n = static_cast<int> (j[szKey].get<double> ());

      return n;
   }

   double Json_Double (const nlohmann::json& j, const char* szKey, double dDefault)
   {
      double d = dDefault;

      if (j.contains (szKey)  &&  j[szKey].is_number ())
         d = j[szKey].get<double> ();

      return d;
   }

   int Aim_Axis (const std::string& sAxis)
   {
      int nAxis = 2;

      if (sAxis == "PositiveX")
         nAxis = 0;
      else if (sAxis == "NegativeX")
         nAxis = 1;
      else if (sAxis == "PositiveY")
         nAxis = 2;
      else if (sAxis == "NegativeY")
         nAxis = 3;
      else if (sAxis == "PositiveZ")
         nAxis = 4;
      else if (sAxis == "NegativeZ")
         nAxis = 5;

      return nAxis;
   }

   int Roll_Axis (const std::string& sAxis)
   {
      int nAxis = 1;

      if (sAxis == "X")
         nAxis = 0;
      else if (sAxis == "Y")
         nAxis = 1;
      else if (sAxis == "Z")
         nAxis = 2;

      return nAxis;
   }

   double Json_Number (const nlohmann::json& j, double dDefault)
   {
      double d = dDefault;

      if (j.is_number ())
         d = j.get<double> ();

      return d;
   }

   void Humanoid_Map (const nlohmann::json& jHuman, GLTF_MODEL& model)
   {
      if (jHuman.contains ("humanBones"))
      {
         const nlohmann::json& jBones = jHuman["humanBones"];
         if (jBones.is_object ())
         {
            for (auto it = jBones.begin (); it != jBones.end (); ++it)
            {
               if (it.value ().is_object ())
               {
                  const int nNode = Json_Int (it.value (), "node", -1);
                  if (nNode >= 0  &&  !it.key ().empty ())
                  {
                     GLTF_HUMANOID bone;
                     bone.sName = it.key ();
                     bone.nNode = nNode;
                     model.aHumanoid.push_back (std::move (bone));
                  }
               }
            }
         }
         else if (jBones.is_array ())
         {
            for (const nlohmann::json& Bone : jBones)
            {
               if (Bone.is_object ())
               {
                  int nNode = Json_Int (Bone, "node", -1);
                  std::string sName;
                  if (Bone.contains ("bone")  &&  Bone["bone"].is_string ())
                     sName = Bone["bone"].get<std::string> ();
                  if (nNode >= 0  &&  !sName.empty ())
                  {
                     GLTF_HUMANOID bone;
                     bone.sName = std::move (sName);
                     bone.nNode = nNode;
                     model.aHumanoid.push_back (std::move (bone));
                  }
               }
            }
         }
      }
   }

   // VRM 0 _BlendMode: 0 Opaque, 1 Cutout, 2 Transparent, 3 TransparentWithZWrite.
   // Only fills in when glTF left the material OPAQUE.
   void Alpha_ApplyVrmBlend (GLTF_MATERIAL& materialOut, double dBlend, double dCutoff)
   {
      if (materialOut.eAlpha == GLTF_MATERIAL::kOPAQUE)
      {
         if (dBlend > 0.5  &&  dBlend < 1.5)
         {
            materialOut.eAlpha       = GLTF_MATERIAL::kMASK;
            materialOut.dAlphaCutoff = static_cast<float> (dCutoff);
         }
         else if (dBlend > 1.5)
            materialOut.eAlpha = GLTF_MATERIAL::kBLEND;
      }
   }

   void Vrm_Extras_Map (const uint8_t* pData, size_t nLen, GLTF_MODEL& model)
   {
      std::string sJson;
      if (Json_FromBytes (pData, nLen, sJson))
      {
         try
         {
            nlohmann::json j = nlohmann::json::parse (sJson);

            if (j.contains ("extensions")  &&  j["extensions"].is_object ())
            {
               const nlohmann::json& ExtRoot = j["extensions"];
               const nlohmann::json* pHuman  = nullptr;
               if (ExtRoot.contains ("VRMC_vrm_animation")  &&  ExtRoot["VRMC_vrm_animation"].is_object ()
                &&  ExtRoot["VRMC_vrm_animation"].contains ("humanoid"))
                  pHuman = &ExtRoot["VRMC_vrm_animation"]["humanoid"];
               else if (ExtRoot.contains ("VRMC_vrm")  &&  ExtRoot["VRMC_vrm"].is_object ()
                &&  ExtRoot["VRMC_vrm"].contains ("humanoid"))
                  pHuman = &ExtRoot["VRMC_vrm"]["humanoid"];
               else if (ExtRoot.contains ("VRM")  &&  ExtRoot["VRM"].is_object ()
                &&  ExtRoot["VRM"].contains ("humanoid"))
                  pHuman = &ExtRoot["VRM"]["humanoid"];
               if (pHuman)
                  Humanoid_Map (*pHuman, model);
            }

            if (j.contains ("materials")  &&  j["materials"].is_array ())
            {
               const nlohmann::json& aMat = j["materials"];
               const size_t nCount = (aMat.size () < model.aMaterial.size ()) ? aMat.size () : model.aMaterial.size ();
               for (size_t nI = 0; nI < nCount; nI++)
               {
                  const nlohmann::json& Mat = aMat[nI];
                  if (Mat.contains ("extensions")  &&  Mat["extensions"].is_object ())
                  {
                     const nlohmann::json& Ext = Mat["extensions"];
                     if (Ext.contains ("VRMC_materials_mtoon")  &&  Ext["VRMC_materials_mtoon"].is_object ())
                     {
                        GLTF_MATERIAL& materialOut = model.aMaterial[nI];
                        materialOut.bUnlit    = false;
                        materialOut.dMetallic = 0.0f;
                        materialOut.dRoughness = 1.0f;
                        const nlohmann::json& Mtoon = Ext["VRMC_materials_mtoon"];
                        if (Mtoon.contains ("shadeColorFactor")  &&  Mtoon["shadeColorFactor"].is_array ()
                         &&  Mtoon["shadeColorFactor"].size () >= 3)
                        {
                           materialOut.shadeColor[0] = static_cast<float> (Json_Number (Mtoon["shadeColorFactor"][0], 1.0));
                           materialOut.shadeColor[1] = static_cast<float> (Json_Number (Mtoon["shadeColorFactor"][1], 1.0));
                           materialOut.shadeColor[2] = static_cast<float> (Json_Number (Mtoon["shadeColorFactor"][2], 1.0));
                        }
                        if (materialOut.eAlpha == GLTF_MATERIAL::kOPAQUE
                         &&  Mtoon.contains ("transparentWithZWrite")  &&  Mtoon["transparentWithZWrite"].is_boolean ()
                         &&  Mtoon["transparentWithZWrite"].get<bool> ())
                           materialOut.eAlpha = GLTF_MATERIAL::kBLEND;
                     }
                     else if (Ext.contains ("KHR_materials_unlit"))
                     {
                        model.aMaterial[nI].bUnlit    = true;
                        model.aMaterial[nI].dMetallic = 0.0f;
                     }
                  }
               }
            }

            if (j.contains ("extensions")  &&  j["extensions"].is_object ()
             &&  j["extensions"].contains ("VRM")  &&  j["extensions"]["VRM"].is_object ()
             &&  j["extensions"]["VRM"].contains ("materialProperties")
             &&  j["extensions"]["VRM"]["materialProperties"].is_array ())
            {
               const nlohmann::json& aProp = j["extensions"]["VRM"]["materialProperties"];
               const size_t nProp = (aProp.size () < model.aMaterial.size ()) ? aProp.size () : model.aMaterial.size ();
               for (size_t nI = 0; nI < nProp; nI++)
               {
                  const nlohmann::json& Prop = aProp[nI];
                  double dBlend  = 0.0;
                  double dCutoff = 0.5;
                  if (Prop.contains ("floatProperties")  &&  Prop["floatProperties"].is_object ())
                  {
                     const nlohmann::json& Fp = Prop["floatProperties"];
                     if (Fp.contains ("_BlendMode"))
                        dBlend = Json_Number (Fp["_BlendMode"], 0.0);
                     if (Fp.contains ("_Cutoff"))
                        dCutoff = Json_Number (Fp["_Cutoff"], 0.5);
                  }
                  Alpha_ApplyVrmBlend (model.aMaterial[nI], dBlend, dCutoff);
               }
            }

            if (j.contains ("nodes")  &&  j["nodes"].is_array ())
            {
               const nlohmann::json& aNode = j["nodes"];
               const int nNode = static_cast<int> (model.aNode.size ());
               for (size_t nI = 0; nI < aNode.size (); nI++)
               {
                  const nlohmann::json& Node = aNode[nI];
                  if (Node.contains ("extensions")  &&  Node["extensions"].is_object ())
                  {
                  const nlohmann::json& Ext = Node["extensions"];
                  if (Ext.contains ("VRMC_node_constraint")  &&  Ext["VRMC_node_constraint"].is_object ())
                  {
                  const nlohmann::json& Con = Ext["VRMC_node_constraint"];
                  if (Con.contains ("constraint")  &&  Con["constraint"].is_object ())
                  {
                  const nlohmann::json& Body = Con["constraint"];

                  GLTF_CONSTRAINT constraint;
                  constraint.nNode = static_cast<int> (nI);

                  if (Body.contains ("rotation")  &&  Body["rotation"].is_object ())
                  {
                     constraint.eKind   = GLTF_CONSTRAINT::kROTATION;
                     constraint.nSource = Json_Int (Body["rotation"], "source", -1);
                     constraint.dWeight = Json_Double (Body["rotation"], "weight", 1.0);
                  }
                  else if (Body.contains ("aim")  &&  Body["aim"].is_object ())
                  {
                     constraint.eKind   = GLTF_CONSTRAINT::kAIM;
                     constraint.nSource = Json_Int (Body["aim"], "source", -1);
                     constraint.dWeight = Json_Double (Body["aim"], "weight", 1.0);
                     if (Body["aim"].contains ("aimAxis")  &&  Body["aim"]["aimAxis"].is_string ())
                        constraint.nAxis = Aim_Axis (Body["aim"]["aimAxis"].get<std::string> ());
                  }
                  else if (Body.contains ("roll")  &&  Body["roll"].is_object ())
                  {
                     constraint.eKind   = GLTF_CONSTRAINT::kROLL;
                     constraint.nSource = Json_Int (Body["roll"], "source", -1);
                     constraint.dWeight = Json_Double (Body["roll"], "weight", 1.0);
                     if (Body["roll"].contains ("rollAxis")  &&  Body["roll"]["rollAxis"].is_string ())
                        constraint.nAxis = Roll_Axis (Body["roll"]["rollAxis"].get<std::string> ());
                  }

                  if (constraint.eKind != GLTF_CONSTRAINT::kNONE
                   &&  constraint.nSource >= 0
                   &&  constraint.nSource < nNode
                   &&  constraint.nNode >= 0
                   &&  constraint.nNode < nNode
                   &&  constraint.nSource != constraint.nNode)
                     model.aConstraint.push_back (constraint);
                  }
                  }
                  }
               }
            }
         }
         catch (...)
         {
         }
      }
   }

   void Vrm_Required_Allow (const uint8_t*& pData, size_t& nLen, std::vector<uint8_t>& aOwned)
   {
      if (pData  &&  nLen >= 20
       &&  pData[0] == 'g'  &&  pData[1] == 'l'  &&  pData[2] == 'T'  &&  pData[3] == 'F')
      {
         const uint32_t nVersion  = U32LE_Read (pData + 4);
         const uint32_t nJsonLen  = U32LE_Read (pData + 12);
         const uint32_t nJsonType = U32LE_Read (pData + 16);
         if (nVersion == 2
          &&  nJsonType == 0x4E4F534A
          &&  20 + nJsonLen <= nLen
          &&  Json_HasVrmc (reinterpret_cast<const char*> (pData + 20), nJsonLen))
         {
            std::string sJson (reinterpret_cast<const char*> (pData + 20), nJsonLen);
            if (Json_AllowVrm (sJson))
            {
               Glb_Pack (sJson, pData + 20 + nJsonLen, nLen - (20 + static_cast<size_t> (nJsonLen)), aOwned);
               pData = aOwned.data ();
               nLen  = aOwned.size ();
            }
         }
      }
      else if (pData  &&  nLen > 0  &&  Json_HasVrmc (reinterpret_cast<const char*> (pData), nLen))
      {
         size_t nFirst = 0;
         while (nFirst < nLen  &&  std::isspace (static_cast<unsigned char> (pData[nFirst])))
            nFirst++;
         if (nFirst < nLen  &&  pData[nFirst] == '{')
         {
            std::string sJson (reinterpret_cast<const char*> (pData), nLen);
            if (Json_AllowVrm (sJson))
            {
               aOwned.assign (sJson.begin (), sJson.end ());
               pData = aOwned.data ();
               nLen  = aOwned.size ();
            }
         }
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
      const uint8_t* pLoad = pData;
      size_t         nLoad = nLen;
      std::vector<uint8_t> aVrm;
      Vrm_Required_Allow (pLoad, nLoad, aVrm);

      auto expBuffer = fastgltf::GltfDataBuffer::FromBytes (reinterpret_cast<const std::byte*> (pLoad), nLoad);
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
         auto expAsset = pParser.loadGltf (expBuffer.get (), std::filesystem::path (), fastgltf::Options::DecomposeNodeMatrices);
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
                  Skins_Map (asset, model, adapter);
                  Animations_Map (asset, model, adapter);
                  if (Json_HasVrmc (reinterpret_cast<const char*> (pLoad), nLoad))
                     Vrm_Extras_Map (pLoad, nLoad, model);
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
