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

#include "Types.h"
#include "control/Control.h"
#include "wasm/Wasm.h"
#include "spirv/SpvPipeline.h"
#include "xr/XrRuntime.h"
#include "ui/Ui_Context.h"

#include <curl/curl.h>

using namespace SNEEZE;

std::string NowIso8601 ()
{
   auto tpNow = std::chrono::system_clock::now ();
   auto tmTime = std::chrono::system_clock::to_time_t (tpNow);

   struct tm tmBuf = {};
#ifdef _WIN32
   gmtime_s (&tmBuf, &tmTime);
#else
   gmtime_r (&tmTime, &tmBuf);
#endif

   char szBuf[32];
   std::strftime (szBuf, sizeof (szBuf), "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
   return std::string (szBuf);
}

/***********************************************************************************************************************************
**  Impl Class
***********************************************************************************************************************************/

class SNEEZE::ENGINE::Impl
{
public:

   Impl (IENGINE* pHost, SNEEZE::ENGINE* pEngine) :
      m_pHost         (pHost),
      m_pEngine       (pEngine),
      m_bInitialized  (false),
      m_pWasm_Runtime (nullptr),
      m_pSpvPipeline  (nullptr),
      m_pXrRuntime    (nullptr),
      m_pUi_Context   (nullptr),
      m_nCurlInit     (CURLE_FAILED_INIT),
      m_pControl      (nullptr),
      m_pConsole      (nullptr),
      m_pNetwork      (nullptr),
      m_pStorage      (nullptr),
      m_pPersona      (nullptr)
   {
   }

   bool Initialize (const CONFIG& Config)
   {
      int nAgentCount = 0;

      m_bInitialized = false;
      m_Config       = Config;

m_pPersona = new persona::PERSONA (m_pEngine);

      if (m_pHost  &&  !m_pHost->sAppDataPath ().empty ())
      {
         m_pWasm_Runtime = new DEP::WASM_RUNTIME (m_pEngine);

         if (m_pWasm_Runtime->Initialize ())
         {
            m_pSpvPipeline = new DEP::SPV_PIPELINE (m_pEngine);

            if (m_pSpvPipeline->Initialize ())
            {
               m_pXrRuntime = new DEP::XR_RUNTIME (m_pEngine);

               if (m_pXrRuntime->Initialize ())
               {
                  m_pUi_Context = new DEP::UI_CONTEXT (m_pEngine);

                  if (m_pUi_Context->Initialize ())
                  {
                     m_nCurlInit = curl_global_init (CURL_GLOBAL_DEFAULT);

                     if (m_nCurlInit == CURLE_OK)
                     {
                        m_pControl = new CONTROL (m_pEngine);

                        if (m_pControl->Initialize (nAgentCount))
                        {
                           if (InitializePaths ())
                           {
                              m_pConsole  = new CONSOLE (m_pEngine);

                              if (m_pConsole->Initialize ())
                              {
                                 m_pNetwork = new NETWORK (m_pEngine);

                                 if (m_pNetwork->Initialize (m_sPath_Root))
                                 {
                                    m_pStorage = new STORAGE (m_pEngine);

                                    if (m_pStorage->Initialize ())
                                    {
                                       m_bInitialized = true;
   
                                       m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "SNEEZE", "Initialized (1 engine thread + " + std::to_string (nAgentCount) + " agents)");
                                    }
                                    else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize storage");
                                 }
                                 else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize network");
                              }
                              else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize console");
                           }
                           else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize paths");
                        }
                        else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize control");
                     }
                     else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "curl_global_init failed (code " + std::to_string (static_cast<int> (m_nCurlInit)) + ")");
                  }
                  else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize UI context");
               }
               else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize XR runtime");
            }
            else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize SPIR-V pipeline");
         }
         else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize WASM runtime");
      }
      else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Host configuration incomplete (sAppDataPath required)");

      return m_bInitialized;
   }

   ~Impl ()
   {
      if (m_bInitialized)
      {
         while (Context_Close (nullptr));

         if (!m_sPath_Transitory_Session.empty ())
            Scrub (m_sPath_Transitory_Session);
      }

      // Network outlives the contexts that use it but is torn down before
      // CONTROL, whose fetch agents must still be alive to drain in-flight
      // assets in ~NETWORK.

      delete m_pStorage;
      m_pStorage = nullptr;

      delete m_pNetwork;
      m_pNetwork = nullptr;

      delete m_pConsole;
      m_pConsole = nullptr;

      delete m_pControl;
      m_pControl = nullptr;

      if (m_nCurlInit == CURLE_OK)
         curl_global_cleanup ();

      delete m_pUi_Context;
      m_pUi_Context = nullptr;

      delete m_pXrRuntime;
      m_pXrRuntime = nullptr;

      delete m_pSpvPipeline;
      m_pSpvPipeline = nullptr;

      delete m_pWasm_Runtime;
      m_pWasm_Runtime = nullptr;

      delete m_pPersona;
      m_pPersona = nullptr;

      m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "SNEEZE", "Shutdown complete");
   }

    // -----------------------------------------------------------------------
   // Transitory path management
   // -----------------------------------------------------------------------

   static constexpr char kTRANSITORY_SESSION  = 's';
   static constexpr char kTRANSITORY_VIEWPORT = 'v';

   bool IsHexString (const std::string& s, size_t nStart, size_t nLen)
   {
      bool bResult = true;

      if (s.size () < nStart + nLen)
         bResult = false;
      else
      {
         for (size_t i = nStart; i < nStart + nLen  &&  bResult; ++i)
         {
            char c = s[i];
            if (!((c >= '0'  &&  c <= '9')  ||  (c >= 'a'  &&  c <= 'f')))
               bResult = false;
         }
      }

      return bResult;
   }

   bool IsValidTransitoryPath (const std::string& sPath)
   {
      bool bResult = false;

      std::filesystem::path fsPath (sPath);
      std::filesystem::path fsParent = fsPath.parent_path ();
      std::string sGeneric = fsPath.generic_string ();
      std::string sLeaf    = fsPath.filename ().generic_string ();

      bool bParentIsTransitory = (fsParent.filename ().generic_string () == ENGINE::sFOLDER_TRANSITORY);
      std::string sMarker      = std::string (ENGINE::sFOLDER_TRANSITORY) + "/";
      size_t nPos              = sGeneric.find (sMarker);
      bool bContainsTransitory = (nPos != std::string::npos);
      bool bOneLevel           = bContainsTransitory  &&  sGeneric.substr (nPos + sMarker.size ()).find ('/') == std::string::npos;
      bool bCorrectLength      = (sLeaf.size () == 9);
      bool bValidPrefix        = bCorrectLength  &&  (sLeaf[0] == kTRANSITORY_SESSION  ||  sLeaf[0] == kTRANSITORY_VIEWPORT);
      bool bValidHex           = bCorrectLength  &&  IsHexString (sLeaf, 1, 8);

      if (bParentIsTransitory  &&  bOneLevel  &&  bValidPrefix  &&  bValidHex)
         bResult = true;

      return bResult;
   }

   std::string CreateTransitoryFolder (const std::string& sTransitoryRoot, char cPrefix)
   {
      std::string sResult;
      std::error_code ec;
      std::random_device rd;

      uint32_t nRandom = rd ();

      char aId[10];
      snprintf (aId, sizeof (aId), "%c%08x", cPrefix, nRandom);

      std::string sPath = (std::filesystem::path (sTransitoryRoot) / aId).generic_string ();

      std::filesystem::create_directories (sPath, ec);

      if (!ec)
         sResult = sPath;
      else
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to create transitory folder " + sPath + ": " + ec.message ());

      return sResult;
   }

   bool InitializePaths ()
   {
      bool bResult = false;
      std::error_code ec;

      m_sPath_Root = (std::filesystem::path (m_pHost->sAppDataPath ()) / "Sneeze" / "Cache").generic_string ();

      m_sPath_Persistent  = (std::filesystem::path (m_sPath_Root) / ENGINE::sFOLDER_PERSISTENT).generic_string ();
      m_sPath_Transitory  = (std::filesystem::path (m_sPath_Root) / ENGINE::sFOLDER_TRANSITORY).generic_string ();

      std::filesystem::create_directories (m_sPath_Persistent, ec);

      if (!ec)
      {
         std::filesystem::create_directories (m_sPath_Transitory, ec);

         if (!ec)
         {
            int nCount = 0;
            for (auto& entry : std::filesystem::directory_iterator (m_sPath_Transitory, ec))
            {
               if (entry.is_directory ())
               {
                  std::string sEntry = entry.path ().generic_string ();
                  if (IsValidTransitoryPath (sEntry))
                  {
                     Scrub (sEntry);
                     nCount++;
                  }
               }
            }

            if (nCount > 0)
               m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "SNEEZE", "Queued " + std::to_string (nCount) + " orphaned transitory folder(s) for cleanup");

            m_sPath_Transitory_Session = CreateTransitoryFolder (m_sPath_Transitory, kTRANSITORY_SESSION);

            if (!m_sPath_Transitory_Session.empty ())
            {
               bResult = true;

               m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "SNEEZE", "Paths initialized (persistent: " + m_sPath_Persistent + ", session: " + m_sPath_Transitory_Session + ")");
            }
            else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to create transitory session folder");
         }
         else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to create transitory folder: " + ec.message ());
      }
      else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to create persistent folder: " + ec.message ());

      return bResult;
   }

   // -----------------------------------------------------------------------
   // Post-fetch queue (delegates to CONTROL)
   // -----------------------------------------------------------------------

   void Scrub (const std::string& sPath)
   {
      if (IsValidTransitoryPath (sPath))
      {
         auto* pJob_Scrub = new JOB_SCRUB (sPath);

         m_pControl->Queue_Post_Scrub (pJob_Scrub);
      }
      else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "REJECTED cleanup path -- failed validation: " + sPath);
   }

   void Queue_Post_Fetch (JOB_FETCH* pJob_Fetch)
   {
      m_pControl->Queue_Post_Fetch (pJob_Fetch);
   }

   void Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor)
   {
      m_pControl->Queue_Post_Compositor (pJob_Compositor);
   }

  // -----------------------------------------------------------------------
   // Context management
   // -----------------------------------------------------------------------

   CONTEXT* Context_Open (ICONTEXT* pHost, const std::string& sUrl, CONTEXT::eSESSION kSession, bool bReset)
   {
      CONTEXT* pContext = nullptr;
      std::string sPath_Temporary;

      sPath_Temporary = CreateTransitoryFolder (m_sPath_Transitory, kTRANSITORY_VIEWPORT);

      if (!sPath_Temporary.empty ())
      {
         std::string sPath_Permanent = (kSession == CONTEXT::kSESSION_PERSISTENT) ? m_sPath_Persistent : m_sPath_Transitory_Session;

         pContext = new CONTEXT (m_pEngine, pHost, kSession, bReset, sPath_Permanent, sPath_Temporary);

         {
            std::lock_guard<std::mutex> guard (m_mxContext);
            m_apContext.push_back (pContext);
         }

         if (!pContext->Initialize (sUrl))
         {
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize context");

            {
               std::lock_guard<std::mutex> guard (m_mxContext);
               m_apContext.erase (std::find (m_apContext.begin (), m_apContext.end (), pContext));
            }

            delete pContext;
            pContext = nullptr;

            Scrub (sPath_Temporary);
         }
      }

      return pContext;
   }

   bool Context_Close (CONTEXT* pContext)
   {
      bool bResult = false;
      std::string sPath_Temporary;

      {
         std::lock_guard<std::mutex> guard (m_mxContext);

         if (!pContext  &&  !m_apContext.empty ())
            pContext = m_apContext.back ();
      }

      if (pContext)
      {
         sPath_Temporary = pContext->Path_Temporary ();

         {
            std::lock_guard<std::mutex> guard (m_mxContext);

            delete pContext;
            
            m_apContext.erase (std::find (m_apContext.begin (), m_apContext.end (), pContext));
         }

         Scrub (sPath_Temporary);

         bResult = true;
      }

      return bResult;
   }

  // -----------------------------------------------------------------------
   // Persona management
   // -----------------------------------------------------------------------

   void Login (const std::string& sFirst, const std::string& sSecond)
   {
      if (m_pPersona)
         m_pPersona->Login (sFirst, sSecond);
   }

   void Logout ()
   {
      m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 1 (signal)");
      m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 2 (communicate)");
      m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 3 (shutdown)");

      {
         std::lock_guard<std::mutex> guard (m_mxContext);
         for (auto* pContext : m_apContext)
            pContext->Logout ();
      }

      m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 4 (destroy)");

      if (m_pPersona)
         m_pPersona->Logout ();
   }

   void SetConfig (const CONFIG& Config)
   {
      std::lock_guard<std::mutex> guard (m_mxConfig);

      m_Config = Config;
   }

   void GetConfig (CONFIG& Config)
   {
      std::lock_guard<std::mutex> guard (m_mxConfig);

      Config = m_Config;
   }

