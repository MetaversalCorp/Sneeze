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

#ifndef SNEEZE_IMAGE_H
#define SNEEZE_IMAGE_H

namespace SNEEZE
{
   namespace IMAGE
   {
      // Decodes an encoded image (PNG, JPEG, BMP, GIF, etc.) held in memory into
      // 8-bit RGBA pixels, row-major and top-to-bottom, 4 bytes per pixel.
      // On success fills nWidth, nHeight, and aPixels and returns true.
      // On failure leaves aPixels empty, sets the dimensions to zero, returns false.
      bool Decode (const std::vector<uint8_t>& aEncoded, int& nWidth, int& nHeight, std::vector<uint8_t>& aPixels);
   }
}

#endif // SNEEZE_IMAGE_H
