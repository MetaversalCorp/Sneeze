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

#include "Network.h"

#include <openssl/evp.h>

using namespace SNEEZE;

// ---------------------------------------------------------------------------
// ASSET_FETCH -- local job class bridging the control-layer fetch pool
//                back to ASSET::FetchComplete.
// ---------------------------------------------------------------------------

class ASSET_FETCH : public JOB_FETCH
{
public:
   ASSET_FETCH (ASSET* pAsset, const std::string& sUrl, const std::string& sPath_Temp, const std::string& sPath_Data, const std::string& sHash, std::unordered_map<std::string, std::string>& umsReqHeaders)
      : JOB_FETCH (true, sUrl, sPath_Temp, sPath_Data, sHash, umsReqHeaders),
        m_pAsset  (pAsset),
        m_bState  (kASSET_STATE_FETCHING)
   {}

   ASSET_FETCH (ASSET* pAsset, eASSET_STATE bState)
      : JOB_FETCH (false),
        m_pAsset  (pAsset),
        m_bState  (bState)
   {}

   void OnFetch_Complete (const FETCH_RESULT& Fetch_Result) override
   {
      m_pAsset->Fetch_Complete (Fetch_Result, m_bState);
   }

private:
   ASSET*            m_pAsset;
   eASSET_STATE      m_bState;
};

// ---------------------------------------------------------------------------

class ASSET::Impl
{
public:
   Impl (ASSET* pAsset, INETWORK_IMPL* pINetwork_Impl, const std::string& sUrl, const std::string& sPathname) :
      m_pAsset           (pAsset),
      m_pINetwork_Impl   (pINetwork_Impl),
      m_sUrl             (sUrl),
      m_sPathname        (sPathname),
      m_bState           (kASSET_STATE_IDLE),
      m_nSizeBytes       (0),
      m_nAccessCount     (0),
      m_nAssetIx         (0),
      m_nHttpStatus      (0),
      m_dFetchQueuedTime (0.0),
      m_dFetchStartTime  (0.0),
      m_dFetchEndTime    (0.0),
      m_bServedFromCache (false),
      m_bReset           (false),
      m_nCount_Open      (0),
      m_nCount_Attach    (0),
      m_pAsset_Fetch     (nullptr)
   {
      m_sCreatedAt      = NowIso8601 ();
      m_sLastAccessedAt = m_sCreatedAt;
   }

   ~Impl ()
   {
      if (m_pAsset_Fetch)
      {
         m_pAsset_Fetch->Cancel ();
         m_pAsset_Fetch = nullptr;
      }
   }

   void TouchAccess ()
   {
      m_sLastAccessedAt = NowIso8601 ();
      m_nAccessCount++;
   }

   // ---------------------------------------------------------------------------
   // Meta file helpers
   // ---------------------------------------------------------------------------

   void Meta_Load ()
   {
      std::string sPathname_Meta = Pathname (kASSET_EXT_META);
      std::string sPathname_Data = Pathname (kASSET_EXT_DATA);

      std::ifstream file (sPathname_Meta);
      if (file.is_open ())
      {
         nlohmann::json jMeta;
         bool bParsed = false;

         try
         {
            file >> jMeta;
            bParsed = true;
         }
         catch (...)
         {
            m_pINetwork_Impl->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Failed to parse sidecar: " + sPathname_Meta);
         }

         if (bParsed)
         {
            std::string sUrl_Meta = jMeta.value ("sUrl", "");
            if (sUrl_Meta == m_sUrl  &&  std::filesystem::exists (sPathname_Data))
            {
               m_sHash              = jMeta.value ("sHash", "");
               m_nAssetIx           = jMeta.value ("nAssetIx", static_cast<uint32_t> (0));
               // Data file confirmed on disk — state is READY
               m_nSizeBytes         = jMeta.value ("nSizeBytes", static_cast<uint64_t> (0));
               m_sCreatedAt         = jMeta.value ("sCreatedAt", "");
               m_nHttpStatus        = jMeta.value ("nHttpStatus", static_cast<long> (0));
               m_bReset             = jMeta.value ("bReset", false);
               m_bServedFromCache   = true;

               if (jMeta.contains ("aReqHeaders"))
               {
                  for (auto& [sKey, sVal] : jMeta["aReqHeaders"].items ())
                     m_umsReqHeaders[sKey] = sVal.get<std::string> ();
               }

               if (jMeta.contains ("aRspHeaders"))
               {
                  for (auto& [sKey, sVal] : jMeta["aRspHeaders"].items ())
                     m_umsRspHeaders[sKey] = sVal.get<std::string> ();
               }

               if (m_nAssetIx > 0)
                  m_bState = kASSET_STATE_READY;
            }
         }
      }
   }

