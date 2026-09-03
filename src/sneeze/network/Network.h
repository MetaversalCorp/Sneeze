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

#ifndef SNEEZE_NETWORK_INETWORKIMPL_H
#define SNEEZE_NETWORK_INETWORKIMPL_H

#include "sneeze/control/Control.h"

namespace SNEEZE
{
   class INETWORK_IMPL
   {
   public:
      INETWORK_IMPL ();
      virtual ~INETWORK_IMPL ();

      virtual ASSET*             Asset_Open   (FILE* pFile)                                                                = 0;
      virtual void               Asset_Close  (FILE* pFile, ASSET* pAsset)                                                 = 0;
      virtual uint32_t           Asset_Index  ()                                                                           = 0;

      virtual std::string        Reset_Stale (const std::string& sKey) const                                               = 0;
      virtual void               Queue_Post_Fetch (JOB_FETCH* pJob_Fetch)                                                  = 0;

      virtual void               Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage)   = 0;

      virtual double             SecondsSinceEpoch () const                                                                = 0;


   private:
   };

   // -----------------------------------------------------------------------
   // ICACHE_IMPL - the single owner a FILE talks to.
   //
   // A FILE reaches everything it needs through this interface: its file
   // lifecycle (File_Clear / File_Close / File_Reset) lives in CACHE, while
   // asset operations, the host, the permanent cache path, and the owning
   // container are forwarded by CACHE to the shared NETWORK.
   // -----------------------------------------------------------------------

   class ICACHE_IMPL
   {
   public:
      ICACHE_IMPL ();
      virtual ~ICACHE_IMPL ();

      virtual ASSET*             Asset_Open  (FILE* pFile)                = 0;
      virtual void               Asset_Close (FILE* pFile, ASSET* pAsset) = 0;

      virtual void               File_Clear  (FILE* pFile)                = 0;
      virtual void               File_Close  (FILE* pFile)                = 0;
      virtual void               File_Reset  (FILE* pFile)                = 0;

      virtual std::string        Reset_Stale () const                     = 0;

      virtual ICONTEXT*          Host        () const                     = 0;
      virtual std::string        Path        () const                     = 0;
      virtual CONTAINER*         Container   () const                     = 0;

   private:
   };

   // -----------------------------------------------------------------------
   // ASSET - internal shared state for a single cached URL.
   //
   // Owned by NETWORK, never exposed to callers directly. One ASSET per URL.
   // Multiple FILE handles may reference the same ASSET.
   // -----------------------------------------------------------------------

   class ASSET
   {
   public:
      ASSET (INETWORK_IMPL* m_pINetwork_Impl, const std::string& sUrl, const std::string& sPathname);
      virtual ~ASSET ();

      // Lifecycle
      void        Open   (FILE* pFile);
      uint32_t    Close  (FILE* pFile);

      bool        Attach (FILE* pFile, bool bFetch_Allowed, const std::string& sStaleAt);
      void        Detach (FILE* pFile);

      // Fetch completion (called by FETCH thread)
      void        Fetch_Complete (const FETCH_RESULT& Fetch_Result, eASSET_STATE bState);

      // Hash verification
      bool        VerifyHash (const std::string& sFilePath, const std::string& sHash) const;

      void ReadData (std::vector<uint8_t>& aData) const;
      std::string RspHeader (const std::string& sName) const;

      // Accessors
      eASSET_STATE         State             () const;
      bool                 IsReset           () const;
      size_t               File_Count        () const;
      const std::string&   Url               () const;
      uint64_t             SizeBytes         () const;
      std::string          CreatedTime       () const;
      std::string          LastAccessTime    () const;
      uint32_t             AccessCount       () const;
      uint32_t             AssetIx           () const;
      const std::string&   Hash              () const;
      bool                 IsHashed          () const;
      std::string          DiskPath          () const;
      const std::string&   Pathname          () const;
      std::string          Path              () const;
      std::string          Pathname          (eASSET_EXT eType) const;
      long                 HttpStatus        () const;
      double               FetchStartTime    () const;
      double               FetchEndTime      () const;
      double               FetchDuration     () const;
      double               FetchQueuedTime   () const;
      double               QueueDuration     () const;
      bool                 IsServedFromCache () const;
      const std::unordered_map<std::string, std::string>& RspHeaders () const;
      const std::unordered_map<std::string, std::string>& ReqHeaders () const;
      const std::string&   RemoteAddress     () const;

      // Modifiers
      void Reset              ();

   private:
      class Impl;
      Impl* m_pImpl;
   };
}
#endif // SNEEZE_NETWORK_INETWORKIMPL_H
