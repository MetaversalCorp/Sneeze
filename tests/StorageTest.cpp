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
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>

using namespace SNEEZE;

static int nPassed = 0;
static int nFailed = 0;

static void Check (bool bCondition, const char* szName)
{
   if (bCondition)
   {
      std::printf ("  PASS: %s\n", szName);
      nPassed++;
   }
   else
   {
      std::printf ("  FAIL: %s\n", szName);
      nFailed++;
   }
}

// ---------------------------------------------------------------------------
// Minimal ISNEEZE for tests
// ---------------------------------------------------------------------------

class STORAGE_TEST_HOST : public SNEEZE::IENGINE
{
public:
   std::string m_sAppDataPath;
   std::string m_sSessionPath;
   std::string m_sRenderer;

   STORAGE_TEST_HOST ()
   {
      m_sAppDataPath = (std::filesystem::path (std::getenv ("APPDATA")) / "Metaversal" / "Sneeze" / "Test").string ();
      m_sSessionPath = m_sAppDataPath;
   }

   std::string const& sAppDataPath () const& override { return m_sAppDataPath; }
   std::string const& sRenderer ()    const& override { return m_sRenderer; }

   void Log (eLOGLEVEL, const std::string& sModule, const std::string& sMessage) override
   {
      std::printf ("    [%s] %s\n", sModule.c_str (), sMessage.c_str ());
   }
};

class STORAGE_TEST_CONTEXT_HOST : public ICONTEXT
{
public:
   int m_nCreatedCount = 0;
   int m_nChangedCount = 0;
   int m_nDeletedCount = 0;

   void OnStorageSiloCreated (SILO*) override { m_nCreatedCount++; }
   void OnStorageSiloDeleted (SILO*) override { m_nDeletedCount++; }
   void OnStorageUnitChanged (SILO*, eSILO_SCOPE, const std::string&) override { m_nChangedCount++; }
};

static STORAGE_TEST_HOST*          s_pHost        = nullptr;
static STORAGE_TEST_CONTEXT_HOST*  s_pContextHost = nullptr;
static ENGINE*                     s_pSneeze      = nullptr;
static STORAGE*                    s_pStorage     = nullptr;
static CONTEXT*                    s_pContext     = nullptr;

static CONTAINER* MakeTestContainer (const std::string& sContainer = "poker")
{
   CONTAINER::CID CID;
   CID.sFingerprint       = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
   CID.sOrganization      = "TestOrg";
   CID.sOrganizationHash  = "abcdef012345";
   CID.sContainer         = sContainer;
   CID.sPersonaHash       = "persona_hash_test";
   CID.eTrust             = kTRUST_VERIFIED;
   return new CONTAINER (s_pContext, &CID);
}

static void CleanTestDir ()
{
   std::error_code ec;
   std::filesystem::remove_all (std::filesystem::path (std::getenv ("APPDATA")) / "Metaversal" / "Sneeze" / "Test", ec);
}

// ---------------------------------------------------------------------------
// Test 1: Initialize and Open/Close
// ---------------------------------------------------------------------------

