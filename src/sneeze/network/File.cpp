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

#include <openssl/sha.h>
#include "Network.h"

using namespace SNEEZE;

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class SNEEZE::FILE::Impl
{
public:
   Impl (FILE* pFile, ICACHE_IMPL* pICache_Impl, uint32_t nFileIx, const std::string& sUrl, const std::string& sHash, bool bCacheEnabled) :
      m_pFile            (pFile),
      m_pICache_Impl     (pICache_Impl),
      m_nFileIx          (nFileIx),
      m_sUrl             (sUrl),
      m_sOpenHash        (sHash),
      m_bCacheEnabled    (bCacheEnabled),
      m_sDiskKey         (ComputeDiskKey (sUrl)),
      m_pAsset           (nullptr),
      m_pListener        (nullptr),
      m_nCount_Attach    (0),
      m_nAssetIx         (0),
      m_bState           (kASSET_STATE_IDLE),
      m_nSizeBytes       (0),
      m_nHttpStatus      (0),
      m_dFetchQueuedTime (0.0),
      m_dFetchStartTime  (0.0),
      m_dFetchEndTime    (0.0),
      m_bServedFromCache (false),
      m_bPending_Clear   (false),
      m_bPending_Close   (false),
      m_bGuarded         (false)
   {
   }

   ~Impl ()
   {
      while (m_nCount_Attach > 0)
         Detach ();

      if (m_pAsset)
      {
         m_pICache_Impl->Asset_Close (m_pFile, m_pAsset);

         m_pAsset = nullptr;
      }
   }

   // ---------------------------------------------------------------------------
   // Lifecycle
   // ---------------------------------------------------------------------------

   bool Initialize (IFILE* pListener)
   {
      bool bClear = false;

      {
         std::lock_guard<std::recursive_mutex> guard (m_mxFile);

         if ((m_pAsset = m_pICache_Impl->Asset_Open (m_pFile)) != nullptr)
         {
            m_pListener = pListener;

            if (m_pListener)
               Attach (true);

            bClear = !m_pICache_Impl->Host ()->OnNetworkFileCreated (m_pFile);
         }
      }

      if (bClear)
         m_pICache_Impl->File_Clear (m_pFile);

      return (m_pAsset != nullptr);
   }

   bool Attach (bool bFetch)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxFile);

      bool bResult = m_pAsset->Attach (m_pFile, bFetch, m_pICache_Impl->Reset_Stale ());

      if (bResult)
         m_nCount_Attach++;

      return bResult;
   }

   void Detach ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxFile);

      if (m_nCount_Attach > 0)
      {
         m_pAsset->Detach (m_pFile);
         m_nCount_Attach--;
      }
   }

   void Clear ()
   {
      m_pICache_Impl->File_Clear (m_pFile);
   }

   void Close ()
   {
      m_pICache_Impl->File_Close (m_pFile);
   }

   void Reset ()
   {
      m_pICache_Impl->File_Reset (m_pFile);
   }

   // ---------------------------------------------------------------------------
   // Pending flags (one-way gates)
   // ---------------------------------------------------------------------------

   bool Pending_Clear ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxFile);

      bool bChanged = false;

      if (!m_bPending_Clear)
      {
         m_bPending_Clear = true;

         m_pICache_Impl->Host ()->OnNetworkFileDeleted (m_pFile);

         bChanged = true;
      }

      return bChanged;
   }

   bool Pending_Close ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxFile);

      bool bChanged = false;

      if (!m_bPending_Close)
      {
         m_bPending_Close = true;

         if (m_pListener)
            Detach ();

         m_pListener = nullptr;

         bChanged = true;
      }

      return bChanged;
   }

   // ---------------------------------------------------------------------------
   // Notifications
   // ---------------------------------------------------------------------------

   void Notify_Changed ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxFile);

      if (!m_bPending_Clear)
         m_pICache_Impl->Host ()->OnNetworkFileChanged (m_pFile);
   }

   // ---------------------------------------------------------------------------
   // Snapshots
   // ---------------------------------------------------------------------------

   void SnapshotInitial ()
   {
      m_nAssetIx = m_pAsset->AssetIx ();
   }

   void SnapshotProgress ()
   {
      m_bState           = m_pAsset->State ();
      m_dFetchQueuedTime = m_pAsset->FetchQueuedTime ();
      m_dFetchStartTime  = m_pAsset->FetchStartTime ();
   }

   void SnapshotFinal ()
   {
      m_bState           = m_pAsset->State ();
      m_sHash            = m_pAsset->Hash ();
      m_sContentType     = m_pAsset->RspHeader ("content-type");
      m_nSizeBytes       = m_pAsset->SizeBytes ();
      m_nHttpStatus      = m_pAsset->HttpStatus ();
      m_dFetchQueuedTime = m_pAsset->FetchQueuedTime ();
      m_dFetchStartTime  = m_pAsset->FetchStartTime ();
      m_dFetchEndTime    = m_pAsset->FetchEndTime ();
      m_bServedFromCache = m_pAsset->IsServedFromCache ();
   }

   // ---------------------------------------------------------------------------
   // Path helpers
   // ---------------------------------------------------------------------------

   static std::string ComputeDiskKey (const std::string& sUrl)
   {
      unsigned char aDigest[SHA_DIGEST_LENGTH];

      SHA1 (reinterpret_cast<const unsigned char*> (sUrl.data ()), sUrl.size (), aDigest);

      static const int kTRUNCATED_BYTES = 12;
      char szHex[kTRUNCATED_BYTES * 2 + 1];
      for (int i = 0; i < kTRUNCATED_BYTES; i++)
         std::sprintf (szHex + i * 2, "%02x", aDigest[i]);
      szHex[kTRUNCATED_BYTES * 2] = '\0';

      return std::string (szHex);
   }

   std::string Path () const
   {
      return (std::filesystem::path (m_pICache_Impl->Path ()) / m_sDiskKey.substr (0, 2)).generic_string ();
   }

   std::string Filename (const std::string& sExt) const
   {
      std::string sName = m_sDiskKey.substr (2);

      if (!sExt.empty ())
         sName += "." + sExt;

      return sName;
   }

   std::string Pathname (const std::string& sExt) const
   {
      return (std::filesystem::path (Path ()) / Filename (sExt)).generic_string ();
   }

