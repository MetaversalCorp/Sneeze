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

#include <nlohmann/json.hpp>
#include <curl/curl.h>

using namespace SNEEZE;

// Fetch thread pool (capped at kMAX_CONCURRENT_FETCHES)
static constexpr int kMAX_CONCURRENT_FETCHES = 16;

/***********************************************************************************************************************************
**  Curl Callbacks
***********************************************************************************************************************************/

size_t FetchWriteCallback (char* pData, size_t nSize, size_t nMembers, void* pUserData)
{
   std::ofstream* stream = static_cast<std::ofstream*>(pUserData);

   stream->write (pData, nSize * nMembers);

   return nSize * nMembers;
}

#if 1
size_t FetchHeaderCallback (char* pData, size_t nSize, size_t nMembers, void* pUserData)
{
   const size_t bytes = nSize * nMembers;
   auto* pmHeaders = static_cast<std::unordered_map<std::string, std::string>*> (pUserData);

   std::string sLine (pData, bytes);

   // Strip CRLF
   while (!sLine.empty () && (sLine.back () == '\r' || sLine.back () == '\n'))
   {
      sLine.pop_back ();
   }

   const auto pos = sLine.find (':');
   if (pos != std::string::npos) 
   {
      std::string sName = sLine.substr (0, pos);
      std::string sValue = sLine.substr (pos + 1);

      std::transform (sName.begin (), sName.end (), sName.begin (), [](unsigned char c) { return std::tolower (c); });

      size_t nStart  = sValue.find_first_not_of (" \t");
      size_t nEnd    = sValue.find_last_not_of (" \t");

      if (nStart == std::string::npos)
      {
         sValue.clear ();
      }
      else
      {
         sValue = sValue.substr (nStart, nEnd - nStart + 1);
      }

      (*pmHeaders)[sName] = sValue;
   }

   return bytes; // must match provided size or libcurl treats it as error [1]
}
#else
size_t FetchHeaderCallback (char* pData, size_t nSize, size_t nMembers, void* pUserData)
{
   auto* pmapHeaders = static_cast<std::unordered_map<std::string, std::string>*> (pUserData);

   size_t nTotal = nSize * nMembers;
   std::string sLine (pData, nTotal);

   auto nColon = sLine.find (':');
   if (nColon != std::string::npos)
   {
      std::string sKey = sLine.substr (0, nColon);
      std::string sValue = sLine.substr (nColon + 1);

      auto nStart = sValue.find_first_not_of (" \t\r\n");
      auto nEnd = sValue.find_last_not_of (" \t\r\n");
      if (nStart != std::string::npos && nEnd != std::string::npos)
         sValue = sValue.substr (nStart, nEnd - nStart + 1);
      else
         sValue.clear ();

      // Sanitize both name and value: header names are supposed to be ASCII
      // per RFC 7230 but some servers cheat. ::tolower on a signed char is UB
      // for bytes >= 0x80, so cast through unsigned char.
      sKey = SNEEZE::ToUtf8 (sKey);
      sValue = SNEEZE::ToUtf8 (sValue);
      std::transform (sKey.begin (), sKey.end (), sKey.begin (),
         [](unsigned char c) { return static_cast<char> (std::tolower (c)); });
      (*pmapHeaders)[sKey] = sValue;
   }

   return nTotal;
}
#endif

/***********************************************************************************************************************************
**  Impl Class
***********************************************************************************************************************************/

class NETWORK::Impl
{
public:
   struct RULE
   {
      std::string                                  sContentType;
      std::string                                  sOlderThan;
   };

public:
   Impl (NETWORK* pNetwork, ENGINE* pEngine) :
      m_pNetwork (pNetwork),
      m_pEngine (pEngine),
      m_bShuttingDown (false),
      m_bCacheEnabled (true),
      m_bDisplayEnabled (true),
      m_nNextAssetIx (1),
      m_nNextFileIx (1),
      m_tpEpoch (std::chrono::steady_clock::now ())
   {
   }

   ~Impl ()
   {
      if (!m_sCachePath.empty ())
      {
         m_bShuttingDown = true;

         {
            std::lock_guard<std::recursive_mutex> guard (m_mutex);
            while (!m_aFetchQueue.empty ())
               m_aFetchQueue.pop ();
         }

         for (auto& t : m_aSlots)
         {
            if (t.joinable ())
               t.join ();
         }
         m_aSlots.clear ();

         {
            std::lock_guard<std::recursive_mutex> guard (m_mutex);

            for (auto& [sUrl, pAsset] : m_mapAssets)
            {
               if (pAsset->State () == STATE_READY)
                  SaveMeta (pAsset.get ());
            }

            SaveRules ();

            m_mapAssets.clear ();
         }

         DeleteFiles ();

         m_sCachePath.clear ();
      }

      curl_global_cleanup ();
   }

