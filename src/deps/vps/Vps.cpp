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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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

   const double SLEW_SECONDS = 0.5;
   const double PI           = 3.14159265358979323846;

   void Quat_Rotate (const double aQ[4], const double aV[3], double aOut[3])
   {
      double dTx = 2.0 * (aQ[1] * aV[2] - aQ[2] * aV[1]);
      double dTy = 2.0 * (aQ[2] * aV[0] - aQ[0] * aV[2]);
      double dTz = 2.0 * (aQ[0] * aV[1] - aQ[1] * aV[0]);

      aOut[0] = aV[0] + aQ[3] * dTx + (aQ[1] * dTz - aQ[2] * dTy);
      aOut[1] = aV[1] + aQ[3] * dTy + (aQ[2] * dTx - aQ[0] * dTz);
      aOut[2] = aV[2] + aQ[3] * dTz + (aQ[0] * dTy - aQ[1] * dTx);
   }

   // Map pose of the camera at capture times the inverse of the device
   // orientation at capture, kept to a turn about +Z. Both frames are level
   // with gravity, so only the heading differs.
   double Yaw_Align (const double aMap[4], const double aDevice[4])
   {
      const double aAxis[3]    = { 1.0, 0.0, 0.0 };
      const double aInverse[4] = { -aDevice[0], -aDevice[1], -aDevice[2], aDevice[3] };
      double       aLocal[3];
      double       aWorld[3];
      double       dYaw = 0.0;

      Quat_Rotate (aInverse, aAxis, aLocal);
      Quat_Rotate (aMap, aLocal, aWorld);
      if (std::hypot (aWorld[0], aWorld[1]) > 1e-6)
         dYaw = std::atan2 (aWorld[1], aWorld[0]);

      return dYaw;
   }

   double Angle_Wrap (double dAngle)
   {
      while (dAngle > PI)
         dAngle -= 2.0 * PI;
      while (dAngle < -PI)
         dAngle += 2.0 * PI;
      return dAngle;
   }

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

   void Launch (CAPTURE::INTRINSICS Intrinsics, std::vector<uint8_t> aRgba, int nWidth, int nHeight, bool bOrient, const double aOrient[4])
   {
      std::lock_guard<std::mutex> guard (m_mxWorker);
      if (!m_bRun)
         return;
      if (m_th.joinable ())
         m_th.join ();

      std::array<double, 4> aCapture = { aOrient[0], aOrient[1], aOrient[2], aOrient[3] };

      m_bBusy = true;
      m_th = std::thread ([this, Intrinsics, aRgba, nWidth, nHeight, bOrient, aCapture] ()
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
               if (bOk  &&  Pose.bValid)
               {
                  m_Pose          = Pose;
                  m_bPoseReady    = true;
                  m_bPose_Orient  = bOrient;
                  m_aPose_Orient  = aCapture;
                  m_sNote         = "pose";
               }
               else
               {
                  m_bPoseReady = false;
                  if (!sError.empty ())
                     m_sNote = sError;
                  else
                     m_sNote = "no pose";
               }
            }
         }

         if (!bOk  &&  !sError.empty ()  &&  m_pEngine)
            m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "VPS", sError);

         m_bBusy = false;
      });
   }

   // A fix taken with the device orientation becomes an alignment: the
   // tracking frame placed in the map, so any turn since the capture is
   // already in the live orientation and nothing goes stale. The first fix
   // snaps; later ones slew in. A fix without orientation (desktop webcam)
   // is applied as an absolute camera pose.
   void Fix_Apply (VIEWPORT* pViewport, const POSE& Pose, bool bOrient, const std::array<double, 4>& aOrient)
   {
      if (bOrient)
      {
         VIEWPORT::ALIGNMENT Target;
         Target.aPosition[0] = Pose.aPosition[0];
         Target.aPosition[1] = Pose.aPosition[1];
         Target.aPosition[2] = Pose.aPosition[2];
         Target.dYaw         = Yaw_Align (Pose.aRotation, aOrient.data ());

         // An earlier fix sent before the device was tracked holds an
         // absolute pose, which would keep the orientation out of the view.
         pViewport->Camera_Deactivate ();

         m_Align_Target = Target;
         if (m_bAligned)
         {
            m_Align_Start = m_Align_Current;
            m_dSlew       = 0.0;
         }
         else
         {
            m_Align_Start   = Target;
            m_Align_Current = Target;
            m_dSlew         = 1.0;
            m_bAligned      = true;
         }
      }
      else
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

         // Passthrough turns on below and a phone starts tracking with it, so
         // ask again now rather than holding this pose for a full interval.
         // Once only: a desktop webcam never tracks and keeps the interval.
         if (!m_bRequeried)
         {
            m_bQueried   = false;
            m_bRequeried = true;
         }
      }

      if (pViewport->Passthrough_Allowed ())
         pViewport->Passthrough (true);
   }

   void Slew_Tick (VIEWPORT* pViewport, double dSeconds)
   {
      if (m_dSlew < 1.0)
      {
         m_dSlew = std::min (1.0, m_dSlew + dSeconds / SLEW_SECONDS);

         double dT = m_dSlew * m_dSlew * (3.0 - 2.0 * m_dSlew);
         for (int i = 0; i < 3; i++)
            m_Align_Current.aPosition[i] = m_Align_Start.aPosition[i] + (m_Align_Target.aPosition[i] - m_Align_Start.aPosition[i]) * dT;
         m_Align_Current.dYaw = Angle_Wrap (m_Align_Start.dYaw + Angle_Wrap (m_Align_Target.dYaw - m_Align_Start.dYaw) * dT);
      }

      pViewport->Alignment_Set (m_Align_Current);
   }

   void Tick (VIEWPORT* pViewport)
   {
      bool bActive = false;
      {
         std::lock_guard<std::mutex> guard (m_mx);
         bActive = !m_bSkipped  &&  !m_sSelected.empty ();
      }

      auto   tpTick   = std::chrono::steady_clock::now ();
      double dSeconds = (m_bTicked) ? std::chrono::duration<double> (tpTick - m_tpTick).count () : 0.0;
      m_tpTick  = tpTick;
      m_bTicked = true;

      if (!bActive)
      {
         if (m_bAligned  &&  pViewport)
            pViewport->Alignment_Clear ();
         m_bAligned = false;
         return;
      }

      POSE                  Pose;
      bool                  bApply  = false;
      bool                  bOrient = false;
      std::array<double, 4> aOrient = { 0.0, 0.0, 0.0, 1.0 };

      {
         std::lock_guard<std::mutex> guard (m_mx);
         if (m_bPoseReady)
         {
            Pose         = m_Pose;
            bOrient      = m_bPose_Orient;
            aOrient      = m_aPose_Orient;
            m_bPoseReady = false;
            bApply       = Pose.bValid;
         }
      }

      if (bApply  &&  pViewport)
         Fix_Apply (pViewport, Pose, bOrient, aOrient);

      if (m_bAligned  &&  pViewport)
         Slew_Tick (pViewport, dSeconds);

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
               // Orientation sampled with the frame: the fix is solved against
               // this pose, not the one current when the answer arrives.
               double aCapture[4] = { 0.0, 0.0, 0.0, 1.0 };
               bool   bCapture    = pViewport  &&  pViewport->Orientation (aCapture);

               m_tpLast   = tpNow;
               m_bQueried = true;
               Launch (Intrinsics, std::move (aRgba), nWidth, nHeight, bCapture, aCapture);
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
   bool                    m_bPose_Orient = false;
   std::array<double, 4>   m_aPose_Orient = { 0.0, 0.0, 0.0, 1.0 };
   bool                    m_bAligned   = false;
   bool                    m_bRequeried = false;
   double                  m_dSlew      = 1.0;
   VIEWPORT::ALIGNMENT     m_Align_Start;
   VIEWPORT::ALIGNMENT     m_Align_Target;
   VIEWPORT::ALIGNMENT     m_Align_Current;
   bool                    m_bTicked = false;
   std::chrono::steady_clock::time_point m_tpTick;
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