public:
   ENGINE*                    m_pEngine;
   bool                       m_bInitialized;

// Config
   std::mutex                 m_mxConfig;
   CONFIG                     m_Config;

// Paths
   std::string                m_sPath_Root;
   std::string                m_sPath_Persistent;
   std::string                m_sPath_Transitory;
   std::string                m_sPath_Transitory_Session;

   // Subsystems
   IENGINE*                   m_pHost;
   persona::PERSONA*          m_pPersona;
   SNEEZE::DEP::WASM_RUNTIME* m_pWasm_Runtime;
   SNEEZE::DEP::SPV_PIPELINE* m_pSpvPipeline;
   SNEEZE::DEP::XR_RUNTIME*   m_pXrRuntime;
   SNEEZE::DEP::UI_CONTEXT*   m_pUi_Context;
   CURLcode                   m_nCurlInit;

   // Control (owns engine thread, agents, metronome, cleanup queue)
   CONTROL*                   m_pControl;

   // Network (engine-wide singleton, shared across all contexts)
   CONSOLE*                   m_pConsole;
   NETWORK*                   m_pNetwork;
   STORAGE*                   m_pStorage;

   // Contexts
   std::mutex                 m_mxContext;
   std::vector<CONTEXT*>      m_apContext;
};

/***********************************************************************************************************************************
**  ENGINE Class
***********************************************************************************************************************************/

