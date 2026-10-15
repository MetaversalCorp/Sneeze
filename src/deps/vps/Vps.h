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

#ifndef SNEEZE_VPS_H
#define SNEEZE_VPS_H

#include <string>
#include <vector>

namespace SNEEZE
{
   class ENGINE;
   class VIEWPORT;

   namespace DEP
   {
      // VPS is the engine's visual-positioning service. Callers see a pose in
      // the Sneeze frame (Z-up, identity looks along +X). A device behind the
      // interface talks to one backend. Placeframe is the first device.
      class VPS
      {
      public:
         struct POSE
         {
            double aPosition[3] = { 0.0, 0.0, 0.0 };
            double aRotation[4] = { 0.0, 0.0, 0.0, 1.0 };
            double dFovY        = 0.0;
            double dConfidence  = 0.0;
            bool   bValid       = false;
         };

         struct CONFIG
         {
            std::string              sUrl;
            std::string              sToken;
            std::string              sUser;
            std::string              sPassword;
            std::vector<std::string> asMap;
            int                      nDevice   = 0;
            double                   dInterval = 2.0;
         };

         struct MAP
         {
            std::string sId;
            std::string sName;
         };

         explicit VPS (ENGINE* pEngine);
         ~VPS ();

         bool Initialize ();

         void Tick (VIEWPORT* pViewport);
         POSE Pose () const;
         bool IsRunning () const;

         // Last localize result: "pose", an error string, or empty before the
         // first answer.
         std::string Note () const;

         bool Maps_Ready () const;
         void Maps (std::vector<MAP>& aMap) const;
         void Map_Select (const std::string& sId);
         std::string Map_Selected () const;

         // Leave localization off. Tick does not read the camera or move the view
         // until a map is selected again.
         void Skip ();
         bool IsSkipped () const;

         // Fetch a hosted vps.json and apply it. Safe to call from the URL bar.
         void Load_Url (const std::string& sUrl);

      private:
         class Impl;
         Impl* m_pImpl;
      };

      // OpenCV camera pose (X right, Y down, Z forward) into the Sneeze camera
      // pose. Position and quaternion are both in the map frame the device
      // asked for with axis convention OPENCV.
      bool Pose_From_Opencv (double dX, double dY, double dZ, double dQx, double dQy, double dQz, double dQw, VPS::POSE& Pose);

      // Placeframe POST /localize body: a JSON array of map localizations.
      // The first entry's camera_from_map_transform is converted with
      // Pose_From_Opencv. confidence_tight is copied when present.
      bool Pose_From_Placeframe (const std::string& sJson, VPS::POSE& Pose);
   }
}

#endif
