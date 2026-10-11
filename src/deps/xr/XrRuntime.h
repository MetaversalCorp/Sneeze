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

#ifndef SNEEZE_XR_RUNTIME_H
#define SNEEZE_XR_RUNTIME_H

#include <XrTracking.h>

#include <string>

namespace SNEEZE
{
   class ENGINE;

   namespace DEP
   {
      // XR_RUNTIME exposes the OpenXR runtime to the engine. The implementation
      // is selected at CMake-configure time: when SNEEZE_ENABLE_XR is ON the
      // real OpenXR loader lives in XrRuntime.cpp; otherwise XrRuntime_Stub.cpp
      // provides a no-op stub (Initialize succeeds with HasRuntime () == false).
      // Either way the header is openxr-free so consumers don't need the SDK.
      //
      // Galaxy XR / Android XR: enable XR_ANDROID_face_tracking and
      // XR_ANDROIDSYS_body_tracking when enumerated; session + trackers require
      // an Android graphics binding (see BeginAndroidSession). Desktop hosts use
      // InjectFaceFixture / InjectBodyFixture for CI and DGX QA.
      class XR_RUNTIME
      {
      public:
         XR_RUNTIME (ENGINE* pEngine);
         ~XR_RUNTIME ();

         bool Initialize ();

         bool        HasRuntime () const;
         std::string GetRuntimeName () const;

         // Phase 0 — extension probe (populated during Initialize when possible).
         XR_CAPABILITIES Capabilities () const;
         void            RefreshExtensionProbe ();

         // Desktop / CI — drive face/body without a headset.
         void InjectFaceFixture (const XR_FACE_STATE& face);
         void InjectBodyFixture (const XR_BODY_STATE& body);
         void ClearFixtures ();

         // Latest face/body (fixture or live Android session).
         bool PollFace (XR_FACE_STATE& outFace) const;
         bool PollBody (XR_BODY_STATE& outBody) const;

         // Android XR session lifecycle (no-ops / false on non-Android builds).
         // pAndroidAppVm / pAndroidActivity are JNI jobject pointers (JavaVM*, jobject).
         // pNativeWindow is ANativeWindow* (may be null for tracking-only PBuffer path).
         bool BeginAndroidSession (void* pJavaVM, void* pActivity, void* pNativeWindow);
         void EndAndroidSession ();
         bool PumpAndroidTracking (); // poll trackers into cached state

         // Avatar bind hint for Space-Time Host (SCENE node name / URL fragment).
         void        SetAvatarBindId (const std::string& sId);
         std::string AvatarBindId () const;
         void        SetBoundMorphWeightsJson (const std::string& json);
         std::string BoundMorphWeightsJson () const;

      private:
         class Impl;
         Impl* m_pImpl;

         friend bool XrAndroid_BeginSession (Impl* impl, void* pJavaVM, void* pActivity, void* pNativeWindow);
         friend void XrAndroid_EndSession (Impl* impl);
         friend bool XrAndroid_PumpTracking (Impl* impl);
      };
   } // namespace DEP
}

#endif // SNEEZE_XR_RUNTIME_H
