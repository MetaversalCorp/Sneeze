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

#ifndef CAPTURE_PINHOLE_H
#define CAPTURE_PINHOLE_H

#include <Capture/Capture.h>

namespace Capture
{
      namespace CAPTURE_PINHOLE
      {
         using INTRINSICS   = CAPTURE::INTRINSICS;
         using eORIENTATION = CAPTURE::eORIENTATION;

         void Clear (INTRINSICS& Intrinsics);
         bool Valid (const INTRINSICS& Intrinsics);

         // Independent axis scale, matching placeframe canonicalize_intrinsics
         // resize (orientation and eModel are left unchanged).
         bool Scale (INTRINSICS& Intrinsics, int nSrcW, int nSrcH, int nDstW, int nDstH);

         eORIENTATION Orientation_From_Sensor (int nDegreesCw);

         bool From_Size     (INTRINSICS& Intrinsics, int nWidth, int nHeight);
         bool From_Hfov     (INTRINSICS& Intrinsics, int nWidth, int nHeight, double dHfovDeg);
         bool From_Focal_Mm (INTRINSICS& Intrinsics, int nWidth, int nHeight, double dFocalMm, double dSensorWMm, double dSensorHMm);
      }
}

#endif // CAPTURE_PINHOLE_H