   void Meta_Save ()
   {
      std::string sPathname_Meta      = Pathname (kASSET_EXT_META);
      std::string sPathname_Meta_Temp = sPathname_Meta + ".temp";

      nlohmann::json jMeta;
      jMeta["sUrl"]            = m_sUrl;
      jMeta["sHash"]           = m_sHash;
      jMeta["nAssetIx"]        = m_nAssetIx;
      jMeta["nSizeBytes"]      = m_nSizeBytes;
      jMeta["sCreatedAt"]      = m_sCreatedAt;
      jMeta["sLastAccessedAt"] = m_sLastAccessedAt;
      jMeta["nAccessCount"]    = m_nAccessCount;
      jMeta["nHttpStatus"]     = m_nHttpStatus;
      jMeta["bReset"]          = m_bReset;

      nlohmann::json jRspHeaders = nlohmann::json::object ();
      for (auto& [sKey, sVal] : m_umsRspHeaders)
         jRspHeaders[sKey] = sVal;
      jMeta["aRspHeaders"] = jRspHeaders;

      nlohmann::json jReqHeaders = nlohmann::json::object ();
      for (auto& [sKey, sVal] : m_umsReqHeaders)
         jReqHeaders[sKey] = sVal;
      jMeta["aReqHeaders"] = jReqHeaders;

      std::error_code ec;
      std::filesystem::create_directories (std::filesystem::path (sPathname_Meta).parent_path (), ec);

      std::ofstream file (sPathname_Meta_Temp, std::ios::trunc);
      if (file.is_open ())
      {
         std::string sDump;
         bool bDumped = false;

         try
         {
            sDump = jMeta.dump (2);
            bDumped = true;
         }
         catch (const nlohmann::json::exception& ex)
         {
            m_pINetwork_Impl->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Meta_Save dump failed for " + m_sUrl + ": " + ex.what ());
            file.close ();
            std::filesystem::remove (sPathname_Meta_Temp, ec);
         }

         if (bDumped)
         {
            file << sDump;
            file.close ();

            std::filesystem::rename (sPathname_Meta_Temp, sPathname_Meta, ec);
            if (ec)
               m_pINetwork_Impl->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Failed to rename meta temp: " + ec.message ());
         }
      }
   }

   void Meta_Reset ()
   {
      std::error_code ec;

      std::filesystem::remove (Pathname (kASSET_EXT_DATA), ec);
      std::filesystem::remove (Pathname (kASSET_EXT_META), ec);
      std::filesystem::remove (Pathname (kASSET_EXT_TEMP), ec);

      ResetState ();
   }

   void ResetState ()
   {
      m_bState = kASSET_STATE_IDLE;
      m_sHash.clear ();
      m_nSizeBytes = 0;
      m_sCreatedAt.clear ();
      m_sLastAccessedAt.clear ();
      m_nAccessCount = 0;
      m_nHttpStatus = 0;
      m_dFetchQueuedTime = 0.0;
      m_dFetchStartTime = 0.0;
      m_dFetchEndTime = 0.0;
      m_bServedFromCache = false;
      m_bReset = false;
      m_nAssetIx = 0;
      m_umsReqHeaders.clear ();
      m_umsRspHeaders.clear ();
   }

   void Evict ()
   {
      m_bState = kASSET_STATE_IDLE;
      m_nSizeBytes = 0;
      m_nHttpStatus = 0;
      m_dFetchQueuedTime = 0.0;
      m_dFetchStartTime = 0.0;
      m_dFetchEndTime = 0.0;
      m_bServedFromCache = false;
      m_umsReqHeaders.clear ();
      m_umsRspHeaders.clear ();
   }

   // ---------------------------------------------------------------------------
   // Disk path helpers
   // ---------------------------------------------------------------------------

   std::string Path () const
   {
      return std::filesystem::path (m_sPathname).parent_path ().generic_string ();
   }

   std::string Pathname (eASSET_EXT eType) const
   {
      static const char* aExt[] = { ".data", ".temp", ".meta" };

      return m_sPathname + aExt[eType];
   }

