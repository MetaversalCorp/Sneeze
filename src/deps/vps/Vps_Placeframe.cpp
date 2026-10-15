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

#include "vps/Vps_Device.h"

#include <Sneeze.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
namespace SNEEZE
{
   extern const char*        const g_szCaCertPem;
   extern const unsigned long       g_nCaCertPemLen;
}
#endif

namespace
{
   size_t Write_Body (char* pData, size_t nSize, size_t nNmemb, void* pUser)
   {
      size_t nBytes = nSize * nNmemb;
      std::string* pBody = static_cast<std::string*> (pUser);
      if (pBody)
         pBody->append (pData, nBytes);
      return nBytes;
   }

   bool Bmp_Encode (int nWidth, int nHeight, const std::vector<uint8_t>& aRgba, std::vector<uint8_t>& aBmp)
   {
      bool bOk = nWidth > 0  &&  nHeight > 0  &&  aRgba.size () >= static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4u;

      if (bOk)
      {
         int            nStride = (nWidth * 3 + 3) & ~3;
         uint32_t       nPixels = static_cast<uint32_t> (nStride * nHeight);
         uint32_t       nFile   = 54u + nPixels;
         unsigned char  aHdr[54];

         std::memset (aHdr, 0, sizeof (aHdr));
         aHdr[0] = 'B';
         aHdr[1] = 'M';
         std::memcpy (aHdr + 2, &nFile, 4);
         aHdr[10] = 54;
         aHdr[14] = 40;
         std::memcpy (aHdr + 18, &nWidth, 4);
         std::memcpy (aHdr + 22, &nHeight, 4);
         aHdr[26] = 1;
         aHdr[28] = 24;
         std::memcpy (aHdr + 34, &nPixels, 4);

         aBmp.assign (aHdr, aHdr + 54);
         aBmp.resize (nFile, 0);

         for (int nY = 0; nY < nHeight; ++nY)
         {
            int nSrcY = nHeight - 1 - nY;
            unsigned char* pRow = aBmp.data () + 54 + static_cast<size_t> (nY) * static_cast<size_t> (nStride);
            const uint8_t* pSrc = aRgba.data () + static_cast<size_t> (nSrcY) * static_cast<size_t> (nWidth) * 4u;
            for (int nX = 0; nX < nWidth; ++nX)
            {
               pRow[nX * 3 + 0] = pSrc[nX * 4 + 2];
               pRow[nX * 3 + 1] = pSrc[nX * 4 + 1];
               pRow[nX * 3 + 2] = pSrc[nX * 4 + 0];
            }
         }
      }

      return bOk;
   }

   std::string Endpoint (const std::string& sUrl, const std::string& sPath)
   {
      std::string sEndpoint = sUrl;
      while (!sEndpoint.empty ()  &&  sEndpoint.back () == '/')
         sEndpoint.pop_back ();
      if (sPath == "/localize"  &&  sEndpoint.size () >= 9  &&  sEndpoint.compare (sEndpoint.size () - 9, 9, "/localize") == 0)
         return sEndpoint;
      return sEndpoint + sPath;
   }

   std::string Form_Encode (const std::string& sValue)
   {
      static const char szHex[] = "0123456789ABCDEF";
      std::string sOut;
      for (size_t n = 0; n < sValue.size (); ++n)
      {
         unsigned char nCh = static_cast<unsigned char> (sValue[n]);
         if ((nCh >= 'A' && nCh <= 'Z') || (nCh >= 'a' && nCh <= 'z') || (nCh >= '0' && nCh <= '9') || nCh == '-' || nCh == '_' || nCh == '.' || nCh == '~')
            sOut.push_back (static_cast<char> (nCh));
         else if (nCh == ' ')
            sOut.push_back ('+');
         else
         {
            sOut.push_back ('%');
            sOut.push_back (szHex[nCh >> 4]);
            sOut.push_back (szHex[nCh & 0x0F]);
         }
      }
      return sOut;
   }

   struct HTTP_RESULT
   {
      long        nStatus = 0;
      std::string sBody;
      std::string sError;
   };

