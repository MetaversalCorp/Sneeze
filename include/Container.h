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

#ifndef SNEEZE_CONTAINER_H
#define SNEEZE_CONTAINER_H

namespace SNEEZE
{
   class CACHE;
   class SILO;
   class STREAM;
   class FABRIC;
   class NODE;

   enum eTRUST
   {
      kTRUST_NONE,
      kTRUST_UNTRUSTED,
      kTRUST_UNVERIFIED,
      kTRUST_EXPIRED,
      kTRUST_VERIFIED,
      kTRUST_ROOT,
   };

   class CONTAINER
   {
   public:

      class CID
      {
      public:
         std::string sFingerprint;
         std::string sOrganization;
         std::string sOrganizationHash;
         std::string sContainer;
         std::string sPersonaHash;
         eTRUST      eTrust;

         CID () : eTrust (kTRUST_NONE) {}

         std::string DisplayName () const;
         std::string Key_Org     () const;
         std::string Key_All     () const;
      };

      CONTAINER (CONTEXT* pContext, const CID* pCID);
     ~CONTAINER ();

      CONTAINER & operator=   (CONTAINER const  & rhs)   = delete;
      CONTAINER & operator=   (CONTAINER       && rhs)   = delete;
      CONTAINER               (CONTAINER const  & other) = delete;
      CONTAINER               (CONTAINER       && other) = delete;

      bool     Open           (bool bReset);
      size_t   Close          ();

      bool     Instance_Open  (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash, const std::vector<uint8_t>& aWasmBytes, const std::vector<uint8_t>& aSnapshot);
      void     Instance_Close (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash);

      uint64_t Node_Root      (uint64_t twFabricIx,        RMAP::MAP::MAP_OBJECT* pMap_Object);
      uint64_t Node_Open      (uint64_t qwComposed_Parent, RMAP::MAP::MAP_OBJECT* pMap_Object);
      bool     Node_Close     (uint64_t twObjectIx);
      NODE*    Node_Find      (uint64_t twObjectIx) const;

      // Proximity-driven lazy loading: request that the node's children stream
      // in. Forwards to the map service in map-managed containers; no-ops when
      // the container has no map service (e.g. WASM-managed fabrics).
      void     Node_Expand    (uint64_t qwComposed);

      uint64_t Branch_Add     (uint64_t twFabricIx, const nlohmann::json& jBranch);

      void     CreateMapSvc   (uint64_t twFabricIx, const std::string& sNamespace, const std::string& sService, const std::string& sConnect, uint16_t wClass_Map, uint64_t twObjectIx_Map);

         CONTEXT*           Context    () const;
      const CID*         Identity   () const;
      const std::string& Key        () const;
      std::string        Reset_Stale () const;
      CACHE*             Cache    () const;
      SILO*              Silo     () const;
      STREAM*            Stream   () const;

      const std::string& Path_Permanent_Org () const;
      const std::string& Path_Temporary_Org () const;
      const std::string& Path_Permanent_All () const;
      const std::string& Path_Temporary_All () const;

   private:
      class Impl;
      Impl* m_pImpl;
   };
}

#endif // SNEEZE_CONTAINER_H