   // ---------------------------------------------------------------------------
   // Hash helpers
   // ---------------------------------------------------------------------------

   static bool ParseSriHash (const std::string& sSri, std::string& sAlgo, std::string& sDigest)
   {
      bool bResult = false;

      auto nDash = sSri.find ('-');
      if (nDash != std::string::npos  &&  nDash > 0  &&  nDash < sSri.size () - 1)
      {
         sAlgo   = sSri.substr (0, nDash);
         sDigest = sSri.substr (nDash + 1);
         bResult = (sAlgo == "sha256"  ||  sAlgo == "sha384"  ||  sAlgo == "sha512");
      }

      return bResult;
   }

   static const EVP_MD* GetEvpMd (const std::string& sAlgo)
   {
      const EVP_MD* pMd = nullptr;
      if (sAlgo == "sha256")
         pMd = EVP_sha256 ();
      else if (sAlgo == "sha384")
         pMd = EVP_sha384 ();
      else if (sAlgo == "sha512")
         pMd = EVP_sha512 ();

      return pMd;
   }

   static std::string ComputeFileHash (const std::string& sFilePath, const std::string& sAlgo)
   {
      std::string sResult;

      const EVP_MD* pMd = GetEvpMd (sAlgo);
      if (pMd)
      {
         std::ifstream file (sFilePath, std::ios::binary);
         if (file.is_open ())
         {
            EVP_MD_CTX* pCtx = EVP_MD_CTX_new ();
            if (pCtx)
            {
               EVP_DigestInit_ex (pCtx, pMd, nullptr);

               char aBuffer[8192];
               while (file.read (aBuffer, sizeof (aBuffer))  ||  file.gcount () > 0)
               {
                  EVP_DigestUpdate (pCtx, aBuffer, static_cast<size_t> (file.gcount ()));
                  if (file.eof ())
                     break;
               }

               unsigned char aDigest[EVP_MAX_MD_SIZE];
               unsigned int nDigestLen = 0;
               EVP_DigestFinal_ex (pCtx, aDigest, &nDigestLen);
               EVP_MD_CTX_free (pCtx);

               std::string sHex;
               sHex.reserve (nDigestLen * 2);
               for (unsigned int i = 0; i < nDigestLen; i++)
               {
                  char szByte[3];
                  std::sprintf (szByte, "%02x", aDigest[i]);
                  sHex += szByte;
               }

               sResult = sHex;
            }
         }
      }

      return sResult;
   }

   // ---------------------------------------------------------------------------
   // Lifecycle
   // ---------------------------------------------------------------------------

   void Open (FILE* pFile)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      if (m_nCount_Open == 0)
      {
         std::error_code ec;
         std::filesystem::create_directories (Path (), ec);
      }

      m_apFile.push_back (pFile);

