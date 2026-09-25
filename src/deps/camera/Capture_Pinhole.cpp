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

#include "camera/Capture_Pinhole.h"

#include <cmath>

using namespace SNEEZE::DEP;

namespace
{
   const double kPi = 3.14159265358979323846;
}

void CAPTURE_PINHOLE::Clear (INTRINSICS& Intrinsics)
{
   Intrinsics.eModel       = CAPTURE::kMODEL_PINHOLE;
   Intrinsics.nWidth       = 0;
   Intrinsics.nHeight      = 0;
   Intrinsics.eOrientation = CAPTURE::kORIENTATION_TOP_LEFT;
   Intrinsics.dFx          = 0.0;
   Intrinsics.dFy          = 0.0;
   Intrinsics.dCx          = 0.0;
   Intrinsics.dCy          = 0.0;
}

bool CAPTURE_PINHOLE::Valid (const INTRINSICS& Intrinsics)
{
   bool bValid = false;

   if (Intrinsics.nWidth > 0  &&  Intrinsics.nHeight > 0  &&  Intrinsics.dFx > 0.0  &&  Intrinsics.dFy > 0.0)
      bValid = true;

   return bValid;
}

bool CAPTURE_PINHOLE::Scale (INTRINSICS& Intrinsics, int nSrcW, int nSrcH, int nDstW, int nDstH)
{
   bool bResult = false;

   if (nSrcW > 0  &&  nSrcH > 0  &&  nDstW > 0  &&  nDstH > 0  &&  Intrinsics.dFx > 0.0  &&  Intrinsics.dFy > 0.0)
   {
      const double dSx = static_cast<double> (nDstW) / static_cast<double> (nSrcW);
      const double dSy = static_cast<double> (nDstH) / static_cast<double> (nSrcH);
      Intrinsics.nWidth  = nDstW;
      Intrinsics.nHeight = nDstH;
      Intrinsics.dFx    *= dSx;
      Intrinsics.dFy    *= dSy;
      Intrinsics.dCx    *= dSx;
      Intrinsics.dCy    *= dSy;
      bResult = true;
   }

   return bResult;
}

CAPTURE_PINHOLE::eORIENTATION CAPTURE_PINHOLE::Orientation_From_Sensor (int nDegreesCw)
{
   eORIENTATION eOrientation = CAPTURE::kORIENTATION_TOP_LEFT;
   int nDeg = nDegreesCw % 360;
   if (nDeg < 0)
      nDeg += 360;
   nDeg = ((nDeg + 45) / 90) * 90;
   if (nDeg >= 360)
      nDeg = 0;

   if (nDeg == 90)
      eOrientation = CAPTURE::kORIENTATION_RIGHT_TOP;
   else if (nDeg == 180)
      eOrientation = CAPTURE::kORIENTATION_BOTTOM_RIGHT;
   else if (nDeg == 270)
      eOrientation = CAPTURE::kORIENTATION_LEFT_BOTTOM;

   return eOrientation;
}

bool CAPTURE_PINHOLE::From_Size (INTRINSICS& Intrinsics, int nWidth, int nHeight)
{
   bool bResult = false;

   Clear (Intrinsics);
   if (nWidth > 0  &&  nHeight > 0)
   {
      Intrinsics.nWidth       = nWidth;
      Intrinsics.nHeight      = nHeight;
      Intrinsics.eOrientation = CAPTURE::kORIENTATION_TOP_LEFT;
      Intrinsics.dFx          = static_cast<double> (nWidth);
      Intrinsics.dFy          = static_cast<double> (nWidth);
      Intrinsics.dCx          = static_cast<double> (nWidth)  * 0.5;
      Intrinsics.dCy          = static_cast<double> (nHeight) * 0.5;
      bResult = true;
   }

   return bResult;
}

bool CAPTURE_PINHOLE::From_Hfov (INTRINSICS& Intrinsics, int nWidth, int nHeight, double dHfovDeg)
{
   bool bResult = false;

   Clear (Intrinsics);
   if (nWidth > 0  &&  nHeight > 0  &&  dHfovDeg > 0.0  &&  dHfovDeg < 179.0)
   {
      const double dHalf = dHfovDeg * 0.5 * kPi / 180.0;
      const double dFx   = (static_cast<double> (nWidth) * 0.5) / std::tan (dHalf);
      if (dFx > 0.0)
      {
         Intrinsics.nWidth       = nWidth;
         Intrinsics.nHeight      = nHeight;
         Intrinsics.eOrientation = CAPTURE::kORIENTATION_TOP_LEFT;
         Intrinsics.dFx          = dFx;
         Intrinsics.dFy          = dFx;
         Intrinsics.dCx          = static_cast<double> (nWidth)  * 0.5;
         Intrinsics.dCy          = static_cast<double> (nHeight) * 0.5;
         bResult = true;
      }
   }

   return bResult;
}

bool CAPTURE_PINHOLE::From_Focal_Mm (INTRINSICS& Intrinsics, int nWidth, int nHeight, double dFocalMm, double dSensorWMm, double dSensorHMm)
{
   bool bResult = false;

   Clear (Intrinsics);
   if (nWidth > 0  &&  nHeight > 0  &&  dFocalMm > 0.0  &&  dSensorWMm > 0.0  &&  dSensorHMm > 0.0)
   {
      Intrinsics.nWidth       = nWidth;
      Intrinsics.nHeight      = nHeight;
      Intrinsics.eOrientation = CAPTURE::kORIENTATION_TOP_LEFT;
      Intrinsics.dFx          = (dFocalMm / dSensorWMm) * static_cast<double> (nWidth);
      Intrinsics.dFy          = (dFocalMm / dSensorHMm) * static_cast<double> (nHeight);
      Intrinsics.dCx          = static_cast<double> (nWidth)  * 0.5;
      Intrinsics.dCy          = static_cast<double> (nHeight) * 0.5;
      bResult = Valid (Intrinsics);
      if (!bResult)
         Clear (Intrinsics);
   }

   return bResult;
}

const char* CAPTURE::Orientation_Name (eORIENTATION eOrientation)
{
   const char* szName = "TOP_LEFT";

   switch (eOrientation)
   {
      case kORIENTATION_TOP_RIGHT:     szName = "TOP_RIGHT";     break;
      case kORIENTATION_BOTTOM_RIGHT:  szName = "BOTTOM_RIGHT";  break;
      case kORIENTATION_BOTTOM_LEFT:   szName = "BOTTOM_LEFT";   break;
      case kORIENTATION_LEFT_TOP:      szName = "LEFT_TOP";      break;
      case kORIENTATION_RIGHT_TOP:     szName = "RIGHT_TOP";     break;
      case kORIENTATION_RIGHT_BOTTOM:  szName = "RIGHT_BOTTOM";  break;
      case kORIENTATION_LEFT_BOTTOM:   szName = "LEFT_BOTTOM";   break;
      default:                         break;
   }

   return szName;
}

const char* CAPTURE::Model_Name (eMODEL eModel)
{
   const char* szName = "PINHOLE";

   switch (eModel)
   {
      case kMODEL_OPENCV:          szName = "OPENCV";          break;
      case kMODEL_OPENCV_FISHEYE:  szName = "OPENCV_FISHEYE";  break;
      default:                     break;
   }

   return szName;
}
