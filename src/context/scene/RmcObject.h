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

#ifndef SNEEZE_SCENE_RMCOBJECT_H
#define SNEEZE_SCENE_RMCOBJECT_H

// Helpers that produce an RMCOBJECT -- the flat wire form of a SOM node (any
// class: root, celestial, terrestrial, physical, panel, light). RMCOBJECT and
// nlohmann::json are supplied by the umbrella <Sneeze.h> (force-included ahead
// of every translation unit via the precompiled header).

namespace SNEEZE
{
   // Zero-clears an RMCOBJECT and seeds an identity transform (unit scale,
   // identity quaternion). A plain zero-fill leaves a degenerate transform, and
   // under universal TRS a zero-scale ancestor collapses every descendant to the
   // origin, so synthetic nodes start from identity just like the JSON decoder.
   void MO_Init (RMAP::MAP::MAP_OBJECT* pMap_Object, bool bZeroMemory);

   // One extra resource next to Resource.sReference (e.g. sKey "vrma").
   // Not a wire field -- the POD cannot grow an unbounded string table.
   struct RESOURCE_SUPPLEMENT
   {
      std::string sKey;
      std::string sReference;
   };

   // Panel controls, laid over the 32-byte MAP_OBJECT_PROPERTIES region.
   // Map's union names celestial / light / terrestrial members; a panel node
   // uses these bytes instead. Zero is the engine default for every field.
   // Automatic texture size (nWidth or nHeight 0) is aspect-correct at
   // PANEL_TEX_REF_H on the unset axis, capped at PANEL_TEX_MAX.
   static constexpr int PANEL_TEX_REF_H   = 512;
   static constexpr int PANEL_TEX_MAX     = 4096;
   static constexpr int PANEL_DENSITY_MAX = 4;

   struct PANEL_PROPERTIES
   {
      enum eSCALE : uint8_t
      {
         kFOV  = 0,   // on-screen size is a fraction of the framed view
         kNODE = 1,   // metres = Bound.Max.xy * accumulated Transform.Scale
      };

      enum eBILLBOARD : uint8_t
      {
         kWORLD  = 0,   // look at the eye, world up
         kCAMERA = 1,   // look at the eye, roll to the camera's screen up
         kOFF    = 2,   // honor the node's world rotation
      };

      uint16_t nWidth;        // raster width in CSS pixels; 0 = automatic
      uint16_t nHeight;       // raster height in CSS pixels; 0 = automatic
      uint8_t  bScale;        // eSCALE
      uint8_t  bBillboard;    // eBILLBOARD
      uint8_t  bDensity;      // 0 or 1 = 1x; 2..PANEL_DENSITY_MAX supersamples and sets the dp ratio
      uint8_t  abReserved[25];
   };

   static_assert (sizeof (PANEL_PROPERTIES) == 32, "panel properties must fill the 32-byte map-object properties region");

   // Fills a wire RMCOBJECT from one node object of a fabric's JSON node tree.
   // The "aChildren" array is the caller's responsibility -- it is not part of
   // the flat wire object. Omitted transform fields decode to identity.
   // Optional Resource.aSupplementary pairs are written to aSupplementary
   // (cleared first); they never enter the POD.
   void MOCelestial_FromJson (const nlohmann::json& j, uint16_t& wClass, uint64_t& twObjectIx, RMAP::MAP::MAP_OBJECT_POD& pMap_Object_Celestial, std::vector<RESOURCE_SUPPLEMENT>& aSupplementary);
}

#endif // SNEEZE_SCENE_RMCOBJECT_H