   bool Initialize ()
   {
      bool bResult = false;

      CURLcode nCurlInit = curl_global_init (CURL_GLOBAL_DEFAULT);
      if (nCurlInit != CURLE_OK)
      {
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "NETWORK", "curl_global_init failed (code " + std::to_string (static_cast<int> (nCurlInit)) + ")");
      }
      else
      {
         m_tpEpoch = std::chrono::steady_clock::now ();
         m_sCachePath = CachePath ();

         if (!m_sCachePath.empty ())
         {
            std::filesystem::create_directories (m_sCachePath);

            LoadRules ();

            m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "NETWORK", "Initialized (path: " + m_sCachePath + ", rules: " + std::to_string (m_aRules.size ()) + ", nAssetIx: " + std::to_string (m_nNextAssetIx) + ")");

            bResult = true;
         }
         else
         {
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "NETWORK", "Failed to determine cache path");
         }
      }

      return bResult;
   }

   void Release (FILE* pFile)
   {
      if (pFile)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         ASSET* pAsset = pFile->Asset ();

         if (pAsset)
         {
            pFile->SnapshotFinal ();
            pAsset->DetachFile (pFile);
            pFile->SetAsset (nullptr);

            if (pAsset->GetFileCount () == 0  &&  pAsset->State () != STATE_FETCHING)
            {
               if (pAsset->IsPendingReset ())
                  ResetAsset (pAsset);
               else if (pAsset->State () == STATE_READY)
                  SaveMeta (pAsset);

               std::string sUrl = pAsset->Url ();
               m_mapAssets.erase (sUrl);
            }
         }

         pFile->SetReleased ();

         if (pFile->IsPendingClear ())
         {
            auto it = std::find (m_apFile.begin (), m_apFile.end (), pFile);
            if (it != m_apFile.end ())
               m_apFile.erase (it);
            delete pFile;
         }
      }
   }

   bool Reopen (FILE* pFile)
   {
      bool bResult = false;

      if (pFile && !pFile->Url ().empty ())
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         std::string sUrl = pFile->Url ();
         uint32_t    nAssetIx = pFile->AssetIx ();

         auto it = m_mapAssets.find (sUrl);

         if (it == m_mapAssets.end ())
         {
            std::string sDiskKey = ComputeDiskKey (sUrl);
            if (LoadMeta (sDiskKey, sUrl))
               it = m_mapAssets.find (sUrl);
         }

         if (it != m_mapAssets.end ())
         {
            ASSET* pAsset = it->second.get ();

            if (pAsset->AssetIx () == nAssetIx)
            {
               pFile->SetAsset (pAsset);
               pAsset->AttachFile (pFile);
               pFile->SnapshotFinal ();
               bResult = true;

               IFILE* pListener = pFile->Listener ();
               if (pListener)
               {
                  if (pAsset->State () == STATE_READY)
                     pListener->OnFileReady (pFile);
                  else
                     pListener->OnFileFailed (pFile);
               }
            }
         }
      }

      return bResult;
   }
   // ---------------------------------------------------------------------------
   // Path helpers
   // ---------------------------------------------------------------------------

   std::string CachePath () const
   {
      std::string sResult;
      std::string sAppDataPath = m_pEngine->Host ()->sAppDataPath ();
      if (!sAppDataPath.empty ())
         sResult = (std::filesystem::path (sAppDataPath) / "Persistent" / "Cache").string ();

      return sResult;
   }

   std::string ComputeDiskKey (const std::string& sUrl) const
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

   std::string DiskKeyToPath (const std::string& sDiskKey, DISKFILE eType) const
   {
      static const char* aExt[] = { ".data", ".temp", ".meta" };

      std::string sDir = sDiskKey.substr (0, 2);
      std::string sFile = sDiskKey.substr (2);

      return (std::filesystem::path (m_sCachePath) / sDir / sFile).string () + aExt[eType];
   }

   // ---------------------------------------------------------------------------
   // SRI hash parsing and verification
   // ---------------------------------------------------------------------------

   bool ParseSriHash (const std::string& sSri, std::string& sAlgo, std::string& sDigest) const
   {
      bool bResult = false;

      auto nDash = sSri.find ('-');
      if (nDash != std::string::npos && nDash > 0 && nDash < sSri.size () - 1)
      {
         sAlgo = sSri.substr (0, nDash);
         sDigest = sSri.substr (nDash + 1);
         bResult = (sAlgo == "sha256" || sAlgo == "sha384" || sAlgo == "sha512");
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

   std::string ComputeFileHash (const std::string& sFilePath, const std::string& sAlgo) const
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
               while (file.read (aBuffer, sizeof (aBuffer)) || file.gcount () > 0)
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

   std::string ComputeDataHash (const uint8_t* pData, size_t nLen, const std::string& sAlgo) const
   {
      std::string sResult;

      const EVP_MD* pMd = GetEvpMd (sAlgo);
      if (pMd && pData)
      {
         unsigned char aDigest[EVP_MAX_MD_SIZE];
         unsigned int nDigestLen = 0;
         EVP_Digest (pData, nLen, aDigest, &nDigestLen, pMd, nullptr);

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

      return sResult;
   }

   // ---------------------------------------------------------------------------
   // Sidecar metadata (per-asset .meta files)
   // ---------------------------------------------------------------------------

   void SaveMeta (ASSET* pAsset)
   {
      std::string sDiskKey = ComputeDiskKey (pAsset->Url ());
      std::string sMetaPath = DiskKeyToPath (sDiskKey, DISKFILE_META);

      std::filesystem::create_directories (std::filesystem::path (sMetaPath).parent_path ());

      nlohmann::json jMeta;
      jMeta["url"] = pAsset->Url ();
      jMeta["hash"] = pAsset->Hash ();
      jMeta["nMetaIx"] = pAsset->AssetIx ();
      jMeta["sizeBytes"] = pAsset->SizeBytes ();
      jMeta["createdAt"] = pAsset->CreatedTime ();
      jMeta["lastAccessedAt"] = pAsset->LastAccessTime ();
      jMeta["accessCount"] = pAsset->AccessCount ();
      jMeta["httpStatus"] = pAsset->HttpStatus ();

      nlohmann::json jHeaders = nlohmann::json::object ();
      for (auto& [sKey, sVal] : pAsset->Headers ())
         jHeaders[sKey] = sVal;
      jMeta["headers"] = jHeaders;

      std::string sTmpPath = sMetaPath + ".temp";
      std::ofstream file (sTmpPath, std::ios::trunc);
      if (file.is_open ())
      {
         std::string sDump;
         try
         {
            sDump = jMeta.dump (2);
         }
         catch (const nlohmann::json::exception& ex)
         {
            // The fetch worker thread runs SaveMeta -- letting an exception
            // escape here aborts the whole process. ToUtf8 in
            // FetchHeaderCallback should keep this from firing, but if a new
            // SaveMeta field starts carrying non-UTF-8 we log + drop the
            // sidecar instead of crashing the app.
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK",
               "SaveMeta dump failed for " + pAsset->Url () + ": " + ex.what ());
            file.close ();
            std::error_code ec;
            std::filesystem::remove (sTmpPath, ec);
            return;
         }
         file << sDump;
         file.close ();

         std::error_code ec;
         std::filesystem::rename (sTmpPath, sMetaPath, ec);
         if (ec)
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Failed to rename meta temp: " + ec.message ());
      }
   }

   bool LoadMeta (const std::string& sDiskKey, const std::string& sUrl)
   {
      bool bResult = false;

      std::string sMetaPath = DiskKeyToPath (sDiskKey, DISKFILE_META);
      std::string sDataPath = DiskKeyToPath (sDiskKey, DISKFILE_DATA);

      std::ifstream file (sMetaPath);
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
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Failed to parse sidecar: " + sMetaPath);
         }

         if (bParsed)
         {
            std::string sMetaUrl = jMeta.value ("url", "");
            if (sMetaUrl == sUrl && std::filesystem::exists (sDataPath))
            {
               std::string sHash = jMeta.value ("hash", "");
               auto pAsset = std::make_unique<ASSET> (m_pNetwork, sUrl, sHash);
               pAsset->SetDiskPath (sDataPath);
               pAsset->SetSizeBytes (jMeta.value ("sizeBytes", static_cast<uint64_t> (0)));
               pAsset->SetCreatedTime (jMeta.value ("createdAt", ""));
               pAsset->SetAssetIx (jMeta.value ("nMetaIx", static_cast<uint32_t> (0)));

               if (jMeta.contains ("headers"))
               {
                  std::unordered_map<std::string, std::string> mapHeaders;
                  for (auto& [sKey, sVal] : jMeta["headers"].items ())
                     mapHeaders[sKey] = sVal.get<std::string> ();
                  pAsset->SetHeaders (mapHeaders);
               }

               pAsset->SetHttpStatus (jMeta.value ("httpStatus", static_cast<long> (0)));
               pAsset->SetServedFromCache (true);
               pAsset->Complete (sDataPath, pAsset->SizeBytes ());

               m_mapAssets[sUrl] = std::move (pAsset);
               bResult = true;
            }
         }
      }

      return bResult;
   }

   // ---------------------------------------------------------------------------
   // Staleness rules (persisted in rules.json)
   // ---------------------------------------------------------------------------

   void LoadRules ()
   {
      std::string sRulesPath = (std::filesystem::path (m_sCachePath) / "rules.json").string ();
      std::ifstream file (sRulesPath);
      if (file.is_open ())
      {
         nlohmann::json jDoc;
         bool bParsed = false;

         try
         {
            file >> jDoc;
            bParsed = true;
         }
         catch (...)
         {
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Failed to parse rules.json -- defaulting to stale");
         }

         if (bParsed)
         {
            m_nNextAssetIx = jDoc.value ("nNextMetaIx", static_cast<uint32_t> (1));

            if (jDoc.contains ("rules"))
            {
               for (auto& jRule : jDoc["rules"])
               {
                  RULE rule;
                  rule.sContentType = jRule.value ("contentType", "");
                  rule.sOlderThan = jRule.value ("olderThan", "");
                  m_aRules.push_back (rule);
               }
            }
         }
      }
      else
      {
         m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "NETWORK", "No rules.json -- creating fresh");
         SaveRules ();
      }
   }

   void SaveRules ()
   {
      if (!m_sCachePath.empty ())
      {
         nlohmann::json jDoc;
         jDoc["nNextMetaIx"] = m_nNextAssetIx;

         nlohmann::json jRules = nlohmann::json::array ();
         for (auto& rule : m_aRules)
         {
            nlohmann::json jRule;
            jRule["contentType"] = rule.sContentType;
            jRule["olderThan"] = rule.sOlderThan;
            jRules.push_back (jRule);
         }
         jDoc["rules"] = jRules;

         std::string sRulesPath = (std::filesystem::path (m_sCachePath) / "rules.json").string ();
         std::string sTmpPath = (std::filesystem::path (m_sCachePath) / "rules.json.temp").string ();

         std::ofstream file (sTmpPath, std::ios::trunc);
         if (file.is_open ())
         {
            file << jDoc.dump (2);
            file.close ();

            std::error_code ec;
            std::filesystem::rename (sTmpPath, sRulesPath, ec);
         }
      }
   }

   bool IsAssetStale (ASSET* pAsset) const
   {
      bool bResult = false;

      std::string sContentType = pAsset->Header ("content-type");
      std::string sCreatedAt = pAsset->CreatedTime ();

      for (auto& rule : m_aRules)
      {
         bool bTypeMatch = rule.sContentType.empty () || rule.sContentType == sContentType;
         bool bTimeMatch = !rule.sOlderThan.empty () && sCreatedAt < rule.sOlderThan;

         if (bTypeMatch && bTimeMatch)
         {
            bResult = true;
            break;
         }
      }

      return bResult;
   }

   // ---------------------------------------------------------------------------
   // Background fetch
   // ---------------------------------------------------------------------------

   void FetchAsset (ASSET* pAsset)
   {
      bool bResult = false;
      std::string sUrl;
      std::string sHash;
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         sUrl = pAsset->Url ();
         sHash = pAsset->Hash ();
         pAsset->SetFetchStartTime (SecondsSinceEpoch ());
      }
      std::string sDiskKey = ComputeDiskKey (sUrl);
      std::string sTmpPath = DiskKeyToPath (sDiskKey, DISKFILE_TEMP);
      std::string sFinalPath;
      uint64_t    nSizeBytes = 0;
      int         nHttpCode = 0;
      std::unordered_map<std::string, std::string> mapHeaders;

      if (m_bShuttingDown == false)
      {
         std::filesystem::create_directories (std::filesystem::path (sTmpPath).parent_path ());

         CURL* pCurl = curl_easy_init ();

         if (pCurl)
         {
            std::ofstream out (sTmpPath, std::ios::binary);

            if (out.is_open ())
            {
               curl_easy_setopt (pCurl, CURLOPT_URL, sUrl.c_str ());
               curl_easy_setopt (pCurl, CURLOPT_WRITEFUNCTION, FetchWriteCallback);
               curl_easy_setopt (pCurl, CURLOPT_WRITEDATA, &out);

               curl_easy_setopt (pCurl, CURLOPT_HEADERFUNCTION, FetchHeaderCallback);
               curl_easy_setopt (pCurl, CURLOPT_HEADERDATA, &mapHeaders);

               curl_easy_setopt (pCurl, CURLOPT_FOLLOWLOCATION, 1L);
               curl_easy_setopt (pCurl, CURLOPT_TIMEOUT, 300L);

#if !defined(_WIN32) && !defined(__APPLE__)
               // Android/Linux: BoringSSL has no default CA store; feed it
               // Mozilla's bundle embedded at configure time. Win32 (SChannel)
               // and Apple (SecureTransport) use their OS-managed stores.
               extern const char*        const g_szCaCertPem;
               extern const unsigned long       g_nCaCertPemLen;
               curl_blob caBlob;
               caBlob.data  = const_cast<void*> (static_cast<const void*> (g_szCaCertPem));
               caBlob.len   = g_nCaCertPemLen;
               caBlob.flags = CURL_BLOB_NOCOPY;
               curl_easy_setopt (pCurl, CURLOPT_CAINFO_BLOB, &caBlob);
#endif

               CURLcode nCode = curl_easy_perform (pCurl);

               out.close ();

               if (nCode == CURLE_OK)
                  curl_easy_getinfo (pCurl, CURLINFO_RESPONSE_CODE, &nHttpCode);

               if (m_bShuttingDown)
                  bResult = false;
               else if (nCode != CURLE_OK || nHttpCode < 200 || nHttpCode >= 300)
               {
                  std::string sErr = "Fetch failed for " + sUrl + " (HTTP " + std::to_string (nHttpCode) + ")";
                  if (nCode != CURLE_OK)
                     sErr += " curl=" + std::to_string (nCode) + " (" + curl_easy_strerror (nCode) + ")";
                  m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", sErr);
               }
               else
               {
                  std::string sAlgo, sExpected, sActual;

                  if (!sHash.empty ())
                  {
                     if (ParseSriHash (sHash, sAlgo, sExpected))
                     {
                        sActual = ComputeFileHash (sTmpPath, sAlgo);
                     }
                  }

                  if (sActual == sExpected)
                  {
                     sFinalPath = DiskKeyToPath (sDiskKey, DISKFILE_DATA);

                     std::error_code ec;
                     std::filesystem::rename (sTmpPath, sFinalPath, ec);
                     if (!ec)
                     {
                        auto nFsSize = std::filesystem::file_size (sFinalPath, ec);
                        if (!ec)
                        {
                           nSizeBytes = static_cast<uint64_t> (nFsSize);

                           bResult = true;
                        }
                     }
                     else
                     {
                        m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Failed to rename " + sTmpPath + " -> " + sFinalPath + ": " + ec.message ());
                     }
                  }
                  else m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Hash mismatch for " + sUrl + " (expected " + sExpected + ", got " + sActual + ")");
               }

               if (bResult == false)
               {
                  std::error_code ec;
                  std::filesystem::remove (sTmpPath, ec);
               }
            }
            else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "NETWORK", "Failed to open temp file: " + sTmpPath);
         }
         else m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "NETWORK", "Curl failed to initialize");

         curl_easy_cleanup (pCurl);
      }

      // Notify
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         pAsset->SetHttpStatus (nHttpCode);
         pAsset->SetFetchEndTime (SecondsSinceEpoch ());

         if (bResult)
         {
            pAsset->SetHeaders (mapHeaders);
            pAsset->Complete (sFinalPath, nSizeBytes);
            NotifyFiles (pAsset->CollectFiles (), STATE_READY);
         }
         else
         {
            if (!mapHeaders.empty ())
               pAsset->SetHeaders (mapHeaders);
            pAsset->SetState (STATE_FAILED);
            NotifyFiles (pAsset->CollectFiles (), STATE_FAILED);
         }

         auto it = m_mapAssets.find (sUrl);
         if (it != m_mapAssets.end () && it->second.get () == pAsset && pAsset->GetFileCount () == 0)
         {
            if (pAsset->State () == STATE_READY)
               SaveMeta (pAsset);
            m_mapAssets.erase (it);
         }
      }

      if (bResult)
      {
         m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "NETWORK", "Cached " + sUrl + " (" + std::to_string (nSizeBytes) + " bytes)");
      }

      DispatchNextFromQueue ();
   }

   NETWORK::FILE* Request (IFILE* pListener, VIEWPORT* pViewport, VIEWPORT::CONTAINER::CID* pCID, const std::string& sUrl, const std::string& sHash, uint32_t bFlags, uint32_t nAssetIx)
   {
      FILE* pFile = nullptr;
      bool bCreate = (bFlags & REQUEST_CREATE) != 0;
      bool bFetch  = (bFlags & REQUEST_FETCH) != 0;

      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         auto it = m_mapAssets.find (sUrl);

         if (it == m_mapAssets.end ())
         {
            std::string sDiskKey = ComputeDiskKey (sUrl);
            if (LoadMeta (sDiskKey, sUrl))
               it = m_mapAssets.find (sUrl);
         }

         if (it == m_mapAssets.end () && bCreate && bFetch)
         {
            auto pAssetPtr = std::make_unique<ASSET> (m_pNetwork, sUrl, sHash);
            pAssetPtr->SetAssetIx (m_nNextAssetIx++);
            m_mapAssets[sUrl] = std::move (pAssetPtr);
            it = m_mapAssets.find (sUrl);
         }

         if (it != m_mapAssets.end ())
         {
            ASSET* pAsset = it->second.get ();

            if (nAssetIx != 0 && pAsset->AssetIx () != nAssetIx)
               it = m_mapAssets.end ();
         }

         if (it != m_mapAssets.end ())
         {
            ASSET* pAsset = it->second.get ();

            if (pAsset->State () == STATE_READY && IsAssetStale (pAsset))
            {
               if (!bFetch)
               {
                  it = m_mapAssets.end ();
               }
               else
               {
                  ResetAsset (pAsset);
               }
            }
         }

         if (it != m_mapAssets.end ())
         {
            ASSET* pAsset = it->second.get ();

            pFile = new NETWORK::FILE (m_pNetwork, pAsset, pCID, pViewport, pListener, m_nNextFileIx++);
            pAsset->AttachFile (pFile);
            m_apFile.push_back (pFile);

            STATE bState = pAsset->State ();
            bool  bNeedsFetch = false;
            bool  bNotifyReady = false;
            bool  bNotifyFailed = false;

            if (bState == STATE_READY && !sHash.empty () && !pAsset->IsHashed ())
            {
               std::string sAlgo, sDigest;
               if (ParseSriHash (sHash, sAlgo, sDigest))
               {
                  std::string sComputed = ComputeFileHash (pAsset->DiskPath (), sAlgo);
                  if (sComputed == sDigest)
                  {
                     pAsset->SetHash (sHash);
                     pAsset->TouchAccess ();
                     pAsset->SetServedFromCache (true);
                  }
                  else
                  {
                     pAsset->SetHash (sHash);
                     bNeedsFetch = true;
                  }
               }
            }
            else if (bState != STATE_FETCHING && !sHash.empty () && pAsset->IsHashed () && pAsset->Hash () != sHash)
            {
               pAsset->SetHash (sHash);
               bNeedsFetch = true;
            }
            else if (bState == STATE_READY && !m_bCacheEnabled)
            {
               pAsset->SetServedFromCache (false);
               bNeedsFetch = true;
            }
            else if (bState == STATE_READY)
            {
               pAsset->TouchAccess ();
               pAsset->SetServedFromCache (true);
               bNotifyReady = true;
            }
            else if (bState == STATE_FAILED)
            {
               bNotifyFailed = true;
            }
            else if (bState == STATE_IDLE)
            {
               if (!sHash.empty ())
                  pAsset->SetHash (sHash);
               bNeedsFetch = true;
            }

            if (bNeedsFetch && bFetch)
            {
               pAsset->SetState (STATE_FETCHING);
               pAsset->SetFetchQueuedTime (SecondsSinceEpoch ());
               pFile->SnapshotProgress ();
               DispatchFetch (pAsset);
            }
            else if (bState != STATE_FETCHING)
            {
               pFile->SnapshotFinal ();
               if (bNeedsFetch)
                  bNotifyFailed = true;
            }

            if (bNotifyReady && pListener)
               pListener->OnFileReady (pFile);
            else if (bNotifyFailed && pListener)
               pListener->OnFileFailed (pFile);

            if (!m_bDisplayEnabled)
               pFile->SetPendingClear (true);

            if (!pFile->IsPendingClear ())
               pFile->Viewport ()->Host ()->OnNetworkFileCreated (pFile);
         }
      }

      return pFile;
   }

   void Clear (FILE* pFile, bool b)
   {
      if (pFile)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         if (pFile->SetPendingClear (b))
         {
            if (b)
            {
               pFile->Viewport ()->Host ()->OnNetworkFileDeleted (pFile);

               if (pFile->IsReleased ())
               {
                  auto it = std::find (m_apFile.begin (), m_apFile.end (), pFile);
                  if (it != m_apFile.end ())
                     m_apFile.erase (it);
                  delete pFile;
               }
            }
            else
            {
               pFile->Viewport ()->Host ()->OnNetworkFileCreated (pFile);
            }
         }
      }
   }

   void Reset (FILE* pFile, bool b)
   {
      if (pFile && pFile->Asset ())
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);

         pFile->Asset ()->SetPendingReset (b);
      }
   }

   // ---------------------------------------------------------------------------
   // Cache management
   // ---------------------------------------------------------------------------

   void Clear ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      auto it = m_apFile.begin ();
      while (it != m_apFile.end ())
      {
         FILE* pFile = *it;

         if (pFile->SetPendingClear (true))
            pFile->Viewport ()->Host ()->OnNetworkFileDeleted (pFile);

         if (pFile->IsReleased ())
         {
            it = m_apFile.erase (it);
            delete pFile;
         }
         else
         {
            ++it;
         }
      }
   }

   void Reset ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      for (auto& [sUrl, pAssetPtr] : m_mapAssets)
      {
         ASSET* pAsset = pAssetPtr.get ();
         pAsset->SetPendingReset (true);
         if (pAsset->GetFileCount () == 0)
            ResetAsset (pAsset);
      }

      std::error_code ec;
      std::filesystem::remove_all (m_sCachePath, ec);
      std::filesystem::create_directories (m_sCachePath);

      SaveRules ();
   }

   void Enumerate (IENUM* pEnum, VIEWPORT* pViewport)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      for (FILE* pFile : m_apFile)
      {
         if (pFile->Viewport () == pViewport)
            pEnum->OnAsset (pFile);
      }
   }

   void AddRule (const std::string& sContentType, const std::string& sOlderThan)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      if (sContentType.empty ())
         m_aRules.clear ();

      RULE rule;
      rule.sContentType = sContentType;
      rule.sOlderThan = sOlderThan;
      m_aRules.push_back (rule);

      SaveRules ();
   }

   void SetCacheEnabled (bool b)
   {
      m_bCacheEnabled = b;
   }

   bool IsCacheEnabled () const
   {
      return m_bCacheEnabled;
   }

   void SetDisplayEnabled (bool b)
   {
      m_bDisplayEnabled = b;
   }

   bool IsDisplayEnabled () const
   {
      return m_bDisplayEnabled;
   }

   // ---------------------------------------------------------------------------
   // Fetch thread management (capped at kMAX_CONCURRENT_FETCHES)
   // ---------------------------------------------------------------------------

   void SweepCompletedThreads ()
   {
      auto it = m_aSlots.begin ();
      while (it != m_aSlots.end ())
      {
         if (!(*it).joinable ())
         {
            it = m_aSlots.erase (it);
         }
         else
         {
            ++it;
         }
      }
   }

   void Worker (ASSET* pAsset)
   {
      FetchAsset (pAsset);
   }

   void DispatchFetch (ASSET* pAsset)
   {
      SweepCompletedThreads ();

      if (static_cast<int> (m_aSlots.size ()) < kMAX_CONCURRENT_FETCHES)
      {
         m_aSlots.emplace_back (&Impl::Worker, this, pAsset);
      }
      else
      {
         m_aFetchQueue.push (pAsset);
      }
   }

   // ---------------------------------------------------------------------------
   // Notification helpers (called under m_mutex -- recursive lock allows re-entry)
   // ---------------------------------------------------------------------------

   void DispatchNextFromQueue ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      if (m_bShuttingDown)
         return;

      SweepCompletedThreads ();

      if (!m_aFetchQueue.empty () && static_cast<int> (m_aSlots.size ()) < kMAX_CONCURRENT_FETCHES)
      {
         ASSET* pNext = m_aFetchQueue.front ();
         m_aFetchQueue.pop ();

         m_aSlots.emplace_back (&Impl::Worker, this, pNext);
      }
   }

   void NotifyFiles (const std::vector<NETWORK::FILE*>& apFiles, NETWORK::STATE bState)
   {
      for (auto* pFile : apFiles)
      {
         pFile->SnapshotFinal ();

         IFILE* pListener = pFile->Listener ();
         if (pListener)
         {
            if (bState == STATE_READY)
               pListener->OnFileReady (pFile);
            else
               pListener->OnFileFailed (pFile);
         }

         if (!pFile->IsPendingClear ())
            pFile->Viewport ()->Host ()->OnNetworkFileChanged (pFile);
      }
   }

   // ---------------------------------------------------------------------------
   // Timing helpers
   // ---------------------------------------------------------------------------

   double SecondsSinceEpoch () const
   {
      auto tpNow = std::chrono::steady_clock::now ();

      return std::chrono::duration<double> (tpNow - m_tpEpoch).count ();
   }

   // ---------------------------------------------------------------------------
   // File ownership
   // ---------------------------------------------------------------------------

   void DeleteFiles ()
   {
      for (auto* pFile : m_apFile)
         delete pFile;
      m_apFile.clear ();
   }

   void ResetAsset (ASSET* pAsset)
   {
      std::string sDiskKey = ComputeDiskKey (pAsset->Url ());
      std::error_code ec;

      std::filesystem::remove (DiskKeyToPath (sDiskKey, DISKFILE_DATA), ec);
      std::filesystem::remove (DiskKeyToPath (sDiskKey, DISKFILE_META), ec);
      std::filesystem::remove (DiskKeyToPath (sDiskKey, DISKFILE_TEMP), ec);

      pAsset->ResetState ();
      pAsset->SetAssetIx (m_nNextAssetIx++);
   }