      m_nCount_Open++;
   }

   uint32_t Close (FILE* pFile)
   {
      // if pFile == nullptr, the fetch thread is releasing its implicit lock

      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      m_nCount_Open--;

      if (pFile)
      {
         auto it = std::find (m_apFile.begin (), m_apFile.end (), pFile);
         if (it != m_apFile.end ())
            m_apFile.erase (it);
      }

      return m_nCount_Open;
   }

   bool Attach (FILE* pFile, bool bFetch_Allowed, const std::string& sStaleAt)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      bool bResult = false;

      if (pFile->AssetIx () == 0  ||  pFile->AssetIx () == m_nAssetIx)
      {
         m_nCount_Attach++;

         if (m_nCount_Attach == 1)
            Meta_Load ();

         std::string sHash         = pFile->OpenHash ();
         bool        bCacheEnabled = pFile->CacheEnabled ();
         bool        bStale        = m_bState == kASSET_STATE_READY  &&  m_sCreatedAt < sStaleAt;

         bool bFetch  = false;
         bool bReady  = false;
         bool bFailed = false;

         eASSET_STATE bState = m_bState;

         if (bState == kASSET_STATE_READY  &&  bStale)
         {
            // Cached but stale per reset — discard and re-fetch
            Meta_Reset ();
            bFetch = true;
         }
         else if (bState == kASSET_STATE_READY  &&  !sHash.empty ()  &&  m_sHash.empty ())
         {
            // Cached without hash — caller now requires integrity verification
            if (VerifyHash (Pathname (kASSET_EXT_DATA), sHash))
            {
               m_sHash = sHash;
               TouchAccess ();
               m_bServedFromCache = true;
               bReady  = true;
            }
            else
            {
               // Hash mismatch — cached data is stale or corrupt, re-fetch
               m_sHash = sHash;
               bFetch = true;
            }
         }
         else if (bState != kASSET_STATE_FETCHING  &&  !sHash.empty ()  &&  !m_sHash.empty ()  &&  m_sHash != sHash)
         {
            // Caller's hash differs from the asset's — content has been revised, re-fetch
            m_sHash = sHash;
            bFetch = true;
         }
         else if (bState == kASSET_STATE_READY  &&  !bCacheEnabled)
         {
            // Cached but caller has caching disabled — force a fresh fetch
            m_bServedFromCache = false;
            bFetch = true;
         }
         else if (bState == kASSET_STATE_READY)
         {
            // Cached and valid — serve from disk
            TouchAccess ();
            m_bServedFromCache = true;
            bReady  = true;
         }
         else if (bState == kASSET_STATE_FAILED)
         {
            // Previous fetch failed — propagate the failure to this caller
            bFailed = true;
         }
         else if (bState == kASSET_STATE_IDLE)
         {
            // Never fetched — first request for this URL
            if (!sHash.empty ())
               m_sHash = sHash;

            bFetch = true;
         }
         // TODO: second attach with a different hash triggers re-fetch, which will
         // notify the first file again with OnFileReady — need to decide whether
         // to suppress re-notification or version-gate callbacks.

         else if (bState == kASSET_STATE_FETCHING)
         {
            // Already in flight — this caller will be notified when it completes
         }

         if (bFetch  &&  bFetch_Allowed  &&  m_nAssetIx == 0)
            m_nAssetIx = m_pINetwork_Impl->Asset_Index ();

         pFile->SnapshotInitial ();

         if (bFetch  &&  bFetch_Allowed)
         {
            if (m_pAsset_Fetch)
            {
               m_pAsset_Fetch->Cancel ();
               m_pAsset_Fetch = nullptr;
            }

            m_bState = kASSET_STATE_FETCHING;
            m_dFetchStartTime = m_pINetwork_Impl->SecondsSinceEpoch ();
            pFile->SnapshotProgress ();

            m_nCount_Open++;
            m_nCount_Attach++;

            std::unordered_map<std::string, std::string> umsReqHeaders;

            umsReqHeaders.insert ({ "User-Agent", "Sneeze/1.0 (Windows NT 10.0; Win64; x64)" });
         // umsReqHeaders.insert ({ "Accept-Encoding", "gzip, deflate, br, zstd" });

            auto* pJob = new ASSET_FETCH (m_pAsset, m_sUrl, Pathname (kASSET_EXT_TEMP), Pathname (kASSET_EXT_DATA), m_sHash, umsReqHeaders);

            m_pAsset_Fetch = pJob;
            m_pINetwork_Impl->Queue_Post_Fetch (pJob);
         }
         else if (bReady  ||  bFailed)
         {
            m_bState = kASSET_STATE_FETCHING;

            m_nCount_Open++;
            m_nCount_Attach++;

            auto* pJob = new ASSET_FETCH (m_pAsset, bState);

            m_pAsset_Fetch = pJob;
            m_pINetwork_Impl->Queue_Post_Fetch (pJob);
         }

         bResult = true;
      }
      else
      {
         // Asset index mismatch — caller holds a stale version, reject the attach
      }

      return bResult;
   }

   void Detach (FILE* pFile)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      m_nCount_Attach--;

      if (m_nCount_Attach == 0)
      {
         if (m_bState == kASSET_STATE_READY)
            Meta_Save ();
         else if (m_bState == kASSET_STATE_FAILED)
            Meta_Reset ();

         Evict ();
      }
   }

   void Reset ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      if (m_bState == kASSET_STATE_READY)
      {
         m_bReset = true;

         Meta_Save ();
      }
      else Meta_Reset ();
   }

   // ---------------------------------------------------------------------------
   // Fetch
   // ---------------------------------------------------------------------------

   void Fetch_Complete (const FETCH_RESULT& Fetch_Result, eASSET_STATE bState)
   {
      std::vector<FILE*> apDelete;

      {
         std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

         if (bState == kASSET_STATE_FETCHING)
         {
            m_dFetchEndTime   = m_pINetwork_Impl->SecondsSinceEpoch ();
            m_nHttpStatus     = Fetch_Result.nHttpStatus;
            m_sRemoteAddress  = Fetch_Result.sRemoteAddress;
            m_umsReqHeaders   = Fetch_Result.mapReqHeaders;
            m_umsRspHeaders   = Fetch_Result.mapRspHeaders;

            if (Fetch_Result.bSuccess)
            {
               m_nSizeBytes      = Fetch_Result.nSizeBytes;
               m_sCreatedAt      = NowIso8601 ();
               m_sLastAccessedAt = m_sCreatedAt;

               bState = kASSET_STATE_READY;
            }
            else bState = kASSET_STATE_FAILED;
         }

         m_bState = bState;

         for (auto* pFile : m_apFile)
         {
            pFile->Guard (true); // the guard defers closure and deletion of a file in the middle of processing a fetch completion

            pFile->SnapshotFinal ();
            pFile->Notify_Changed ();

            IFILE* pListener = pFile->Listener ();
            if (pListener)
            {
               if (m_bState == kASSET_STATE_READY)
                  pListener->OnFileReady (pFile);
               else pListener->OnFileFailed (pFile);
            }

            if (!pFile->Guard (false))
               apDelete.push_back (pFile);
         }

         m_pAsset_Fetch = nullptr;
      }

      for (auto* pFile : apDelete)
         pFile->Close ();

      Detach (nullptr);
      m_pINetwork_Impl->Asset_Close (nullptr, m_pAsset);
   }

   // ---------------------------------------------------------------------------
   // Data access
   // ---------------------------------------------------------------------------

   void ReadData (std::vector<uint8_t>& aData) const
   {
      aData.clear ();

      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      if (m_bState == kASSET_STATE_READY)
      {
         std::ifstream file (Pathname (kASSET_EXT_DATA), std::ios::binary | std::ios::ate);
         if (file.is_open ())
         {
            auto nSize = file.tellg ();
            if (nSize > 0)
            {
               aData.resize (static_cast<size_t> (nSize));
               file.seekg (0, std::ios::beg);
               file.read (reinterpret_cast<char*> (aData.data ()), nSize);
            }
         }
      }
   }

   std::string RspHeader (const std::string& sName) const
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxAsset);

      std::string sResult;

      auto it = m_umsRspHeaders.find (sName);
      if (it != m_umsRspHeaders.end ())
         sResult = it->second;

      return sResult;
   }

   // ---------------------------------------------------------------------------
   // Hash verification
   // ---------------------------------------------------------------------------

   bool VerifyHash (const std::string& sFilePath, const std::string& sHash) const
   {
      bool bResult = true;

      if (!sHash.empty ())
      {
         bResult = false;
         std::string sAlgo, sExpected;
         if (ParseSriHash (sHash, sAlgo, sExpected))
         {
            std::string sActual = ComputeFileHash (sFilePath, sAlgo);
            bResult = (sActual == sExpected);
         }
      }

      return bResult;
   }

