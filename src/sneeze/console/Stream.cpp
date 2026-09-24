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
#include <iomanip>

using namespace SNEEZE;

// ---------------------------------------------------------------------------
// STREAM::Impl
// ---------------------------------------------------------------------------

class STREAM::Impl
{
public:
   Impl (ICONSOLE_IMPL* pIConsole_Impl, CONTAINER* pContainer) :
      m_pIConsole_Impl   (pIConsole_Impl),
      m_pContainer       (pContainer),
      m_bAttached        (false),
      m_nBlocks          (0),
      m_nEntries_Block   (0),
      m_nBlock           (-1),
      m_nBlockEntryCount (0),
      m_nGroupDepth      (0)
   {
   }

   void Initialize (int nBlocks, int nEntries_Block)
   {
      m_nBlocks        = nBlocks;
      m_nEntries_Block = nEntries_Block;

      std::error_code ec;
      std::filesystem::create_directories (Path (), ec);

      Meta_Load ();
   }

   ~Impl ()
   {
      if (m_bAttached)
         Detach ();

      for (int nBlock = 0; nBlock < (int) m_apBlock.size (); nBlock++)
         delete m_apBlock[nBlock];

      m_apBlock.clear ();
   }

   // ---------------------------------------------------------------------------
   // Path helpers
   // ---------------------------------------------------------------------------

   std::string Path () const
   {
      return (std::filesystem::path (m_pContainer->Path_Temporary_All ()) / "Console").generic_string ();
   }

   std::string Filename (uint32_t nBlock, const std::string& sExt = "") const
   {
      char sBlock[5];
      snprintf (sBlock, sizeof (sBlock), "%04u", nBlock);

      std::string sName = sBlock;

      if (!sExt.empty ())
         sName += "." + sExt;

      return sName;
   }

   std::string Pathname (uint32_t nBlock, const std::string& sExt = "") const
   {
      return (std::filesystem::path (Path ()) / Filename (nBlock, sExt)).generic_string ();
   }

   std::string Pathname_Meta () const
   {
      return (std::filesystem::path (Path ()) / "console.meta").generic_string ();
   }

   // ---------------------------------------------------------------------------
   // Meta sidecar - read on Initialize, written on Detach.
   // ---------------------------------------------------------------------------

   void Meta_Load ()
   {
      std::string sPathname_Meta = Pathname_Meta ();

      std::ifstream file (sPathname_Meta);
      if (file.is_open ())
      {
         try
         {
            nlohmann::json jMeta = nlohmann::json::parse (file);

            m_nBlock           = jMeta.value (STREAM_META_KEY_BLOCK, -1);
            m_nBlockEntryCount = jMeta.value (STREAM_META_KEY_BLOCK_ENTRY_COUNT, 0);
         }
         catch (...)
         {
            m_nBlock           = -1;
            m_nBlockEntryCount = 0;
         }
      }

      if (m_nBlock >= 0)
      {
         int nFirstBlock = std::max (0, m_nBlock - m_nBlocks + 1);

         for (int nBlock = nFirstBlock; nBlock <= m_nBlock; nBlock++)
            m_apBlock.push_back (new BLOCK (m_pIConsole_Impl, nBlock, Pathname (nBlock, "log")));
      }
   }

   void Meta_Save ()
   {
      std::string sPathname_Meta      = Pathname_Meta ();
      std::string sPathname_Meta_Temp = sPathname_Meta + ".temp";

      nlohmann::json jMeta;

      jMeta[STREAM_META_KEY_BLOCK]             = m_nBlock;
      jMeta[STREAM_META_KEY_BLOCK_ENTRY_COUNT] = m_nBlockEntryCount;

      const CONTAINER::CID* pCID = m_pContainer->Identity ();
      jMeta[STREAM_META_KEY_FINGERPRINT]       = pCID->sFingerprint;
      jMeta[STREAM_META_KEY_ORGANIZATION]      = pCID->sOrganization;
      jMeta[STREAM_META_KEY_ORGANIZATION_HASH] = pCID->sOrganizationHash;
      jMeta[STREAM_META_KEY_CONTAINER]         = pCID->sContainer;
      jMeta[STREAM_META_KEY_PERSONA_HASH]      = pCID->sPersonaHash;

      std::error_code ec;
      std::filesystem::create_directories (std::filesystem::path (sPathname_Meta).parent_path (), ec);

      std::ofstream ofs (sPathname_Meta_Temp, std::ios::trunc);
      if (ofs.is_open ())
      {
         ofs << jMeta.dump (2);
         ofs.close ();

         std::filesystem::rename (sPathname_Meta_Temp, sPathname_Meta, ec);
      }
   }