private:
   NETWORK*                               m_pNetwork;
   ENGINE*                                m_pEngine;
   std::string                            m_sCachePath;

   std::unordered_map<std::string, std::unique_ptr<ASSET>> m_mapAssets;

   mutable std::recursive_mutex           m_mutex;

   std::vector<std::thread>               m_aSlots;
   std::queue<ASSET*>                     m_aFetchQueue;

   bool                                   m_bShuttingDown;
   bool                                   m_bCacheEnabled;
   bool                                   m_bDisplayEnabled;

   // Staleness rules + asset index counter
   std::vector<RULE>                      m_aRules;
   uint32_t                               m_nNextAssetIx;

   // Network inspector
   std::vector<FILE*>                     m_apFile;
   uint32_t                               m_nNextFileIx;
   std::chrono::steady_clock::time_point  m_tpEpoch;
};

// ---------------------------------------------------------------------------
// NETWORK
// ---------------------------------------------------------------------------

NETWORK::NETWORK (ENGINE* pEngine) :
   m_pImpl (new Impl (this, pEngine))
{
}

bool NETWORK::Initialize ()
{
   return m_pImpl->Initialize ();
}

NETWORK::~NETWORK ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Request / Release
// ---------------------------------------------------------------------------

NETWORK::FILE* NETWORK::Request (IFILE* pListener, VIEWPORT* pViewport, VIEWPORT::CONTAINER::CID* pCID, const std::string& sUrl)
{
   return Request (pListener, pViewport, pCID, sUrl, std::string (), kREQUEST_DEFAULT, 0);
}