public:
   ASSET*                        m_pAsset;
   INETWORK_IMPL*                m_pINetwork_Impl;
   std::string                   m_sUrl;
   std::string                   m_sHash;
   std::string                   m_sPathname;
   eASSET_STATE                  m_bState;

   uint64_t                      m_nSizeBytes;
   std::string                   m_sCreatedAt;
   std::string                   m_sLastAccessedAt;
   uint32_t                      m_nAccessCount;
   uint32_t                      m_nAssetIx;

   long                          m_nHttpStatus;
   double                        m_dFetchQueuedTime;
   double                        m_dFetchStartTime;
   double                        m_dFetchEndTime;
   bool                          m_bServedFromCache;
   bool                          m_bReset;
   uint32_t                      m_nCount_Open;
   uint32_t                      m_nCount_Attach;

   std::string                   m_sRemoteAddress;

   IJOB*                         m_pAsset_Fetch;

   std::vector<FILE*>            m_apFile;

   mutable std::recursive_mutex  m_mxAsset;

   std::unordered_map<std::string, std::string> m_umsRspHeaders;
   std::unordered_map<std::string, std::string> m_umsReqHeaders;
};

// ---------------------------------------------------------------------------
// ASSET
// ---------------------------------------------------------------------------

ASSET::ASSET (INETWORK_IMPL* pINetwork_Impl, const std::string& sUrl, const std::string& sPathname) :
   m_pImpl (new Impl (this, pINetwork_Impl, sUrl, sPathname))
{
}

