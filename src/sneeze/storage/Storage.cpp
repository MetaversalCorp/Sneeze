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

#include "Storage.h"

using namespace SNEEZE;

ISTORAGE_IMPL::ISTORAGE_IMPL () {}
ISTORAGE_IMPL::~ISTORAGE_IMPL () {}

class STORAGE::Impl : public ISTORAGE_IMPL
{
public:
   Impl (STORAGE* pStorage, ENGINE* pEngine) :
      m_pStorage (pStorage),
      m_pEngine  (pEngine)
   {
   }

   bool Initialize ()
   {
      m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "STORAGE", "Initialized");

      return true;
   }

   ~Impl ()
   {
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Silo);

         // Engine-level teardown: every CONTAINER has already closed its SILO,
         // so this is a leak safety net. The owning contexts/containers are gone,
         // so no OnStorageSiloDeleted callback can be routed - delete directly.

         for (auto* pSilo : m_apSilo)
            delete pSilo;

         m_apSilo.clear ();
      }

      {
         std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Unit);

         // not sure why we're doing this

         for (auto& pair : m_umpUnit)
            delete pair.second;

         m_umpUnit.clear ();
      }
   }

   // ---------------------------------------------------------------------------
   // Silo management
   // ---------------------------------------------------------------------------

   SILO* Silo_Open (CONTAINER* pContainer)
   {
      SILO* pSilo = nullptr;

      if (pContainer)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Silo);

         pSilo = new SILO (this, pContainer);

         m_apSilo.push_back (pSilo);

         pSilo->Initialize ();

         pContainer->Context ()->Host ()->OnStorageSiloCreated (pSilo);
      }

      return pSilo;
   }

   void Silo_Close (CONTAINER* pContainer, SILO* pSilo)
   {
      if (pSilo)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Silo);

         pContainer->Context ()->Host ()->OnStorageSiloDeleted (pSilo);

         auto it = std::find (m_apSilo.begin (), m_apSilo.end (), pSilo);
         if (it != m_apSilo.end ())
            m_apSilo.erase (it);

         delete pSilo;
      }
   }

   void Silo_Enum (IENUM_SILO* pEnum)
   {
      if (pEnum)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Silo);

         for (SILO* pSilo : m_apSilo)
            pEnum->OnSilo (pSilo);
      }
   }

   // ---------------------------------------------------------------------------
   // ISTORAGE_IMPL
   // ---------------------------------------------------------------------------

   UNIT* Unit_Open (SILO* pSilo, eSILO_SCOPE eScope) override
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Unit);

      UNIT* pUnit = nullptr;

      std::string sPathname = pSilo->Pathname (eScope);

      auto it = m_umpUnit.find (sPathname);
      if (it == m_umpUnit.end ())
      {
         pUnit = new UNIT (this, eScope, sPathname);

         m_umpUnit[sPathname] = pUnit;
      }
      else pUnit = it->second;

      pUnit->Open (pSilo);

      return pUnit;
   }

   void Unit_Close (SILO* pSilo, UNIT* pUnit) override
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStorage_Unit);

      if (pUnit->Close (pSilo) == 0)
      {
         m_umpUnit.erase (pUnit->Pathname ()); // should be the same as pSilo->Pathname ()

         delete pUnit;
      }
   }

   void Log (IENGINE::eLOGLEVEL Level, const std::string& sModule, const std::string& sMessage) override
   {
      m_pEngine->Log (Level, sModule, sMessage);
   }

   STORAGE*                                m_pStorage;
   ENGINE*                                 m_pEngine;

   mutable std::recursive_mutex            m_mxStorage_Silo;
   mutable std::recursive_mutex            m_mxStorage_Unit;
   std::vector<SILO*>                      m_apSilo;
   std::unordered_map<std::string, UNIT*> m_umpUnit;
};

// ===========================================================================
// STORAGE
// ===========================================================================

STORAGE::STORAGE (ENGINE* pEngine) :
   m_pImpl (new Impl (this, pEngine))
{
}

bool STORAGE::Initialize ()
{
   return m_pImpl->Initialize ();
}

STORAGE::~STORAGE ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Container lifecycle
// ---------------------------------------------------------------------------

SILO* STORAGE::Silo_Open  (CONTAINER* pContainer)              { return m_pImpl->Silo_Open  (pContainer); }
void  STORAGE::Silo_Close (CONTAINER* pContainer, SILO* pSilo) {        m_pImpl->Silo_Close (pContainer, pSilo); }
void  STORAGE::Silo_Enum  (IENUM_SILO* pEnum)                  {        m_pImpl->Silo_Enum  (pEnum); }
