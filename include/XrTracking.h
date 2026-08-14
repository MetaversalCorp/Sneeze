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

#ifndef SNEEZE_XR_TRACKING_H
#define SNEEZE_XR_TRACKING_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SNEEZE
{
   // OpenXR-free tracking PODs for hosts (Space-Time Host, Android XR APK).
   // Face indices match XR_ANDROID_face_tracking / XrFaceParameterIndicesANDROID (68).
   // Body joints match XR_ANDROIDSYS_body_tracking upper-body set (14).

   static constexpr int kXR_FACE_PARAMETER_COUNT = 68;
   static constexpr int kXR_BODY_UPPER_JOINT_COUNT = 14;

   struct XR_FACE_STATE
   {
      bool     bValid = false;
      bool     bTracking = false;
      int64_t  nSampleTimeNs = 0;
      std::array<float, kXR_FACE_PARAMETER_COUNT> aParameters {};
   };

   struct XR_BODY_JOINT
   {
      bool  bValid = false;
      float fPosX = 0.f, fPosY = 0.f, fPosZ = 0.f;
      float fOriX = 0.f, fOriY = 0.f, fOriZ = 0.f, fOriW = 1.f;
   };

   struct XR_BODY_STATE
   {
      bool     bValid = false;
      int64_t  nSampleTimeNs = 0;
      int      nJointCount = 0;
      std::array<XR_BODY_JOINT, kXR_BODY_UPPER_JOINT_COUNT> aJoints {};
   };

   struct XR_CAPABILITIES
   {
      bool bHasRuntime = false;
      bool bExtensionFaceAndroid = false;
      bool bExtensionBodyAndroidSys = false;
      bool bExtensionHandExt = false;
      bool bSessionActive = false;
      bool bFaceTrackerActive = false;
      bool bBodyTrackerActive = false;
      bool bFixtureMode = false;
      std::string sRuntimeName;
      std::vector<std::string> aEnumeratedExtensions;
   };

   // WebXR Expression Tracking draft keys — same order as OpenNexus
   // openxrFaceParameterMap.js / OPENXR_ANDROID_FACE_PARAMETER_WEBXR_KEYS.
   const char* const* XrFaceParameterWebXrKeys ();
   int                XrFaceParameterWebXrKeyCount ();

   // Humanoid bone names for ANDROIDSYS upper-body joints (VRM-style).
   const char* const* XrBodyUpperJointHumanoidNames ();
   int                XrBodyUpperJointHumanoidNameCount ();

   // Dense OpenXR face floats → sparse WebXR-key JSON object body (no outer braces).
   // includeZero=false omits zero weights.
   std::string XrFaceStateToWebXrWeightsJson (const XR_FACE_STATE& face, bool includeZero = false);

   // Body joints → JSON array of { name, valid, pos:[x,y,z], ori:[x,y,z,w] }.
   std::string XrBodyStateToJson (const XR_BODY_STATE& body);

   // Combined nativeFaceBridge / Space-Time payload:
   // { "source":"openxr", "t":ms, "openxrParameters":[68], "weights":{...}, "body":{...} }
   std::string XrTrackingPayloadJson (const XR_FACE_STATE& face, const XR_BODY_STATE& body,
                                      int64_t nEpochMs, const char* szSource = "openxr");
}

#endif // SNEEZE_XR_TRACKING_H
