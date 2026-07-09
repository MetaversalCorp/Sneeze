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

#include "basis/Basis.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef SNEEZE_TEST_DATA_DIR
#define SNEEZE_TEST_DATA_DIR "tests/data"
#endif

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

static bool ReadFile (const std::string& sPath, std::vector<uint8_t>& aBytes)
{
   bool bResult = false;

   std::ifstream file (sPath, std::ios::binary | std::ios::ate);
   if (file.is_open ())
   {
      std::streamsize nSize = file.tellg ();
      file.seekg (0, std::ios::beg);

      aBytes.resize (static_cast<size_t> (nSize));
      if (nSize > 0  &&  file.read (reinterpret_cast<char*> (aBytes.data ()), nSize))
         bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Test 1: transcoder is linked and initializes
// ---------------------------------------------------------------------------
static void TestAvailable ()
{
   std::printf ("\n[Test 1] Transcoder available\n");

   // Exercises basist::basisu_transcoder_init through the wrapper -- proves the
   // basisu_transcoder library is linked and callable at runtime.
   Check (SNEEZE::BASIS::Available (), "Transcoder available (init ran)");
}

// ---------------------------------------------------------------------------
// Test 2: FORMAT <-> ANARI compressedImage2D 'format' string mapping
// ---------------------------------------------------------------------------
static void TestFormatMapping ()
{
   std::printf ("\n[Test 2] Format mapping\n");

   Check (std::strcmp (SNEEZE::BASIS::Format_Anari (SNEEZE::BASIS::kFORMAT_BC7), "BC7") == 0, "BC7 maps to ANARI 'BC7'");
   Check (std::strcmp (SNEEZE::BASIS::Format_Anari (SNEEZE::BASIS::kFORMAT_ETC2_RGBA8), "ETC2_R8G8B8A8") == 0, "ETC2 maps to ANARI 'ETC2_R8G8B8A8'");
   Check (SNEEZE::BASIS::Format_Anari (SNEEZE::BASIS::kFORMAT_RGBA8) == nullptr, "RGBA8 has no compressed format string");
   Check (SNEEZE::BASIS::Format_Anari (SNEEZE::BASIS::kFORMAT_None) == nullptr, "None has no compressed format string");
}

// ---------------------------------------------------------------------------
// Test 3: target selection from a device's advertised format list
// ---------------------------------------------------------------------------
static void TestTargetSelection ()
{
   std::printf ("\n[Test 3] Target selection\n");

   Check (SNEEZE::BASIS::Target_Choose ("ETC2_R8G8B8A8,BC7") == SNEEZE::BASIS::kFORMAT_BC7, "Prefers BC7 when advertised");
   Check (SNEEZE::BASIS::Target_Choose ("ASTC_4x4,ETC2_R8G8B8A8") == SNEEZE::BASIS::kFORMAT_ASTC_4x4, "Prefers ASTC over ETC2");
   Check (SNEEZE::BASIS::Target_Choose ("ETC2_R8G8B8A8") == SNEEZE::BASIS::kFORMAT_ETC2_RGBA8, "Falls back to ETC2");
   Check (SNEEZE::BASIS::Target_Choose ("") == SNEEZE::BASIS::kFORMAT_None, "No formats -> kFORMAT_None");
   Check (SNEEZE::BASIS::Target_Choose (nullptr) == SNEEZE::BASIS::kFORMAT_None, "Null formats -> kFORMAT_None");
}

// ---------------------------------------------------------------------------
// Test 4: non-KTX2 input is rejected cleanly
// ---------------------------------------------------------------------------
static void TestTranscodeReject ()
{
   std::printf ("\n[Test 4] Reject invalid input\n");

   std::vector<uint8_t> aEncoded = { 0x00, 0x11, 0x22, 0x33, };
   int nWidth  = 7;
   int nHeight = 7;
   std::vector<uint8_t> aBlocks = { 0xFF, };

   bool bDecoded = SNEEZE::BASIS::Transcode (aEncoded, SNEEZE::BASIS::kFORMAT_BC7, nWidth, nHeight, aBlocks);
   Check (!bDecoded, "Invalid KTX2 input rejected");
   Check (nWidth == 0  &&  nHeight == 0, "Rejected transcode clears dimensions");
   Check (aBlocks.empty (), "Rejected transcode clears output");
}

// ---------------------------------------------------------------------------
// Test 5: a real KTX2 texture transcodes to BC7 blocks
// ---------------------------------------------------------------------------
static void TestTranscodeBc7 ()
{
   std::printf ("\n[Test 5] Transcode KTX2 to BC7\n");

   std::string sPath = std::string (SNEEZE_TEST_DATA_DIR) + "/kodim23.ktx2";

   std::vector<uint8_t> aKtx2;
   if (!ReadFile (sPath, aKtx2))
   {
      Check (false, "Sample KTX2 read from disk");
      std::printf ("    (expected at %s)\n", sPath.c_str ());
      return;
   }

   int nWidth  = 0;
   int nHeight = 0;
   std::vector<uint8_t> aBc7;
   bool bOk = SNEEZE::BASIS::Transcode (aKtx2, SNEEZE::BASIS::kFORMAT_BC7, nWidth, nHeight, aBc7);

   Check (bOk, "KTX2 transcoded to BC7");
   Check (nWidth > 0  &&  nHeight > 0, "BC7 transcode reports dimensions");

   // BC7 is a 4x4-block, 16-bytes-per-block format, so the output size is
   // exactly ceil(w/4) * ceil(h/4) * 16.
   size_t nBlocks = size_t ((nWidth + 3) / 4) * size_t ((nHeight + 3) / 4);
   Check (!aBc7.empty ()  &&  aBc7.size () == nBlocks * 16, "BC7 output size matches block count");

   std::printf ("    kodim23.ktx2 -> BC7  %dx%d  %zu bytes\n", nWidth, nHeight, aBc7.size ());
}

// ---------------------------------------------------------------------------
// Test 6: the RGBA8 fallback (used when a GPU advertises no compressed format)
// ---------------------------------------------------------------------------
static void TestTranscodeRgba8 ()
{
   std::printf ("\n[Test 6] Transcode KTX2 to RGBA8 (fallback)\n");

   std::string sPath = std::string (SNEEZE_TEST_DATA_DIR) + "/kodim23.ktx2";

   std::vector<uint8_t> aKtx2;
   if (!ReadFile (sPath, aKtx2))
   {
      Check (false, "Sample KTX2 read from disk");
      std::printf ("    (expected at %s)\n", sPath.c_str ());
      return;
   }

   int nWidth  = 0;
   int nHeight = 0;
   std::vector<uint8_t> aRgba;
   bool bOk = SNEEZE::BASIS::Transcode (aKtx2, SNEEZE::BASIS::kFORMAT_RGBA8, nWidth, nHeight, aRgba);

   Check (bOk, "KTX2 transcoded to RGBA8");
   Check (nWidth > 0  &&  nHeight > 0, "RGBA8 transcode reports dimensions");
   Check (aRgba.size () == size_t (nWidth) * size_t (nHeight) * 4, "RGBA8 output size is w*h*4");

   std::printf ("    kodim23.ktx2 -> RGBA8  %dx%d  %zu bytes\n", nWidth, nHeight, aRgba.size ());
}

// ---------------------------------------------------------------------------

int RunBasisTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Basis Universal Transcoder Test Suite ===\n");

   TestAvailable ();
   TestFormatMapping ();
   TestTargetSelection ();
   TestTranscodeReject ();
   TestTranscodeBc7 ();
   TestTranscodeRgba8 ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
