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

#ifndef SNEEZE_CAMERAFRAME_H
#define SNEEZE_CAMERAFRAME_H

#include <string>

namespace SNEEZE
{
   class VIEWPORT;

   // -----------------------------------------------------------------------
   // How the primary fabric wants the camera and the render scale held.
   //
   // Absent every key, the compositor keeps today's behaviour: one render
   // scale derived from the live root-anchored bounds, and an active scene
   // pose re-seeded from that scale each frame.
   //
   // The same keys are read from the primary fabric's Data.Camera object and
   // from the query string of the URL that opened it. A query key overrides
   // the same Data key. Names are case-insensitive. Accepted names are
   // Bounds (bBounds), Scale (dScale), Position, and Rotation.
   //
   //   Bounds    false / 0 / off     do not re-seed the camera from the live
   //                                 bounds. This is the motion that lifts the
   //                                 eye and pitches it down as the sphere
   //                                 grows. The default elevated orbit is left
   //                                 where it is unless Position / Rotation
   //                                 replace it.
   //             true / 1 / on       keep re-seeding (the default).
   //   Scale     false / 0 / off     do not resize the scene to the live
   //                                 bounds; render scale stays 1 (metres).
   //             <positive number>   use that render scale instead.
   //             true / on / auto    keep deriving the scale (the default).
   //   Position  x,y,z               camera position in world metres.
   //   Rotation  x,y,z,w             camera orientation quaternion. Identity
   //                                 looks along +X in the Z-up world.
   //
   // Either position or rotation may be set alone. The node-tree walk is not
   // part of this; it still composes every node's transform the same way.
   // In an OpenXR session, Position / Rotation with Bounds off place the rig
   // once (the HMD still tracks from there; look is kept level, so only the
   // heading of Rotation applies). With no explicit frame the spawn pose is
   // still not applied to the headset.
   // -----------------------------------------------------------------------

   struct CAMERA_FRAME
   {
      bool   bBounds   = true;
      bool   bScale    = true;
      bool   bScaleSet = false;
      double dScale    = 1.0;
      bool   bPosition = false;
      bool   bRotation = false;
      double aPosition[3] = { 0.0, 0.0, 0.0 };
      double aRotation[4] = { 0.0, 0.0, 0.0, 1.0 };
   };

   // sPayload is the primary fabric's JSON payload. sUrl is the URL that
   // opened it, query included. Starts from Frame as given and overlays keys.
   void CameraFrame_Read (CAMERA_FRAME& Frame, const std::string& sPayload, const std::string& sUrl);

   // Overlay Position / Rotation onto the viewport camera when either was set.
   void CameraFrame_Bind (const CAMERA_FRAME& Frame, VIEWPORT* pViewport);

   // Render scale for this frame. dTargetExtent / dMaxReach when the fabric
   // left scaling dynamic; otherwise the held scale.
   double CameraFrame_Scale (const CAMERA_FRAME& Frame, double dMaxReach, double dTargetExtent, double dMinReach);

   // Re-seed the orbit from an active scene pose. While Bounds is dynamic this
   // happens every frame, so the pose tracks the settling scale. While Bounds
   // is held it happens once, then bHeld stops further writes. User input
   // deactivates the pose before this is called, so a held frame still lets
   // the viewer take over.
   void CameraFrame_Pose (const CAMERA_FRAME& Frame, VIEWPORT* pViewport, double dRenderScale, double dMaxReach, double dMinReach, bool& bHeld);
}

#endif // SNEEZE_CAMERAFRAME_H
