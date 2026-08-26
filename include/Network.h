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

#ifndef SNEEZE_NETWORK_H
#define SNEEZE_NETWORK_H

namespace SNEEZE
{
   class ASSET;
   class INETWORK_IMPL;
   class ICACHE_IMPL;
   class CONTAINER;

   // ---------------------------------------------------------------------------
   // Network enums and interfaces
   // ---------------------------------------------------------------------------

   enum eASSET_STATE
   {
      kASSET_STATE_IDLE       = 0,
      kASSET_STATE_FETCHING   = 1,
      kASSET_STATE_VALIDATING = 2,
      kASSET_STATE_READY      = 3,
      kASSET_STATE_FAILED     = 4,
   };

   enum eASSET_EXT
   {
      kASSET_EXT_DATA = 0,
      kASSET_EXT_TEMP = 1,
      kASSET_EXT_META = 2,
   };

   class FILE;

   class IFILE
   {
   public:
      virtual ~IFILE () {}
      virtual void OnFileReady  (FILE* pFile) = 0;
      virtual void OnFileFailed (FILE* pFile) = 0;
   };

   class IENUM_FILE
   {
   public:
      virtual ~IENUM_FILE () {}
      virtual void OnAsset (FILE* pFile) = 0;
   };

   // ---------------------------------------------------------------------------
   // FILE — per-caller handle to a cached resource.
   //
   // Created by NETWORK::File_Open(), returned to the caller as a raw pointer.
   // Owns a snapshot of the asset's display-level fields so the inspector can
   // read them after Close.
   // ---------------------------------------------------------------------------

   class FILE
   {
   public:
      FILE (ICACHE_IMPL* pICache_Impl, uint32_t nFileIx, const std::string& sUrl, const std::string& sHash, bool bCacheEnabled);
      ~FILE ();

      // --- Snapshot fields (always available, even after Close) ---

      eASSET_STATE     State             () const;
      bool             IsReady           () const;

      std::string      Url               () const;
      std::string      Hash              () const;
      bool             IsHashed          () const;

      uint32_t         FileIx            () const;
      uint32_t         AssetIx           () const;
      long             HttpStatus        () const;
      double           FetchQueuedTime   () const;
      double           FetchStartTime    () const;
      double           FetchEndTime      () const;
      double           FetchDuration     () const;
      bool             IsServedFromCache () const;

      std::string      ContentType       () const;
      uint64_t         SizeBytes         () const;

      // --- ASSET-dependent (require attached ASSET, empty/default after Close) ---

      void             ReadData          (std::vector<uint8_t>& aData) const;
//    std::string      Header (const std::string& sName) const;
      std::string      DiskPath          () const;
      std::string      CreatedTime       () const;
      std::string      LastAccessTime    () const;
      uint32_t         AccessCount       () const;
      const std::unordered_map<std::string, std::string>& ReqHeaders () const;
      const std::unordered_map<std::string, std::string>& RspHeaders () const;

      const std::string&   RemoteAddress     () const;

      // --- Container ---

      std::string ContainerName () const;

      // --- Paths ---

      std::string        Path () const;
      std::string        Filename (const std::string& sExt = "") const;
      std::string        Pathname (const std::string& sExt = "") const;

      // --- Listener ---

      IFILE* Listener () const;
      void   Listener (IFILE* pListener);

      // --- Open-time state (locked in at construction) ---

      const std::string& OpenHash  () const;
      bool               CacheEnabled () const;

      // --- Lifecycle ---

      bool   Initialize (IFILE* pListener = nullptr);
      bool   Attach     ();
      void   Detach     ();
      bool   Guard      (bool bValue);
      void   Clear      ();
      void   Close      ();
      void   Reset      ();

      // --- Internal (NETWORK use only) ---

      bool   IsPending_Clear () const;
      bool   IsPending_Close () const;

      bool   Pending_Clear ();
      bool   Pending_Close ();
      void   Pending_Reset ();

      void   Notify_Changed ();

      void   SnapshotInitial ();
      void   SnapshotProgress ();
      void   SnapshotFinal ();

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // CACHE — per-container handle to the network's file tier.
   //
   // Opened from NETWORK::Cache_Open() for a specific CONTAINER and held by
   // that container for its lifetime. Owns the container's FILE handles and
   // exposes File_Open(). Files persist across restarts; files with a
   // cryptographic hash are additionally integrity-verified.
   //
   // The underlying deduplicated ASSET store (the disk cache itself) is owned
   // by NETWORK and shared across every CACHE — a CACHE forwards asset
   // operations to its NETWORK and contributes only the file-handle layer.
   // ---------------------------------------------------------------------------

   class CACHE
   {
   public:

      CACHE (INETWORK_IMPL* pINetwork_Impl, CONTAINER* pContainer);
      ~CACHE ();

      void  Initialize ();

      // --- Identity ---

      std::string DisplayName () const;

      // --- File operations ---

      FILE* File_Open (const std::string& sUrl, IFILE* pListener);
      FILE* File_Open (const std::string& sUrl, const std::string& sHash, uint32_t nAssetIx = 0, IFILE* pListener = nullptr);

      void  File_Enum (IENUM_FILE* pEnum);

      // --- Cache management ---

      void  SetCacheEnabled (bool b);
      bool  IsCacheEnabled () const;

      void  Clear ();

      // --- Paths ---

      std::string Path     () const;
      std::string Filename (const std::string& sExt = "") const;
      std::string Pathname (const std::string& sExt = "") const;

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // IENUM_CACHE — enumeration callback interface (caches).
   // ---------------------------------------------------------------------------

   class IENUM_CACHE
   {
   public:
      virtual ~IENUM_CACHE () {}
      virtual void OnCache (CACHE* pCache) = 0;
   };

   // ---------------------------------------------------------------------------
   // NETWORK — the network resource system.
   //
   // Fetches remote resources, caches them on disk, and serves them to callers
   // through per-container CACHE handles. Owns the deduplicated ASSET store and
   // the background fetch machinery. Each CONTAINER opens a CACHE via
   // Cache_Open() and returns it via Cache_Close().
   //
   // Assets are loaded lazily on first File_Open() (on a CACHE). Only assets
   // with active FILE handles live in memory; the .meta sidecar is flushed to
   // disk when the last active handle closes.
   //
   // Background fetches are capped at 16 concurrent threads. Overflow fetches
   // queue and are dispatched as threads complete.
   // ---------------------------------------------------------------------------

   class NETWORK
   {
   public:

      // -----------------------------------------------------------------------
      // NETWORK public API
      // -----------------------------------------------------------------------

      explicit NETWORK (ENGINE* pEngine);
      ~NETWORK ();

      bool Initialize (const std::string& sPath_Root);

      // --- Cache management ---

      CACHE* Cache_Open  (CONTAINER* pContainer);
      void   Cache_Close (CONTAINER* pContainer, CACHE* pCache);
      void   Cache_Enum  (IENUM_CACHE* pEnum);

      // --- Reset ---

      void        Reset       (const std::string& sKey);
      std::string Time_Start  () const;

   private:
      class Impl;
      Impl* m_pImpl;
   };
}
#endif // SNEEZE_NETWORK_H
