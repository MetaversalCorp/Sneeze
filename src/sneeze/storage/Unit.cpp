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

// JSON keys for a UNIT's on-disk ".meta" sidecar. The size/timestamp/count keys
// are both read (Meta_Load) and written (Meta_Save); the identity keys are
// write-only. One constant per key keeps the load and save paths aligned.
#define UNIT_META_KEY_FINGERPRINT       "sFingerprint"
#define UNIT_META_KEY_ORGANIZATION      "sOrganization"
#define UNIT_META_KEY_ORGANIZATION_HASH "sOrganizationHash"
#define UNIT_META_KEY_CONTAINER         "sContainer"
#define UNIT_META_KEY_PERSONA_HASH      "sPersonaHash"
#define UNIT_META_KEY_TRUST             "eTrust"
#define UNIT_META_KEY_SCOPE             "eScope"
#define UNIT_META_KEY_SIZE_BYTES        "nSizeBytes"
#define UNIT_META_KEY_CREATED_AT        "sCreatedAt"
#define UNIT_META_KEY_LAST_ACCESSED_AT  "sLastAccessedAt"
#define UNIT_META_KEY_ACCESS_COUNT      "nAccessCount"

// ===========================================================================
// Helpers
// ===========================================================================

class UNIT::Impl
{
public:
   Impl (UNIT* pUnit, ISTORAGE_IMPL* pIStorage_Impl, eSILO_SCOPE eScope, const std::string& sPathname) :
      m_pUnit          (pUnit),
      m_pIStorage_Impl (pIStorage_Impl),
      m_eScope         (eScope),
      m_sPathname      (sPathname),
      m_bLoaded        (false),
      m_bDirty         (false),
      m_nCount_Open    (0),
      m_nCount_Load    (0),
      m_nSizeBytes     (0),
      m_nAccessCount   (0)
   {
      std::string sPathname_Meta = m_sPathname + ".meta";

      std::ifstream file (sPathname_Meta);
      if (file.is_open ())
      {
         try
         {
            nlohmann::json jMeta = nlohmann::json::parse (file);
            m_nSizeBytes      = jMeta.value (UNIT_META_KEY_SIZE_BYTES, static_cast<uint64_t> (0));
            m_sCreatedAt      = jMeta.value (UNIT_META_KEY_CREATED_AT, "");
            m_sLastAccessedAt = jMeta.value (UNIT_META_KEY_LAST_ACCESSED_AT, "");
            m_nAccessCount    = jMeta.value (UNIT_META_KEY_ACCESS_COUNT, static_cast<uint32_t> (0));
         }
         catch (...) {}
      }
   }

public:
   // ---------------------------------------------------------------------------
   // JSON path navigation
   //
   // Supports dot notation and array brackets: "game.poker.table[5].card-color"
   // Sets pParent to the parent object/array and sFinalKey to the last segment.
   // ---------------------------------------------------------------------------

   void NavigatePath (const std::string& sPath, nlohmann::json*& pParent, std::string& sFinalKey) const
   {
      pParent = const_cast<nlohmann::json*> (&m_jData);
      sFinalKey.clear ();

      if (!sPath.empty ())
      {
         std::vector<std::string> aSegments;
         std::string sCurrent;

         for (size_t i = 0; i < sPath.size (); i++)
         {
            char c = sPath[i];
            if (c == '.')
            {
               if (!sCurrent.empty ())
               {
                  aSegments.push_back (sCurrent);
                  sCurrent.clear ();
               }
            }
            else if (c == '[')
            {
               if (!sCurrent.empty ())
               {
                  aSegments.push_back (sCurrent);
                  sCurrent.clear ();
               }
               i++;
               std::string sIndex;
               while (i < sPath.size () && sPath[i] != ']')
                  sIndex += sPath[i++];
               aSegments.push_back ("[" + sIndex + "]");
            }
            else
            {
               sCurrent += c;
            }
         }
         if (!sCurrent.empty ())
            aSegments.push_back (sCurrent);

         if (!aSegments.empty ())
         {
            sFinalKey = aSegments.back ();
            aSegments.pop_back ();

            for (auto& sSeg : aSegments)
            {
               if (sSeg.size () > 2 && sSeg[0] == '[' && sSeg.back () == ']')
               {
                  int nIdx = std::stoi (sSeg.substr (1, sSeg.size () - 2));
                  if (!pParent->is_array ())
                     *pParent = nlohmann::json::array ();
                  while (static_cast<size_t> (nIdx) >= pParent->size ())
                     pParent->push_back (nlohmann::json::object ());
                  pParent = &(*pParent)[nIdx];
               }
               else
               {
                  if (!pParent->is_object ())
                     *pParent = nlohmann::json::object ();
                  if (!pParent->contains (sSeg))
                     (*pParent)[sSeg] = nlohmann::json::object ();
                  pParent = &(*pParent)[sSeg];
               }
            }
         }
      }
   }

