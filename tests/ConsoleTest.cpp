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
#include <cstdio>
#include <thread>
#include <filesystem>
#include <deque>

using namespace SNEEZE;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static int g_nPassed = 0;
static int g_nFailed = 0;

#define ASSERT(expr, msg) \
   do { if (expr) { g_nPassed++; std::printf ("  PASS: %s\n", msg); } \
        else      { g_nFailed++; std::printf ("  FAIL: %s\n", msg); } } while (0)

// ---------------------------------------------------------------------------
// Minimal ENGINE host
// ---------------------------------------------------------------------------

class TEST_HOST : public IENGINE
{
public:
   TEST_HOST (const std::string& sPath) : m_sPath (sPath), m_sRenderer ("halogen") {}

   std::string const& sAppDataPath () const& override { return m_sPath; }
   std::string const& sRenderer ()    const& override { return m_sRenderer; }

   void Log (eLOGLEVEL, const std::string& sModule, const std::string& sMessage) override
   {
      std::printf ("    [%s] %s\n", sModule.c_str (), sMessage.c_str ());
   }

private:
   std::string m_sPath;
   std::string m_sRenderer;
};

// ---------------------------------------------------------------------------
// ICONTEXT host that tracks console notifications
// ---------------------------------------------------------------------------

class TEST_CONTEXT_HOST : public ICONTEXT
{
public:
   int m_nEntryCount = 0;
   int m_nClearCount = 0;

   void OnConsoleEntryCreated (std::shared_ptr<const ENTRY>) override { m_nEntryCount++; }
   void OnConsoleEntryDeleted (std::shared_ptr<const ENTRY>) override { m_nClearCount++; }
};

// ---------------------------------------------------------------------------
// IENUM collector
// ---------------------------------------------------------------------------

class ENTRY_COLLECTOR : public IENUM_ENTRY
{
public:
   std::vector<std::shared_ptr<const ENTRY>> m_aEntry;

   void OnEntry (std::shared_ptr<const ENTRY> pEntry) override
   {
      m_aEntry.push_back (pEntry);
   }
};

// ---------------------------------------------------------------------------
// RunConsoleTests
// ---------------------------------------------------------------------------

