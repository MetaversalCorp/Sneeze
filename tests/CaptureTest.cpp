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

#include "camera/Capture.h"

#include <cstdint>
#include <cstdio>
#include <vector>

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

static void TestEnumerate ()
{
   std::printf ("\n[Test 1] Enumerate camera devices\n");

   SNEEZE::DEP::CAPTURE capture (nullptr);
   Check (capture.Initialize (), "CAPTURE::Initialize succeeds without an ENGINE");

   const int nCount = capture.Device_Count ();
   Check (nCount >= 0, "Device_Count is non-negative");
   std::printf ("    Found %d camera device(s)\n", nCount);
   for (int i = 0; i < nCount; i++)
      std::printf ("    [%d] %s\n", i, capture.Device_Name (i).c_str ());
}

static void TestOpenGuards ()
{
   std::printf ("\n[Test 2] Open guards (no device is opened)\n");

   SNEEZE::DEP::CAPTURE capture (nullptr);
   capture.Initialize ();

   Check (!capture.Device_Open (-1), "Device_Open (-1) fails");
   Check (!capture.Device_IsOpen (0), "Device 0 is not open");

   int nWidth = 1, nHeight = 1;
   uint64_t nFrameIx = 1;
   std::vector<uint8_t> aRgba;
   Check (!capture.Frame_Latest (0, nWidth, nHeight, aRgba, nFrameIx), "Frame_Latest on a closed device fails");
   Check (nWidth == 0  &&  nHeight == 0  &&  nFrameIx == 0, "Frame_Latest clears outputs on failure");
}

int RunCaptureTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Capture Test Suite ===\n");

   TestEnumerate ();
   TestOpenGuards ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);
   return (nFailed > 0) ? 1 : 0;
}
