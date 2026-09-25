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

#ifndef SNEEZE_CAPTURE_PLATFORM_H
#define SNEEZE_CAPTURE_PLATFORM_H

#include "camera/Capture.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SNEEZE
{
   namespace DEP
   {
      namespace CAPTURE_PLATFORM
      {
         struct INFO
         {
            std::string sName;
         };

         struct FRAME
         {
            int                  nWidth   = 0;
            int                  nHeight  = 0;
            uint64_t             nFrameIx = 0;
            std::vector<uint8_t> aRgba;
         };

         bool     Startup ();
         void     Shutdown ();
         void     Enumerate (std::vector<INFO>& aInfo);
         uint32_t Open (int nIndex);
         void     Close (uint32_t nHandle);
         bool     Latest (uint32_t nHandle, FRAME& Frame);
         bool     Intrinsics (uint32_t nHandle, CAPTURE::INTRINSICS& Intrinsics);
      }
   } // namespace DEP
}

#endif // SNEEZE_CAPTURE_PLATFORM_H