SNEEZE::ENGINE::ENGINE (IENGINE* pHost) :
   m_pImpl (new Impl (pHost, this))
{
}

bool SNEEZE::ENGINE::Initialize (const CONFIG& Config)
{
   return m_pImpl->Initialize (Config);
}

SNEEZE::ENGINE::~ENGINE ()
{
   delete m_pImpl;
}

void SNEEZE::ENGINE::SetConfig (const CONFIG& Config)
{
   m_pImpl->SetConfig (Config);
}

void SNEEZE::ENGINE::GetConfig (CONFIG& Config)
{
   m_pImpl->GetConfig (Config);
}

// ---------------------------------------------------------------------------
// Context management
// ---------------------------------------------------------------------------

SNEEZE::CONTEXT* SNEEZE::ENGINE::Context_Open (ICONTEXT* pHost, const std::string& sUrl, CONTEXT::eSESSION kSession, bool bReset)
{
   return m_pImpl->Context_Open (pHost, sUrl, kSession, bReset);
}

bool SNEEZE::ENGINE::Context_Close (CONTEXT* pContext)
{
   bool bResult = false;

   if (pContext)
      bResult = m_pImpl->Context_Close (pContext);

   return bResult;
}

SNEEZE::IENGINE*           SNEEZE::ENGINE::Host            () const { return m_pImpl->m_pHost; }
const std::string&         SNEEZE::ENGINE::Path_Persistent () const { return m_pImpl->m_sPath_Persistent; }
const std::string&         SNEEZE::ENGINE::Path_Session    () const { return m_pImpl->m_sPath_Transitory_Session; }

