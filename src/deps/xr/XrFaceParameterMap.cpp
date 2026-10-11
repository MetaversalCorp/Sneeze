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
//
// Port of OpenNexus3DStudio/src/library/openxrFaceParameterMap.js

#include <XrTracking.h>

#include <cstdio>
#include <sstream>
#include <string>

namespace SNEEZE
{
   static const char* const kFaceKeys[kXR_FACE_PARAMETER_COUNT] = {
      "brow_lowerer_left",
      "brow_lowerer_right",
      "cheek_puff_left",
      "cheek_puff_right",
      "cheek_raiser_left",
      "cheek_raiser_right",
      "cheek_suck_left",
      "cheek_suck_right",
      "chin_raiser_bottom",
      "chin_raiser_top",
      "dimpler_left",
      "dimpler_right",
      "eyes_closed_left",
      "eyes_closed_right",
      "eyes_look_down_left",
      "eyes_look_down_right",
      "eyes_look_left_left",
      "eyes_look_left_right",
      "eyes_look_right_left",
      "eyes_look_right_right",
      "eyes_look_up_left",
      "eyes_look_up_right",
      "inner_brow_raiser_left",
      "inner_brow_raiser_right",
      "jaw_drop",
      "jaw_sideways_left",
      "jaw_sideways_right",
      "jaw_thrust",
      "lid_tightener_left",
      "lid_tightener_right",
      "lip_corner_depressor_left",
      "lip_corner_depressor_right",
      "lip_corner_puller_left",
      "lip_corner_puller_right",
      "lip_funneler_left_bottom",
      "lip_funneler_left_top",
      "lip_funneler_right_bottom",
      "lip_funneler_right_top",
      "lip_pressor_left",
      "lip_pressor_right",
      "lip_pucker_left",
      "lip_pucker_right",
      "lip_stretcher_left",
      "lip_stretcher_right",
      "lip_suck_left_bottom",
      "lip_suck_left_top",
      "lip_suck_right_bottom",
      "lip_suck_right_top",
      "lip_tightener_left",
      "lip_tightener_right",
      "lips_toward",
      "lower_lip_depressor_left",
      "lower_lip_depressor_right",
      "mouth_left",
      "mouth_right",
      "nose_wrinkler_left",
      "nose_wrinkler_right",
      "outer_brow_raiser_left",
      "outer_brow_raiser_right",
      "upper_lid_raiser_left",
      "upper_lid_raiser_right",
      "upper_lip_raiser_left",
      "upper_lip_raiser_right",
      "tongue_out",
      "tongue_left",
      "tongue_right",
      "tongue_up",
      "tongue_down"
   };

   static const char* const kBodyNames[kXR_BODY_UPPER_JOINT_COUNT] = {
      "hips",
      "spine",
      "chest",   // ribs → chest (VRM)
      "upperChest",
      "neck",
      "head",
      "leftShoulder",
      "rightShoulder",
      "leftUpperArm",
      "rightUpperArm",
      "leftLowerArm",
      "rightLowerArm",
      "leftHand",
      "rightHand"
   };

   const char* const* XrFaceParameterWebXrKeys () { return kFaceKeys; }
   int XrFaceParameterWebXrKeyCount () { return kXR_FACE_PARAMETER_COUNT; }

   const char* const* XrBodyUpperJointHumanoidNames () { return kBodyNames; }
   int XrBodyUpperJointHumanoidNameCount () { return kXR_BODY_UPPER_JOINT_COUNT; }

   static void AppendEscaped (std::ostringstream& os, const char* s)
   {
      os << '"';
      for (const char* p = s; *p; ++p) {
         if (*p == '"' || *p == '\\')
            os << '\\';
         os << *p;
      }
      os << '"';
   }

   std::string XrFaceStateToWebXrWeightsJson (const XR_FACE_STATE& face, bool includeZero)
   {
      std::ostringstream os;
      os << '{';
      bool first = true;
      for (int i = 0; i < kXR_FACE_PARAMETER_COUNT; ++i) {
         const float v = face.aParameters[static_cast<size_t> (i)];
         if (!includeZero && v == 0.f)
            continue;
         if (!first)
            os << ',';
         first = false;
         AppendEscaped (os, kFaceKeys[i]);
         os << ':';
         char buf[32];
         std::snprintf (buf, sizeof (buf), "%.6g", static_cast<double> (v));
         os << buf;
      }
      os << '}';
      return os.str ();
   }

