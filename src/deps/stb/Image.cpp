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

#include <Image.h>

#include "stb_image.h"

#include <webp/decode.h>

namespace SNEEZE
{
   namespace IMAGE
   {
      // A WebP file is a RIFF container: bytes 0-3 spell "RIFF", bytes 8-11
      // spell "WEBP". stb_image has no WebP decoder, so detect the header here
      // and route those bytes to libwebp instead.
      static bool IsWebP (const std::vector<uint8_t>& aEncoded)
      {
         return aEncoded.size () >= 12
             && aEncoded[0] == 'R'  &&  aEncoded[1] == 'I'  &&  aEncoded[2]  == 'F'  &&  aEncoded[3]  == 'F'
             && aEncoded[8] == 'W'  &&  aEncoded[9] == 'E'  &&  aEncoded[10] == 'B'  &&  aEncoded[11] == 'P';
      }

      bool Decode (const std::vector<uint8_t>& aEncoded, int& nWidth, int& nHeight, std::vector<uint8_t>& aPixels)
      {
         bool bResult = false;

         nWidth  = 0;
         nHeight = 0;
         aPixels.clear ();

         if (!aEncoded.empty ())
         {
            int nW = 0, nH = 0;

            if (IsWebP (aEncoded))
            {
               // WebPDecodeRGBA returns tightly-packed, top-to-bottom RGBA --
               // the same layout stb_image produces with req_comp = 4 -- so the
               // downstream contract (RGBA, row-major, no padding) is identical.
               uint8_t* pWebP = WebPDecodeRGBA (aEncoded.data (), aEncoded.size (), &nW, &nH);

               if (pWebP)
               {
                  nWidth  = nW;
                  nHeight = nH;
                  aPixels.assign (pWebP, pWebP + (static_cast<size_t> (nW) * static_cast<size_t> (nH) * 4));

                  WebPFree (pWebP);

                  bResult = true;
               }
            }
            else
            {
               int nChannels = 0;

               unsigned char* pPixels = stbi_load_from_memory (aEncoded.data (), static_cast<int> (aEncoded.size ()), &nW, &nH, &nChannels, 4);

               if (pPixels)
               {
                  nWidth  = nW;
                  nHeight = nH;
                  aPixels.assign (pPixels, pPixels + (static_cast<size_t> (nW) * static_cast<size_t> (nH) * 4));

                  stbi_image_free (pPixels);

                  bResult = true;
               }
            }
         }

         return bResult;
      }
   }
}