static void TestInitializeAndOpenClose ()
{
   std::printf ("\n[Test 1] Initialize and Open/Close\n");

   STORAGE* pStorage = s_pStorage;
   Check (pStorage != nullptr, "Storage exists");

   auto* pContainer = MakeTestContainer ();
   SILO* pSilo = pStorage->Silo_Open (pContainer);
   Check (pSilo != nullptr, "Open returns SILO");
   Check (pSilo->DisplayName () == pContainer->Identity ()->DisplayName (), "SILO holds correct CID");
   Check (pSilo != nullptr, "Silo opened successfully");

   pSilo->Attach ();
   Check (pSilo->DisplayName () == pContainer->Identity ()->DisplayName (), "CID preserved after Attach");

   Check (s_pContextHost->m_nCreatedCount >= 1, "OnStorageSiloCreated fired");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 2: Basic Set/Get/Has/Remove
// ---------------------------------------------------------------------------

static void TestBasicOperations ()
{
   std::printf ("\n[Test 2] Basic Set/Get/Has/Remove\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ();
   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();

   s_pContextHost->m_nChangedCount = 0;

   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name", "Dean");
   Check (s_pContextHost->m_nChangedCount == 1, "OnStorageUnitChanged fired on Set");

   auto jValue = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name");
   Check (jValue.is_string (), "Get returns string type");
   Check (jValue.get<std::string> () == "Dean", "Get returns correct value");

   Check (pSilo->Has (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name"), "Has returns true for existing key");
   Check (!pSilo->Has (kSILO_SCOPE_PERMANENT_CONTAINER, "player.missing"), "Has returns false for missing key");

   pSilo->Remove (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name");
   Check (!pSilo->Has (kSILO_SCOPE_PERMANENT_CONTAINER, "player.name"), "Remove deletes key");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 3: Nested path navigation
// ---------------------------------------------------------------------------

static void TestPathNavigation ()
{
   std::printf ("\n[Test 3] Nested path navigation\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ();
   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();

   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "game.poker.table.color", "green");
   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "game.poker.table.seats", 8);

   auto jColor = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "game.poker.table.color");
   Check (jColor.is_string ()  &&  jColor.get<std::string> () == "green", "Deep nested string");

   auto jSeats = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "game.poker.table.seats");
   Check (jSeats.is_number_integer ()  &&  jSeats.get<int> () == 8, "Deep nested number");

   Check (pSilo->Has (kSILO_SCOPE_PERMANENT_CONTAINER, "game.poker.table.color"), "Has works for deep path");
   Check (!pSilo->Has (kSILO_SCOPE_PERMANENT_CONTAINER, "game.poker.table.missing"), "Has fails for missing deep path");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 4: Array index access
// ---------------------------------------------------------------------------

static void TestArrayAccess ()
{
   std::printf ("\n[Test 4] Array index access\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ();
   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();

   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "scores[0]", 100);
   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "scores[1]", 200);
   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "scores[2]", 300);

   auto j0 = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "scores[0]");
   auto j1 = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "scores[1]");
   auto j2 = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "scores[2]");

   Check (j0.is_number ()  &&  j0.get<int> () == 100, "Array index 0");
   Check (j1.is_number ()  &&  j1.get<int> () == 200, "Array index 1");
   Check (j2.is_number ()  &&  j2.get<int> () == 300, "Array index 2");

   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "game.players[0].name", "Alice");
   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "game.players[1].name", "Bob");

   auto jAlice = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "game.players[0].name");
   auto jBob   = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "game.players[1].name");

   Check (jAlice.is_string ()  &&  jAlice.get<std::string> () == "Alice", "Nested array object [0]");
   Check (jBob.is_string ()  &&  jBob.get<std::string> () == "Bob", "Nested array object [1]");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 5: Persistence across Open/Close cycles
// ---------------------------------------------------------------------------

static void TestPersistence ()
{
   std::printf ("\n[Test 5] Persistence across Open/Close\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ("persist-test");

   {
      SILO* pSilo = pStorage->Silo_Open (pContainer);
      pSilo->Attach ();
      pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "saved.value", 42);
      pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "saved.text", "hello");
      pSilo->Detach ();
      pStorage->Silo_Close (pContainer, pSilo);
   }

   {
      SILO* pSilo = pStorage->Silo_Open (pContainer);
      pSilo->Attach ();
      auto jValue = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "saved.value");
      auto jText  = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "saved.text");

      Check (jValue.is_number ()  &&  jValue.get<int> () == 42, "Number persisted across Open/Close");
      Check (jText.is_string ()  &&  jText.get<std::string> () == "hello", "String persisted across Open/Close");

      pSilo->Detach ();
      pStorage->Silo_Close (pContainer, pSilo);
   }

   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 6: Organization storage shared across containers
// ---------------------------------------------------------------------------

