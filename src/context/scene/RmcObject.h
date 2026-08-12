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

   // Fills a wire RMCOBJECT from one node object of a fabric's JSON node tree.
   // The "aChildren" array is the caller's responsibility -- it is not part of
   // the flat wire object. Omitted transform fields decode to identity.
   void MOCelestial_FromJson (const nlohmann::json& j, RMAP::MAP::MAP_OBJECT_POD& pMap_Object_Celestial);
}

#endif // SNEEZE_SCENE_RMCOBJECT_H
