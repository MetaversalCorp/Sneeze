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

// ---------------------------------------------------------------------------
// SILO::Impl
// ---------------------------------------------------------------------------

class SILO::Impl
{
public:
   Impl (SILO* pSilo, ISTORAGE_IMPL* pIStorage_Impl, CONTAINER* pContainer) :
      m_pSilo          (pSilo),
      m_pIStorage_Impl (pIStorage_Impl),
      m_pContainer     (pContainer),
      m_bAttached      (false)
   {
      for (int nScope = 0; nScope < kSILO_SCOPE_COUNT; nScope++)
         m_apUnit[nScope] = nullptr;
   }

   void Initialize ()
   {
      for (int nScope = 0; nScope < kSILO_SCOPE_COUNT; nScope++)
      {
         eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (nScope);

         m_apUnit[nScope] = m_pIStorage_Impl->Unit_Open (m_pSilo, eScope);
      }
   }

   ~Impl ()
   {
      if (m_bAttached)
         Detach ();

      for (int nScope = 0; nScope < kSILO_SCOPE_COUNT; nScope++)
      {
         if (m_apUnit[nScope])
         {
            m_pIStorage_Impl->Unit_Close (m_pSilo, m_apUnit[nScope]);
            m_apUnit[nScope] = nullptr;
         }
      }
   }

   // ---------------------------------------------------------------------------
   // Path helpers
   // ---------------------------------------------------------------------------

   std::string Path (eSILO_SCOPE eScope) const
   {
      const std::string* psBasePath = nullptr;

      switch (eScope)
      {
         case kSILO_SCOPE_PERMANENT_ORG:       psBasePath = &m_pContainer->Path_Permanent_Org (); break;
         default:
         case kSILO_SCOPE_PERMANENT_CONTAINER: psBasePath = &m_pContainer->Path_Permanent_All (); break;
         case kSILO_SCOPE_TEMPORARY_ORG:       psBasePath = &m_pContainer->Path_Temporary_Org (); break;
         case kSILO_SCOPE_TEMPORARY_CONTAINER: psBasePath = &m_pContainer->Path_Temporary_All (); break;
      }

      return (std::filesystem::path (*psBasePath) / "Storage").generic_string ();
   }

   std::string Filename (eSILO_SCOPE eScope, const std::string& sExt = "") const
   {
      std::string sName = (eScope == kSILO_SCOPE_PERMANENT_ORG || eScope == kSILO_SCOPE_TEMPORARY_ORG) ? "organization" : "container";

      if (!sExt.empty ())
         sName += "." + sExt;

      return sName;
   }

   std::string Pathname (eSILO_SCOPE eScope, const std::string& sExt = "") const
   {
      return (std::filesystem::path (Path (eScope)) / Filename (eScope, sExt)).generic_string ();
   }

   // ---------------------------------------------------------------------------
   // Attach / Detach
   // ---------------------------------------------------------------------------

   void Attach ()
   {
      std::lock_guard<std::mutex> guard (m_mxSilo);

      if (!m_bAttached)
      {
         m_bAttached = true;

         for (int nScope = 0; nScope < kSILO_SCOPE_COUNT; nScope++)
            m_apUnit[nScope]->Attach ();
      }
   }

   void Detach ()
   {
      std::lock_guard<std::mutex> guard (m_mxSilo);

      if (m_bAttached)
      {
         for (int nScope = 0; nScope < kSILO_SCOPE_COUNT; nScope++)
            m_apUnit[nScope]->Detach (m_pContainer);

         m_bAttached = false;
      }
   }

   // ---------------------------------------------------------------------------
   // UNIT Pass-through
   // ---------------------------------------------------------------------------

   nlohmann::json Get (eSILO_SCOPE eScope, const std::string& sPath) const
   {
      return m_apUnit[eScope]->Get (sPath);
   }

   void Set (eSILO_SCOPE eScope, const std::string& sPath, const nlohmann::json& jValue)
   {
      m_apUnit[eScope]->Set (sPath, jValue);
   }

   void Remove (eSILO_SCOPE eScope, const std::string& sPath)
   {
      m_apUnit[eScope]->Remove (sPath);
   }

   bool Has (eSILO_SCOPE eScope, const std::string& sPath) const
   {
      return m_apUnit[eScope]->Has (sPath);
   }

   std::string Json (eSILO_SCOPE eScope) const
   {
      return m_apUnit[eScope]->Json ();
   }

   void Json (eSILO_SCOPE eScope, const std::string& sJson)
   {
      m_apUnit[eScope]->Json (sJson);
   }

public:
   SILO*                            m_pSilo;
   ISTORAGE_IMPL*                   m_pIStorage_Impl;
   CONTAINER*                       m_pContainer;
   UNIT*                            m_apUnit[kSILO_SCOPE_COUNT];
   std::mutex                       m_mxSilo;
   bool                             m_bAttached;
};

// ===========================================================================
// STORAGE::SILO
// ===========================================================================

SILO::SILO (ISTORAGE_IMPL* pIStorage_Impl, CONTAINER* pContainer) :
   m_pImpl (new Impl (this, pIStorage_Impl, pContainer))
{
}

void SILO::Initialize ()
{
   m_pImpl->Initialize ();
}

SILO::~SILO ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::string SILO::DisplayName ()                                                                           const { return m_pImpl->m_pContainer->Identity ()->DisplayName (); }
CONTAINER*  SILO::Container   ()                                                                           const { return m_pImpl->m_pContainer; }

std::string SILO::Path        (eSILO_SCOPE eScope)                                                         const { return m_pImpl->Path     (eScope);       }
std::string SILO::Filename    (eSILO_SCOPE eScope, const std::string& sExt)                                const { return m_pImpl->Filename (eScope, sExt); }
std::string SILO::Pathname    (eSILO_SCOPE eScope, const std::string& sExt)                                const { return m_pImpl->Pathname (eScope, sExt); }

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// UNIT Caching
// ---------------------------------------------------------------------------

void        SILO::Attach      ()                                                                                 {        m_pImpl->Attach    (); }
void        SILO::Detach      ()                                                                                 {        m_pImpl->Detach    (); }

// ---------------------------------------------------------------------------
// UNIT Pass-through
// ---------------------------------------------------------------------------

nlohmann::json SILO::Get      (eSILO_SCOPE eScope, const std::string& sPath)                               const { return m_pImpl->Get    (eScope, sPath); }
void           SILO::Set      (eSILO_SCOPE eScope, const std::string& sPath, const nlohmann::json& jValue)       {        m_pImpl->Set    (eScope, sPath, jValue); }
void           SILO::Remove   (eSILO_SCOPE eScope, const std::string& sPath)                                     {        m_pImpl->Remove (eScope, sPath); }
bool           SILO::Has      (eSILO_SCOPE eScope, const std::string& sPath)                               const { return m_pImpl->Has    (eScope, sPath); }
std::string    SILO::Json     (eSILO_SCOPE eScope)                                                         const { return m_pImpl->Json   (eScope); }
void           SILO::Json     (eSILO_SCOPE eScope, const std::string& sJson)                                     {        m_pImpl->Json   (eScope, sJson); }

