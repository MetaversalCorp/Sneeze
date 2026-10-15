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

#include "vps/Vps.h"
#include "vps/Vps_Device.h"

#include <Sneeze.h>
#include <Viewport.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <nlohmann/json.hpp>

namespace SNEEZE { namespace DEP {
   bool Vps_Http_Get (const std::string& sUrl, std::string& sBody, std::string& sError);
}}

namespace
{
   class DEVICE_STUB : public SNEEZE::DEP::IVPS_DEVICE
   {
   public:
      bool Localize (const SNEEZE::DEP::CAPTURE::INTRINSICS&, const std::vector<uint8_t>&, int, int, SNEEZE::DEP::VPS::POSE& Pose, std::string& sError) override
      {
         Pose.bValid = false;
         sError      = "VPS device is the stub";
         return false;
      }

      bool Catalog (std::vector<SNEEZE::DEP::VPS::MAP>&, std::string& sError) override
      {
         sError = "VPS device is the stub";
         return false;
      }

      void Maps (const std::vector<std::string>&) override
      {
      }
   };

   std::string Env_Or (const char* szName, const std::string& sFallback)
   {
      const char* szValue = std::getenv (szName);
      std::string sResult = sFallback;
      if (szValue  &&  szValue[0] != '\0')
         sResult = szValue;
      return sResult;
   }
}

class SNEEZE::DEP::VPS::Impl
{
public:
   explicit Impl (ENGINE* pEngine)
      : m_pEngine (pEngine)
      , m_pDevice (new DEVICE_STUB ())
      , m_bRun (false)
      , m_bBusy (false)
      , m_bPoseReady (false)
      , m_bOpened (false)
      , m_bQueried (false)
      , m_tpLast ()
   {
   }

   ~Impl ()
   {
      m_bRun = false;
      if (m_thFetch.joinable ())
         m_thFetch.join ();
      if (m_thCatalog.joinable ())
         m_thCatalog.join ();
      if (m_th.joinable ())
         m_th.join ();
      Close_Device ();
      delete m_pDevice;
      m_pDevice = nullptr;
   }

   void Load (const std::string& sAppData)
   {
      CONFIG Config;
      bool   bFile = false;

      if (!sAppData.empty ())
      {
         std::filesystem::path sPath = std::filesystem::path (sAppData) / "vps.json";
         std::ifstream         File (sPath);
         if (File)
         {
            try
            {
               nlohmann::json jConfig;
               File >> jConfig;
               Config.sUrl      = jConfig.value ("url", std::string ());
               Config.sToken    = jConfig.value ("token", std::string ());
               Config.sUser     = jConfig.value ("username", std::string ());
               Config.sPassword = jConfig.value ("password", std::string ());
               Config.nDevice   = jConfig.value ("device", 0);
               Config.dInterval = jConfig.value ("interval", 2.0);
               if (jConfig.contains ("maps")  &&  jConfig["maps"].is_array ())
               {
                  for (const nlohmann::json& jMap : jConfig["maps"])
                  {
                     if (jMap.is_string ())
                        Config.asMap.push_back (jMap.get<std::string> ());
                  }
               }
               bFile = !Config.sUrl.empty ();
            }
            catch (...)
            {
               if (m_pEngine)
                  m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "VPS", "vps.json could not be read");
            }
         }
      }

      Config.sUrl      = Env_Or ("SNEEZE_VPS_URL", Config.sUrl);
      Config.sToken    = Env_Or ("SNEEZE_VPS_TOKEN", Config.sToken);
      Config.sUser     = Env_Or ("SNEEZE_VPS_USER", Config.sUser);
      Config.sPassword = Env_Or ("SNEEZE_VPS_PASSWORD", Config.sPassword);

      const char* szMaps = std::getenv ("SNEEZE_VPS_MAPS");
      if (szMaps  &&  szMaps[0] != '\0')
      {
         Config.asMap.clear ();
         std::stringstream ssMaps (szMaps);
         std::string       sMap;
         while (std::getline (ssMaps, sMap, ','))
         {
            if (!sMap.empty ())
               Config.asMap.push_back (sMap);
         }
      }