NETWORK::FILE* NETWORK::Request (IFILE* pListener, VIEWPORT* pViewport, VIEWPORT::CONTAINER::CID* pCID, const std::string& sUrl, const std::string& sHash, uint32_t bFlags, uint32_t nAssetIx)
{
   return m_pImpl->Request (pListener, pViewport, pCID, sUrl, sHash, bFlags, nAssetIx);
}

void NETWORK::Release (FILE* pFile)
{
   m_pImpl->Release (pFile);
}

bool NETWORK::Reopen (FILE* pFile)
{
   return m_pImpl->Reopen (pFile);
}

void NETWORK::Clear (FILE* pFile, bool b)
{
   m_pImpl->Clear (pFile, b);
}

void NETWORK::Reset (FILE* pFile, bool b)
{
   m_pImpl->Reset (pFile, b);
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void NETWORK::Clear ()
{
   m_pImpl->Clear ();
}

void NETWORK::Reset ()
{
   m_pImpl->Reset ();
}

void NETWORK::Enumerate (IENUM* pEnum, VIEWPORT* pViewport)
{
   m_pImpl->Enumerate (pEnum, pViewport);
}

void NETWORK::AddRule (const std::string& sContentType, const std::string& sOlderThan)
{
   m_pImpl->AddRule (sContentType, sOlderThan);
}

void NETWORK::SetCacheEnabled (bool b) 
{ 
   m_pImpl->SetCacheEnabled (b);
}

bool NETWORK::IsCacheEnabled () const 
{ 
   return m_pImpl->IsCacheEnabled ();
}

void NETWORK::SetDisplayEnabled (bool b)
{ 
   m_pImpl->SetDisplayEnabled (b);
}

bool NETWORK::IsDisplayEnabled () const
{ 
   return m_pImpl->IsDisplayEnabled ();
}
