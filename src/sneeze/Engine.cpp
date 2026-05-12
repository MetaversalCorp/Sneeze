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
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#include "Types.h"
#include "control/Control.h"
#include "astro/RMCObject.h"
#include "wasm/WasmRuntime.h"
#include "spirv/SpvPipeline.h"
#include "xr/XrRuntime.h"
#include "ui/UiContext.h"

using namespace SNEEZE;

/***********************************************************************************************************************************
**  Impl Class
***********************************************************************************************************************************/

class SNEEZE::ENGINE::Impl
{
public:

   Impl (IENGINE* pHost, ENGINE* pEngine) :
      m_pHost        (pHost),
      m_pEngine      (pEngine),
      m_bInitialized (false),
      m_pWasmRuntime (nullptr),
      m_pSpvPipeline (nullptr),
      m_pXrRuntime   (nullptr),
      m_pUiContext   (nullptr),
      m_pControl     (nullptr),
      m_pNetwork     (nullptr),
      m_pStorage     (nullptr),
      m_pPersona     (nullptr)
   {
   }

   bool Initialize ()
   {
      int nAgentCount = 0;

      m_bInitialized = false;

m_pPersona = new persona::PERSONA (m_pEngine);

      if (m_pHost  &&  !m_pHost->sAppDataPath ().empty ())
      {
         m_pWasmRuntime = new DEP::WASM_RUNTIME ();

         if (m_pWasmRuntime->Initialize (m_pEngine))
         {
            m_pSpvPipeline = new DEP::SPV_PIPELINE ();

            if (m_pSpvPipeline->Initialize (m_pEngine))
            {
               m_pXrRuntime = new DEP::XR_RUNTIME ();

               if (m_pXrRuntime->Initialize (m_pEngine))
               {
                  m_pUiContext = new DEP::UI_CONTEXT ();

                  if (m_pUiContext->Initialize (m_pEngine))
                  {
                     m_pNetwork = new NETWORK (m_pEngine);

                     if (m_pNetwork->Initialize ())
                     {
                        m_pStorage = new STORAGE (m_pEngine);

                        if (m_pStorage->Initialize ())
                        {
                           m_pControl = new CONTROL (m_pEngine);

                           if (m_pControl->Initialize (nAgentCount))
                           {
                              if (InitializePaths ())
                              {
                                 m_bInitialized = true;

                                 m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "SNEEZE", "Initialized (1 engine thread + " + std::to_string (nAgentCount) + " agents)");
                              }
                              else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize paths");
                           }
                           else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize control");
                        }
                        else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize storage");
                     }
                     else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize network");
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
         while (Viewport_Close (nullptr));

         if (!m_sPath_Transitory_Session.empty ())
            Cleanup_Queue (m_sPath_Transitory_Session);
      }

      delete m_pControl;
      m_pControl = nullptr;

      delete m_pStorage;
      m_pStorage = nullptr;

      delete m_pNetwork;
      m_pNetwork = nullptr;

      delete m_pUiContext;
      m_pUiContext = nullptr;

      delete m_pXrRuntime;
      m_pXrRuntime = nullptr;

      delete m_pSpvPipeline;
      m_pSpvPipeline = nullptr;

      delete m_pWasmRuntime;
      m_pWasmRuntime = nullptr;

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

      std::string sRoot = (std::filesystem::path (m_pHost->sAppDataPath ()) / "Sneeze" / "Cache").generic_string ();

      m_sPath_Persistent  = (std::filesystem::path (sRoot) / ENGINE::sFOLDER_PERSISTENT).generic_string ();
      m_sPath_Transitory  = (std::filesystem::path (sRoot) / ENGINE::sFOLDER_TRANSITORY).generic_string ();

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
                     m_pControl->Cleanup_Queue (sEntry);
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
   // Cleanup queue (delegates to CONTROL)
   // -----------------------------------------------------------------------

   void Cleanup_Queue (const std::string& sPath)
   {
      if (IsValidTransitoryPath (sPath))
      {
         m_pControl->Cleanup_Queue (sPath);
      }
      else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "REJECTED cleanup path -- failed validation: " + sPath);
   }

  // -----------------------------------------------------------------------
   // Viewport management
   // -----------------------------------------------------------------------

   VIEWPORT* Viewport_Open (IVIEWPORT* pHost, const std::string& sUrl, VIEWPORT::eSESSION kSession)
   {
      VIEWPORT* pViewport = nullptr;
      std::string sPath_Transitory;

      sPath_Transitory = CreateTransitoryFolder (m_sPath_Transitory, kTRANSITORY_VIEWPORT);

      if (!sPath_Transitory.empty ())
      {
         pViewport = new VIEWPORT (m_pEngine, pHost);

         {
            std::lock_guard<std::mutex> guard (m_mutexViewport);
            m_apViewport.push_back (pViewport);
         }

         if (!pViewport->Initialize (sUrl, sPath_Transitory))
         {
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "SNEEZE", "Failed to initialize viewport");

            {
               std::lock_guard<std::mutex> guard (m_mutexViewport);
               m_apViewport.erase (std::find (m_apViewport.begin (), m_apViewport.end (), pViewport));
            }

            delete pViewport;
            pViewport = nullptr;

            Cleanup_Queue (sPath_Transitory);
         }
      }

      return pViewport;
   }

   bool Viewport_Close (VIEWPORT* pViewport)
   {
      bool bResult = false;
      std::string sPath_Transitory;

      {
         std::lock_guard<std::mutex> guard (m_mutexViewport);

         if (!pViewport  &&  !m_apViewport.empty ())
            pViewport = m_apViewport.back ();
      }

      if (pViewport)
      {
         sPath_Transitory = pViewport->sPath_Transitory ();

         pViewport->Shutdown ();

         {
            std::lock_guard<std::mutex> guard (m_mutexViewport);
            m_apViewport.erase (std::find (m_apViewport.begin (), m_apViewport.end (), pViewport));
         }

         delete pViewport;
         bResult = true;

         Cleanup_Queue (sPath_Transitory);
      }

      return bResult;
   }

   void Viewport_Capture ()
   {
      m_mutexViewport.lock ();
   }

   const std::vector<VIEWPORT*>& Viewport_GetList () const
   {
      return m_apViewport;
   }

   void Viewport_Release ()
   {
      m_mutexViewport.unlock ();
   }

public:
   // Subsystems
   IENGINE*                   m_pHost;
   NETWORK*                   m_pNetwork;
   STORAGE*                   m_pStorage;
   persona::PERSONA*          m_pPersona;

   // Paths
   std::string                m_sPath_Persistent;
   std::string                m_sPath_Transitory;
   std::string                m_sPath_Transitory_Session;

private:
   ENGINE*                    m_pEngine;
   bool                       m_bInitialized;

   SNEEZE::DEP::WASM_RUNTIME* m_pWasmRuntime;
   SNEEZE::DEP::SPV_PIPELINE* m_pSpvPipeline;
   SNEEZE::DEP::XR_RUNTIME*   m_pXrRuntime;
   SNEEZE::DEP::UI_CONTEXT*   m_pUiContext;
   
   // Control (owns engine thread, agents, metronome, cleanup queue)
   CONTROL*                   m_pControl;

   // Viewports
   std::mutex                 m_mutexViewport;
   std::vector<VIEWPORT*>     m_apViewport;
};

/***********************************************************************************************************************************
**  ENGINE Class
***********************************************************************************************************************************/

ENGINE::ENGINE (IENGINE* pHost) :
   m_pImpl (new Impl (pHost, this))
{
}

ENGINE::~ENGINE ()
{
   delete m_pImpl;
}

bool ENGINE::Initialize ()
{
   return m_pImpl->Initialize ();
}

// ---------------------------------------------------------------------------
// Viewport management
// ---------------------------------------------------------------------------

VIEWPORT* ENGINE::Viewport_Open (IVIEWPORT* pHost, const std::string& sUrl, VIEWPORT::eSESSION kSession)
{
   return m_pImpl->Viewport_Open (pHost, sUrl, kSession);
}

bool ENGINE::Viewport_Close (VIEWPORT* pViewport)
{
   bool bResult = false;

   if (pViewport)
      bResult = m_pImpl->Viewport_Close (pViewport);

   return bResult;
}

void ENGINE::Viewport_Capture ()
{
   m_pImpl->Viewport_Capture ();
}

const std::vector<VIEWPORT*>& ENGINE::Viewport_GetList () const
{ 
   return m_pImpl->Viewport_GetList ();
}

void ENGINE::Viewport_Release ()
{
   m_pImpl->Viewport_Release ();
}

IENGINE*  ENGINE::Host () const              { return m_pImpl->m_pHost;      }
const std::string& ENGINE::sPath_Persistent () const { return m_pImpl->m_sPath_Persistent; }
const std::string& ENGINE::sPath_Session    () const { return m_pImpl->m_sPath_Transitory_Session; }

NETWORK*  ENGINE::Network () const           { return m_pImpl->m_pNetwork;   }
STORAGE*  ENGINE::Storage () const           { return m_pImpl->m_pStorage;   }
persona::PERSONA* ENGINE::Persona () const   { return m_pImpl->m_pPersona;   }

// ---------------------------------------------------------------------------
// Logging and notifications
// ---------------------------------------------------------------------------

void ENGINE::Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage)
{
   if (m_pImpl->m_pHost)
      m_pImpl->m_pHost->Log (Level, sModule, sMessage);
}

// ---------------------------------------------------------------------------
// Persona
// ---------------------------------------------------------------------------

void ENGINE::Login (const std::string& sFirst, const std::string& sSecond)
{
   if (m_pImpl->m_pPersona)
      m_pImpl->m_pPersona->Login (sFirst, sSecond);
}

void ENGINE::Logout ()
{
   Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 1 (signal)");
   Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 2 (communicate)");
   Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 3 (shutdown)");
///   s_pWasmRuntime.DestroyAllStores ();

   if (m_pImpl->m_pNetwork)
      m_pImpl->m_pNetwork->Clear ();

   Log (IENGINE::kLOGLEVEL_Trace, "SNEEZE", "Teardown phase 4 (destroy)");

   if (m_pImpl->m_pPersona)
      m_pImpl->m_pPersona->Logout ();
}

void ENGINE::ChangePersona (const std::string& sFirst, const std::string& sSecond)
{
   Logout ();
   Login (sFirst, sSecond);
}