public:
   FILE*                          m_pFile;
   ICACHE_IMPL*                   m_pICache_Impl;
   ASSET*                         m_pAsset;
   IFILE*                         m_pListener;
   uint32_t                       m_nCount_Attach;
   std::recursive_mutex           m_mxFile;

   std::string                    m_sDiskKey;
   std::string                    m_sUrl;
   std::string                    m_sOpenHash;
   uint32_t                       m_nFileIx;
   uint32_t                       m_nAssetIx;
   bool                           m_bCacheEnabled;

   eASSET_STATE                   m_bState;
   double                         m_dFetchQueuedTime;
   double                         m_dFetchStartTime;

   std::string                    m_sHash;
   std::string                    m_sContentType;
   uint64_t                       m_nSizeBytes;
   long                           m_nHttpStatus;
   double                         m_dFetchEndTime;
   bool                           m_bServedFromCache;

   bool                           m_bPending_Clear;
   bool                           m_bPending_Close;
   std::atomic<bool>              m_bGuarded;
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

SNEEZE::FILE::FILE (ICACHE_IMPL* pICache_Impl, uint32_t nFileIx, const std::string& sUrl, const std::string& sHash, bool bCacheEnabled) :
   m_pImpl (new Impl (this, pICache_Impl, nFileIx, sUrl, sHash, bCacheEnabled))
{
}

bool SNEEZE::FILE::Initialize (IFILE* pListener)
{
   return m_pImpl->Initialize (pListener);
}

SNEEZE::FILE::~FILE ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Attach / Detach / Close
// ---------------------------------------------------------------------------

bool SNEEZE::FILE::Attach        ()                              { return m_pImpl->Attach           (false); }
void SNEEZE::FILE::Detach        ()                              {        m_pImpl->Detach           (); }

bool SNEEZE::FILE::Guard         (bool bValue)                   { return m_pImpl->m_bGuarded.exchange (bValue); }
void SNEEZE::FILE::Clear         ()                              {        m_pImpl->Clear            (); }
void SNEEZE::FILE::Close         ()                              {        m_pImpl->Close            (); }
void SNEEZE::FILE::Reset         ()                              {        m_pImpl->Reset            (); }

bool SNEEZE::FILE::Pending_Clear ()                              { return m_pImpl->Pending_Clear    (); }
bool SNEEZE::FILE::Pending_Close ()                              { return m_pImpl->Pending_Close    (); }
void SNEEZE::FILE::Pending_Reset ()                              {        m_pImpl->m_pAsset->Reset  (); }

// ---------------------------------------------------------------------------
// Notify — host callbacks
// ---------------------------------------------------------------------------

void SNEEZE::FILE::Notify_Changed   ()                           {        m_pImpl->Notify_Changed (); }

// ---------------------------------------------------------------------------
// Snapshot — copies display fields from the attached ASSET
// ---------------------------------------------------------------------------

