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

#include "vps/Vps.h"

#include <cmath>
#include <cstdio>
#include <string>

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

static bool Near (double dA, double dB)
{
   return std::fabs (dA - dB) < 1e-6;
}

static void TestOpencvIdentity ()
{
   SNEEZE::DEP::VPS::POSE Pose;
   bool bOk = SNEEZE::DEP::Pose_From_Opencv (1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0, Pose);

   Check (bOk, "OpenCV identity converts");
   Check (Pose.bValid, "OpenCV identity is a valid pose");
   Check (Near (Pose.aPosition[0], 3.0)  &&  Near (Pose.aPosition[1], 1.0)  &&  Near (Pose.aPosition[2], -2.0), "OpenCV position maps to Z-up");
   Check (Near (Pose.aRotation[0], 0.0)  &&  Near (Pose.aRotation[1], 0.0)  &&  Near (Pose.aRotation[2], 0.0)  &&  Near (Pose.aRotation[3], 1.0), "OpenCV identity rotation is Sneeze identity");
}

static void TestPlaceframeJson ()
{
   const char* szJson =
      "[{\"id\":\"00000000-0000-0000-0000-000000000001\","
      "\"camera_from_map_transform\":{\"translation\":{\"x\":1,\"y\":2,\"z\":3},\"rotation\":{\"x\":0,\"y\":0,\"z\":0,\"w\":1}},"
      "\"map_transform\":{\"translation\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"x\":0,\"y\":0,\"z\":0,\"w\":1}},"
      "\"metrics\":{\"confidence_tight\":0.8}}]";

   SNEEZE::DEP::VPS::POSE Pose;
   bool bOk = SNEEZE::DEP::Pose_From_Placeframe (szJson, Pose);

   Check (bOk  &&  Pose.bValid, "Placeframe JSON yields a pose");
   Check (Near (Pose.dConfidence, 0.8), "Placeframe confidence_tight is kept");
   Check (Near (Pose.aPosition[0], 3.0)  &&  Near (Pose.aPosition[1], 1.0)  &&  Near (Pose.aPosition[2], -2.0), "Placeframe translation uses the OpenCV map");
   const char* szNullConfidence =
      "[{\"camera_from_map_transform\":{\"translation\":{\"x\":1,\"y\":2,\"z\":3},\"rotation\":{\"x\":0,\"y\":0,\"z\":0,\"w\":1}},"
      "\"metrics\":{\"confidence_tight\":null}}]";
   Check (SNEEZE::DEP::Pose_From_Placeframe (szNullConfidence, Pose)  &&  Pose.bValid, "null confidence does not drop the pose");
   Check (!SNEEZE::DEP::Pose_From_Placeframe ("[]", Pose), "empty Placeframe array is not a fix");
   Check (!SNEEZE::DEP::Pose_From_Placeframe ("not-json", Pose), "broken Placeframe JSON is not a fix");
}

int RunVpsTests (int, char**)
{
   std::printf ("VPS\n");
   TestOpencvIdentity ();
   TestPlaceframeJson ();
   std::printf ("VPS: %d passed, %d failed\n", nPassed, nFailed);
   return (nFailed > 0) ? 1 : 0;
}
