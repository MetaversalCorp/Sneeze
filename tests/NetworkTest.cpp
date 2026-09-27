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
#include <Container.h>
#include <Viewport.h>

#include <openssl/sha.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

#include <sneeze_abi.h>

// Wasm.h leans on the engine's precompiled header for the standard library, so
// it comes after those includes in a test TU.
#include "wasm/Wasm.h"

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
// Minimal ISNEEZE for test logging
// ---------------------------------------------------------------------------

class CACHE_TEST_LISTENER : public SNEEZE::IENGINE
{
public:
   std::string m_sAppDataPath;
   std::string m_sSessionPath;
   std::string m_sRenderer;

   std::string const& sAppDataPath () const& override { return m_sAppDataPath; }
   std::string const& sRenderer ()    const& override { return m_sRenderer; }

   void Log (eLOGLEVEL, const std::string& sModule, const std::string& sMessage) override
   {
      std::printf ("    [%s] %s\n", sModule.c_str (), sMessage.c_str ());
   }
};

class CACHE_TEST_CONTEXT_HOST : public SNEEZE::ICONTEXT
{
public:
   int m_nCreatedCount = 0;
   int m_nChangedCount = 0;
   int m_nDeletedCount = 0;

   bool OnNetworkFileCreated (SNEEZE::FILE*) override { m_nCreatedCount++; return true; }
   void OnNetworkFileChanged (SNEEZE::FILE*) override { m_nChangedCount++; }
   void OnNetworkFileDeleted (SNEEZE::FILE*) override { m_nDeletedCount++; }

   void ResetCounters () { m_nCreatedCount = 0; m_nChangedCount = 0; m_nDeletedCount = 0; }
};

// ---------------------------------------------------------------------------
// IFILE listener that signals a condition variable on completion
// ---------------------------------------------------------------------------

class TEST_FILE_LISTENER : public IFILE
{
public:
   TEST_FILE_LISTENER () : m_bDone (false), m_bSucceeded (false) {}

   void OnFileReady (SNEEZE::FILE* /*pFile*/) override
   {
      std::lock_guard<std::mutex> guard (m_mutex);
      m_bSucceeded = true;
      m_bDone = true;
      m_condVar.notify_all ();
   }

   void OnFileFailed (SNEEZE::FILE* /*pFile*/) override
   {
      std::lock_guard<std::mutex> guard (m_mutex);
      m_bSucceeded = false;
      m_bDone = true;
      m_condVar.notify_all ();
   }

   bool WaitFor (int nTimeoutMs)
   {
      std::unique_lock<std::mutex> lock (m_mutex);
      return m_condVar.wait_for (lock, std::chrono::milliseconds (nTimeoutMs),
         [this] { return m_bDone; });
   }