      const char* szDevice = std::getenv ("SNEEZE_VPS_DEVICE");
      if (szDevice  &&  szDevice[0] != '\0')
         Config.nDevice = std::atoi (szDevice);

      const char* szInterval = std::getenv ("SNEEZE_VPS_INTERVAL");
      if (szInterval  &&  szInterval[0] != '\0')
         Config.dInterval = std::atof (szInterval);

      if (Config.dInterval < 0.5)
         Config.dInterval = 0.5;

      Arm (Config, bFile);
   }

   void Arm (CONFIG Config, bool bFile)
   {
      m_Config = Config;
      delete m_pDevice;
      m_pDevice = nullptr;

      if (!m_Config.sUrl.empty ())
      {
         m_pDevice = Device_Placeframe_New (m_pEngine, m_Config);
         m_bRun    = true;
         if (m_pEngine)
            m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "VPS", "Placeframe device " + m_Config.sUrl);
         m_thCatalog = std::thread ([this] ()
         {
            std::vector<VPS::MAP> aMap;
            std::string           sError;
            bool                  bOk = m_pDevice  &&  m_pDevice->Catalog (aMap, sError);

            if (bOk)
            {
               std::lock_guard<std::mutex> guard (m_mx);
               m_aCatalog     = aMap;
               m_bCatalogReady = true;
            }
            else if (!sError.empty ()  &&  m_pEngine)
               m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "VPS", sError);
         });
      }
      else
      {
         m_pDevice = new DEVICE_STUB ();
         m_bRun    = false;
         if (m_pEngine  &&  bFile)
            m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "VPS", "idle");
      }
   }

   void Load_Text (const std::string& sJson)
   {
      Stop_Workers ();
      Close_Device ();

      {
         std::lock_guard<std::mutex> guard (m_mx);
         m_aCatalog.clear ();
         m_bCatalogReady = false;
         m_sSelected.clear ();
         m_bSkipped = false;
         m_bPoseReady = false;
         m_bQueried = false;
      }

      CONFIG Config;
      bool   bFile = false;

      try
      {
         nlohmann::json jConfig = nlohmann::json::parse (sJson);
         Config.sUrl      = jConfig.value ("url", std::string ());
         Config.sToken    = jConfig.value ("token", std::string ());
         Config.sUser     = jConfig.value ("username", std::string ());
         Config.sPassword = jConfig.value ("password", std::string ());
         Config.nDevice   = jConfig.value ("device", 0);
         Config.dInterval = jConfig.value ("interval", 2.0);
         if (jConfig.contains ("maps")  &&  jConfig["maps"].is_array ())
         {
            for (const nlohmann::json& jMap : jConfig["maps"])
            {
               if (jMap.is_string ())
                  Config.asMap.push_back (jMap.get<std::string> ());
            }
         }
         bFile = !Config.sUrl.empty ();
      }
      catch (...)
      {
         if (m_pEngine)
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "VPS", "vps.json could not be read");
      }

      if (Config.dInterval < 0.5)
         Config.dInterval = 0.5;

      Arm (Config, bFile);
   }

   void Close_Device ()
   {
      CAPTURE* pCapture = (m_pEngine) ? m_pEngine->Capture () : nullptr;
      if (m_bOpened  &&  pCapture)
         pCapture->Device_Close (m_Config.nDevice);
      m_bOpened = false;
   }

   void Stop_Workers ()
   {
      m_bRun = false;
      if (m_thCatalog.joinable ())
         m_thCatalog.join ();
      std::lock_guard<std::mutex> guard (m_mxWorker);
      if (m_th.joinable ())
         m_th.join ();
   }

   void Launch (CAPTURE::INTRINSICS Intrinsics, std::vector<uint8_t> aRgba, int nWidth, int nHeight)
   {
      std::lock_guard<std::mutex> guard (m_mxWorker);
      if (!m_bRun)
         return;
      if (m_th.joinable ())
         m_th.join ();

      m_bBusy = true;
      m_th = std::thread ([this, Intrinsics, aRgba, nWidth, nHeight] ()
      {
         POSE        Pose;
         std::string sError;
         bool        bOk = false;

         if (m_pDevice)
            bOk = m_pDevice->Localize (Intrinsics, aRgba, nWidth, nHeight, Pose, sError);

         {
            std::lock_guard<std::mutex> guard (m_mx);
            if (!m_bSkipped)
            {
               m_Pose       = Pose;
               m_bPoseReady = bOk  &&  Pose.bValid;
               if (m_bPoseReady)
                  m_sNote = "pose";
               else if (!sError.empty ())
                  m_sNote = sError;
               else
                  m_sNote = "no pose";
            }
         }

         if (!bOk  &&  !sError.empty ()  &&  m_pEngine)
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "VPS", sError);

         m_bBusy = false;
      });
   }

   void Tick (VIEWPORT* pViewport)
   {
      {
         std::lock_guard<std::mutex> guard (m_mx);
         if (m_bSkipped  ||  m_sSelected.empty ())
            return;
      }

      POSE Pose;
      bool bApply = false;

      {
         std::lock_guard<std::mutex> guard (m_mx);
         if (m_bPoseReady)
         {
            Pose         = m_Pose;
            m_bPoseReady = false;
            bApply       = Pose.bValid;
         }
      }

         if (bApply  &&  pViewport)
         {
            VIEWPORT::CAMERA Camera;
            Camera.aPosition[0] = Pose.aPosition[0];
            Camera.aPosition[1] = Pose.aPosition[1];
            Camera.aPosition[2] = Pose.aPosition[2];
            Camera.aRotation[0] = Pose.aRotation[0];
            Camera.aRotation[1] = Pose.aRotation[1];
            Camera.aRotation[2] = Pose.aRotation[2];
            Camera.aRotation[3] = Pose.aRotation[3];
            Camera.dFovY        = Pose.dFovY;
            pViewport->Camera (Camera);
            if (pViewport->Passthrough_Allowed ())
               pViewport->Passthrough (true);
         }

      if (m_bRun  &&  !m_bBusy  &&  !m_sSelected.empty ()  &&  m_pEngine  &&  m_pEngine->Capture ())
      {
         auto tpNow = std::chrono::steady_clock::now ();
         double dElapsed = std::chrono::duration<double> (tpNow - m_tpLast).count ();
         if (!m_bQueried  ||  dElapsed >= m_Config.dInterval)
         {
            CAPTURE* pCapture = m_pEngine->Capture ();
            bool     bOpen    = pCapture->Device_IsOpen (m_Config.nDevice);
            if (!bOpen)
            {
               bOpen     = pCapture->Device_Open (m_Config.nDevice);
               m_bOpened = bOpen;
            }

            int                  nWidth = 0;
            int                  nHeight = 0;
            uint64_t             nFrame = 0;
            std::vector<uint8_t> aRgba;
            CAPTURE::INTRINSICS  Intrinsics;

            if (bOpen  &&  pCapture->Frame_Latest (m_Config.nDevice, nWidth, nHeight, aRgba, nFrame)  &&  pCapture->Device_Intrinsics (m_Config.nDevice, Intrinsics))
            {
               m_tpLast   = tpNow;
               m_bQueried = true;
               Launch (Intrinsics, std::move (aRgba), nWidth, nHeight);
            }
         }
      }
   }

   ENGINE*                 m_pEngine;
   IVPS_DEVICE*            m_pDevice;
   CONFIG                  m_Config;
   std::mutex              m_mx;
   POSE                    m_Pose;
   std::string             m_sNote;
   std::atomic<bool>       m_bRun;
   std::atomic<bool>       m_bBusy;
   bool                    m_bPoseReady;
   bool                    m_bOpened;
   bool                    m_bQueried;
   std::mutex              m_mxWorker;
   std::thread             m_th;
   std::thread             m_thCatalog;
   std::thread             m_thFetch;
   std::vector<VPS::MAP>   m_aCatalog;
   bool                    m_bCatalogReady = false;
   std::string             m_sSelected;
   bool                    m_bSkipped = false;
   std::chrono::steady_clock::time_point m_tpLast;
};