SNEEZE::persona::PERSONA*  SNEEZE::ENGINE::Persona         () const { return m_pImpl->m_pPersona; }
SNEEZE::DEP::WASM_RUNTIME* SNEEZE::ENGINE::Wasm_Runtime    () const { return m_pImpl->m_pWasm_Runtime; }
SNEEZE::DEP::UI_CONTEXT*   SNEEZE::ENGINE::Ui_Context      () const { return m_pImpl->m_pUi_Context; }
SNEEZE::DEP::XR_RUNTIME*   SNEEZE::ENGINE::XrRuntime       () const { return m_pImpl->m_pXrRuntime; }
SNEEZE::CONSOLE*           SNEEZE::ENGINE::Console         () const { return m_pImpl->m_pConsole; }
SNEEZE::NETWORK*           SNEEZE::ENGINE::Network         () const { return m_pImpl->m_pNetwork; }
SNEEZE::STORAGE*           SNEEZE::ENGINE::Storage         () const { return m_pImpl->m_pStorage; }

void SNEEZE::ENGINE::Queue_Post_Fetch (JOB_FETCH* pJob_Fetch)
{
   m_pImpl->Queue_Post_Fetch (pJob_Fetch);
}

void SNEEZE::ENGINE::Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor)
{
   m_pImpl->Queue_Post_Compositor (pJob_Compositor);
}

// ---------------------------------------------------------------------------
// Logging and notifications
// ---------------------------------------------------------------------------

void SNEEZE::ENGINE::Log (SNEEZE::IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage)
{
   if (m_pImpl->m_pHost)
      m_pImpl->m_pHost->Log (Level, sModule, sMessage);
}

// ---------------------------------------------------------------------------
// Persona
// ---------------------------------------------------------------------------

void SNEEZE::ENGINE::Login (const std::string& sFirst, const std::string& sSecond)
{
   m_pImpl->Login (sFirst, sSecond);
}

void SNEEZE::ENGINE::Logout ()
{
   m_pImpl->Logout ();
}

void SNEEZE::ENGINE::ChangePersona (const std::string& sFirst, const std::string& sSecond)
{
   m_pImpl->Logout ();
   m_pImpl->Login (sFirst, sSecond);
}
