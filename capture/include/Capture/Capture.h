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

#ifndef CAPTURE_CAPTURE_H
#define CAPTURE_CAPTURE_H

#include <cstdint>
#include <string>
#include <vector>

namespace Capture
{
   enum eLOG
   {
      kLOG_Info    = 0,
      kLOG_Warning = 1,
      kLOG_Error   = 2,
   };

   typedef void (*LOG_FN) (eLOG eLevel, const char* szMessage, void* pUser);

   // Live video from OS camera devices. Platform backends: Media Foundation
   // (Windows), AVFoundation (macOS/iOS), V4L2 (Linux), Camera2 NDK (Android,
   // including Quest). Frames are straight-alpha RGBA8, top-down. The host
   // application still declares OS camera permissions; this library cannot.
   class CAPTURE
   {
   public:
      explicit CAPTURE (LOG_FN fnLog = nullptr, void* pUser = nullptr);
      ~CAPTURE ();

      bool Initialize ();

      int         Device_Count () const;
      std::string Device_Name  (int nIndex) const;

      bool Device_Open   (int nIndex);
      void Device_Close  (int nIndex);
      bool Device_IsOpen (int nIndex) const;

      enum eORIENTATION
      {
         kORIENTATION_TOP_LEFT      = 1,
         kORIENTATION_TOP_RIGHT     = 2,
         kORIENTATION_BOTTOM_RIGHT  = 3,
         kORIENTATION_BOTTOM_LEFT   = 4,
         kORIENTATION_LEFT_TOP      = 5,
         kORIENTATION_RIGHT_TOP     = 6,
         kORIENTATION_RIGHT_BOTTOM  = 7,
         kORIENTATION_LEFT_BOTTOM   = 8,
      };

      static const char* Orientation_Name (eORIENTATION eOrientation);

      enum eMODEL
      {
         kMODEL_PINHOLE        = 1,
         kMODEL_OPENCV         = 2,
         kMODEL_OPENCV_FISHEYE = 3,
      };

      static const char* Model_Name (eMODEL eModel);

      struct INTRINSICS
      {
         eMODEL       eModel       = kMODEL_PINHOLE;
         int          nWidth       = 0;
         int          nHeight      = 0;
         eORIENTATION eOrientation = kORIENTATION_TOP_LEFT;
         double       dFx          = 0.0;
         double       dFy          = 0.0;
         double       dCx          = 0.0;
         double       dCy          = 0.0;
      };

      bool Device_Intrinsics (int nIndex, INTRINSICS& Intrinsics) const;

      bool Frame_Latest (int nIndex, int& nWidth, int& nHeight, std::vector<uint8_t>& aRgba, uint64_t& nFrameIx);

   private:
      class Impl;
      Impl* m_pImpl;
   };
}

#endif // CAPTURE_CAPTURE_H
