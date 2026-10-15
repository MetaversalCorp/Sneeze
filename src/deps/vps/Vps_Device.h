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

#ifndef SNEEZE_VPS_DEVICE_H
#define SNEEZE_VPS_DEVICE_H

#include "camera/Capture.h"
#include "vps/Vps.h"

#include <string>
#include <vector>

namespace SNEEZE
{
   class ENGINE;

   namespace DEP
   {
      class IVPS_DEVICE
      {
      public:
         virtual ~IVPS_DEVICE () = default;

         virtual bool Localize (const CAPTURE::INTRINSICS& Intrinsics, const std::vector<uint8_t>& aRgba, int nWidth, int nHeight, VPS::POSE& Pose, std::string& sError) = 0;
         virtual bool Catalog (std::vector<VPS::MAP>& aMap, std::string& sError) = 0;
         virtual void Maps (const std::vector<std::string>& asMap) = 0;
      };

      IVPS_DEVICE* Device_Placeframe_New (ENGINE* pEngine, const VPS::CONFIG& Config);
   }
}

#endif