   void Ca_Set (CURL* pCurl)
   {
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
      curl_blob caBlob;
      caBlob.data  = const_cast<void*> (static_cast<const void*> (SNEEZE::g_szCaCertPem));
      caBlob.len   = SNEEZE::g_nCaCertPemLen;
      caBlob.flags = CURL_BLOB_NOCOPY;
      curl_easy_setopt (pCurl, CURLOPT_CAINFO_BLOB, &caBlob);
#else
      (void) pCurl;
#endif
   }

   HTTP_RESULT Http (const std::string& sUrl, const std::string& sToken, const std::string& sPost, const char* szContentType)
   {
      HTTP_RESULT Result;
      CURL* pCurl = curl_easy_init ();
      curl_slist* pHdr = nullptr;
      std::string sAuth;

      if (!pCurl)
         Result.sError = "curl_easy_init failed";
      else
      {
         pHdr = curl_slist_append (pHdr, "ngrok-skip-browser-warning: true");
         if (szContentType)
            pHdr = curl_slist_append (pHdr, szContentType);
         if (!sToken.empty ())
         {
            sAuth = "Authorization: Bearer " + sToken;
            pHdr = curl_slist_append (pHdr, sAuth.c_str ());
         }

         curl_easy_setopt (pCurl, CURLOPT_URL, sUrl.c_str ());
         Ca_Set (pCurl);
         curl_easy_setopt (pCurl, CURLOPT_HTTPHEADER, pHdr);
         curl_easy_setopt (pCurl, CURLOPT_WRITEFUNCTION, Write_Body);
         curl_easy_setopt (pCurl, CURLOPT_WRITEDATA, &Result.sBody);
         curl_easy_setopt (pCurl, CURLOPT_TIMEOUT, 20L);
         curl_easy_setopt (pCurl, CURLOPT_NOSIGNAL, 1L);
         if (!sPost.empty ())
         {
            curl_easy_setopt (pCurl, CURLOPT_POST, 1L);
            curl_easy_setopt (pCurl, CURLOPT_POSTFIELDS, sPost.c_str ());
            curl_easy_setopt (pCurl, CURLOPT_POSTFIELDSIZE, static_cast<long> (sPost.size ()));
         }

         CURLcode eCode = curl_easy_perform (pCurl);
         curl_easy_getinfo (pCurl, CURLINFO_RESPONSE_CODE, &Result.nStatus);
         if (eCode != CURLE_OK)
            Result.sError = curl_easy_strerror (eCode);
      }

      if (pHdr)
         curl_slist_free_all (pHdr);
      if (pCurl)
         curl_easy_cleanup (pCurl);
      return Result;
   }
}

namespace SNEEZE { namespace DEP {

bool Vps_Http_Get (const std::string& sUrl, std::string& sBody, std::string& sError)
{
   HTTP_RESULT Result = Http (sUrl, "", "", nullptr);
   bool bOk = Result.sError.empty ()  &&  (Result.nStatus == 200);
   if (bOk)
      sBody = Result.sBody;
   else if (!Result.sError.empty ())
      sError = Result.sError;
   else
      sError = "vps.json HTTP " + std::to_string (Result.nStatus);
   return bOk;
}

}}

class DEVICE_PLACEFRAME : public SNEEZE::DEP::IVPS_DEVICE
{
public:
   DEVICE_PLACEFRAME (SNEEZE::ENGINE* pEngine, const SNEEZE::DEP::VPS::CONFIG& Config)
      : m_pEngine (pEngine)
      , m_Config (Config)
      , m_sToken (Config.sToken)
   {
      (void) m_pEngine;
   }

   bool Login (std::string& sError)
   {
      bool bOk = false;
      std::string sBody = "grant_type=password&client_id=placeframe-api&scope=openid&username=" + Form_Encode (m_Config.sUser) + "&password=" + Form_Encode (m_Config.sPassword);
      HTTP_RESULT HttpResult = Http (Endpoint (m_Config.sUrl, "/auth/realms/placeframe-dev/protocol/openid-connect/token"), "", sBody, "Content-Type: application/x-www-form-urlencoded");

      if (!HttpResult.sError.empty ())
         sError = HttpResult.sError;
      else if (HttpResult.nStatus != 200)
         sError = "Placeframe login HTTP " + std::to_string (HttpResult.nStatus);
      else
      {
         try
         {
            nlohmann::json jToken = nlohmann::json::parse (HttpResult.sBody);
            m_sToken = jToken.value ("access_token", std::string ());
            bOk = !m_sToken.empty ();
            if (!bOk)
               sError = "Placeframe login returned no access token";
         }
         catch (...)
         {
            sError = "Placeframe login response was not JSON";
         }
      }

      return bOk;
   }

