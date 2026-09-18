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

#include "ui/Ui_Context.h"
#include "ui/Ui_Render.h"
#include "camera/Capture.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/SystemInterface.h>

#include <chrono>
#include <string>

using namespace SNEEZE::DEP;

namespace
{
   // System interface: elapsed time + log routing into the engine.
   class UI_SYSTEM : public Rml::SystemInterface
   {
   public:
      UI_SYSTEM ()
         : m_pEngine (nullptr)
      {
         tpStart = std::chrono::steady_clock::now ();
      }

      void SetSneeze (SNEEZE::ENGINE* pEngine) { m_pEngine = pEngine; }

      double GetElapsedTime () override
      {
         auto tpNow = std::chrono::steady_clock::now ();
         return std::chrono::duration<double> (tpNow - tpStart).count ();
      }

      bool LogMessage (Rml::Log::Type nType, const Rml::String& sMessage) override
      {
         if (!m_pEngine)
            return true;

         SNEEZE::IENGINE::eLOGLEVEL eLevel = SNEEZE::IENGINE::kLOGLEVEL_Info;
         if (nType == Rml::Log::LT_ERROR)   eLevel = SNEEZE::IENGINE::kLOGLEVEL_Error;
         if (nType == Rml::Log::LT_WARNING) eLevel = SNEEZE::IENGINE::kLOGLEVEL_Warning;
         if (nType == Rml::Log::LT_DEBUG)   eLevel = SNEEZE::IENGINE::kLOGLEVEL_Trace;

         m_pEngine->Log (eLevel, "UI_CONTEXT", sMessage);
         return true;
      }

   private:
      SNEEZE::ENGINE* m_pEngine;
      std::chrono::steady_clock::time_point tpStart;
   };

   static UI_SYSTEM pUiSystem;
} // anonymous namespace

UI_CONTEXT::UI_CONTEXT (ENGINE* pEngine)
   : m_pEngine (pEngine)
   , m_pRender (nullptr)
   , m_bInitialized (false)
{
}

UI_CONTEXT::~UI_CONTEXT ()
{
   if (m_bInitialized)
   {
      Rml::Shutdown ();
      m_bInitialized = false;
   }
   // The render interface must outlive Rml::Shutdown (which releases geometry
   // and textures back through it), so it is destroyed last.
   delete m_pRender;
   m_pRender = nullptr;
}

bool UI_CONTEXT::Initialize ()
{
   m_pRender = new UI_RENDER ();

   pUiSystem.SetSneeze (m_pEngine);
   Rml::SetSystemInterface (&pUiSystem);
   Rml::SetRenderInterface (m_pRender);

   m_bInitialized = Rml::Initialise ();
   if (!m_bInitialized)
   {
      m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "UI_CONTEXT", "Rml::Initialise failed");
   }
   else
   {
      m_pRender->Capture (m_pEngine->Capture ());
      Rml::String sVersion = Rml::GetVersion ();
      m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "UI_CONTEXT", "RmlUi " + std::string (sVersion.c_str ()) + " initialized");
   }

   return m_bInitialized;
}
