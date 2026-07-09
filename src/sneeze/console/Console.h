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

#ifndef SNEEZE_CONSOLE_ICONSOLEIMPL_H
#define SNEEZE_CONSOLE_ICONSOLEIMPL_H

// JSON keys for the console-entry record, serialized by ENTRY and read back by
// both ENTRY and BLOCK -- shared here so the two never drift apart.
#define CONSOLE_ENTRY_KEY_LEVEL        "level"
#define CONSOLE_ENTRY_KEY_MESSAGE      "message"
#define CONSOLE_ENTRY_KEY_STAMP        "stamp"
#define CONSOLE_ENTRY_KEY_INDEX        "index"
#define CONSOLE_ENTRY_KEY_GROUP_DEPTH  "groupDepth"
#define CONSOLE_ENTRY_KEY_COLLAPSED    "collapsed"
#define CONSOLE_ENTRY_KEY_SYSTEM       "system"
#define CONSOLE_ENTRY_KEY_STACK_TRACE  "stackTrace"
#define CONSOLE_ENTRY_KEY_SOURCE       "source"

// JSON keys for a STREAM's on-disk meta record.
#define STREAM_META_KEY_BLOCK             "block"
#define STREAM_META_KEY_BLOCK_ENTRY_COUNT "blockEntryCount"
#define STREAM_META_KEY_FINGERPRINT       "fingerprint"
#define STREAM_META_KEY_ORGANIZATION      "organization"
#define STREAM_META_KEY_ORGANIZATION_HASH "organizationHash"
#define STREAM_META_KEY_CONTAINER         "container"
#define STREAM_META_KEY_PERSONA_HASH      "personaHash"

namespace SNEEZE
{
   class ICONSOLE_IMPL
   {
   public:
      ICONSOLE_IMPL ();
      virtual ~ICONSOLE_IMPL ();

      virtual std::shared_ptr<const ENTRY> Entry_Create (CONTAINER* pContainer, eENTRY_LEVEL eLevel, const std::string& sMessage, uint32_t nGroupDepth, bool bCollapsed, bool bSystem) = 0;
      virtual std::shared_ptr<const ENTRY> Entry_Find   (uint32_t nIndex) = 0;

   private:
   };

   // -----------------------------------------------------------------------
   // BLOCK -- one per JSONL log file on disk. Owned directly by STREAM.
   //
   // Caches ENTRY shared_ptrs in memory. Each block file holds up to
   // Entries_Block() entries.
   // -----------------------------------------------------------------------

   class BLOCK
   {
   public:
      BLOCK (ICONSOLE_IMPL* pIConsole_Impl, uint32_t nIndex, const std::string& sPathname);
      virtual ~BLOCK ();

      // --- State ---

      bool                IsLoaded () const;
      uint32_t            GetIndex () const;

      // --- Entry access ---

      void                Write (std::shared_ptr<const ENTRY> pEntry);
      void                Entry_Enum (IENUM_ENTRY* pEnum) const;

      // --- Lifecycle ---

      void                Attach ();
      void                Detach (CONTAINER* pContainer);
      void                Load   (CONTAINER* pContainer);
      void                Evict  ();

      // --- Meta ---

      const std::string&  Pathname () const;
      uint32_t            EntryCount () const;

   private:
      class Impl;
      Impl* m_pImpl;
   };
}
#endif // SNEEZE_CONSOLE_ICONSOLEIMPL_H
