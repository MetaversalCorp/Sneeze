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

#ifndef SNEEZE_CAPTURE_H
#define SNEEZE_CAPTURE_H

#include <cstdint>
#include <string>
#include <vector>

namespace SNEEZE
{
   class ENGINE;

   namespace DEP
   {
      // CAPTURE -- live video from OS camera devices. Named apart from the
      // 3D view CAMERA on VIEWPORT. One per ENGINE. Platform backends:
      // Media Foundation (Windows), AVFoundation (macOS/iOS), V4L2 (Linux),
      // Camera2 NDK (Android, including Quest). Frames are straight-alpha
      // RGBA8, top-down. UI_RENDER premultiplies when sampling into RmlUi.
      class CAPTURE
      {
      public:
         explicit CAPTURE (ENGINE* pEngine);
         ~CAPTURE ();

         bool Initialize ();

         int         Device_Count () const;
         std::string Device_Name  (int nIndex) const;

         bool Device_Open   (int nIndex);
         void Device_Close  (int nIndex);
         bool Device_IsOpen (int nIndex) const;

         // Copies the latest frame. nFrameIx increments each new sample so a
         // consumer can skip redraws. Returns false if the device is closed
         // or no sample has arrived yet (permission pending, warming up).
         bool Frame_Latest (int nIndex, int& nWidth, int& nHeight, std::vector<uint8_t>& aRgba, uint64_t& nFrameIx);

      private:
         class Impl;
         Impl* m_pImpl;
      };
   } // namespace DEP
}

#endif // SNEEZE_CAPTURE_H
