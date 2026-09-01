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
// ENTRY
// ===========================================================================

ENTRY::ENTRY (CONTAINER* pContainer, eENTRY_LEVEL eLevel, const std::string& sMessage, uint32_t nIndex, uint32_t nGroupDepth, bool bCollapsed, bool bSystem, const std::string& sStackTrace, const std::string& sSource) :
   m_pContainer  (pContainer),
   m_eLevel      (eLevel),
   m_sMessage    (sMessage),
   m_nIndex      (nIndex),
   m_nGroupDepth (nGroupDepth),
   m_bCollapsed  (bCollapsed),
   m_bSystem     (bSystem),
   m_sStackTrace (sStackTrace),
   m_sSource     (sSource),
   m_tpStamp     (std::chrono::system_clock::now ())
{
}

// ---------------------------------------------------------------------------
// Const accessors
// ---------------------------------------------------------------------------

SNEEZE::CONTAINER*                    ENTRY::Container   () const { return m_pContainer; }
eENTRY_LEVEL                          ENTRY::Level       () const { return m_eLevel; }
const std::string&                    ENTRY::Message     () const { return m_sMessage; }
uint32_t                              ENTRY::Index       () const { return m_nIndex; }
uint32_t                              ENTRY::GroupDepth  () const { return m_nGroupDepth; }
bool                                  ENTRY::IsCollapsed () const { return m_bCollapsed; }
bool                                  ENTRY::IsSystem    () const { return m_bSystem; }
const std::string&                    ENTRY::StackTrace  () const { return m_sStackTrace; }
const std::string&                    ENTRY::Source      () const { return m_sSource; }
std::chrono::system_clock::time_point ENTRY::tpStamp     () const { return m_tpStamp; }

// ---------------------------------------------------------------------------
// LevelString
// ---------------------------------------------------------------------------

void ENTRY::LevelString (eENTRY_LEVEL eLevel, std::string& sLevel)
{
   sLevel = "log";

   switch (eLevel)
   {
      case kENTRY_LEVEL_DEBUG: sLevel = "debug"; break;
      case kENTRY_LEVEL_LOG:   sLevel = "log";   break;
      case kENTRY_LEVEL_INFO:  sLevel = "info";  break;
      case kENTRY_LEVEL_WARN:  sLevel = "warn";  break;
      case kENTRY_LEVEL_ERROR: sLevel = "error"; break;
   }
}

// ---------------------------------------------------------------------------
// FormatStamp
// ---------------------------------------------------------------------------

std::string ENTRY::FormatStamp () const
{
   auto tStamp        = std::chrono::system_clock::to_time_t (m_tpStamp);
   auto nMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds> (m_tpStamp.time_since_epoch ()).count () % 1000;

   struct tm tmLocal;
#ifdef _WIN32
   localtime_s (&tmLocal, &tStamp);
#else
   localtime_r (&tStamp, &tmLocal);
#endif

   char sStamp[32];
   snprintf (sStamp, sizeof (sStamp), "%02d:%02d:%02d.%03d", tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec, static_cast<int> (nMilliseconds));

   return sStamp;
}

// ---------------------------------------------------------------------------
// MessageParts - parse the message into components.
//
// The message may be a JSON array of mixed types (strings, objects):
//   ["hello", { "a": 5 }, "world"]
//
// If the message is not a JSON array, the entire string is returned as a
// single element. JSON parsing errors return the raw message.
// ---------------------------------------------------------------------------

void ENTRY::MessageParts (std::vector<std::string> &aParts) const
{
   if (!m_sMessage.empty ()  &&  m_sMessage.front () == '[')
   {
      try
      {
         nlohmann::json jArray = nlohmann::json::parse (m_sMessage);
         if (jArray.is_array ())
         {
            for (const auto& jItem : jArray)
            {
               if (jItem.is_string ())
                  aParts.push_back (jItem.get<std::string> ());
               else
                  aParts.push_back (jItem.dump ());
            }
         }
         else aParts.push_back (m_sMessage);
      }
      catch (...)
      {
         aParts.push_back (m_sMessage);
      }
   }
   else aParts.push_back (m_sMessage);
}

// ---------------------------------------------------------------------------
// ToJson - serialize to a JSON object for JSONL disk storage.
// ---------------------------------------------------------------------------

nlohmann::json ENTRY::ToJson () const
{
   nlohmann::json jEntry;
   std::string sLevel;

   LevelString (m_eLevel, sLevel);

   jEntry[CONSOLE_ENTRY_KEY_LEVEL]       = sLevel;
   jEntry[CONSOLE_ENTRY_KEY_MESSAGE]     = m_sMessage;
   jEntry[CONSOLE_ENTRY_KEY_STAMP]       = std::chrono::duration<double> (m_tpStamp.time_since_epoch ()).count ();
   jEntry[CONSOLE_ENTRY_KEY_INDEX]       = m_nIndex;
   jEntry[CONSOLE_ENTRY_KEY_GROUP_DEPTH] = m_nGroupDepth;
   jEntry[CONSOLE_ENTRY_KEY_COLLAPSED]   = m_bCollapsed;
   jEntry[CONSOLE_ENTRY_KEY_SYSTEM]      = m_bSystem;

   if (!m_sStackTrace.empty ())
      jEntry[CONSOLE_ENTRY_KEY_STACK_TRACE] = m_sStackTrace;

   if (!m_sSource.empty ())
      jEntry[CONSOLE_ENTRY_KEY_SOURCE] = m_sSource;

   return jEntry;
}

// ---------------------------------------------------------------------------
// FromJson - deserialize from a JSONL line. The CID pointer is provided by
// the STREAM that owns the block file (ENTRY does not resolve CIDs itself).
// ---------------------------------------------------------------------------

std::shared_ptr<const ENTRY> ENTRY::FromJson (const nlohmann::json& jEntry, CONTAINER* pContainer)
{
   eENTRY_LEVEL eLevel = kENTRY_LEVEL_LOG;
   std::string sLevelStr = jEntry.value (CONSOLE_ENTRY_KEY_LEVEL, "log");
   if      (sLevelStr == "debug") eLevel = kENTRY_LEVEL_DEBUG;
   else if (sLevelStr == "info")  eLevel = kENTRY_LEVEL_INFO;
   else if (sLevelStr == "warn")  eLevel = kENTRY_LEVEL_WARN;
   else if (sLevelStr == "error") eLevel = kENTRY_LEVEL_ERROR;

   auto pEntry = std::make_shared<ENTRY>
   (
      pContainer,
      eLevel,
      jEntry.value (CONSOLE_ENTRY_KEY_MESSAGE,     std::string ()),
      jEntry.value (CONSOLE_ENTRY_KEY_INDEX,       static_cast<uint32_t> (0)),
      jEntry.value (CONSOLE_ENTRY_KEY_GROUP_DEPTH, static_cast<uint32_t> (0)),
      jEntry.value (CONSOLE_ENTRY_KEY_COLLAPSED,   false),
      jEntry.value (CONSOLE_ENTRY_KEY_SYSTEM,      false),
      jEntry.value (CONSOLE_ENTRY_KEY_STACK_TRACE, std::string ()),
      jEntry.value (CONSOLE_ENTRY_KEY_SOURCE,      std::string ())
   );

   double dStamp = jEntry.value (CONSOLE_ENTRY_KEY_STAMP, 0.0);
   if (dStamp > 0.0)
      pEntry->m_tpStamp = std::chrono::system_clock::time_point (std::chrono::duration_cast<std::chrono::system_clock::duration> (std::chrono::duration<double> (dStamp)));

   return pEntry;
}
