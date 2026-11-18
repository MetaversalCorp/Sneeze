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

#include "CameraFrame.h"

#include <cstdio>
#include <string>

static int g_nTests  = 0;
static int g_nPassed = 0;

static void Check (bool bPass, const char* sName)
{
   g_nTests++;

   if (bPass)
   {
      g_nPassed++;
      printf ("  PASS: %s\n", sName);
   }
   else
      printf ("  FAIL: %s\n", sName);
}

static SNEEZE::CAMERA_FRAME Read (const char* sPayload, const char* sUrl)
{
   SNEEZE::CAMERA_FRAME Frame;

   SNEEZE::CameraFrame_Read (Frame, sPayload ? sPayload : "", sUrl ? sUrl : "");

   return Frame;
}

int RunCameraFrameTests (int nArgc, char** aArgv)
{
   (void) nArgc;
   (void) aArgv;

   printf ("\n========================================\n");
   printf ("Camera Frame Tests\n");
   printf ("========================================\n");

   {
      SNEEZE::CAMERA_FRAME Frame = Read ("{}", "");

      Check (Frame.bBounds, "default bounds stay dynamic");
      Check (Frame.bScale && !Frame.bScaleSet, "default scale stays dynamic");
      Check (!Frame.bPosition && !Frame.bRotation, "default pose is unset");
   }

   {
      const char* sPayload =
         "{"
         "\"Data\":{"
         "\"Camera\":{"
         "\"Bounds\":false,"
         "\"Scale\":0.02,"
         "\"Position\":[1,2,3],"
         "\"Rotation\":[0,0,0,1]"
         "}"
         "}"
         "}";

      SNEEZE::CAMERA_FRAME Frame = Read (sPayload, "https://h.com/atc.msf");

      Check (!Frame.bBounds, "Data bounds held");
      Check (Frame.bScaleSet && Frame.dScale > 0.019 && Frame.dScale < 0.021, "Data scale number");
      Check (Frame.bPosition && Frame.aPosition[0] == 1.0 && Frame.aPosition[1] == 2.0 && Frame.aPosition[2] == 3.0, "Data position");
      Check (Frame.bRotation && Frame.aRotation[3] == 1.0, "Data rotation");
   }

   {
      const char* sPayload =
         "{"
         "\"Data\":{"
         "\"camera\":{"
         "\"bBounds\":true,"
         "\"dScale\":false,"
         "\"Position\":[9,9,9]"
         "}"
         "}"
         "}";

      SNEEZE::CAMERA_FRAME Frame = Read (sPayload, "https://h.com/atc.msf?Bounds=false&Scale=1&Position=4,5,6&Rotation=0,0,0,1");

      Check (!Frame.bBounds, "query bounds overrides Data");
      Check (Frame.bScaleSet && Frame.dScale == 1.0, "query scale number overrides Data false");
      Check (Frame.bPosition && Frame.aPosition[0] == 4.0 && Frame.aPosition[1] == 5.0 && Frame.aPosition[2] == 6.0, "query position overrides Data");
      Check (Frame.bRotation && Frame.aRotation[3] == 1.0, "query rotation");
   }

   {
      SNEEZE::CAMERA_FRAME Frame = Read ("{}", "https://h.com/atc.msf?bounds=off&scale=off");

      Check (!Frame.bBounds, "query bounds off");
      Check (!Frame.bScale && !Frame.bScaleSet && Frame.dScale == 1.0, "query scale off is unit scale");
   }

   {
      SNEEZE::CAMERA_FRAME Frame = Read ("{\"Data\":{\"Camera\":{\"Position\":\"1, 2, 3\"}}}", "");

      Check (Frame.bPosition && Frame.aPosition[2] == 3.0, "position string");
      Check (!Frame.bRotation, "rotation stays unset");
   }

   {
      SNEEZE::CAMERA_FRAME Frame = Read ("{\"Data\":{\"Camera\":{\"Position\":[1,2]}}}", "https://h.com/atc.msf?Rotation=1,2,3");

      Check (!Frame.bPosition, "short position is ignored");
      Check (!Frame.bRotation, "short rotation is ignored");
   }

   {
      double dScale = SNEEZE::CameraFrame_Scale (SNEEZE::CAMERA_FRAME (), 10.0, 5.0, 1e-6);

      Check (dScale > 0.49 && dScale < 0.51, "dynamic scale is target over reach");
   }

   {
      SNEEZE::CAMERA_FRAME Frame;

      Frame.bScale    = false;
      Frame.bScaleSet = false;

      Check (SNEEZE::CameraFrame_Scale (Frame, 10.0, 5.0, 1e-6) == 1.0, "held scale is 1");

      Frame.bScaleSet = true;
      Frame.dScale    = 0.25;

      Check (SNEEZE::CameraFrame_Scale (Frame, 10.0, 5.0, 1e-6) == 0.25, "explicit scale wins over reach");
   }

   printf ("\n========================================\n");
   printf ("Camera Frame Tests: %d/%d passed\n", g_nPassed, g_nTests);
   printf ("========================================\n");

   return (g_nPassed == g_nTests) ? 0 : 1;
}