   bool Catalog (std::vector<SNEEZE::DEP::VPS::MAP>& aMap, std::string& sError) override
   {
      bool bOk = Ensure (sError);
      HTTP_RESULT HttpResult;

      aMap.clear ();
      if (bOk)
         HttpResult = Http (Endpoint (m_Config.sUrl, "/localization-maps"), m_sToken, "", nullptr);

      if (!bOk)
         bOk = false;
      else if (!HttpResult.sError.empty ())
      {
         sError = HttpResult.sError;
         bOk = false;
      }
      else if (HttpResult.nStatus != 200)
      {
         sError = "Placeframe maps HTTP " + std::to_string (HttpResult.nStatus);
         bOk = false;
      }
      else
      {
         try
         {
            nlohmann::json jMaps = nlohmann::json::parse (HttpResult.sBody);
            if (jMaps.is_array ())
            {
               for (const nlohmann::json& jMap : jMaps)
               {
                  if (jMap.contains ("id")  &&  jMap["id"].is_string ())
                  {
                     SNEEZE::DEP::VPS::MAP Map;
                     Map.sId = jMap["id"].get<std::string> ();
                     if (jMap.contains ("name")  &&  jMap["name"].is_string ())
                        Map.sName = jMap["name"].get<std::string> ();
                     aMap.push_back (Map);
                  }
               }
            }
            bOk = !aMap.empty ();
            if (!bOk)
               sError = "Placeframe has no localization maps";
         }
         catch (...)
         {
            sError = "Placeframe map list was not JSON";
            bOk = false;
         }
      }

      return bOk;
   }

   bool Has_Map ()
   {
      std::lock_guard<std::mutex> guard (m_mxMap);
      return !m_Config.asMap.empty ();
   }

   void Maps (const std::vector<std::string>& asMap) override
   {
      std::lock_guard<std::mutex> guard (m_mxMap);
      m_Config.asMap = asMap;
   }

   bool Ensure (std::string& sError)
   {
      bool bOk = true;

      if (m_sToken.empty ())
      {
         if (m_Config.sUser.empty ()  ||  m_Config.sPassword.empty ())
         {
            sError = "Placeframe credentials are empty";
            bOk = false;
         }
         else
            bOk = Login (sError);
      }

      return bOk;
   }

