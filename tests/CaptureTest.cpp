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
#include "camera/Capture_Pinhole.h"

#include <cstdint>
#include <cstdio>
#include <string>
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

   SNEEZE::DEP::CAPTURE::INTRINSICS Pinhole;
   Pinhole.nWidth = 9;
   Pinhole.dFx    = 9.0;
   Check (!capture.Device_Intrinsics (0, Pinhole), "Device_Intrinsics on a closed device fails");
   Check (Pinhole.nWidth == 0  &&  Pinhole.dFx == 0.0, "Device_Intrinsics clears outputs on failure");
}

static bool Near (double dA, double dB)
{
   double d = dA - dB;
   if (d < 0.0)
      d = -d;
   return d < 1.0e-6;
}

static void TestPinhole ()
{
   std::printf ("\n[Test 3] Placeframe pinhole helpers\n");

   using namespace SNEEZE::DEP;
   CAPTURE::INTRINSICS Pinhole;

   Check (!CAPTURE_PINHOLE::Valid (Pinhole), "default INTRINSICS is not valid");
   Check (CAPTURE_PINHOLE::From_Size (Pinhole, 640, 480), "From_Size accepts a stream size");
   Check (Pinhole.eModel == CAPTURE::kMODEL_PINHOLE, "From_Size writes kMODEL_PINHOLE");
   Check (std::string (CAPTURE::Model_Name (Pinhole.eModel)) == "PINHOLE", "Model_Name is the COLMAP/OpenVPS PINHOLE token");
   Check (std::string (CAPTURE::Model_Name (CAPTURE::kMODEL_OPENCV)) == "OPENCV", "OPENCV model name is reserved");
   Check (std::string (CAPTURE::Model_Name (CAPTURE::kMODEL_OPENCV_FISHEYE)) == "OPENCV_FISHEYE", "OPENCV_FISHEYE model name is reserved");
   Check (Pinhole.nWidth == 640  &&  Pinhole.nHeight == 480, "From_Size stores width/height");
   Check (Near (Pinhole.dFx, 640.0)  &&  Near (Pinhole.dFy, 640.0), "From_Size uses square-pixel fx");
   Check (Near (Pinhole.dCx, 320.0)  &&  Near (Pinhole.dCy, 240.0), "From_Size centers the principal point");
   Check (Pinhole.eOrientation == CAPTURE::kORIENTATION_TOP_LEFT, "From_Size is TOP_LEFT");
   Check (std::string (CAPTURE::Orientation_Name (Pinhole.eOrientation)) == "TOP_LEFT", "Orientation_Name is the placeframe TOP_LEFT token");

   Pinhole.dFx = 2000.0;
   Pinhole.dFy = 1500.0;
   Pinhole.dCx = 2000.0;
   Pinhole.dCy = 1500.0;
   Pinhole.nWidth  = 4000;
   Pinhole.nHeight = 3000;
   Check (CAPTURE_PINHOLE::Scale (Pinhole, 4000, 3000, 800, 600), "Scale to stream size");
   Check (Pinhole.nWidth == 800  &&  Pinhole.nHeight == 600, "Scale writes destination size");
   Check (Near (Pinhole.dFx, 400.0)  &&  Near (Pinhole.dFy, 300.0), "Scale fx/fy");
   Check (Near (Pinhole.dCx, 400.0)  &&  Near (Pinhole.dCy, 300.0), "Scale cx/cy");

   Check (CAPTURE_PINHOLE::Orientation_From_Sensor (0)   == CAPTURE::kORIENTATION_TOP_LEFT,     "sensor 0 is TOP_LEFT");
   Check (CAPTURE_PINHOLE::Orientation_From_Sensor (90)  == CAPTURE::kORIENTATION_RIGHT_TOP,    "sensor 90 is RIGHT_TOP");
   Check (CAPTURE_PINHOLE::Orientation_From_Sensor (180) == CAPTURE::kORIENTATION_BOTTOM_RIGHT, "sensor 180 is BOTTOM_RIGHT");
   Check (CAPTURE_PINHOLE::Orientation_From_Sensor (270) == CAPTURE::kORIENTATION_LEFT_BOTTOM,  "sensor 270 is LEFT_BOTTOM");
   Check (std::string (CAPTURE::Orientation_Name (CAPTURE::kORIENTATION_LEFT_TOP)) == "LEFT_TOP", "LEFT_TOP name matches placeframe");

   Check (CAPTURE_PINHOLE::From_Focal_Mm (Pinhole, 640, 480, 4.0, 4.0, 3.0), "From_Focal_Mm");
   Check (Near (Pinhole.dFx, 640.0)  &&  Near (Pinhole.dFy, 640.0), "From_Focal_Mm fx/fy");
   Check (Near (Pinhole.dCx, 320.0)  &&  Near (Pinhole.dCy, 240.0), "From_Focal_Mm principal point");

   Check (CAPTURE_PINHOLE::From_Hfov (Pinhole, 640, 480, 90.0), "From_Hfov 90 deg");
   Check (Near (Pinhole.dFx, 320.0)  &&  Near (Pinhole.dCx, 320.0), "From_Hfov fx is half-width at 90 deg");
   Check (!CAPTURE_PINHOLE::From_Hfov (Pinhole, 640, 480, 0.0), "From_Hfov rejects 0");
   Check (!CAPTURE_PINHOLE::Scale (Pinhole, 0, 1, 640, 480), "Scale rejects a zero source");
}

int RunCaptureTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Capture Test Suite ===\n");

   TestEnumerate ();
   TestOpenGuards ();
   TestPinhole ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);
   return (nFailed > 0) ? 1 : 0;
}
