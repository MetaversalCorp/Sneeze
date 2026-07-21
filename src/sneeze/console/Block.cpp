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

#include "Console.h"

using namespace SNEEZE;

// ===========================================================================
// BLOCK::Impl
// ===========================================================================

class BLOCK::Impl
{
public:
   Impl (ICONSOLE_IMPL* pIConsole_Impl, uint32_t nIndex, const std::string& sPathname) :
      m_pIConsole_Impl (pIConsole_Impl),
      m_nIndex         (nIndex),
      m_sPathname      (sPathname),
      m_bLoaded        (false),
      m_nCount_Load    (0),
      m_nEntryCount    (0)
   {
   }

   // ---------------------------------------------------------------------------
   // Attach / Detach
   // ---------------------------------------------------------------------------

   void Attach ()
   {
      if (++m_nCount_Load == 1)
         Load (nullptr);
   }

   void Detach (CONTAINER* pContainer)
   {
      if (m_nCount_Load > 0 && --m_nCount_Load == 0)
         Evict ();
   }

   void Entry_Enum (IENUM_ENTRY* pEnum) const
   {
      if (pEnum)
      {
         std::lock_guard<std::recursive_mutex> guard (m_mutex);
   
         for (const auto& pEntry : m_aEntry)
            pEnum->OnEntry (pEntry);
      }
   }
   
   // ---------------------------------------------------------------------------
   // Load — read the JSONL file into the in-memory entry cache.
   // ---------------------------------------------------------------------------

   void Load (CONTAINER* pContainer)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      if (!m_bLoaded)
      {
         m_aEntry.clear ();

         std::ifstream ifs (m_sPathname);
         if (ifs.is_open ())
         {
            std::string sLine;
            while (std::getline (ifs, sLine))
            {
               if (!sLine.empty ())
               {
                  try
                  {
                     nlohmann::json jEntry = nlohmann::json::parse (sLine);

                     uint32_t nIndex = jEntry.value (CONSOLE_ENTRY_KEY_INDEX, static_cast<uint32_t> (UINT32_MAX));
                     auto pEntry = (nIndex != UINT32_MAX) ? m_pIConsole_Impl->Entry_Find (nIndex) : nullptr;

                     if (!pEntry)
                        pEntry = ENTRY::FromJson (jEntry, pContainer);

                     if (pEntry)
                     {
                        m_aEntry.push_back (pEntry);
                        m_nEntryCount++;
                     }
                  }
                  catch (...)
                  {
                  }
               }
            }
         }

         m_bLoaded = true;
      }
   }

   // ---------------------------------------------------------------------------
   // Write — append one JSONL line to the block file.
   // ---------------------------------------------------------------------------

   void Write (std::shared_ptr<const ENTRY> pEntry)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      if (!m_ofsBlock.is_open ())
      {
         std::error_code ec;
         std::filesystem::create_directories (std::filesystem::path (m_sPathname).parent_path (), ec);

         m_ofsBlock.open (m_sPathname, std::ios::app);
      }

      if (m_ofsBlock.is_open ())
      {
         m_ofsBlock << pEntry->ToJson ().dump () << "\n";
         m_nEntryCount++;

         if (m_bLoaded)
            m_aEntry.push_back (pEntry);
      }
   }

   // ---------------------------------------------------------------------------
   // Evict — drop the in-memory entry cache.
   // ---------------------------------------------------------------------------

   void Evict ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mutex);

      if (m_ofsBlock.is_open ())
         m_ofsBlock.close ();

      m_aEntry.clear ();
      m_bLoaded = false;
   }

   ICONSOLE_IMPL*                                             m_pIConsole_Impl;
   uint32_t                                                   m_nIndex;
   std::string                                                m_sPathname;

   std::ofstream                                              m_ofsBlock;
   bool                                                       m_bLoaded;
   uint32_t                                                   m_nCount_Load;
   uint32_t                                                   m_nEntryCount;

   mutable std::recursive_mutex                               m_mutex;
   std::vector<std::shared_ptr<const ENTRY>>         m_aEntry;
};

// ===========================================================================
// BLOCK
// ===========================================================================

BLOCK::BLOCK (ICONSOLE_IMPL* pIConsole_Impl, uint32_t nIndex, const std::string& sPathname) :
   m_pImpl (new Impl (pIConsole_Impl, nIndex, sPathname))
{
}

BLOCK::~BLOCK ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool                 BLOCK::IsLoaded    () const { return m_pImpl->m_bLoaded; }
uint32_t             BLOCK::GetIndex    () const { return m_pImpl->m_nIndex; }
const std::string&   BLOCK::Pathname    () const { return m_pImpl->m_sPathname; }
uint32_t             BLOCK::EntryCount  () const { return m_pImpl->m_nEntryCount; }

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

void     BLOCK::Attach     ()                                    { m_pImpl->Attach     (); }
void     BLOCK::Detach     (CONTAINER* pContainer)               { m_pImpl->Detach     (pContainer); }
void     BLOCK::Entry_Enum (IENUM_ENTRY* pEnum)            const { m_pImpl->Entry_Enum (pEnum); }

void     BLOCK::Load       (CONTAINER* pContainer)               { m_pImpl->Load       (pContainer); }
void     BLOCK::Evict      ()                                    { m_pImpl->Evict      (); }

void     BLOCK::Write      (std::shared_ptr<const ENTRY> pEntry) { m_pImpl->Write      (pEntry); }
