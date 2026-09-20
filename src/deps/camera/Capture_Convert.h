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

#ifndef SNEEZE_CAPTURE_CONVERT_H
#define SNEEZE_CAPTURE_CONVERT_H

#include <cstdint>
#include <vector>

namespace SNEEZE
{
   namespace DEP
   {
      namespace CAPTURE_CONVERT
      {
         // nStride is bytes per source row. Negative stride means bottom-up
         // (Windows RGB32). Output is always straight RGBA8, top-down, A=255.
         void Bgra (const uint8_t* pSrc, int nWidth, int nHeight, int nStride, std::vector<uint8_t>& aRgba);
         void Rgb  (const uint8_t* pSrc, int nWidth, int nHeight, int nStride, std::vector<uint8_t>& aRgba);
         void Yuy2 (const uint8_t* pSrc, int nWidth, int nHeight, int nStride, std::vector<uint8_t>& aRgba);
         void Nv12 (const uint8_t* pY, int nStrideY, const uint8_t* pUv, int nStrideUv, int nWidth, int nHeight, std::vector<uint8_t>& aRgba);
         void Yuv420 (const uint8_t* pY, int nStrideY, const uint8_t* pU, int nStrideU, int nPixelU,
                      const uint8_t* pV, int nStrideV, int nPixelV, int nWidth, int nHeight, std::vector<uint8_t>& aRgba);
      }
   } // namespace DEP
}

#endif // SNEEZE_CAPTURE_CONVERT_H
