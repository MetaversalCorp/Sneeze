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

#ifndef SNEEZE_BASIS_H
#define SNEEZE_BASIS_H

#include <cstdint>
#include <vector>

namespace SNEEZE
{
   // Thin wrapper around the Basis Universal transcoder. Decodes KTX2 / .basis
   // GPU-supercompressed textures (glTF's KHR_texture_basisu) that stb_image
   // cannot handle. See Basis.md.
   namespace BASIS
   {
      // A transcode target. The compressed entries name a GPU block format the
      // renderer can upload directly (via ANARI's compressedImage2D sampler);
      // kFORMAT_RGBA8 is the uncompressed fallback for GPUs that support none.
      enum FORMAT
      {
         kFORMAT_None = 0,
         kFORMAT_RGBA8,
         kFORMAT_BC7,
         kFORMAT_ASTC_4x4,
         kFORMAT_ETC2_RGBA8,
      };

      // Initializes the transcoder's lookup tables (idempotent) and reports
      // whether the transcoder is usable. Safe to call more than once.
      bool Available ();

      // The ANARI compressedImage2D 'format' string for a compressed FORMAT
      // (e.g. "BC7"), or nullptr for kFORMAT_None / kFORMAT_RGBA8.
      const char* Format_Anari (FORMAT eFormat);

      // Picks the best transcode target from a device's advertised compressed
      // formats -- the comma-separated ANARI format strings reported by the
      // "halogen.textureFormats" property. Returns kFORMAT_None when none match.
      FORMAT Target_Choose (const char* szDeviceFormat);

      // Transcodes a KTX2 / .basis blob to eFormat. On success fills
      // nWidth/nHeight/aOut (compressed blocks, or RGBA8 for kFORMAT_RGBA8) and
      // returns true; on failure clears them and returns false. STUB: not yet
      // implemented -- always returns false.
      bool Transcode (const std::vector<uint8_t>& aEncoded, FORMAT eFormat, int& nWidth, int& nHeight, std::vector<uint8_t>& aOut);
   }
}
#endif // SNEEZE_BASIS_H