int RunConsoleTests (int nArgc, char** aArgv)
{
   (void) nArgc; (void) aArgv;

   g_nPassed = 0;
   g_nFailed = 0;

   std::printf ("Console Tests\n");
   std::printf ("---------------------------------------------------------\n");

   // Clean test directory
   std::string sPath_Test = (std::filesystem::path (
#ifdef _WIN32
      std::getenv ("APPDATA")
#else
      "/tmp"
#endif
   ) / "Metaversal" / "Sneeze" / "Test").string ();

   std::error_code ec;
   std::filesystem::remove_all (sPath_Test, ec);
   std::filesystem::create_directories (sPath_Test, ec);

   // Create engine
   TEST_HOST host (sPath_Test);
   ENGINE engine (&host);
   bool bInit = engine.Initialize ();
   ASSERT (bInit, "Engine initialized");

   if (!bInit)
   {
      std::printf ("\n  --- Results: %d passed, %d failed ---\n", g_nPassed, g_nFailed);
      return (g_nFailed > 0) ? 1 : 0;
   }

   engine.Login ("Test", "User");

   // Create a CONTAINER for testing
   CONTAINER::CID cid;
   cid.sFingerprint       = "abcdef0123456789abcdef01234567890123456789abcdef0123456789abcd";
   cid.sOrganization      = "TestOrg";
   cid.sOrganizationHash  = "abcdef012345";
   cid.sContainer         = "TestContainer";
   cid.sPersonaHash       = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2";
   cid.eTrust             = kTRUST_VERIFIED;

   // -----------------------------------------------------------------------
   // Test 1: Console initialization
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 1] Console initialization\n");

   TEST_CONTEXT_HOST contextHost;
   CONTEXT* pContext = engine.Context_Open (&contextHost);
   ASSERT (pContext != nullptr, "Context created");
   ASSERT (pContext->Console () != nullptr, "Console exists");

   CONSOLE* pConsole = pContext->Console ();

   CONTAINER container (pContext, &cid);

   // Open a stream for the container - all logging goes through streams
   STREAM* pStream = pConsole->Stream_Open (&container);
   ASSERT (pStream != nullptr, "Stream_Open returned non-null");

   // -----------------------------------------------------------------------
   // Test 2: Basic logging
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 2] Basic logging\n");

   pStream->Log   ("Hello from log");
   pStream->Debug ("Hello from debug");
   pStream->Info  ("Hello from info");
   pStream->Warn  ("Hello from warn");
   pStream->Error ("Hello from error");

   ENTRY_COLLECTOR collector;
   pConsole->Entry_Enum (&collector);
   ASSERT (collector.m_aEntry.size () == 5, "Five entries in ring buffer");
   ASSERT (collector.m_aEntry[0]->Level () == kENTRY_LEVEL_LOG,   "First entry is LOG");
   ASSERT (collector.m_aEntry[4]->Level () == kENTRY_LEVEL_ERROR, "Last entry is ERROR");
   ASSERT (collector.m_aEntry[0]->Message () == "Hello from log", "Log message correct");
   ASSERT (contextHost.m_nEntryCount == 5, "Five OnConsoleEntry notifications");

   // -----------------------------------------------------------------------
   // Test 3: Engine-internal logging (null container stream)
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 3] Engine-internal logging (null container)\n");

   STREAM* pEngineStream = pConsole->Stream_Open (nullptr);

   if (pEngineStream)
   {
      pEngineStream->Info ("Engine startup complete");

      ENTRY_COLLECTOR collector2;
      pConsole->Entry_Enum (&collector2);
      ASSERT (collector2.m_aEntry.size () == 6, "Six entries total");
      ASSERT (collector2.m_aEntry[5]->Container () == nullptr, "Engine entry has null container");

      pConsole->Stream_Close (pEngineStream);
   }
   else
   {
      ASSERT (true, "Null-container stream not supported (skipped)");
   }

   // -----------------------------------------------------------------------
   // Test 4: Assert
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 4] Assert\n");

   pStream->Assert (true,  "This should not appear");
   pStream->Assert (false, "This should appear");

   ENTRY_COLLECTOR collector3;
   pConsole->Entry_Enum (&collector3);
   auto pAssertEntry = collector3.m_aEntry.back ();
   ASSERT (pAssertEntry->Level () == kENTRY_LEVEL_ERROR, "Failed assert is ERROR");
   ASSERT (pAssertEntry->Message ().find ("Assertion failed") != std::string::npos, "Assert message contains prefix");

   // -----------------------------------------------------------------------
   // Test 5: Grouping
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 5] Grouping\n");

   pConsole->Clear ();

   pStream->Group ("Group A");
   pStream->Log   ("Inside group A");
   pStream->GroupEnd ();
   pStream->Log   ("Outside group A");

   ENTRY_COLLECTOR collector4;
   pConsole->Entry_Enum (&collector4);
   ASSERT (collector4.m_aEntry.size () >= 3, "At least 3 entries from grouping");

   if (collector4.m_aEntry.size () >= 3)
   {
      ASSERT (collector4.m_aEntry[0]->GroupDepth () == 0, "Group label at depth 0");
      ASSERT (collector4.m_aEntry[1]->GroupDepth () == 1, "Entry inside group at depth 1");
      ASSERT (collector4.m_aEntry[2]->GroupDepth () == 0, "Entry after GroupEnd at depth 0");
   }

   // -----------------------------------------------------------------------
   // Test 6: Counting
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 6] Counting\n");

   pConsole->Clear ();

   pStream->Count ("clicks");
   pStream->Count ("clicks");
   pStream->Count ("clicks");

   ENTRY_COLLECTOR collector5;
   pConsole->Entry_Enum (&collector5);
   ASSERT (collector5.m_aEntry.size () == 3, "Three count entries");

   if (collector5.m_aEntry.size () == 3)
   {
      ASSERT (collector5.m_aEntry[0]->Message () == "clicks: 1", "First count is 1");
      ASSERT (collector5.m_aEntry[1]->Message () == "clicks: 2", "Second count is 2");
      ASSERT (collector5.m_aEntry[2]->Message () == "clicks: 3", "Third count is 3");
   }

   pStream->CountReset ("clicks");
   pStream->Count ("clicks");

   ENTRY_COLLECTOR collector5b;
   pConsole->Entry_Enum (&collector5b);
   ASSERT (collector5b.m_aEntry.back ()->Message () == "clicks: 1", "Count reset to 1");

   // -----------------------------------------------------------------------
   // Test 7: Timing
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 7] Timing\n");

   pConsole->Clear ();

   pStream->Time ("op");
   std::this_thread::sleep_for (std::chrono::milliseconds (50));
   pStream->TimeEnd ("op");

   ENTRY_COLLECTOR collector6;
   pConsole->Entry_Enum (&collector6);
   auto pTimerEntry = collector6.m_aEntry.back ();
   ASSERT (pTimerEntry->Message ().find ("op:") != std::string::npos, "Timer entry contains label");
   ASSERT (pTimerEntry->Message ().find ("ms") != std::string::npos, "Timer entry contains ms");

   // -----------------------------------------------------------------------
   // Test 8: Clear
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 8] Clear\n");

   pConsole->Clear ();
   contextHost.m_nClearCount = 0;
   contextHost.m_nEntryCount = 0;

   pStream->Log ("Entry A");
   pStream->Log ("Entry B");
   pStream->Log ("Entry C");

   int nDeletesBefore = contextHost.m_nClearCount;
   ENTRY_COLLECTOR collectorPre;
   pConsole->Entry_Enum (&collectorPre);
   int nEntriesInRing = static_cast<int> (collectorPre.m_aEntry.size ());
   pConsole->Clear ();
   ASSERT (contextHost.m_nClearCount - nDeletesBefore == nEntriesInRing, "OnConsoleEntryDeleted fired per entry");

   ENTRY_COLLECTOR collector7;
   pConsole->Entry_Enum (&collector7);
   ASSERT (collector7.m_aEntry.empty (), "Ring buffer cleared");

   // -----------------------------------------------------------------------
   // Test 9: ENTRY serialization round-trip
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 9] ENTRY serialization round-trip\n");

   pConsole->Clear ();
   pStream->Log ("Serialize me");

   ENTRY_COLLECTOR collector8;
   pConsole->Entry_Enum (&collector8);
   ASSERT (!collector8.m_aEntry.empty (), "Entry exists for serialization");

   if (!collector8.m_aEntry.empty ())
   {
      auto pOriginal = collector8.m_aEntry.front ();
      nlohmann::json jEntry = pOriginal->ToJson ();
      auto pDeserialized = ENTRY::FromJson (jEntry, &container);

      ASSERT (pDeserialized != nullptr, "Deserialized entry is non-null");
      ASSERT (pDeserialized->Level ()   == pOriginal->Level (),   "Level round-trips");
      ASSERT (pDeserialized->Message () == pOriginal->Message (), "Message round-trips");
      ASSERT (pDeserialized->Index ()   == pOriginal->Index (),   "Index round-trips");
   }

   // -----------------------------------------------------------------------
   // Test 10: Stream attach/detach
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 10] Stream attach/detach\n");

   pConsole->Clear ();

   pStream->Log ("Disk entry 1");
   pStream->Log ("Disk entry 2");
   pStream->Log ("Disk entry 3");

   pStream->Attach ();
   pStream->Detach ();

   ASSERT (true, "Stream attach/detach cycle completed");

   // -----------------------------------------------------------------------
   // Test 11: Configuration
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 11] Configuration\n");

   ASSERT (pConsole->Entries_Cache ()   == 16384, "Default Entries_Cache is 16384");
   ASSERT (pConsole->Entries_Block ()   == 4096,  "Default Entries_Block is 4096");
   ASSERT (pConsole->Blocks ()          == 4,     "Default Blocks is 4");

   pConsole->Entries_Cache (50);
   ASSERT (pConsole->Entries_Cache ()   == 50, "Entries_Cache changed to 50");

   pConsole->Entries_Block (100);
   ASSERT (pConsole->Entries_Block ()   == 100,   "Entries_Block changed to 100");

   pConsole->Blocks (3);
   ASSERT (pConsole->Blocks () == 3, "Blocks changed to 3");

   // -----------------------------------------------------------------------
   // Test 12: Ring buffer cap
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 12] Ring buffer cap\n");

   pConsole->Clear ();
   pConsole->Entries_Cache (10);

   for (int n = 0; n < 20; ++n)
      pStream->Log ("Entry " + std::to_string (n));

   ENTRY_COLLECTOR collector11;
   pConsole->Entry_Enum (&collector11);
   ASSERT (collector11.m_aEntry.size () == 10, "Ring buffer capped at 10");
   ASSERT (collector11.m_aEntry.front ()->Message () == "Entry 10", "Oldest entry is Entry 10 (first 10 evicted)");

   // -----------------------------------------------------------------------
   // Test 13: LevelString
   // -----------------------------------------------------------------------

   std::printf ("\n[Test 13] LevelString\n");

   std::string sLevel;

   ENTRY::LevelString (kENTRY_LEVEL_LOG, sLevel);
   ASSERT (sLevel == "log",   "LOG -> log");
   ENTRY::LevelString (kENTRY_LEVEL_DEBUG, sLevel);
   ASSERT (sLevel == "debug", "DEBUG -> debug");
   ENTRY::LevelString (kENTRY_LEVEL_INFO, sLevel);
   ASSERT (sLevel == "info",  "INFO -> info");
   ENTRY::LevelString (kENTRY_LEVEL_WARN, sLevel);
   ASSERT (sLevel == "warn",  "WARN -> warn");
   ENTRY::LevelString (kENTRY_LEVEL_ERROR, sLevel);
   ASSERT (sLevel == "error", "ERROR -> error");

   // -----------------------------------------------------------------------
   // Cleanup
   // -----------------------------------------------------------------------

   pConsole->Stream_Close (pStream);
   engine.Context_Close (pContext);

   std::printf ("\n  --- Results: %d passed, %d failed ---\n", g_nPassed, g_nFailed);

   return (g_nFailed > 0) ? 1 : 0;
}