static void TestOrgSharing ()
{
   std::printf ("\n[Test 6] Organization storage shared across containers\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainerA = MakeTestContainer ("container-a");
   auto* pContainerB = MakeTestContainer ("container-b");

   SILO* pSiloA = pStorage->Silo_Open (pContainerA);
   pSiloA->Attach ();
   pSiloA->Set (kSILO_SCOPE_PERMANENT_ORG, "org.setting", "shared-value");

   SILO* pSiloB = pStorage->Silo_Open (pContainerB);
   pSiloB->Attach ();
   auto jOrgB = pSiloB->Get (kSILO_SCOPE_PERMANENT_ORG, "org.setting");

   Check (jOrgB.is_string ()  &&  jOrgB.get<std::string> () == "shared-value",
      "Org storage visible to second container");


   pSiloA->Detach ();
   pStorage->Silo_Close (pContainerA, pSiloA);
   pSiloB->Detach ();
   pStorage->Silo_Close (pContainerB, pSiloB);
   delete pContainerA;
   delete pContainerB;
}

// ---------------------------------------------------------------------------
// Test 7: Scope isolation
// ---------------------------------------------------------------------------

static void TestScopeIsolation ()
{
   std::printf ("\n[Test 7] Scope isolation\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ("scope-test");
   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();

   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "key", "permanent");
   pSilo->Set (kSILO_SCOPE_TEMPORARY_CONTAINER, "key", "temporary");
   pSilo->Set (kSILO_SCOPE_PERMANENT_ORG, "key", "org-permanent");
   pSilo->Set (kSILO_SCOPE_TEMPORARY_ORG, "key", "org-temporary");

   auto j1 = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "key");
   auto j2 = pSilo->Get (kSILO_SCOPE_TEMPORARY_CONTAINER, "key");
   auto j3 = pSilo->Get (kSILO_SCOPE_PERMANENT_ORG, "key");
   auto j4 = pSilo->Get (kSILO_SCOPE_TEMPORARY_ORG, "key");

   Check (j1.get<std::string> () == "permanent", "PERMANENT_COMPANY isolated");
   Check (j2.get<std::string> () == "temporary", "TEMPORARY_COMPANY isolated");
   Check (j3.get<std::string> () == "org-permanent", "PERMANENT_ORG isolated");
   Check (j4.get<std::string> () == "org-temporary", "TEMPORARY_ORG isolated");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 8: JSONL crash recovery
// ---------------------------------------------------------------------------

static void TestCrashRecovery ()
{
   std::printf ("\n[Test 8] JSONL crash recovery\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ("crash-test");
   const CONTAINER::CID* pCID = pContainer->Identity ();

   // Compute the actual path that STORAGE will use (company scope: under the container)
   std::filesystem::path sPath = std::filesystem::path (s_pContext->Path_Permanent ()) / pCID->Key_All () / "Storage";

   std::filesystem::create_directories (sPath);

   std::filesystem::path sPathname_Json = sPath / "container.json";
   std::filesystem::path sPathname_Log  = sPath / "container.log";

   // Write a base JSON file
   {
      std::ofstream f (sPathname_Json, std::ios::trunc);
      f << "{\"base\": 1, \"will-remove\": true}";
   }

   // Write a simulated .log file (as if we crashed mid-session)
   {
      std::ofstream f (sPathname_Log, std::ios::trunc);
      f << "[\"Set\",\"recovered\",\"yes\"]\n";
      f << "[\"Set\",\"base\",99]\n";
      f << "[\"Remove\",\"will-remove\"]\n";
   }

   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();

   auto jRecovered = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "recovered");
   auto jBase      = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "base");
   auto jRemoved   = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "will-remove");

   Check (jRecovered.is_string ()  &&  jRecovered.get<std::string> () == "yes",
      "Log Set replayed on recovery");
   Check (jBase.is_number ()  &&  jBase.get<int> () == 99,
      "Log Set overwrites base value");
   Check (jRemoved.is_null (),
      "Log Remove replayed on recovery");

   Check (!std::filesystem::exists (sPathname_Log), "Log file deleted after recovery");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 9: Bulk JSON get/set
// ---------------------------------------------------------------------------

static void TestBulkJson ()
{
   std::printf ("\n[Test 9] Bulk JSON get/set\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ("bulk-test");
   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();

   pSilo->Json (kSILO_SCOPE_PERMANENT_CONTAINER,
      "{\"bulk\": {\"a\": 1, \"b\": 2}, \"list\": [10, 20, 30]}");

   auto jA = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "bulk.a");
   auto jB = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "bulk.b");
   auto jList1 = pSilo->Get (kSILO_SCOPE_PERMANENT_CONTAINER, "list[1]");

   Check (jA.is_number ()  &&  jA.get<int> () == 1, "Bulk set: nested value a");
   Check (jB.is_number ()  &&  jB.get<int> () == 2, "Bulk set: nested value b");
   Check (jList1.is_number ()  &&  jList1.get<int> () == 20, "Bulk set: array access");

   std::string sJson = pSilo->Json (kSILO_SCOPE_PERMANENT_CONTAINER);
   Check (sJson.find ("\"bulk\"") != std::string::npos, "Json contains data");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);
   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 10: Meta sidecar written on Close