   // ---------------------------------------------------------------------------
   // JSONL Changelog - crash durability
   // ---------------------------------------------------------------------------

   void Log_Append (const std::string& sOp, const std::string& sPath, const nlohmann::json& jValue)
   {
      std::string sPathname_Log = m_sPathname + ".log";

      std::ofstream file (sPathname_Log, std::ios::app);
      if (file.is_open ())
      {
         nlohmann::json jEntry = nlohmann::json::array ();
         jEntry.push_back (sOp);
         jEntry.push_back (sPath);
         if (!jValue.is_null ())
            jEntry.push_back (jValue);
         file << jEntry.dump () << "\n";
      }
   }

   void Log_Replay ()
   {
      std::string sPathname_Log = m_sPathname + ".log";

      std::ifstream file (sPathname_Log);
      if (file.is_open ())
      {
         std::string sLine;
         while (std::getline (file, sLine))
         {
            if (sLine.empty ())
               continue;

            try
            {
               nlohmann::json jEntry = nlohmann::json::parse (sLine);
               if (!jEntry.is_array () || jEntry.size () < 2)
                  continue;

               std::string sOp = jEntry[0].get<std::string> ();
               std::string sPath = jEntry[1].get<std::string> ();

               if (sOp == "Set" && jEntry.size () >= 3)
               {
                  nlohmann::json* pParent = nullptr;
                  std::string sFinalKey;
                  NavigatePath (sPath, pParent, sFinalKey);

                  if (pParent)
                  {
                     if (sFinalKey.empty ())
                     {
                        *pParent = jEntry[2];
                     }
                     else
                     {
                        if (sFinalKey.size () > 2 && sFinalKey[0] == '[' && sFinalKey.back () == ']')
                        {
                           int nIdx = std::stoi (sFinalKey.substr (1, sFinalKey.size () - 2));
                           if (!pParent->is_array ())
                              *pParent = nlohmann::json::array ();
                           while (static_cast<size_t> (nIdx) >= pParent->size ())
                              pParent->push_back (nlohmann::json ());
                           (*pParent)[nIdx] = jEntry[2];
                        }
                        else
                        {
                           if (!pParent->is_object ())
                              *pParent = nlohmann::json::object ();
                           (*pParent)[sFinalKey] = jEntry[2];
                        }
                     }
                  }
               }
               else if (sOp == "Remove")
               {
                  nlohmann::json* pParent = nullptr;
                  std::string sFinalKey;
                  NavigatePath (sPath, pParent, sFinalKey);

                  if (pParent)
                  {
                     if (sFinalKey.empty ())
                     {
                        *pParent = nlohmann::json::object ();
                     }
                     else
                     {
                        if (sFinalKey.size () > 2 && sFinalKey[0] == '[' && sFinalKey.back () == ']')
                        {
                           int nIdx = std::stoi (sFinalKey.substr (1, sFinalKey.size () - 2));
                           if (pParent->is_array () && nIdx >= 0 && static_cast<size_t> (nIdx) < pParent->size ())
                              pParent->erase (nIdx);
                        }
                        else
                        {
                           if (pParent->is_object () && pParent->contains (sFinalKey))
                              pParent->erase (sFinalKey);
                        }
                     }
                  }
               }
            }
            catch (...) {}
         }

         m_bDirty = true;
      }
   }

   void Log_Delete ()
   {
      std::string sPathname_Log = m_sPathname + ".log";
      
      std::error_code ec;
      std::filesystem::remove (sPathname_Log, ec);
   }

   void Open (SILO* pSilo)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      if (m_nCount_Open == 0)
      {
         std::error_code ec;
         std::filesystem::create_directories (std::filesystem::path (m_sPathname).parent_path (), ec);
      }

      m_apSilo.push_back (pSilo);