   bool Localize (const SNEEZE::DEP::CAPTURE::INTRINSICS& Intrinsics, const std::vector<uint8_t>& aRgba, int nWidth, int nHeight, SNEEZE::DEP::VPS::POSE& Pose, std::string& sError) override
   {
      bool           bOk   = false;
      CURL*          pCurl = nullptr;
      curl_mime*     pMime = nullptr;
      curl_slist*    pHdr  = nullptr;
      std::string    sBody;
      std::string    sAuth;
      std::vector<uint8_t> aBmp;
      long           nHttp = 0;

      Pose.bValid = false;
      sError.clear ();

      if (m_Config.sUrl.empty ())
         sError = "Placeframe URL is empty";
      else if (!Ensure (sError))
         bOk = false;
      else if (!Has_Map ())
         sError = "Placeframe map is not selected";
      else if (!Bmp_Encode (nWidth, nHeight, aRgba, aBmp))
         sError = "Placeframe frame is empty";
      else if ((pCurl = curl_easy_init ()) == nullptr)
         sError = "curl_easy_init failed";
      else
      {
         std::vector<std::string> asMap;
         {
            std::lock_guard<std::mutex> guard (m_mxMap);
            asMap = m_Config.asMap;
         }
         nlohmann::json jMaps = nlohmann::json::array ();
         for (size_t nMap = 0; nMap < asMap.size (); ++nMap)
            jMaps.push_back (asMap[nMap]);

         double dSx = (Intrinsics.nWidth > 0) ? static_cast<double> (nWidth) / static_cast<double> (Intrinsics.nWidth) : 1.0;
         double dSy = (Intrinsics.nHeight > 0) ? static_cast<double> (nHeight) / static_cast<double> (Intrinsics.nHeight) : 1.0;
         const char* szOrientation = SNEEZE::DEP::CAPTURE::Orientation_Name (Intrinsics.eOrientation);
         if (!szOrientation)
            szOrientation = "TOP_LEFT";

         nlohmann::json jCamera = {
            { "width", nWidth },
            { "height", nHeight },
            { "orientation", szOrientation },
            { "fx", Intrinsics.dFx * dSx },
            { "fy", Intrinsics.dFy * dSy },
            { "cx", Intrinsics.dCx * dSx },
            { "cy", Intrinsics.dCy * dSy },
         };

         std::string sMaps   = jMaps.dump ();
         std::string sCamera = jCamera.dump ();
         std::string sUrl    = Endpoint (m_Config.sUrl, "/localize");

         pMime = curl_mime_init (pCurl);
         if (pMime)
         {
            curl_mimepart* pPart = curl_mime_addpart (pMime);
            curl_mime_name (pPart, "map_ids");
            curl_mime_data (pPart, sMaps.c_str (), CURL_ZERO_TERMINATED);

            pPart = curl_mime_addpart (pMime);
            curl_mime_name (pPart, "camera_config");
            curl_mime_data (pPart, sCamera.c_str (), CURL_ZERO_TERMINATED);

            pPart = curl_mime_addpart (pMime);
            curl_mime_name (pPart, "axis_convention");
            curl_mime_data (pPart, "OPENCV", CURL_ZERO_TERMINATED);

            pPart = curl_mime_addpart (pMime);
            curl_mime_name (pPart, "image");
            curl_mime_filename (pPart, "frame.bmp");
            curl_mime_type (pPart, "image/bmp");
            curl_mime_data (pPart, reinterpret_cast<const char*> (aBmp.data ()), aBmp.size ());
         }

         pHdr = curl_slist_append (pHdr, "ngrok-skip-browser-warning: true");
         if (!m_sToken.empty ())
         {
            sAuth = "Authorization: Bearer " + m_sToken;
            pHdr  = curl_slist_append (pHdr, sAuth.c_str ());
         }

         curl_easy_setopt (pCurl, CURLOPT_URL, sUrl.c_str ());
         Ca_Set (pCurl);
         curl_easy_setopt (pCurl, CURLOPT_MIMEPOST, pMime);
         curl_easy_setopt (pCurl, CURLOPT_HTTPHEADER, pHdr);
         curl_easy_setopt (pCurl, CURLOPT_WRITEFUNCTION, Write_Body);
         curl_easy_setopt (pCurl, CURLOPT_WRITEDATA, &sBody);
         curl_easy_setopt (pCurl, CURLOPT_TIMEOUT, 20L);
         curl_easy_setopt (pCurl, CURLOPT_NOSIGNAL, 1L);

         CURLcode eCode = curl_easy_perform (pCurl);
         curl_easy_getinfo (pCurl, CURLINFO_RESPONSE_CODE, &nHttp);

         if (eCode != CURLE_OK)
            sError = curl_easy_strerror (eCode);
         else if (nHttp == 401)
         {
            m_sToken.clear ();
            sError = "Placeframe localize HTTP 401";
         }
         else if (nHttp != 200  &&  nHttp != 201)
            sError = "Placeframe localize HTTP " + std::to_string (nHttp);
         else if (!SNEEZE::DEP::Pose_From_Placeframe (sBody, Pose))
            sError = "Placeframe localize returned no pose";
         else
         {
            if (Intrinsics.dFy > 1e-6)
               Pose.dFovY = 2.0 * std::atan (static_cast<double> (nHeight) / (2.0 * Intrinsics.dFy * dSy));
            bOk = Pose.bValid;
         }
      }

      if (pMime)
         curl_mime_free (pMime);
      if (pHdr)
         curl_slist_free_all (pHdr);
      if (pCurl)
         curl_easy_cleanup (pCurl);

      return bOk;
   }

private:
   SNEEZE::ENGINE*          m_pEngine;
   SNEEZE::DEP::VPS::CONFIG m_Config;
   std::mutex               m_mxMap;
   std::string              m_sToken;
};

SNEEZE::DEP::IVPS_DEVICE* SNEEZE::DEP::Device_Placeframe_New (ENGINE* pEngine, const VPS::CONFIG& Config)
{
   return new DEVICE_PLACEFRAME (pEngine, Config);
}