// ---------------------------------------------------------------------------

static void TestMetaSidecar ()
{
   std::printf ("\n[Test 10] Meta sidecar\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainer = MakeTestContainer ("meta-test");

   SILO* pSilo = pStorage->Silo_Open (pContainer);
   pSilo->Attach ();
   pSilo->Set (kSILO_SCOPE_PERMANENT_CONTAINER, "data", "value");

   std::string sPathname_Meta = pSilo->Pathname (kSILO_SCOPE_PERMANENT_CONTAINER, "meta");

   pSilo->Detach ();
   pStorage->Silo_Close (pContainer, pSilo);

   Check (std::filesystem::exists (sPathname_Meta), "Meta sidecar file created on Close");

   if (std::filesystem::exists (std::filesystem::path (sPathname_Meta)))
   {
      std::ifstream f (sPathname_Meta);
      nlohmann::json jMeta = nlohmann::json::parse (f);

      Check (jMeta.value ("container", "") == "meta-test", "Meta contains container");
      Check (jMeta.value ("organization", "") == "TestOrg", "Meta contains organization");
      Check (jMeta.contains ("createdAt"), "Meta contains createdAt");
      Check (jMeta.contains ("sizeBytes"), "Meta contains sizeBytes");
   }

   delete pContainer;
}

// ---------------------------------------------------------------------------
// Test 11: Change notification fans out to all silos holding a shared UNIT
// ---------------------------------------------------------------------------

static void TestChangeFanOut ()
{
   std::printf ("\n[Test 11] Change notification fan-out\n");

   STORAGE* pStorage = s_pStorage;
   auto* pContainerA = MakeTestContainer ("fanout-a");
   auto* pContainerB = MakeTestContainer ("fanout-b");

   SILO* pSiloA = pStorage->Silo_Open (pContainerA);
   pSiloA->Attach ();
   SILO* pSiloB = pStorage->Silo_Open (pContainerB);
   pSiloB->Attach ();

   s_pContextHost->m_nChangedCount = 0;

   pSiloA->Set (kSILO_SCOPE_PERMANENT_ORG, "org.broadcast", "ping");

   Check (s_pContextHost->m_nChangedCount == 2, "One write to a shared org UNIT notifies both holding silos");

   pSiloA->Detach ();
   pStorage->Silo_Close (pContainerA, pSiloA);
   pSiloB->Detach ();
   pStorage->Silo_Close (pContainerB, pSiloB);
   delete pContainerA;
   delete pContainerB;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int RunStorageTests (int nArgc, char** aArgv)
{
   (void) nArgc; (void) aArgv;

   nPassed = 0;
   nFailed = 0;

   std::printf ("=== Storage Test Suite ===\n");

   CleanTestDir ();

   s_pHost = new STORAGE_TEST_HOST ();
   s_pSneeze = new ENGINE (s_pHost);
   s_pContextHost = new STORAGE_TEST_CONTEXT_HOST ();

   bool bEngineInit = s_pSneeze->Initialize ();
   Check (bEngineInit, "Engine initialized");

   s_pContext = s_pSneeze->Context_Open (s_pContextHost);
   Check (s_pContext != nullptr, "Context opened");

   s_pStorage = s_pContext->Storage ();
   Check (s_pStorage != nullptr, "Context storage exists");

   bool bRun = bEngineInit  &&  s_pContext  &&  s_pStorage;

   if (bRun)
   {
      TestInitializeAndOpenClose ();
      TestBasicOperations ();
      TestPathNavigation ();
      TestArrayAccess ();
      TestPersistence ();
      TestOrgSharing ();
      TestScopeIsolation ();
      TestCrashRecovery ();
      TestBulkJson ();
      TestMetaSidecar ();
      TestChangeFanOut ();
   }

   s_pStorage = nullptr;
   s_pSneeze->Context_Close (s_pContext);
   s_pContext = nullptr;
   delete s_pSneeze;
   s_pSneeze = nullptr;
   delete s_pContextHost;
   s_pContextHost = nullptr;
   delete s_pHost;
   s_pHost = nullptr;

   CleanTestDir ();

   std::printf ("\n  --- Results: %d passed, %d failed ---\n", nPassed, nFailed);
   return (nFailed > 0) ? 1 : 0;
}