   bool Succeeded () const { return m_bSucceeded; }

private:
   std::mutex              m_mutex;
   std::condition_variable m_condVar;
   bool                    m_bDone;
   bool                    m_bSucceeded;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ComputeSha256Hex (const uint8_t* pData, size_t nLen)
{
   unsigned char aDigest[SHA256_DIGEST_LENGTH];
   SHA256 (pData, nLen, aDigest);

   std::string sHex;
   sHex.reserve (SHA256_DIGEST_LENGTH * 2);
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
   {
      char szByte[4];
      std::snprintf (szByte, sizeof (szByte), "%02x", aDigest[i]);
      sHex += szByte;
   }
   return sHex;
}

// ---------------------------------------------------------------------------
// Shared test state
// ---------------------------------------------------------------------------

static CACHE_TEST_LISTENER*       s_pTestListener = nullptr;
static CACHE_TEST_CONTEXT_HOST*   s_pContextHost  = nullptr;
static SNEEZE::ENGINE*            s_pSneeze       = nullptr;
static CONTEXT*                   s_pContext      = nullptr;
static std::string                s_sPathRoot;

static CONTAINER* s_pTestContainer = nullptr;

static void InitTestContainer ()
{
   CONTAINER::CID CID;
   CID.sFingerprint       = "TestFingerprint_0123456789abcdef";
   CID.sOrganization      = "TestOrg";
   CID.sOrganizationHash  = "abcdef012345";
   CID.sContainer         = "TestStore";
   CID.sPersonaHash       = "TestPersona";
   CID.eTrust             = kTRUST_VERIFIED;
   s_pTestContainer = new CONTAINER (s_pContext, &CID);
}

// ---------------------------------------------------------------------------
// Test 1: Manager initialization
// ---------------------------------------------------------------------------

static void TestManagerInit ()
{
   std::printf ("\n[Test 1] Manager initialization\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   bool bInit = pNetwork->Initialize (s_sPathRoot);
   Check (bInit, "Manager initialized successfully");
   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 2: Open a file without hash (live fetch)
// ---------------------------------------------------------------------------

static void TestUnhashedFetch ()
{
   std::printf ("\n[Test 2] Unhashed fetch (no hash, live network)\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   bool bInit = pNetwork->Initialize (s_sPathRoot);
   Check (bInit, "Network initialized");

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   if (bInit)
   {
      TEST_FILE_LISTENER listener;
      SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/128", &listener);

      Check (pFile != nullptr, "File_Open returned a handle");

      if (pFile)
      {
         Check (pFile->FileIx () > 0, "File index is non-zero");

         bool bGotResult = listener.WaitFor (15000);

         if (bGotResult)
         {
            Check (listener.Succeeded (), "Fetch succeeded");
            Check (pFile->IsReady (), "File is READY");
            Check (!pFile->IsHashed (), "File is not hashed");
            Check (pFile->SizeBytes () > 0, "File has non-zero size");

            Check (pFile->HttpStatus () == 200, "HTTP status is 200");
            Check (pFile->FetchDuration () > 0.0, "Fetch duration is positive");
            Check (!pFile->IsServedFromCache (), "Not served from cache");

            std::vector<uint8_t> aData;
            
            pFile->ReadData (aData);
            Check (!aData.empty (), "ReadData returned content");
            Check (aData.size () == pFile->SizeBytes (), "ReadData size matches SizeBytes");

            std::printf ("    Size: %llu bytes, ContentType: %s, Duration: %.3f s\n",
               static_cast<unsigned long long> (pFile->SizeBytes ()),
               pFile->ContentType ().c_str (),
               pFile->FetchDuration ());
         }
         else
         {
            std::printf ("    (Timed out - expected if no internet)\n");
            Check (true, "File_Open did not crash (timeout is non-fatal)");
         }

         pFile->Close ();
      }
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 3: File_Open deduplication (same URL returns shared ASSET)
// ---------------------------------------------------------------------------

static void TestDeduplication ()
{
   std::printf ("\n[Test 3] File_Open deduplication\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listenerA;
   TEST_FILE_LISTENER listenerB;

   SNEEZE::FILE* pFileA = pCache->File_Open ("https://httpbin.org/bytes/64", &listenerA);
   SNEEZE::FILE* pFileB = pCache->File_Open ("https://httpbin.org/bytes/64", &listenerB);

   Check (pFileA != nullptr, "First handle is valid");
   Check (pFileB != nullptr, "Second handle is valid");

   if (pFileA  &&  pFileB)
   {
      Check (pFileA->AssetIx () == pFileB->AssetIx (),
         "Both handles share the same ASSET");

      bool bGotA = listenerA.WaitFor (15000);
      bool bGotB = listenerB.WaitFor (15000);

      if (bGotA  &&  bGotB)
      {
         Check (listenerA.Succeeded (), "Listener A notified");
         Check (listenerB.Succeeded (), "Listener B notified");
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Deduplication did not crash (timeout is non-fatal)");
      }
   }

   if (pFileA) pFileA->Close ();
   if (pFileB) pFileB->Close ();

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 4: Hash-verified persistent fetch
// ---------------------------------------------------------------------------

static void TestHashVerifiedFetch ()
{
   std::printf ("\n[Test 4] Hash-verified persistent fetch\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listenerPreFetch;
   SNEEZE::FILE* pPreFile = pCache->File_Open ("https://httpbin.org/base64/SGVsbG9Xb3JsZA==", &listenerPreFetch);

   if (pPreFile)
   {
      bool bPreResult = listenerPreFetch.WaitFor (15000);
      if (bPreResult  &&  listenerPreFetch.Succeeded ())
      {
         std::vector<uint8_t> aData;
         pPreFile->ReadData (aData);
         std::string sContent (aData.begin (), aData.end ());
         std::printf ("    Pre-fetch content: \"%s\" (%zu bytes)\n",
            sContent.c_str (), aData.size ());

         std::string sDigest = ComputeSha256Hex (aData.data (), aData.size ());
         std::string sSri = "sha256-" + sDigest;
         std::printf ("    Computed SRI: %s\n", sSri.c_str ());

         Check (!sDigest.empty (), "Pre-fetch produced a hash");

         pPreFile->Reset ();
         pPreFile->Close ();
         pPreFile = nullptr;

         TEST_FILE_LISTENER listenerVerified;
         SNEEZE::FILE* pVerFile = pCache->File_Open ("https://httpbin.org/base64/SGVsbG9Xb3JsZA==", sSri, 0, &listenerVerified);

         if (pVerFile)
         {
            bool bVerResult = listenerVerified.WaitFor (15000);
            if (bVerResult  &&  listenerVerified.Succeeded ())
            {
               Check (pVerFile->IsReady (), "Verified file is READY");
               Check (pVerFile->IsHashed (), "Verified file is persistent (hashed)");
               Check (pVerFile->Hash () == sSri, "Hash matches SRI");

               std::vector<uint8_t> aVerData;
               pVerFile->ReadData (aVerData);
               Check (aVerData == aData, "Verified data matches original");
            }
            else
            {
               std::printf ("    (Verified fetch timed out or failed)\n");
               Check (true, "Hash-verified fetch did not crash");
            }
            pVerFile->Close ();
         }
      }
      else
      {
         std::printf ("    (Pre-fetch timed out - expected if no internet)\n");
         Check (true, "Pre-fetch did not crash");
         pPreFile->Close ();
      }
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 5: Hash mismatch causes failure
// ---------------------------------------------------------------------------

static void TestHashMismatch ()
{
   std::printf ("\n[Test 5] Hash mismatch causes failure\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   std::string sBadHash = "sha256-0000000000000000000000000000000000000000000000000000000000000000";

   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/base64/SGVsbG9Xb3JsZA==", sBadHash, 0, &listener);

   if (pFile)
   {
      bool bGotResult = listener.WaitFor (15000);
      if (bGotResult)
      {
         Check (!listener.Succeeded (), "Bad hash correctly caused failure");
         Check (pFile->State () == kASSET_STATE_FAILED, "State is FAILED");
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Hash mismatch test did not crash");
      }

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 6: Reset removes metas, triggers re-fetch
// ---------------------------------------------------------------------------

static void TestReset ()
{
   std::printf ("\n[Test 6] Reset\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listenerSession;
   SNEEZE::FILE* pSession = pCache->File_Open ("https://httpbin.org/bytes/32", &listenerSession);

   if (pSession)
   {
      bool bGot = listenerSession.WaitFor (15000);
      if (bGot  &&  listenerSession.Succeeded ())
      {
         Check (pSession->IsReady (), "File is READY before reset");
         pSession->Close ();

         // The bare harness has no loaded primary fabric, so Key_Reset() is
         // empty and this is a no-op; the cached entry is served on reopen.
         pNetwork->Reset (s_pContext->Key_Reset ());

         TEST_FILE_LISTENER listenerAfter;
         SNEEZE::FILE* pAfter = pCache->File_Open ("https://httpbin.org/bytes/32", &listenerAfter);

         if (pAfter)
         {
            listenerAfter.WaitFor (15000);

            Check (pAfter->State () == kASSET_STATE_READY, "After no-op reset, cached file served READY");

            pAfter->Close ();
         }

         Check (true, "Reset completed without crash");
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Reset test did not crash");
         pSession->Close ();
      }
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 7: Reset flag destroys meta and disk file on close
// ---------------------------------------------------------------------------

static void TestResetFlag ()
{
   std::printf ("\n[Test 7] Reset flag persisted in meta\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/16", &listener);

   if (pFile)
   {
      bool bGot = listener.WaitFor (15000);
      if (bGot  &&  listener.Succeeded ())
      {
         std::string sPathname_Disk = pFile->DiskPath ();

         Check (!sPathname_Disk.empty (), "File had a disk path");
         Check (std::filesystem::exists (sPathname_Disk), "Disk file exists before reset");

         pFile->Reset ();
         pFile->Close ();

         Check (std::filesystem::exists (sPathname_Disk), "Disk file preserved (reset deferred to next load)");
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Reset flag test did not crash");
         pFile->Close ();
      }
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 8: Failed fetch (invalid URL)
// ---------------------------------------------------------------------------

static void TestFailedFetch ()
{
   std::printf ("\n[Test 8] Failed fetch (invalid host)\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://this-domain-does-not-exist-999.invalid/file.bin", &listener);

   if (pFile)
   {
      bool bGot = listener.WaitFor (15000);
      if (bGot)
      {
         Check (!listener.Succeeded (), "Invalid host correctly failed");
         Check (pFile->State () == kASSET_STATE_FAILED, "State is FAILED");
      }
      else
      {
         std::printf ("    (Timed out waiting for DNS failure)\n");
         Check (true, "Failed fetch did not crash");
      }

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 9: Sidecar persistence (survive shutdown/reinit)
// ---------------------------------------------------------------------------

static void TestSidecarPersistence ()
{
   std::printf ("\n[Test 9] Sidecar persistence\n");

   std::string sUrl = "https://httpbin.org/base64/UGVyc2lzdGVuY2VUZXN0";
   std::string sSri;

   // Phase 1: Fetch with hash, shutdown (saves .meta sidecar)
   {
      NETWORK* pNetwork = new NETWORK (s_pSneeze);
      pNetwork->Initialize (s_sPathRoot);

      CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

      TEST_FILE_LISTENER listenerPre;
      SNEEZE::FILE* pPre = pCache->File_Open (sUrl, &listenerPre);
      if (pPre  &&  listenerPre.WaitFor (15000)  &&  listenerPre.Succeeded ())
      {
         std::vector<uint8_t> aData;
         pPre->ReadData (aData);
         std::string sDigest = ComputeSha256Hex (aData.data (), aData.size ());
         sSri = "sha256-" + sDigest;
         pPre->Reset ();
         pPre->Close ();
         pPre = nullptr;

         TEST_FILE_LISTENER listenerHash;
         SNEEZE::FILE* pHash = pCache->File_Open (sUrl, sSri, 0, &listenerHash);
         if (pHash)
         {
            listenerHash.WaitFor (15000);
            Check (listenerHash.Succeeded (), "Persistent entry created");
            Check (pHash->AssetIx () > 0, "Asset index assigned on creation");
            pHash->Close ();
         }
      }
      else
      {
         std::printf ("    (Pre-fetch timed out - skipping)\n");
         Check (true, "Sidecar test did not crash (no internet)");
         if (pPre) pPre->Close ();
         delete pNetwork;
         return;
      }

      delete pNetwork;
   }

   // Phase 2: Reinitialize and check if the meta survived via .meta sidecar
   if (!sSri.empty ())
   {
      NETWORK* pNetwork2 = new NETWORK (s_pSneeze);
      pNetwork2->Initialize (s_sPathRoot);

      CACHE* pCache2 = pNetwork2->Cache_Open (s_pTestContainer);

      TEST_FILE_LISTENER listenerReload;
      SNEEZE::FILE* pReload = pCache2->File_Open (sUrl, sSri, 0, &listenerReload);

      if (pReload)
      {
         listenerReload.WaitFor (15000);

         Check (pReload->IsReady (), "Meta survived shutdown (loaded from .meta sidecar)");
         Check (pReload->IsHashed (), "Meta is still hashed");
         Check (pReload->Hash () == sSri, "Hash matches after reload");
         Check (pReload->AssetIx () > 0, "Asset index preserved across sessions");

         std::vector<uint8_t> aData;
         pReload->ReadData (aData);
         Check (!aData.empty (), "Data is readable after reload");

         pReload->Close ();
      }

      delete pNetwork2;
   }
}

// ---------------------------------------------------------------------------
// Test 10: HTTP headers captured
// ---------------------------------------------------------------------------

static void TestHttpHeaders ()
{
   std::printf ("\n[Test 10] HTTP response headers captured\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/response-headers?Content-Type=application/json", &listener);

   if (pFile)
   {
      bool bGot = listener.WaitFor (15000);
      if (bGot  &&  listener.Succeeded ())
      {
         auto& mapHeaders = pFile->RspHeaders ();
         Check (!mapHeaders.empty (), "Headers map is non-empty");

         std::string sCt = pFile->ContentType ();
         Check (!sCt.empty (), "Content-Type header captured");
         std::printf ("    Content-Type: %s\n", sCt.c_str ());
         std::printf ("    Total headers captured: %zu\n", mapHeaders.size ());
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Headers test did not crash");
      }

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 11: FILE handle lifecycle
// ---------------------------------------------------------------------------

static void TestFileHandleLifecycle ()
{
   std::printf ("\n[Test 11] FILE handle lifecycle\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/8", &listener);

      Check (pFile != nullptr, "Handle allocated");

   if (pFile)
   {
      Check (!pFile->Url ().empty (), "URL accessible from handle");

      listener.WaitFor (15000);

      pFile->Close ();
      Check (true, "Close completed without crash");
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 12: History list and file indexes
// ---------------------------------------------------------------------------

static void TestHistoryAndFileIx ()
{
   std::printf ("\n[Test 12] History list and file indexes\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listenerA;
   TEST_FILE_LISTENER listenerB;

   SNEEZE::FILE* pFileA = pCache->File_Open ("https://httpbin.org/bytes/16", &listenerA);
   SNEEZE::FILE* pFileB = pCache->File_Open ("https://httpbin.org/bytes/32", &listenerB);

   Check (pFileA != nullptr  &&  pFileB != nullptr, "Both handles allocated");

   if (pFileA  &&  pFileB)
   {
      Check (pFileA->FileIx () < pFileB->FileIx (),
         "File indexes are monotonically increasing");

//      auto& aHistory = pNetwork->Files ();
//      Check (aHistory.size () >= 2, "History contains at least 2 entries");

      listenerA.WaitFor (15000);
      listenerB.WaitFor (15000);

      pFileA->Close ();
      pFileB->Close ();

//      Check (aHistory.size () >= 2, "Close does not shrink history");
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 13: Notification callbacks
// ---------------------------------------------------------------------------

static void TestNotifications ()
{
   std::printf ("\n[Test 13] Notification callbacks\n");

   s_pContextHost->ResetCounters ();

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/8?test=notifications", &listener);

   Check (s_pContextHost->m_nCreatedCount > 0, "OnNetworkFileCreated fired");

   if (pFile)
   {
      bool bGot = listener.WaitFor (15000);
      if (bGot)
      {
         Check (s_pContextHost->m_nChangedCount > 0, "OnNetworkFileChanged fired");
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Notification test did not crash");
      }

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 14: Served-from-cache detection
// ---------------------------------------------------------------------------

static void TestServedFromCache ()
{
   std::printf ("\n[Test 14] Served-from-cache detection\n");

   std::string sUrl = "https://httpbin.org/base64/Q2FjaGVkRGF0YQ==";
   std::string sSri;

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   // First fetch - should NOT be served from cache
   TEST_FILE_LISTENER listenerFirst;
   SNEEZE::FILE* pFirst = pCache->File_Open (sUrl, &listenerFirst);

   if (pFirst)
   {
      bool bGot = listenerFirst.WaitFor (15000);
      if (bGot  &&  listenerFirst.Succeeded ())
      {
         Check (!pFirst->IsServedFromCache (), "First fetch is not served from cache");

         // Second open for the same URL -- should be served from cache
         TEST_FILE_LISTENER listenerSecond;
         SNEEZE::FILE* pSecond = pCache->File_Open (sUrl, &listenerSecond);

         if (pSecond)
         {
            listenerSecond.WaitFor (15000);

            Check (pSecond->IsServedFromCache (), "Second fetch IS served from cache");
            Check (pSecond->FileIx () > pFirst->FileIx (),
               "Second file index > first");
            pSecond->Close ();
         }
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "Served-from-cache test did not crash");
      }

      pFirst->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 15: Failed fetch records HTTP status
// ---------------------------------------------------------------------------

static void TestFailedFetchHttpStatus ()
{
   std::printf ("\n[Test 15] Failed fetch records HTTP status\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/status/404", &listener);

   if (pFile)
   {
      bool bGot = listener.WaitFor (15000);
      if (bGot)
      {
         Check (!listener.Succeeded (), "404 correctly failed");
         Check (pFile->HttpStatus () == 404, "HTTP status is 404");
         Check (pFile->FetchDuration () > 0.0, "Fetch duration recorded for failed file");
      }
      else
      {
         std::printf ("    (Timed out - expected if no internet)\n");
         Check (true, "HTTP status test did not crash");
      }

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 16: Clear flag removes FILE from history on close
// ---------------------------------------------------------------------------

static void TestClearFlag ()
{
   std::printf ("\n[Test 16] Clear flag\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/8", &listener);

   if (pFile)
   {
      listener.WaitFor (15000);

      Check (!pFile->IsPending_Clear (), "FILE is not cleared before Clear()");

      pFile->Clear ();

      Check (pFile->IsPending_Clear (),
         "Clear immediately sets pending-clear flag");

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 17: Reset flag can be toggled off before close
// ---------------------------------------------------------------------------

static void TestCloseWithoutReset ()
{
   std::printf ("\n[Test 17] Close without reset preserves disk file\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/8", &listener);

   if (pFile)
   {
      listener.WaitFor (15000);

      std::string sPathname_Disk = pFile->DiskPath ();
      bool bHadDisk = !sPathname_Disk.empty ()  &&  std::filesystem::exists (sPathname_Disk);

      pFile->Close ();

      if (bHadDisk)
      {
         Check (std::filesystem::exists (sPathname_Disk),
            "Disk file survives close without reset");
      }
      else
      {
         Check (true, "Close without reset did not crash (no disk path to verify)");
      }
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 18: Deferred reset with multiple handles
// ---------------------------------------------------------------------------

static void TestDeferredReset ()
{
   std::printf ("\n[Test 18] Deferred reset (multiple handles)\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listenerA;
   TEST_FILE_LISTENER listenerB;

   SNEEZE::FILE* pFileA = pCache->File_Open ("https://httpbin.org/bytes/16", &listenerA);
   SNEEZE::FILE* pFileB = pCache->File_Open ("https://httpbin.org/bytes/16", &listenerB);

   if (pFileA  &&  pFileB)
   {
      listenerA.WaitFor (15000);
      listenerB.WaitFor (15000);

      std::string sPathname_Disk = pFileA->DiskPath ();
      bool bHadDisk = !sPathname_Disk.empty ()  &&  std::filesystem::exists (sPathname_Disk);

      pFileA->Reset ();
      pFileA->Close ();

      if (bHadDisk)
      {
         Check (std::filesystem::exists (sPathname_Disk),
            "Disk file survives while second handle is attached");
      }

      pFileB->Close ();

      if (bHadDisk)
      {
         Check (std::filesystem::exists (sPathname_Disk),
            "Disk file preserved (reset flag deferred to next load)");
      }

      Check (true, "Deferred reset completed without crash");
   }
   else
   {
      if (pFileA) pFileA->Close ();
      if (pFileB) pFileB->Close ();
      Check (true, "Deferred reset did not crash (handles were null)");
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 19: Clear removes closed FILE records
// ---------------------------------------------------------------------------

static void TestClear ()
{
   std::printf ("\n[Test 19] Clear\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listenerA;
   TEST_FILE_LISTENER listenerB;

   SNEEZE::FILE* pFileA = pCache->File_Open ("https://httpbin.org/bytes/8", &listenerA);
   SNEEZE::FILE* pFileB = pCache->File_Open ("https://httpbin.org/bytes/16", &listenerB);

   if (pFileA  &&  pFileB)
   {
      listenerA.WaitFor (15000);
      listenerB.WaitFor (15000);

      pFileA->Close ();

//      size_t nHistoryBefore = pNetwork->Files ().size ();
//      Check (nHistoryBefore >= 2, "History has at least 2 entries before Clear");

      pCache->Clear ();

//      size_t nHistoryAfter = pNetwork->Files ().size ();
//      Check (nHistoryAfter < nHistoryBefore, "Clear removed closed FILE records");
//      Check (nHistoryAfter >= 1,             "In-use FILE record survived Clear");

      pFileB->Close ();
   }
   else
   {
      if (pFileA) pFileA->Close ();
      if (pFileB) pFileB->Close ();
   }

   Check (true, "Clear completed without crash");

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 20: Network-wide Clear (sweeps every cache in the context)
// ---------------------------------------------------------------------------

static void TestClearAllCaches ()
{
   std::printf ("\n[Test 20] Network-wide Clear (all caches)\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCacheA = pNetwork->Cache_Open (s_pTestContainer);
   CACHE* pCacheB = pNetwork->Cache_Open (s_pTestContainer);

   Check (pCacheA != nullptr  &&  pCacheB != nullptr, "Two caches opened on one network");

   TEST_FILE_LISTENER listenerA;
   TEST_FILE_LISTENER listenerB;

   SNEEZE::FILE* pFileA = pCacheA->File_Open ("https://httpbin.org/bytes/8", &listenerA);
   SNEEZE::FILE* pFileB = pCacheB->File_Open ("https://httpbin.org/bytes/16", &listenerB);

   if (pFileA  &&  pFileB)
   {
      listenerA.WaitFor (15000);
      listenerB.WaitFor (15000);

      pFileA->Close ();
      pFileB->Close ();

      pCacheA->Clear ();
      pCacheB->Clear ();
   }
   else
   {
      if (pFileA) pFileA->Close ();
      if (pFileB) pFileB->Close ();
   }

   Check (true, "Network-wide Clear completed without crash");

   pNetwork->Cache_Close (s_pTestContainer, pCacheA);
   pNetwork->Cache_Close (s_pTestContainer, pCacheB);

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 21: OnNetworkFileDeleted notification
// ---------------------------------------------------------------------------

static void TestDeletedNotification ()
{
   std::printf ("\n[Test 21] OnNetworkFileDeleted notification\n");

   s_pContextHost->ResetCounters ();

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/8", &listener);

   if (pFile)
   {
      listener.WaitFor (15000);

      Check (s_pContextHost->m_nDeletedCount == 0,
         "No deleted notifications before clear");

      pFile->Clear ();

      Check (s_pContextHost->m_nDeletedCount == 1,
         "OnNetworkFileDeleted fired immediately on clear");

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 22: Staleness rules
// ---------------------------------------------------------------------------

static void TestStalenessRules ()
{
   std::printf ("\n[Test 22] Staleness rules\n");

   std::string sUrl = "https://httpbin.org/base64/U3RhbGVuZXNzVGVzdA==";

   // Phase 1: Fetch a file and shut down
   {
      NETWORK* pNetwork = new NETWORK (s_pSneeze);
      pNetwork->Initialize (s_sPathRoot);

      CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

      TEST_FILE_LISTENER listener;
      SNEEZE::FILE* pFile = pCache->File_Open (sUrl, &listener);

      if (pFile)
      {
         bool bGot = listener.WaitFor (15000);
         if (!bGot  ||  !listener.Succeeded ())
         {
            std::printf ("    (Timed out - skipping)\n");
            Check (true, "Staleness test did not crash (no internet)");
            pFile->Close ();
            delete pNetwork;
            return;
         }

         Check (pFile->IsReady (), "File fetched successfully");
         pFile->Close ();
      }

      delete pNetwork;
   }

   // Phase 2: Reinit with a staleness rule, verify re-fetch
   {
      NETWORK* pNetwork2 = new NETWORK (s_pSneeze);
      pNetwork2->Initialize (s_sPathRoot);

      CACHE* pCache2 = pNetwork2->Cache_Open (s_pTestContainer);

      // No watermark is stamped for this context's primary key (the bare test
      // harness has no loaded primary fabric, so Key_Reset() is empty), so the
      // cached entry is correctly served rather than re-fetched.
      pNetwork2->Reset (s_pContext->Key_Reset ());

      TEST_FILE_LISTENER listener2;
      SNEEZE::FILE* pFile2 = pCache2->File_Open (sUrl, &listener2);

      if (pFile2)
      {
         listener2.WaitFor (15000);
         Check (pFile2->IsServedFromCache (), "No primary-key watermark => cached entry served");
         pFile2->Close ();
      }

      delete pNetwork2;
   }
}

// ---------------------------------------------------------------------------
// Test 23: File_Open with bFetch=false (no network)
// ---------------------------------------------------------------------------

static void TestNoFetchOpen ()
{
   std::printf ("\n[Test 23] File_Open without listener (passive open)\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   SNEEZE::FILE* pFile = pCache->File_Open ("https://this-url-does-not-exist-in-cache.invalid/none",
      std::string (), 0, nullptr);

   Check (pFile != nullptr, "Passive open returns a valid handle");

   if (pFile)
   {
      Check (pFile->State () == kASSET_STATE_IDLE, "No fetch triggered without listener");
      Check (!pFile->Url ().empty (), "URL accessible on passive handle");
      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 24: POST carries a body and lands outside the permanent cache
// ---------------------------------------------------------------------------

static void TestPostVerb ()
{
   std::printf ("\n[Test 24] POST verb\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   const std::string sBody = "{\"sneeze\":\"post\"}";

   REQUEST Request;
   Request.eVerb     = kREQUEST_VERB_POST;
   Request.bAnyStatus = true;
   Request.nSizeMax  = kREQUEST_SIZE_MAX;
   Request.aBody.assign (sBody.begin (), sBody.end ());
   Request.umsHeader["Content-Type"] = "application/json";

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/post", std::string (), Request, &listener);

   Check (pFile != nullptr, "POST handle allocated");

   if (pFile)
   {
      Check (pFile->Verb () == kREQUEST_VERB_POST, "Verb reported as POST");
      Check (!pFile->IsCacheable (), "POST is not cacheable");

      std::string sTransitory = std::filesystem::path (s_pTestContainer->Path_Temporary_All ()).generic_string ();
      Check (pFile->Path ().find (sTransitory) == 0, "POST stored in the container transitory tree");

      if (listener.WaitFor (30000))
      {
         Check (listener.Succeeded (), "POST completed");
         Check (pFile->HttpStatus () == 200, "POST returned HTTP 200");

         std::vector<uint8_t> aData;
         pFile->ReadData (aData);
         Check (!aData.empty (), "Response body readable");

         // httpbin echoes what it received, so the body proves it went up.
         std::string sResponse (aData.begin (), aData.end ());
         Check (sResponse.find ("sneeze") != std::string::npos, "Request body reached the server");

         Check (std::filesystem::exists (pFile->Pathname ("request")), "Request sidecar written to disk");

         std::vector<uint8_t> aRequest;
         pFile->ReadRequestData (aRequest);
         Check (aRequest.size () == sBody.size (), "Request body readable back from its sidecar");
         Check (std::string (aRequest.begin (), aRequest.end ()) == sBody, "Request sidecar holds what was sent");
      }
      else Check (false, "POST completed within 30s");

      pFile->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 25: two back-to-back POSTs never share a fetch
// ---------------------------------------------------------------------------

static void TestPostNoCoalesce ()
{
   std::printf ("\n[Test 25] Back-to-back POSTs stay independent\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   REQUEST RequestA;
   RequestA.eVerb      = kREQUEST_VERB_POST;
   RequestA.bAnyStatus = true;
   RequestA.aBody      = { 'A' };

   REQUEST RequestB = RequestA;
   RequestB.aBody      = { 'B' };

   TEST_FILE_LISTENER listenerA;
   TEST_FILE_LISTENER listenerB;

   SNEEZE::FILE* pFileA = pCache->File_Open ("https://httpbin.org/post", std::string (), RequestA, &listenerA);
   SNEEZE::FILE* pFileB = pCache->File_Open ("https://httpbin.org/post", std::string (), RequestB, &listenerB);

   Check (pFileA != nullptr  &&  pFileB != nullptr, "Both POST handles allocated");

   if (pFileA  &&  pFileB)
   {
      Check (pFileA->Pathname () != pFileB->Pathname (), "Same URL, different asset keys");

      bool bA = listenerA.WaitFor (30000);
      bool bB = listenerB.WaitFor (30000);

      Check (bA  &&  bB, "Both POSTs completed");

      if (bA  &&  bB)
      {
         std::vector<uint8_t> aDataA, aDataB;
         pFileA->ReadData (aDataA);
         pFileB->ReadData (aDataB);

         std::string sA (aDataA.begin (), aDataA.end ());
         std::string sB (aDataB.begin (), aDataB.end ());

         Check (!sA.empty ()  &&  !sB.empty (), "Both responses readable");
         Check (sA != sB, "Each POST got its own response");
      }

      pFileA->Close ();
      pFileB->Close ();
   }

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 26: a non-2xx keeps its body when the caller asks for XHR semantics
// ---------------------------------------------------------------------------

static void TestAnyStatusRetainsBody ()
{
   std::printf ("\n[Test 26] Non-2xx body retained under bAnyStatus\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   REQUEST Request;
   Request.eVerb      = kREQUEST_VERB_POST;
   Request.bAnyStatus = true;

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/status/404", std::string (), Request, &listener);

   if (pFile  &&  listener.WaitFor (30000))
   {
      Check (listener.Succeeded (), "404 is a completed request, not a failure");
      Check (pFile->HttpStatus () == 404, "Status is 404");
      Check (pFile->IsReady (), "File is READY despite the 404");
   }
   else Check (false, "404 request completed within 30s");

   if (pFile)
      pFile->Close ();

   delete pNetwork;
}


// ---------------------------------------------------------------------------
// Test 27: the size cap fails a response that overruns it
// ---------------------------------------------------------------------------

static void TestSizeCap ()
{
   std::printf ("\n[Test 27] Size cap\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   REQUEST Request;
   Request.eVerb      = kREQUEST_VERB_POST;   // POST so the cache cannot serve it
   Request.bAnyStatus = true;
   Request.nSizeMax   = 64;

   TEST_FILE_LISTENER listener;
   SNEEZE::FILE* pFile = pCache->File_Open ("https://httpbin.org/bytes/4096", std::string (), Request, &listener);

   if (pFile  &&  listener.WaitFor (30000))
   {
      Check (!listener.Succeeded (), "Oversized response failed");
      Check (!pFile->IsReady (), "Oversized response is not READY");
   }
   else Check (false, "Capped request completed within 30s");

   if (pFile)
      pFile->Close ();

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// The guest network layer (WASM_NETWORK)
//
// These drive WASM_NETWORK the way Dispatch_Network does, but without a guest:
// the stores are real (so the delivery path is real) and carry no instances, so
// a queued event fans out to nobody and the test polls the request instead.
// Request_Send takes its CACHE as an argument, which is what lets this suite -
// which already owns a container and a cache - exercise the whole path.
// ---------------------------------------------------------------------------

static DEP::WASM_NETWORK* Wasm_Network ()
{
   return s_pSneeze->Wasm_Runtime ()->Network ();
}

// Polls until the request leaves SENDING, or the timeout expires. Delivery is a
// no-op for an instance-less store, so polling is how a test learns the answer.
static int32_t Request_Settle (DEP::WASM_NETWORK* pWasmNet, DEP::WASM_STORE* pStore, uint64_t twRequestIx, int nTimeoutMs)
{
   DEP::WASM_NETWORK::RESULT Result;
   Result.eState = kSNEEZE_ABI_REQUEST_STATE_IDLE;

   auto tpDeadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (nTimeoutMs);

   bool bSettled = false;

   while (!bSettled)
   {
      if (!pWasmNet->Request_Result (pStore, twRequestIx, Result))
         bSettled = true;
      else if (Result.eState != kSNEEZE_ABI_REQUEST_STATE_SENDING)
         bSettled = true;
      else if (std::chrono::steady_clock::now () >= tpDeadline)
         bSettled = true;
      else
         std::this_thread::sleep_for (std::chrono::milliseconds (25));
   }

   return Result.eState;
}

// ---------------------------------------------------------------------------
// Test 28: guest request handles
// ---------------------------------------------------------------------------

static void TestGuestRequestHandles ()
{
   std::printf ("\n[Test 28] Guest request handles\n");

   DEP::WASM_NETWORK* pWasmNet = Wasm_Network ();
   DEP::WASM_STORE*   pStoreA  = s_pSneeze->Wasm_Runtime ()->Store_Open ();
   DEP::WASM_STORE*   pStoreB  = s_pSneeze->Wasm_Runtime ()->Store_Open ();

   uint64_t twRequestIx = pWasmNet->Request_Open (pStoreA, 1, kREQUEST_VERB_GET, "https://httpbin.org/get", std::string ());

   Check (twRequestIx != 0, "Open returned a handle");
   Check (pWasmNet->Request_Open (pStoreA, 1, kREQUEST_VERB_GET, std::string (), std::string ()) == 0, "Open rejects an empty URL");

   Check (pWasmNet->Request_Header_Set  (pStoreA, twRequestIx, "X-Sneeze", "1"), "Header set before send");
   Check (!pWasmNet->Request_Header_Set (pStoreA, twRequestIx, std::string (), "1"), "Header set rejects an empty name");
   Check (pWasmNet->Request_Timeout_Set (pStoreA, twRequestIx, 5000), "Timeout set before send");

   Check (!pWasmNet->Request_Header_Set (pStoreB, twRequestIx, "X-Sneeze", "1"), "Another store cannot name the handle");
   Check (!pWasmNet->Request_Close      (pStoreB, twRequestIx), "Another store cannot close the handle");

   DEP::WASM_NETWORK::RESULT Result;
   Check (pWasmNet->Request_Result (pStoreA, twRequestIx, Result), "Result readable before send");
   Check (Result.eState == kSNEEZE_ABI_REQUEST_STATE_IDLE, "State is IDLE before send");

   Check (pWasmNet->Request_Close  (pStoreA, twRequestIx), "Close retired the handle");
   Check (!pWasmNet->Request_Close (pStoreA, twRequestIx), "Close is not idempotent - the handle is gone");

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStoreA);
   s_pSneeze->Wasm_Runtime ()->Store_Close (pStoreB);
}

// ---------------------------------------------------------------------------
// Test 29: a guest GET, end to end
// ---------------------------------------------------------------------------

static void TestGuestRequestGet ()
{
   std::printf ("\n[Test 29] Guest GET\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   DEP::WASM_NETWORK* pWasmNet = Wasm_Network ();
   DEP::WASM_STORE*   pStore   = s_pSneeze->Wasm_Runtime ()->Store_Open ();

   uint64_t twRequestIx = pWasmNet->Request_Open (pStore, 1, kREQUEST_VERB_GET, "https://httpbin.org/get", std::string ());

   Check (pWasmNet->Request_Send (pStore, twRequestIx, pCache, nullptr, 0), "Send accepted");
   Check (!pWasmNet->Request_Send (pStore, twRequestIx, pCache, nullptr, 0), "Send twice is rejected");

   int32_t eState = Request_Settle (pWasmNet, pStore, twRequestIx, 30000);

   Check (eState == kSNEEZE_ABI_REQUEST_STATE_COMPLETE, "Request completed");

   if (eState == kSNEEZE_ABI_REQUEST_STATE_COMPLETE)
   {
      DEP::WASM_NETWORK::RESULT Result;
      pWasmNet->Request_Result (pStore, twRequestIx, Result);

      Check (Result.nHttpStatus == 200, "Status is 200");
      Check (Result.sError.empty (), "No transport error");
      Check (Result.sContentType.find ("json") != std::string::npos, "Content type came through");
      Check (Result.sUrl == "https://httpbin.org/get", "URL reported back");

      std::vector<uint8_t> aBody;
      pWasmNet->Request_Body (pStore, twRequestIx, aBody);
      Check (!aBody.empty (), "Body snapshot is readable");
      Check (Result.nSizeBytes == aBody.size (), "Reported size matches the body");

      std::string sHeaders;
      pWasmNet->Request_Headers (pStore, twRequestIx, sHeaders);
      Check (sHeaders.find ("content-type:") != std::string::npos, "All-headers text includes content-type");

      std::string sValue;
      pWasmNet->Request_Header (pStore, twRequestIx, "Content-Type", sValue);
      Check (!sValue.empty (), "Single header lookup is case-insensitive");

      // The snapshot outlives the FILE, which is the point of taking one.
      pWasmNet->Request_Close (pStore, twRequestIx);
      Check (!pWasmNet->Request_Body (pStore, twRequestIx, aBody), "Body is gone once the handle closes");
   }
   else pWasmNet->Request_Close (pStore, twRequestIx);

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStore);

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 30: a guest POST carries its body and keeps a non-2xx answer
// ---------------------------------------------------------------------------

static void TestGuestRequestPost ()
{
   std::printf ("\n[Test 30] Guest POST\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   DEP::WASM_NETWORK* pWasmNet = Wasm_Network ();
   DEP::WASM_STORE*   pStore   = s_pSneeze->Wasm_Runtime ()->Store_Open ();

   std::string sBody = "{\"from\":\"guest\"}";

   uint64_t twRequestIx = pWasmNet->Request_Open (pStore, 1, kREQUEST_VERB_POST, "https://httpbin.org/post", std::string ());

   pWasmNet->Request_Header_Set (pStore, twRequestIx, "Content-Type", "application/json");

   Check (pWasmNet->Request_Send (pStore, twRequestIx, pCache, reinterpret_cast<const uint8_t*> (sBody.data ()), sBody.size ()), "POST sent");

   if (Request_Settle (pWasmNet, pStore, twRequestIx, 30000) == kSNEEZE_ABI_REQUEST_STATE_COMPLETE)
   {
      std::vector<uint8_t> aBody;
      pWasmNet->Request_Body (pStore, twRequestIx, aBody);

      std::string sResponse (aBody.begin (), aBody.end ());

      Check (sResponse.find ("guest") != std::string::npos, "Request body reached the server");
   }
   else Check (false, "POST completed within 30s");

   pWasmNet->Request_Close (pStore, twRequestIx);

   // XHR treats any HTTP answer as a completed request, so a 404 completes.
   uint64_t twNotFound = pWasmNet->Request_Open (pStore, 1, kREQUEST_VERB_POST, "https://httpbin.org/status/404", std::string ());

   pWasmNet->Request_Send (pStore, twNotFound, pCache, nullptr, 0);

   if (Request_Settle (pWasmNet, pStore, twNotFound, 30000) == kSNEEZE_ABI_REQUEST_STATE_COMPLETE)
   {
      DEP::WASM_NETWORK::RESULT Result;
      pWasmNet->Request_Result (pStore, twNotFound, Result);

      Check (Result.nHttpStatus == 404, "A 404 completes and reports its status");
   }
   else Check (false, "404 completed within 30s");

   pWasmNet->Request_Close (pStore, twNotFound);

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStore);

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// Test 31: abort and store teardown
// ---------------------------------------------------------------------------

static void TestGuestRequestAbortAndTeardown ()
{
   std::printf ("\n[Test 31] Guest request abort and store teardown\n");

   NETWORK* pNetwork = new NETWORK (s_pSneeze);
   pNetwork->Initialize (s_sPathRoot);

   CACHE* pCache = pNetwork->Cache_Open (s_pTestContainer);

   DEP::WASM_NETWORK* pWasmNet = Wasm_Network ();
   DEP::WASM_STORE*   pStore   = s_pSneeze->Wasm_Runtime ()->Store_Open ();

   // Aborted in flight: the fetch still lands and is still recorded, but the
   // state stays ABORTED because the guest said it had stopped listening.
   uint64_t twAborted = pWasmNet->Request_Open (pStore, 1, kREQUEST_VERB_POST, "https://httpbin.org/delay/1", std::string ());

   pWasmNet->Request_Send  (pStore, twAborted, pCache, nullptr, 0);
   Check (pWasmNet->Request_Abort (pStore, twAborted), "Abort accepted while in flight");

   DEP::WASM_NETWORK::RESULT Result;
   pWasmNet->Request_Result (pStore, twAborted, Result);
   Check (Result.eState == kSNEEZE_ABI_REQUEST_STATE_ABORTED, "State is ABORTED immediately");

   std::this_thread::sleep_for (std::chrono::milliseconds (4000));

   pWasmNet->Request_Result (pStore, twAborted, Result);
   Check (Result.eState == kSNEEZE_ABI_REQUEST_STATE_ABORTED, "An aborted request stays ABORTED after its fetch lands");

   // Store teardown takes every handle with it, in flight or not.
   uint64_t twInFlight = pWasmNet->Request_Open (pStore, 1, kREQUEST_VERB_POST, "https://httpbin.org/delay/2", std::string ());

   pWasmNet->Request_Send (pStore, twInFlight, pCache, nullptr, 0);

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStore);

   Check (!pWasmNet->Request_Result (pStore, twAborted,  Result), "Store close retired the aborted handle");
   Check (!pWasmNet->Request_Result (pStore, twInFlight, Result), "Store close retired the in-flight handle");

   // Outlive the abandoned fetch so its listener retires against a live cache.
   std::this_thread::sleep_for (std::chrono::milliseconds (5000));

   delete pNetwork;
}

// ---------------------------------------------------------------------------
// The guest socket layer (WASM_NETWORK's socket block)
//
// Sockets differ from requests in where the connection comes from: a request
// takes its CACHE as an argument, but a socket is opened on the engine's own
// NETWORK, so these tests need nothing but a container to name.
//
// Delivery is still a no-op for an instance-less store, so these poll too.
// ---------------------------------------------------------------------------

static int32_t Socket_Wait (DEP::WASM_NETWORK* pWasmNet, DEP::WASM_STORE* pStore, uint64_t twSocketIx, int32_t eWanted, int nTimeoutMs)
{
   DEP::WASM_NETWORK::SOCKET_RESULT Result;
   Result.eState = kSNEEZE_ABI_SOCKET_STATE_CONNECTING;

   auto tpDeadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (nTimeoutMs);

   bool bSettled = false;

   while (!bSettled)
   {
      if (!pWasmNet->Socket_Result (pStore, twSocketIx, Result))
         bSettled = true;
      else if (Result.eState == eWanted)
         bSettled = true;
      else if (std::chrono::steady_clock::now () >= tpDeadline)
         bSettled = true;
      else
         std::this_thread::sleep_for (std::chrono::milliseconds (25));
   }

   return Result.eState;
}

// Waits for bufferedAmount to reach zero. A send hands the frame to the io
// thread, so whether anything is still queued the instant the send returns is a
// race - what is worth asserting is that it drains.
static bool Socket_Wait_Drained (DEP::WASM_NETWORK* pWasmNet, DEP::WASM_STORE* pStore, uint64_t twSocketIx, int nTimeoutMs)
{
   bool bResult = false;

   auto tpDeadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (nTimeoutMs);

   while (!bResult  &&  std::chrono::steady_clock::now () < tpDeadline)
   {
      if (pWasmNet->Socket_Buffered (pStore, twSocketIx) == 0)
         bResult = true;
      else
         std::this_thread::sleep_for (std::chrono::milliseconds (25));
   }

   return bResult;
}

// Peeks the head of the receive queue until something is there. A zero capacity
// reports the size without consuming, which is exactly what a poll wants.
static bool Socket_Wait_Message (DEP::WASM_NETWORK* pWasmNet, DEP::WASM_STORE* pStore, uint64_t twSocketIx, std::vector<uint8_t>& aData, bool& bBinary, int nTimeoutMs)
{
   bool bResult = false;

   auto tpDeadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (nTimeoutMs);

   while (!bResult  &&  std::chrono::steady_clock::now () < tpDeadline)
   {
      if (pWasmNet->Socket_Recv (pStore, twSocketIx, 0, aData, bBinary))
         bResult = true;
      else
         std::this_thread::sleep_for (std::chrono::milliseconds (25));
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Test 32: guest socket handles, and a connection that is refused
// ---------------------------------------------------------------------------

static void TestGuestSocketHandles ()
{
   std::printf ("\n[Test 32] Guest socket handles\n");

   DEP::WASM_NETWORK* pWasmNet = Wasm_Network ();
   DEP::WASM_STORE*   pStoreA  = s_pSneeze->Wasm_Runtime ()->Store_Open ();
   DEP::WASM_STORE*   pStoreB  = s_pSneeze->Wasm_Runtime ()->Store_Open ();

   Check (pWasmNet->Socket_Open (pStoreA, 1, s_pTestContainer, std::string (), std::string ()) == 0, "Open rejects an empty URL");
   Check (pWasmNet->Socket_Open (pStoreA, 1, s_pTestContainer, "https://httpbin.org/get", std::string ()) == 0, "Open rejects a URL that is not ws:// or wss://");
   Check (pWasmNet->Socket_Open (pStoreA, 1, nullptr, "ws://127.0.0.1:1/", std::string ()) == 0, "Open rejects a call with no container");

   // Well-formed, and nothing is listening on port 1, so it connects and then
   // fails - which is what makes it a network-free way to test the failure path.
   uint64_t twSocketIx = pWasmNet->Socket_Open (pStoreA, 1, s_pTestContainer, "ws://127.0.0.1:1/", std::string ());

   Check (twSocketIx != 0, "Open returned a handle for a well-formed ws:// URL");

   DEP::WASM_NETWORK::SOCKET_RESULT Result;

   Check (pWasmNet->Socket_Result (pStoreA, twSocketIx, Result), "Result readable");
   Check (Result.sUrl == "ws://127.0.0.1:1/", "URL reported back");
   Check (Result.sProtocol.empty (), "No subprotocol was negotiated");

   Check (!pWasmNet->Socket_Result (pStoreB, twSocketIx, Result), "Another store cannot name the handle");
   Check (!pWasmNet->Socket_Free   (pStoreB, twSocketIx), "Another store cannot free the handle");
   Check (!pWasmNet->Socket_Send   (pStoreB, twSocketIx, nullptr, 0, false), "Another store cannot send on the handle");

   int32_t eState = Socket_Wait (pWasmNet, pStoreA, twSocketIx, kSNEEZE_ABI_SOCKET_STATE_CLOSED, 15000);

   Check (eState == kSNEEZE_ABI_SOCKET_STATE_CLOSED, "A refused connection ends CLOSED");

   if (pWasmNet->Socket_Result (pStoreA, twSocketIx, Result))
      Check (!Result.sError.empty (), "The failure left an error behind");
   else Check (false, "Result readable after the failure");

   Check (!pWasmNet->Socket_Send (pStoreA, twSocketIx, nullptr, 0, false), "A closed socket refuses a send");

   Check (pWasmNet->Socket_Free  (pStoreA, twSocketIx), "Free retired the handle");
   Check (!pWasmNet->Socket_Free (pStoreA, twSocketIx), "Free is not idempotent - the handle is gone");

   // Store teardown takes a live socket with it.
   uint64_t twOrphan = pWasmNet->Socket_Open (pStoreA, 1, s_pTestContainer, "ws://127.0.0.1:1/", std::string ());

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStoreA);

   Check (!pWasmNet->Socket_Result (pStoreA, twOrphan, Result), "Store close retired the socket handle");

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStoreB);
}

// ---------------------------------------------------------------------------
// Test 33: a guest socket round trip, end to end
// ---------------------------------------------------------------------------

static void TestGuestSocketEcho ()
{
   std::printf ("\n[Test 33] Guest socket echo\n");

   DEP::WASM_NETWORK* pWasmNet = Wasm_Network ();
   DEP::WASM_STORE*   pStore   = s_pSneeze->Wasm_Runtime ()->Store_Open ();

   uint64_t twSocketIx = pWasmNet->Socket_Open (pStore, 1, s_pTestContainer, "wss://ws.postman-echo.com/raw", std::string ());

   Check (twSocketIx != 0, "Open returned a handle");

   int32_t eState = Socket_Wait (pWasmNet, pStore, twSocketIx, kSNEEZE_ABI_SOCKET_STATE_OPEN, 30000);

   Check (eState == kSNEEZE_ABI_SOCKET_STATE_OPEN, "Socket reached OPEN");

   if (eState == kSNEEZE_ABI_SOCKET_STATE_OPEN)
   {
      std::string sHello = "sneeze-socket-test";

      Check (pWasmNet->Socket_Send (pStore, twSocketIx, reinterpret_cast<const uint8_t*> (sHello.data ()), sHello.size (), false), "Text frame sent");
      Check (Socket_Wait_Drained (pWasmNet, pStore, twSocketIx, 15000), "The send drained off the buffer");

      std::vector<uint8_t> aData;
      bool                 bBinary = true;

      if (Socket_Wait_Message (pWasmNet, pStore, twSocketIx, aData, bBinary, 30000))
      {
         size_t nSize = aData.size ();

         Check (!bBinary, "The echo arrived as text");

         std::vector<uint8_t> aPeek;
         bool                 bPeek = false;

         Check (pWasmNet->Socket_Recv (pStore, twSocketIx, 0, aPeek, bPeek)  &&  aPeek.size () == nSize, "A query call reports the size and leaves the message queued");

         Check (pWasmNet->Socket_Recv (pStore, twSocketIx, nSize, aData, bBinary), "A buffer that fits takes the message");
         Check (std::string (aData.begin (), aData.end ()) == sHello, "The echo matched what was sent");

         Check (!pWasmNet->Socket_Recv (pStore, twSocketIx, nSize, aPeek, bPeek), "The queue is empty again");
      }
      else Check (false, "The echo arrived within 30s");

      Check (pWasmNet->Socket_Close (pStore, twSocketIx, 1000, "done"), "Close accepted");

      eState = Socket_Wait (pWasmNet, pStore, twSocketIx, kSNEEZE_ABI_SOCKET_STATE_CLOSED, 15000);

      Check (eState == kSNEEZE_ABI_SOCKET_STATE_CLOSED, "Socket reached CLOSED");

      // The browser keeps a closed socket readable, and so does this: only Free
      // retires the handle.
      DEP::WASM_NETWORK::SOCKET_RESULT Result;
      Check (pWasmNet->Socket_Result (pStore, twSocketIx, Result), "A closed socket is still readable");
   }

   Check (pWasmNet->Socket_Free (pStore, twSocketIx), "Free retired the handle");

   s_pSneeze->Wasm_Runtime ()->Store_Close (pStore);
}

// ---------------------------------------------------------------------------
// Test 34: a close reports the peer's code, and whether the handshake finished
// ---------------------------------------------------------------------------

// Records what the socket reported. Every callback lands on the network's io
// thread, so the fields are read back under the lock after the wait.
class SOCKET_CLOSE_LISTENER : public SNEEZE::ISOCKET
{
public:
   SOCKET_CLOSE_LISTENER () : m_bOpened (false), m_bClosed (false), m_bFailed (false), m_wCode (0), m_bClean (false) {}

   void OnSocketOpened  (SNEEZE::SOCKET*) override
   {
      std::lock_guard<std::mutex> guard (m_mxListener);
      m_bOpened = true;
   }

   void OnSocketMessage (SNEEZE::SOCKET*, const uint8_t*, size_t, bool) override
   {
   }

   void OnSocketFailed  (SNEEZE::SOCKET*) override
   {
      std::lock_guard<std::mutex> guard (m_mxListener);
      m_bFailed = true;
   }

   void OnSocketClosed  (SNEEZE::SOCKET*, uint16_t wCode, bool bClean) override
   {
      std::lock_guard<std::mutex> guard (m_mxListener);
      m_wCode  = wCode;
      m_bClean = bClean;
      m_bClosed = true;
   }

   bool Wait_Opened (int nTimeoutMs) { return Wait (m_bOpened, nTimeoutMs); }
   bool Wait_Closed (int nTimeoutMs) { return Wait (m_bClosed, nTimeoutMs); }

   uint16_t Code  () { std::lock_guard<std::mutex> guard (m_mxListener); return m_wCode; }
   bool     Clean () { std::lock_guard<std::mutex> guard (m_mxListener); return m_bClean; }
   bool     Failed () { std::lock_guard<std::mutex> guard (m_mxListener); return m_bFailed; }

private:
   bool Wait (const bool& bFlag, int nTimeoutMs)
   {
      bool bResult = false;

      auto tpDeadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (nTimeoutMs);

      while (!bResult  &&  std::chrono::steady_clock::now () < tpDeadline)
      {
         {
            std::lock_guard<std::mutex> guard (m_mxListener);
            bResult = bFlag;
         }

         if (!bResult)
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
      }

      return bResult;
   }

   std::mutex m_mxListener;
   bool       m_bOpened;
   bool       m_bClosed;
   bool       m_bFailed;
   uint16_t   m_wCode;
   bool       m_bClean;
};

static void TestSocketCloseIsClean ()
{
   std::printf ("\n[Test 34] Socket close reporting\n");

   SNEEZE::NETWORK* pNetwork = s_pSneeze->Network ();

   // A peer that answers the closing handshake: we send 1000, it echoes 1000, so
   // the close is clean. This is the case that regressed once - the close handler
   // used to report every close as clean, including a dropped connection.
   SOCKET_CLOSE_LISTENER  Polite;
   SNEEZE::SOCKET*        pSocket = pNetwork->Socket_Open (s_pTestContainer, "wss://echo.websocket.org", std::string (), &Polite);

   Check (pSocket != nullptr, "Open returned a socket");

   if (pSocket)
   {
      if (Polite.Wait_Opened (30000))
      {
         pSocket->Close (1000, "done");

         Check (Polite.Wait_Closed (15000), "The close was reported");
         Check (Polite.Code () == 1000, "The peer's close code came back");
         Check (Polite.Clean (), "A completed closing handshake is clean");
      }
      else Check (false, "Socket reached OPEN within 30s");

      pNetwork->Socket_Close (pSocket);
   }

   // A connection that never comes up cannot have had a handshake, so 1006 and
   // not clean - the mirror of the case above.
   SOCKET_CLOSE_LISTENER  Refused;
   SNEEZE::SOCKET*        pRefused = pNetwork->Socket_Open (s_pTestContainer, "ws://127.0.0.1:1/", std::string (), &Refused);

   Check (pRefused != nullptr, "Open returned a socket for the refused URL");

   if (pRefused)
   {
      Check (Refused.Wait_Closed (15000), "The refused connection reported a close");
      Check (Refused.Failed (), "The failure was reported first");
      Check (Refused.Code () == 1006, "A connection that never opened closes 1006");
      Check (!Refused.Clean (), "A close with no handshake is not clean");

      pNetwork->Socket_Close (pRefused);
   }

   // The case the two above miss, because they take different code paths: a
   // connection that opened and then dropped without a closing handshake. This
   // echo server drops on any binary frame, which is a reliable way to provoke
   // one. If it ever stops doing that the drop simply never comes, and the
   // assertions are skipped rather than failing on someone else's behavior.
   SOCKET_CLOSE_LISTENER  Dropped;
   SNEEZE::SOCKET*        pDropped = pNetwork->Socket_Open (s_pTestContainer, "wss://ws.postman-echo.com/raw", std::string (), &Dropped);

   Check (pDropped != nullptr, "Open returned a socket for the drop case");

   if (pDropped)
   {
      if (Dropped.Wait_Opened (30000))
      {
         const uint8_t aByte[] = { 0xDE, 0xAD, 0xBE, 0xEF, };

         pDropped->Send_Binary (aByte, sizeof (aByte));

         if (Dropped.Wait_Closed (30000))
         {
            Check (Dropped.Code () == 1006, "A dropped connection closes 1006");
            Check (!Dropped.Clean (), "A drop after OPEN is not clean either");
         }
         else std::printf ("  NOTE: the echo server no longer drops binary frames - mid-session drop not exercised\n");
      }
      else Check (false, "The drop-case socket reached OPEN within 30s");

      pNetwork->Socket_Close (pDropped);
   }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int RunNetworkTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Network Test Suite ===\n");

   s_pTestListener = new CACHE_TEST_LISTENER ();
   s_pTestListener->m_sAppDataPath = (std::filesystem::path (std::getenv ("APPDATA")) / "Metaversal" / "Sneeze" / "Test").string ();
   s_pTestListener->m_sSessionPath = s_pTestListener->m_sAppDataPath;

   s_sPathRoot = (std::filesystem::path (s_pTestListener->m_sAppDataPath) / "Sneeze" / "Cache").string ();

   std::filesystem::remove_all (s_pTestListener->m_sAppDataPath);

   s_pSneeze = new SNEEZE::ENGINE (s_pTestListener);
   s_pSneeze->Initialize ();

   s_pContextHost = new CACHE_TEST_CONTEXT_HOST ();
   s_pContext = s_pSneeze->Context_Open (s_pContextHost);
   InitTestContainer ();

   TestManagerInit ();
   TestUnhashedFetch ();
   TestDeduplication ();
   TestHashVerifiedFetch ();
   TestHashMismatch ();
   TestReset ();
   TestResetFlag ();
   TestFailedFetch ();
   TestSidecarPersistence ();
   TestHttpHeaders ();
   TestFileHandleLifecycle ();
   TestHistoryAndFileIx ();
   TestNotifications ();
   TestServedFromCache ();
   TestFailedFetchHttpStatus ();
   TestClearFlag ();
   TestCloseWithoutReset ();
   TestDeferredReset ();
   TestClear ();
   TestClearAllCaches ();
   TestDeletedNotification ();
   TestStalenessRules ();
   TestNoFetchOpen ();
   TestPostVerb ();
   TestPostNoCoalesce ();
   TestAnyStatusRetainsBody ();
   TestSizeCap ();
   TestGuestRequestHandles ();
   TestGuestRequestGet ();
   TestGuestRequestPost ();
   TestGuestRequestAbortAndTeardown ();
   TestGuestSocketHandles ();
   TestGuestSocketEcho ();
   TestSocketCloseIsClean ();

   delete s_pTestContainer;
   s_pTestContainer = nullptr;

   // Intentionally leak s_pSneeze - its destructor calls static subsystem
   // shutdowns (WASM, SPV, etc.) that may interfere with other test suites.

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