ASSET::~ASSET ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void        ASSET::Open          (FILE* pFile)                                                   {        m_pImpl->Open   (pFile); }
uint32_t    ASSET::Close         (FILE* pFile)                                                   { return m_pImpl->Close  (pFile); }
bool        ASSET::Attach        (FILE* pFile, bool bFetch_Allowed, const std::string& sStaleAt) { return m_pImpl->Attach (pFile, bFetch_Allowed, sStaleAt); }
void        ASSET::Detach        (FILE* pFile)                                                   {        m_pImpl->Detach (pFile); }
void        ASSET::Reset         ()                                                              {        m_pImpl->Reset  (); }

// ---------------------------------------------------------------------------
// Fetch
// ---------------------------------------------------------------------------

void        ASSET::Fetch_Complete (const FETCH_RESULT& Fetch_Result, eASSET_STATE bState) { m_pImpl->Fetch_Complete (Fetch_Result, bState); }

// ---------------------------------------------------------------------------
// Data access
// ---------------------------------------------------------------------------

void        ASSET::ReadData      (std::vector<uint8_t>& aData) const        {        m_pImpl->ReadData      (aData); }
std::string ASSET::RspHeader     (const std::string& sName) const           { return m_pImpl->RspHeader     (sName); }

// ---------------------------------------------------------------------------
// Hash verification
// ---------------------------------------------------------------------------

bool        ASSET::VerifyHash    (const std::string& sFilePath, const std::string& sHash) const { return m_pImpl->VerifyHash (sFilePath, sHash); }

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

eASSET_STATE         ASSET::State ()                        const { return m_pImpl->m_bState;            }
bool                 ASSET::IsReset ()                      const { return m_pImpl->m_bReset;            }
size_t               ASSET::File_Count ()                   const { return m_pImpl->m_apFile.size ();    }
const std::string&   ASSET::Url ()                          const { return m_pImpl->m_sUrl;              }
uint64_t             ASSET::SizeBytes ()                    const { return m_pImpl->m_nSizeBytes;        }
std::string          ASSET::CreatedTime ()                  const { return m_pImpl->m_sCreatedAt;        }
std::string          ASSET::LastAccessTime ()               const { return m_pImpl->m_sLastAccessedAt;   }
uint32_t             ASSET::AccessCount ()                  const { return m_pImpl->m_nAccessCount;      }
uint32_t             ASSET::AssetIx ()                      const { return m_pImpl->m_nAssetIx;          }
const std::string&   ASSET::Hash ()                         const { return m_pImpl->m_sHash;             }
bool                 ASSET::IsHashed ()                     const { return !m_pImpl->m_sHash.empty ();   }
std::string          ASSET::Path ()                         const { return m_pImpl->Path ();             }
const std::string&   ASSET::Pathname ()                     const { return m_pImpl->m_sPathname;         }
std::string          ASSET::Pathname (eASSET_EXT eType)     const { return m_pImpl->Pathname (eType);    }
std::string          ASSET::DiskPath ()                     const { return m_pImpl->Pathname (kASSET_EXT_DATA); }
long                 ASSET::HttpStatus ()                   const { return m_pImpl->m_nHttpStatus;       }
double               ASSET::FetchStartTime ()               const { return m_pImpl->m_dFetchStartTime;   }
double               ASSET::FetchEndTime ()                 const { return m_pImpl->m_dFetchEndTime;     }
double               ASSET::FetchDuration ()                const { return m_pImpl->m_dFetchEndTime - m_pImpl->m_dFetchStartTime; }
double               ASSET::FetchQueuedTime ()              const { return m_pImpl->m_dFetchQueuedTime;  }
double               ASSET::QueueDuration ()                const { return m_pImpl->m_dFetchStartTime - m_pImpl->m_dFetchQueuedTime; }
bool                 ASSET::IsServedFromCache ()            const { return m_pImpl->m_bServedFromCache;  }
const std::string&   ASSET::RemoteAddress ()                const { return m_pImpl->m_sRemoteAddress;    }

const std::unordered_map<std::string, std::string>&   ASSET::RspHeaders ()      const { return m_pImpl->m_umsRspHeaders; }
const std::unordered_map<std::string, std::string>&   ASSET::ReqHeaders ()      const { return m_pImpl->m_umsReqHeaders; }