      m_nCount_Open++;

      pSilo->Container ()->Context ()->Host ()->OnStorageUnitCreated (pSilo, m_eScope);
   }

   uint32_t Close (SILO* pSilo)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      pSilo->Container ()->Context ()->Host ()->OnStorageUnitDeleted (pSilo, m_eScope);

      m_nCount_Open--;

      auto it = std::find (m_apSilo.begin (), m_apSilo.end (), pSilo);
      if (it != m_apSilo.end ())
         m_apSilo.erase (it);

      return m_nCount_Open;
   }

   void Notify_Changed (const std::string& sPath)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      for (SILO* pSilo : m_apSilo)
         pSilo->Container ()->Context ()->Host ()->OnStorageUnitChanged (pSilo, m_eScope, sPath);
   }

   void Attach () 
   { 
      if (++m_nCount_Load == 1) 
         Load (); 
   }

   void Detach (CONTAINER* pContainer)
   {
      if (m_nCount_Load > 0 && --m_nCount_Load == 0)
      {
         Meta_Save (pContainer);

         if (m_bDirty)
            Save ();

         Evict ();
      }
   }

   void Load ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      if (!m_bLoaded)
      {
         m_jData = nlohmann::json::object ();

         std::string sPathname_Json = m_sPathname + ".json";
         if (std::filesystem::exists (sPathname_Json))
         {
            std::ifstream file (sPathname_Json);
            if (file.is_open ())
            {
               try
               {
                  m_jData = nlohmann::json::parse (file);
               }
               catch (...)
               {
                  m_jData = nlohmann::json::object ();
               }
            }
         }

         Log_Replay ();

         if (m_sCreatedAt.empty ())
            m_sCreatedAt = NowIso8601 ();

         m_bLoaded = true;

         if (m_bDirty)
            Save ();
      }
   }

   void Save ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      if (m_bLoaded)
      {
         std::string sPathname_Json      = m_sPathname + ".json";
         std::string sPathname_Json_Temp = sPathname_Json + ".temp";

         std::error_code ec;
         std::filesystem::create_directories (std::filesystem::path (sPathname_Json).parent_path (), ec);

         std::ofstream file (sPathname_Json_Temp, std::ios::trunc);
         if (file.is_open ())
         {
            file << m_jData.dump (2);
            file.close ();

            std::filesystem::rename (sPathname_Json_Temp, sPathname_Json, ec);

            if (!ec)
            {
               auto nSize = std::filesystem::file_size (sPathname_Json, ec);
               if (!ec)
                  m_nSizeBytes = static_cast<uint64_t> (nSize);
            }
         }

         Log_Delete ();
         m_bDirty = false;
      }
   }

   void Evict ()
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      if (m_bDirty)
         Save ();

      m_jData = nlohmann::json ();
      m_bLoaded = false;
   }

   void TouchAccess ()
   {
      m_sLastAccessedAt = NowIso8601 ();
      m_nAccessCount++;
   }

   void Meta_Save (CONTAINER* pContainer)
   {
      std::string sPathname_Meta      = m_sPathname + ".meta";
      std::string sPathname_Meta_Temp = sPathname_Meta + ".temp";

      const CONTAINER::CID* pCID = pContainer->Identity ();
      nlohmann::json jMeta;

      jMeta[UNIT_META_KEY_FINGERPRINT]       = pCID->sFingerprint;
      jMeta[UNIT_META_KEY_ORGANIZATION]      = pCID->sOrganization;
      jMeta[UNIT_META_KEY_ORGANIZATION_HASH] = pCID->sOrganizationHash;
      jMeta[UNIT_META_KEY_CONTAINER]         = pCID->sContainer;
      jMeta[UNIT_META_KEY_PERSONA_HASH]      = pCID->sPersonaHash;
      jMeta[UNIT_META_KEY_TRUST]             = static_cast<int> (pCID->eTrust);

      jMeta[UNIT_META_KEY_SCOPE]             = static_cast<int> (m_eScope);
      jMeta[UNIT_META_KEY_SIZE_BYTES]        = m_nSizeBytes;
      jMeta[UNIT_META_KEY_CREATED_AT]        = m_sCreatedAt;
      jMeta[UNIT_META_KEY_LAST_ACCESSED_AT]  = m_sLastAccessedAt;
      jMeta[UNIT_META_KEY_ACCESS_COUNT]      = m_nAccessCount;

      std::error_code ec;
      std::filesystem::create_directories (std::filesystem::path (sPathname_Meta).parent_path (), ec);

      std::ofstream file (sPathname_Meta_Temp, std::ios::trunc);
      if (file.is_open ())
      {
         file << jMeta.dump (2);
         file.close ();

         std::filesystem::rename (sPathname_Meta_Temp, sPathname_Meta, ec);
      }
   }

   // ---------------------------------------------------------------------------
   // JSON access
   // ---------------------------------------------------------------------------

   nlohmann::json Get (const std::string& sPath) const
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      nlohmann::json* pParent = nullptr;
      std::string sFinalKey;
      NavigatePath (sPath, pParent, sFinalKey);

      nlohmann::json jResult;

      if (pParent)
      {
         if (sFinalKey.empty ())
         {
            jResult = *pParent;
         }
         else
         {
            if (sFinalKey.size () > 2  &&  sFinalKey[0] == '['  &&  sFinalKey.back () == ']')
            {
               int nIdx = std::stoi (sFinalKey.substr (1, sFinalKey.size () - 2));
               if (pParent->is_array ()  &&  nIdx >= 0  &&  static_cast<size_t> (nIdx) < pParent->size ())
                  jResult = (*pParent)[nIdx];
            }
            else if (pParent->is_object ()  &&  pParent->contains (sFinalKey))
               jResult = (*pParent)[sFinalKey];
         }
      }

      return jResult;
   }

   void Set (const std::string& sPath, const nlohmann::json& jValue)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      nlohmann::json* pParent = nullptr;
      std::string sFinalKey;
      NavigatePath (sPath, pParent, sFinalKey);

      if (pParent)
      {
         if (sFinalKey.empty ())
         {
            *pParent = jValue;

            m_bDirty = true;
            TouchAccess ();
            Log_Append ("Set", sPath, jValue);

            Notify_Changed (sPath);
         }
         else
         {
            if (sFinalKey.size () > 2  &&  sFinalKey[0] == '['  &&  sFinalKey.back () == ']')
            {
               int nIdx = std::stoi (sFinalKey.substr (1, sFinalKey.size () - 2));
               if (!pParent->is_array ())
                  *pParent = nlohmann::json::array ();
               while (static_cast<size_t> (nIdx) >= pParent->size ())
                  pParent->push_back (nlohmann::json ());
               (*pParent)[nIdx] = jValue;
            }
            else
            {
               if (!pParent->is_object ())
                  *pParent = nlohmann::json::object ();
               (*pParent)[sFinalKey] = jValue;
            }

            m_bDirty = true;
            TouchAccess ();
            Log_Append ("Set", sPath, jValue);

            Notify_Changed (sPath);
         }
      }
   }

   void Remove (const std::string& sPath)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      nlohmann::json* pParent = nullptr;
      std::string sFinalKey;
      NavigatePath (sPath, pParent, sFinalKey);

      if (pParent)
      {
         if (sFinalKey.empty ())
         {
            *pParent = nlohmann::json::object ();

            m_bDirty = true;
            TouchAccess ();
            Log_Append ("Remove", sPath, nlohmann::json ());

            Notify_Changed (sPath);
         }
         else
         {
            if (sFinalKey.size () > 2  &&  sFinalKey[0] == '['  &&  sFinalKey.back () == ']')
            {
               int nIdx = std::stoi (sFinalKey.substr (1, sFinalKey.size () - 2));
               if (pParent->is_array ()  &&  nIdx >= 0  &&  static_cast<size_t> (nIdx) < pParent->size ())
                  pParent->erase (nIdx);
            }
            else
            {
               if (pParent->is_object ()  &&  pParent->contains (sFinalKey))
                  pParent->erase (sFinalKey);
            }

            m_bDirty = true;
            TouchAccess ();
            Log_Append ("Remove", sPath, nlohmann::json ());

            Notify_Changed (sPath);
         }
      }
   }

   bool Has (const std::string& sPath) const
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      nlohmann::json* pParent = nullptr;
      std::string sFinalKey;
      NavigatePath (sPath, pParent, sFinalKey);

      bool bHas = false;
      if (pParent)
      {
         if (sFinalKey.empty ())
         {
            bHas = true;
         }
         else
         {
            if (sFinalKey.size () > 2  &&  sFinalKey[0] == '['  &&  sFinalKey.back () == ']')
            {
               int nIdx = std::stoi (sFinalKey.substr (1, sFinalKey.size () - 2));
               bHas = pParent->is_array ()  &&  nIdx >= 0  &&  static_cast<size_t> (nIdx) < pParent->size ();
            }
            else
               bHas = pParent->is_object ()  &&  pParent->contains (sFinalKey);
         }
      }

      return bHas;
   }

   std::string Json () const
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      return m_jData.dump (2);
   }

   void Json (const std::string& sJson)
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxUnit);

      try
      {
         m_jData = nlohmann::json::parse (sJson);
      }
      catch (...)
      {
         m_jData = nlohmann::json::object ();
      }

      m_bDirty = true;
      TouchAccess ();
      Log_Append ("Set", "", m_jData);

      Notify_Changed ("");
   }

   UNIT*                         m_pUnit;
   ISTORAGE_IMPL*                m_pIStorage_Impl;
   eSILO_SCOPE                   m_eScope;
   std::string                   m_sPathname;

   nlohmann::json                m_jData;
   bool                          m_bLoaded;
   bool                          m_bDirty;
   uint32_t                      m_nCount_Open;
   uint32_t                      m_nCount_Load;
   std::vector<SILO*>            m_apSilo;

   // Meta sidecar fields
   uint64_t                      m_nSizeBytes;
   std::string                   m_sCreatedAt;
   std::string                   m_sLastAccessedAt;
   uint32_t                      m_nAccessCount;

   mutable std::recursive_mutex  m_mxUnit;
};

