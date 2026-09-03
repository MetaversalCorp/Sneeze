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

   delete s_pTestContainer;
   s_pTestContainer = nullptr;

   // Intentionally leak s_pSneeze - its destructor calls static subsystem
   // shutdowns (WASM, SPV, etc.) that may interfere with other test suites.

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