   // ---------------------------------------------------------------------------
   // Attach / Detach
   // ---------------------------------------------------------------------------

   void Attach ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      if (!m_bAttached)
      {
         m_bAttached = true;

         for (int nBlock = 0; nBlock < (int) m_apBlock.size (); nBlock++)
            m_apBlock[nBlock]->Attach ();
      }
   }

   void Detach ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      if (m_bAttached)
      {
         for (int nBlock = 0; nBlock < (int) m_apBlock.size (); nBlock++)
            m_apBlock[nBlock]->Detach (m_pContainer);

         Meta_Save ();

         m_bAttached = false;
      }
   }

   // ---------------------------------------------------------------------------
   // Rotate - create a new block, advance to it, delete the oldest if the
   // rolling window exceeds m_nBlocks.
   // ---------------------------------------------------------------------------

   void Rotate ()
   {
      m_nBlock++;
      m_nBlockEntryCount = 0;

      m_apBlock.push_back (new BLOCK (m_pIConsole_Impl, m_nBlock, Pathname (m_nBlock, "log")));

      if (m_bAttached)
         m_apBlock.back ()->Attach ();

      if ((int) m_apBlock.size () > m_nBlocks)
      {
         BLOCK* pBlock = m_apBlock.front ();

         if (m_bAttached)
            pBlock->Detach (m_pContainer);

         delete pBlock;
         m_apBlock.erase (m_apBlock.begin ());

         int nOldBlock = m_nBlock - m_nBlocks;
         std::error_code ec;
         std::filesystem::remove (Pathname (nOldBlock, "log"), ec);
      }
   }

   // ---------------------------------------------------------------------------
   // Entry - core write path. Calls back to CONSOLE for entry creation
   // (timestamp, sequence, ring buffer), then writes to the active block.
   // ---------------------------------------------------------------------------

   void Entry (eENTRY_LEVEL eLevel, const std::string& sMessage, bool bCollapsed = false, bool bSystem = false)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      if (m_nBlock < 0  ||  m_nBlockEntryCount >= m_nEntries_Block)
         Rotate ();

      auto pEntry = m_pIConsole_Impl->Entry_Create (m_pContainer, eLevel, sMessage, m_nGroupDepth, bCollapsed, bSystem);

      m_apBlock.back ()->Write (pEntry);
      m_nBlockEntryCount++;
   }

   // ---------------------------------------------------------------------------
   // Grouping
   // ---------------------------------------------------------------------------

   void Group (const std::string& sLabel, bool bCollapsed)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      Entry (kENTRY_LEVEL_LOG, sLabel, bCollapsed);
      m_nGroupDepth++;
   }

   void GroupEnd ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      if (m_nGroupDepth > 0)
         m_nGroupDepth--;
   }

   // ---------------------------------------------------------------------------
   // Counting
   // ---------------------------------------------------------------------------

   void Count (const std::string& sLabel)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      uint32_t nCount = ++m_umpCount[sLabel];
      Entry (kENTRY_LEVEL_INFO, sLabel + ": " + std::to_string (nCount));
   }

   void CountReset (const std::string& sLabel)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      m_umpCount.erase (sLabel);
   }

   // ---------------------------------------------------------------------------
   // Timing
   // ---------------------------------------------------------------------------

   void Time (const std::string& sLabel)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      m_umpTime[sLabel] = std::chrono::steady_clock::now ();
   }

   void TimeEnd (const std::string& sLabel)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      auto it = m_umpTime.find (sLabel);
      if (it != m_umpTime.end ())
      {
         auto tpNow = std::chrono::steady_clock::now ();
         double dElapsed = std::chrono::duration<double, std::milli> (tpNow - it->second).count ();
         m_umpTime.erase (it);

         std::ostringstream oss;
         oss << sLabel << ": " << std::fixed << std::setprecision (3) << dElapsed << "ms";
         Entry (kENTRY_LEVEL_INFO, oss.str ());
      }
   }

   void TimeLog (const std::string& sLabel)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxStream);

      auto it = m_umpTime.find (sLabel);
      if (it != m_umpTime.end ())
      {
         auto tpNow = std::chrono::steady_clock::now ();
         double dElapsed = std::chrono::duration<double, std::milli> (tpNow - it->second).count ();

         std::ostringstream oss;
         oss << sLabel << ": " << std::fixed << std::setprecision (3) << dElapsed << "ms";
         Entry (kENTRY_LEVEL_INFO, oss.str ());
      }
   }