SNEEZE::DEP::VPS::VPS (ENGINE* pEngine)
   : m_pImpl (new Impl (pEngine))
{
}

SNEEZE::DEP::VPS::~VPS ()
{
   delete m_pImpl;
   m_pImpl = nullptr;
}

bool SNEEZE::DEP::VPS::Initialize ()
{
   bool bOk = m_pImpl != nullptr;
   if (bOk)
   {
      std::string sApp;
      if (m_pImpl->m_pEngine  &&  m_pImpl->m_pEngine->Host ())
         sApp = m_pImpl->m_pEngine->Host ()->sAppDataPath ();
      m_pImpl->Load (sApp);
   }
   return bOk;
}

void SNEEZE::DEP::VPS::Tick (VIEWPORT* pViewport)
{
   if (m_pImpl)
      m_pImpl->Tick (pViewport);
}

SNEEZE::DEP::VPS::POSE SNEEZE::DEP::VPS::Pose () const
{
   POSE Pose;
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      Pose = m_pImpl->m_Pose;
   }
   return Pose;
}

std::string SNEEZE::DEP::VPS::Note () const
{
   std::string sNote;
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      sNote = m_pImpl->m_sNote;
   }
   return sNote;
}

bool SNEEZE::DEP::VPS::IsRunning () const
{
   bool bRun = false;
   if (m_pImpl)
      bRun = m_pImpl->m_bRun;
   return bRun;
}