   std::string XrBodyStateToJson (const XR_BODY_STATE& body)
   {
      std::ostringstream os;
      os << "{\"valid\":" << (body.bValid ? "true" : "false")
         << ",\"jointCount\":" << body.nJointCount
         << ",\"joints\":[";
      const int n = body.nJointCount > 0 ? body.nJointCount : kXR_BODY_UPPER_JOINT_COUNT;
      for (int i = 0; i < n && i < kXR_BODY_UPPER_JOINT_COUNT; ++i) {
         if (i)
            os << ',';
         const XR_BODY_JOINT& j = body.aJoints[static_cast<size_t> (i)];
         os << "{\"name\":";
         AppendEscaped (os, kBodyNames[i]);
         os << ",\"valid\":" << (j.bValid ? "true" : "false")
            << ",\"pos\":[" << j.fPosX << ',' << j.fPosY << ',' << j.fPosZ << ']'
            << ",\"ori\":[" << j.fOriX << ',' << j.fOriY << ',' << j.fOriZ << ',' << j.fOriW << "]}";
      }
      os << "]}";
      return os.str ();
   }

   std::string XrTrackingPayloadJson (const XR_FACE_STATE& face, const XR_BODY_STATE& body,
                                      int64_t nEpochMs, const char* szSource)
   {
      std::ostringstream os;
      os << "{\"source\":";
      AppendEscaped (os, szSource ? szSource : "openxr");
      os << ",\"t\":" << nEpochMs;
      if (face.bValid || face.bTracking) {
         os << ",\"openxrParameters\":[";
         for (int i = 0; i < kXR_FACE_PARAMETER_COUNT; ++i) {
            if (i)
               os << ',';
            char buf[32];
            std::snprintf (buf, sizeof (buf), "%.6g",
               static_cast<double> (face.aParameters[static_cast<size_t> (i)]));
            os << buf;
         }
         os << "],\"weights\":" << XrFaceStateToWebXrWeightsJson (face, false);
      }
      os << ",\"body\":" << XrBodyStateToJson (body) << '}';
      return os.str ();
   }

   static bool ParseFloatArrayAfterKey (const std::string& json, const char* key, float* out, int maxCount)
   {
      const std::string needle = std::string ("\"") + key + "\":";
      const size_t pos = json.find (needle);
      if (pos == std::string::npos)
         return false;
      size_t i = pos + needle.size ();
      while (i < json.size () && json[i] != '[')
         ++i;
      if (i >= json.size ())
         return false;
      ++i;
      int idx = 0;
      while (idx < maxCount && i < json.size ()) {
         while (i < json.size () && (json[i] < '0' || json[i] > '9') && json[i] != '-' && json[i] != '.')
            ++i;
         if (i >= json.size () || json[i] == ']')
            break;
         size_t end = i;
         while (end < json.size () && (std::isdigit (static_cast<unsigned char> (json[end])) || json[end] == '.' || json[end] == '-' || json[end] == 'e' || json[end] == 'E' || json[end] == '+'))
            ++end;
         try {
            out[idx++] = std::stof (json.substr (i, end - i));
         } catch (...) {
            break;
         }
         i = end;
         while (i < json.size () && json[i] != ',' && json[i] != ']')
            ++i;
         if (i < json.size () && json[i] == ',')
            ++i;
      }
      return idx > 0;
   }

   static void ApplyWeightKeyToFace (const std::string& json, XR_FACE_STATE& face)
   {
      for (int k = 0; k < kXR_FACE_PARAMETER_COUNT; ++k) {
         const std::string key = std::string ("\"") + kFaceKeys[k] + "\":";
         const size_t pos = json.find (key);
         if (pos == std::string::npos)
            continue;
         size_t i = pos + key.size ();
         while (i < json.size () && (json[i] == ' ' || json[i] == '\t'))
            ++i;
         size_t end = i;
         while (end < json.size () && (std::isdigit (static_cast<unsigned char> (json[end])) || json[end] == '.' || json[end] == '-'))
            ++end;
         try {
            const float v = std::stof (json.substr (i, end - i));
            face.aParameters[static_cast<size_t> (k)] = v;
            face.bValid = true;
            face.bTracking = true;
         } catch (...) {
            /* skip */
         }
      }
   }

   bool XrTrackingPayloadParse (const std::string& json, XR_FACE_STATE& outFace, XR_BODY_STATE& outBody)
   {
      outFace = {};
      outBody = {};
      float params[kXR_FACE_PARAMETER_COUNT] {};
      if (ParseFloatArrayAfterKey (json, "openxrParameters", params, kXR_FACE_PARAMETER_COUNT)) {
         outFace.bValid = true;
         outFace.bTracking = true;
         for (int i = 0; i < kXR_FACE_PARAMETER_COUNT; ++i)
            outFace.aParameters[static_cast<size_t> (i)] = params[i];
      } else {
         ApplyWeightKeyToFace (json, outFace);
      }
      if (json.find ("\"body\"") != std::string::npos) {
         outBody.bValid = true;
         outBody.nJointCount = kXR_BODY_UPPER_JOINT_COUNT;
         for (int j = 0; j < kXR_BODY_UPPER_JOINT_COUNT; ++j) {
            outBody.aJoints[static_cast<size_t> (j)].bValid = true;
            outBody.aJoints[static_cast<size_t> (j)].fOriW = 1.f;
         }
      }
      return outFace.bValid || outFace.bTracking || outBody.bValid;
   }
}