public:
   ICONSOLE_IMPL*                                                         m_pIConsole_Impl;
   CONTAINER*                                                             m_pContainer;
   std::vector<BLOCK*>                                                    m_apBlock;
   std::recursive_mutex                                                   m_mxStream;
   bool                                                                   m_bAttached;

   int                                                                    m_nBlocks;
   int                                                                    m_nEntries_Block;
   int                                                                    m_nBlock;
   int                                                                    m_nBlockEntryCount;

   uint32_t                                                               m_nGroupDepth;

   std::unordered_map<std::string, uint32_t>                              m_umpCount;
   std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_umpTime;
};

// ===========================================================================
// STREAM
// ===========================================================================

STREAM::STREAM (ICONSOLE_IMPL* pIConsole_Impl, CONTAINER* pContainer) :
   m_pImpl (new Impl (pIConsole_Impl, pContainer))
{
}

void STREAM::Initialize (int nBlocks, int nEntries_Block)
{
   m_pImpl->Initialize (nBlocks, nEntries_Block);
}

STREAM::~STREAM ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::string STREAM::DisplayName    ()                                         const { return m_pImpl->m_pContainer->Identity ()->DisplayName (); }

std::string STREAM::Path          ()                                         const { return m_pImpl->Path     (); }
std::string STREAM::Filename      (uint32_t nBlock, const std::string& sExt) const { return m_pImpl->Filename (nBlock, sExt); }
std::string STREAM::Pathname      (uint32_t nBlock, const std::string& sExt) const { return m_pImpl->Pathname (nBlock, sExt); }

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// STREAM Caching
// ---------------------------------------------------------------------------

void STREAM::Attach              ()                                                           {                  m_pImpl->Attach (); }
void STREAM::Detach              ()                                                           {                  m_pImpl->Detach (); }

// ---------------------------------------------------------------------------
// STREAM Pass-through
// ---------------------------------------------------------------------------

void     STREAM::Log             (                 const std::string& sMessage, bool bSystem) {                  m_pImpl->Entry      (kENTRY_LEVEL_LOG,                          sMessage, false, bSystem); }
void     STREAM::Debug           (                 const std::string& sMessage, bool bSystem) {                  m_pImpl->Entry      (kENTRY_LEVEL_DEBUG,                        sMessage, false, bSystem); }
void     STREAM::Info            (                 const std::string& sMessage, bool bSystem) {                  m_pImpl->Entry      (kENTRY_LEVEL_INFO,                         sMessage, false, bSystem); }
void     STREAM::Warn            (                 const std::string& sMessage, bool bSystem) {                  m_pImpl->Entry      (kENTRY_LEVEL_WARN,                         sMessage, false, bSystem); }
void     STREAM::Error           (                 const std::string& sMessage, bool bSystem) {                  m_pImpl->Entry      (kENTRY_LEVEL_ERROR,                        sMessage, false, bSystem); }
void     STREAM::Assert          (bool bCondition, const std::string& sMessage, bool bSystem) { if (!bCondition) m_pImpl->Entry      (kENTRY_LEVEL_ERROR, "Assertion failed: " + sMessage, false, bSystem); }

void     STREAM::Group           (                 const std::string& sLabel)                 {                  m_pImpl->Group      (sLabel, false); }
void     STREAM::GroupCollapsed  (                 const std::string& sLabel)                 {                  m_pImpl->Group      (sLabel, true);  }
void     STREAM::GroupEnd        ()                                                           {                  m_pImpl->GroupEnd   ();              }

void     STREAM::Count           (                 const std::string& sLabel)                 {                  m_pImpl->Count      (sLabel); }
void     STREAM::CountReset      (                 const std::string& sLabel)                 {                  m_pImpl->CountReset (sLabel); }

void     STREAM::Time            (                 const std::string& sLabel)                 {                  m_pImpl->Time       (sLabel); }
void     STREAM::TimeEnd         (                 const std::string& sLabel)                 {                  m_pImpl->TimeEnd    (sLabel); }
void     STREAM::TimeLog         (                 const std::string& sLabel)                 {                  m_pImpl->TimeLog    (sLabel); }