bool SNEEZE::DEP::VPS::Maps_Ready () const
{
   bool bReady = false;
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      bReady = m_pImpl->m_bCatalogReady;
   }
   return bReady;
}

void SNEEZE::DEP::VPS::Maps (std::vector<MAP>& aMap) const
{
   aMap.clear ();
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      aMap = m_pImpl->m_aCatalog;
   }
}

void SNEEZE::DEP::VPS::Map_Select (const std::string& sId)
{
   if (m_pImpl  &&  !sId.empty ())
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      m_pImpl->m_bSkipped  = false;
      m_pImpl->m_sSelected = sId;
      m_pImpl->m_bQueried  = false;
      if (m_pImpl->m_pDevice)
         m_pImpl->m_pDevice->Maps ({ sId });
      if (m_pImpl->m_pEngine)
         m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "VPS", "map " + sId);
   }
}

void SNEEZE::DEP::VPS::Skip ()
{
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      m_pImpl->m_bSkipped   = true;
      m_pImpl->m_sSelected.clear ();
      m_pImpl->m_bPoseReady = false;
      m_pImpl->m_bQueried   = false;
      if (m_pImpl->m_pEngine)
         m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "VPS", "skipped");
   }
}

bool SNEEZE::DEP::VPS::IsSkipped () const
{
   bool bSkipped = false;
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      bSkipped = m_pImpl->m_bSkipped;
   }
   return bSkipped;
}

std::string SNEEZE::DEP::VPS::Map_Selected () const
{
   std::string sId;
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> guard (m_pImpl->m_mx);
      sId = m_pImpl->m_sSelected;
   }
   return sId;
}

void SNEEZE::DEP::VPS::Load_Url (const std::string& sUrl)
{
   if (m_pImpl  &&  !sUrl.empty ())
   {
      if (m_pImpl->m_thFetch.joinable ())
         m_pImpl->m_thFetch.join ();

      m_pImpl->m_thFetch = std::thread ([this, sUrl] ()
      {
         std::string sBody;
         std::string sError;
         bool        bOk = Vps_Http_Get (sUrl, sBody, sError);

         if (!bOk)
         {
            if (sError.empty ())
               sError = "vps.json fetch failed";
            if (m_pImpl->m_pEngine)
               m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "VPS", sError);
         }
         else
         {
            if (m_pImpl->m_pEngine)
               m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "VPS", "vps.json " + sUrl);
            m_pImpl->Load_Text (sBody);
         }
      });
   }
}