void SNEEZE::FILE::SnapshotInitial  () { m_pImpl->SnapshotInitial (); }
void SNEEZE::FILE::SnapshotProgress () { m_pImpl->SnapshotProgress (); }
void SNEEZE::FILE::SnapshotFinal    () { m_pImpl->SnapshotFinal (); }

// ---------------------------------------------------------------------------
// ASSET-dependent accessors (require attached ASSET)
// ---------------------------------------------------------------------------

//std::string                                       SNEEZE::FILE::Header            (const std::string& sName) const { return m_pImpl->m_pAsset->Header (sName); }
void                                                SNEEZE::FILE::ReadData          (std::vector<uint8_t>& aData) const { return m_pImpl->m_pAsset->ReadData (aData); }

std::string                                         SNEEZE::FILE::DiskPath          () const { return m_pImpl->m_pAsset->DiskPath (); }
std::string                                         SNEEZE::FILE::CreatedTime       () const { return m_pImpl->m_pAsset->CreatedTime (); }
std::string                                         SNEEZE::FILE::LastAccessTime    () const { return m_pImpl->m_pAsset->LastAccessTime (); }
uint32_t                                            SNEEZE::FILE::AccessCount       () const { return m_pImpl->m_pAsset->AccessCount (); }
const std::unordered_map<std::string, std::string>& SNEEZE::FILE::RspHeaders        () const { return m_pImpl->m_pAsset->RspHeaders (); }
const std::unordered_map<std::string, std::string>& SNEEZE::FILE::ReqHeaders        () const { return m_pImpl->m_pAsset->ReqHeaders (); }
std::string                                         SNEEZE::FILE::ContainerName     () const { return m_pImpl->m_pICache_Impl->Container ()->Identity ()->DisplayName (); }
eASSET_STATE                                        SNEEZE::FILE::State             () const { return m_pImpl->m_bState; }
bool                                                SNEEZE::FILE::IsReady           () const { return m_pImpl->m_bState == kASSET_STATE_READY; }
std::string                                         SNEEZE::FILE::Url               () const { return m_pImpl->m_sUrl; }
std::string                                         SNEEZE::FILE::Hash              () const { return m_pImpl->m_sHash; }
bool                                                SNEEZE::FILE::IsHashed          () const { return !m_pImpl->m_sHash.empty (); }
uint32_t                                            SNEEZE::FILE::FileIx            () const { return m_pImpl->m_nFileIx; }
uint32_t                                            SNEEZE::FILE::AssetIx           () const { return m_pImpl->m_nAssetIx; }
long                                                SNEEZE::FILE::HttpStatus        () const { return m_pImpl->m_nHttpStatus; }
double                                              SNEEZE::FILE::FetchQueuedTime   () const { return m_pImpl->m_dFetchQueuedTime; }
double                                              SNEEZE::FILE::FetchStartTime    () const { return m_pImpl->m_dFetchStartTime; }
double                                              SNEEZE::FILE::FetchEndTime      () const { return m_pImpl->m_dFetchEndTime; }
double                                              SNEEZE::FILE::FetchDuration     () const { return m_pImpl->m_dFetchEndTime - m_pImpl->m_dFetchStartTime; }
bool                                                SNEEZE::FILE::IsServedFromCache () const { return m_pImpl->m_bServedFromCache; }
std::string                                         SNEEZE::FILE::ContentType       () const { return m_pImpl->m_sContentType; }
uint64_t                                            SNEEZE::FILE::SizeBytes         () const { return m_pImpl->m_nSizeBytes; }

bool                                                SNEEZE::FILE::IsPending_Clear   () const { return m_pImpl->m_bPending_Clear; }
bool                                                SNEEZE::FILE::IsPending_Close   () const { return m_pImpl->m_bPending_Close; }
std::string                                         SNEEZE::FILE::Path              () const { return m_pImpl->Path (); }
std::string                                         SNEEZE::FILE::Filename          (const std::string& sExt) const { return m_pImpl->Filename (sExt); }
std::string                                         SNEEZE::FILE::Pathname          (const std::string& sExt) const { return m_pImpl->Pathname (sExt); }

IFILE*                                              SNEEZE::FILE::Listener          () const { return m_pImpl->m_pListener; }

const std::string&                                  SNEEZE::FILE::OpenHash          () const { return m_pImpl->m_sOpenHash; }
bool                                                SNEEZE::FILE::CacheEnabled      () const { return m_pImpl->m_bCacheEnabled; }

const std::string&                                  SNEEZE::FILE::RemoteAddress     () const { return m_pImpl->m_pAsset->RemoteAddress (); }