// ===========================================================================
// UNIT
// ===========================================================================

UNIT::UNIT (ISTORAGE_IMPL* pIStorage_Impl, eSILO_SCOPE eScope, const std::string& sPathname) :
   m_pImpl (new Impl (this, pIStorage_Impl, eScope, sPathname))
{
}

UNIT::~UNIT ()
{
   delete m_pImpl;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool                    UNIT::IsLoaded       () const { return m_pImpl->m_bLoaded; }
bool                    UNIT::IsDirty        () const { return m_pImpl->m_bDirty; }
SNEEZE::eSILO_SCOPE     UNIT::GetScope       () const { return m_pImpl->m_eScope; }
const std::string&      UNIT::Pathname       () const { return m_pImpl->m_sPathname; }
uint64_t                UNIT::SizeBytes      () const { return m_pImpl->m_nSizeBytes; }
const std::string&      UNIT::CreatedTime    () const { return m_pImpl->m_sCreatedAt; }
const std::string&      UNIT::LastAccessTime () const { return m_pImpl->m_sLastAccessedAt; }
uint32_t                UNIT::AccessCount    () const { return m_pImpl->m_nAccessCount; }

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

void           UNIT::Open       (SILO* pSilo)                                             {        m_pImpl->Open  (pSilo); }
uint32_t       UNIT::Close      (SILO* pSilo)                                             { return m_pImpl->Close (pSilo); }

void           UNIT::Attach     ()                                                        {        m_pImpl->Attach      (); }
void           UNIT::Detach     (CONTAINER* pContainer)                                   {        m_pImpl->Detach      (pContainer); }

void           UNIT::Load       ()                                                        {        m_pImpl->Load        (); }
void           UNIT::Save       ()                                                        {        m_pImpl->Save        (); }
void           UNIT::Evict      ()                                                        {        m_pImpl->Evict       (); }

void           UNIT::TouchAccess ()                                                       {        m_pImpl->TouchAccess (); }
void           UNIT::Meta_Save  (CONTAINER* pContainer)                                   {        m_pImpl->Meta_Save   (pContainer); }

nlohmann::json UNIT::Get        (const std::string& sPath)                          const { return m_pImpl->Get         (sPath); }
void           UNIT::Set        (const std::string& sPath, const nlohmann::json& j)       {        m_pImpl->Set         (sPath, j); }
void           UNIT::Remove     (const std::string& sPath)                                {        m_pImpl->Remove      (sPath); }
bool           UNIT::Has        (const std::string& sPath)                          const { return m_pImpl->Has         (sPath); }
std::string    UNIT::Json       ()                                                  const { return m_pImpl->Json        (); }
void           UNIT::Json       (const std::string& sJson)                                {        m_pImpl->Json        (sJson); }
