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

#include "basis/Basis.h"

#include <basisu/transcoder/basisu_transcoder.h>

#include <cstring>

namespace SNEEZE
{
   namespace BASIS
   {
      bool Available ()
      {
         // Builds the transcoder's global lookup tables on first call; a no-op
         // on subsequent calls. Referencing it here is also what pulls the
         // basisu_transcoder static library into the link.
         basist::basisu_transcoder_init ();
         return true;
      }

      const char* Format_Anari (FORMAT eFormat)
      {
         const char* szResult = nullptr;

         switch (eFormat)
         {
            case kFORMAT_BC7:        szResult = "BC7";           break;
            case kFORMAT_ASTC_4x4:   szResult = "ASTC_4x4";      break;
            case kFORMAT_ETC2_RGBA8: szResult = "ETC2_R8G8B8A8"; break;
            default:                 szResult = nullptr;         break;
         }

         return szResult;
      }

      FORMAT Target_Choose (const char* szDeviceFormat)
      {
         FORMAT eResult = kFORMAT_None;

         // Preference order: BC7 (desktop) > ASTC (mobile, high quality) >
         // ETC2 (broad mobile baseline). szDeviceFormat is the comma-joined
         // list of ANARI format tokens the GPU actually supports.
         if (szDeviceFormat != nullptr)
         {
            if (std::strstr (szDeviceFormat, "BC7") != nullptr)
               eResult = kFORMAT_BC7;
            else if (std::strstr (szDeviceFormat, "ASTC_4x4") != nullptr)
               eResult = kFORMAT_ASTC_4x4;
            else if (std::strstr (szDeviceFormat, "ETC2_R8G8B8A8") != nullptr)
               eResult = kFORMAT_ETC2_RGBA8;
         }

         return eResult;
      }

      namespace
      {
         // Maps a transcode target to the basisu transcoder format. Returns
         // false for targets with no direct mapping (None).
         bool Format_Basisu (FORMAT eFormat, basist::transcoder_texture_format& eBasisu)
         {
            bool bResult = true;

            switch (eFormat)
            {
               case kFORMAT_BC7:        eBasisu = basist::transcoder_texture_format::cTFBC7_RGBA;      break;
               case kFORMAT_ASTC_4x4:   eBasisu = basist::transcoder_texture_format::cTFASTC_4x4_RGBA; break;
               case kFORMAT_ETC2_RGBA8: eBasisu = basist::transcoder_texture_format::cTFETC2_RGBA;     break;
               case kFORMAT_RGBA8:      eBasisu = basist::transcoder_texture_format::cTFRGBA32;         break;
               default:                 bResult = false;                                               break;
            }

            return bResult;
         }
      }

      bool Transcode (const std::vector<uint8_t>& aEncoded, FORMAT eFormat, int& nWidth, int& nHeight, std::vector<uint8_t>& aOut)
      {
         bool bResult = false;

         nWidth  = 0;
         nHeight = 0;
         aOut.clear ();

         basist::transcoder_texture_format eBasisu;
         if (!aEncoded.empty ()  &&  Format_Basisu (eFormat, eBasisu))
         {
            // Idempotent; the transcoder's lookup tables must exist before use.
            basist::basisu_transcoder_init ();

            basist::ktx2_transcoder transcoder;
            if (transcoder.init (aEncoded.data (), static_cast<uint32_t> (aEncoded.size ()))
                  &&  transcoder.start_transcoding ())
            {
               // KHR_texture_basisu binds a single 2D image: level 0, layer 0,
               // face 0 (the base mip of the only image).
               basist::ktx2_image_level_info info;
               if (transcoder.get_image_level_info (info, 0, 0, 0))
               {
                  const bool bUncompressed = basist::basis_transcoder_format_is_uncompressed (eBasisu);
                  const uint32_t nUnitCount = bUncompressed
                     ? info.m_orig_width * info.m_orig_height   // pixels
                     : info.m_total_blocks;                     // blocks
                  const uint32_t nUnitBytes = basist::basis_get_bytes_per_block_or_pixel (eBasisu);

                  aOut.resize (static_cast<size_t> (nUnitCount) * nUnitBytes);

                  if (transcoder.transcode_image_level (0, 0, 0, aOut.data (), nUnitCount, eBasisu))
                  {
                     nWidth  = static_cast<int> (info.m_orig_width);
                     nHeight = static_cast<int> (info.m_orig_height);
                     bResult = true;
                  }
                  else
                  {
                     aOut.clear ();
                  }
               }
            }
         }

         return bResult;
      }
   }
}
