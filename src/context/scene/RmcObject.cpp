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

#include "RmcObject.h"

#include <cstring>
#include <cstdlib>

// JSON keys for the SOM node schema (RMCOBJECT) as authored in a fabric's node
// tree, grouped by sub-object. Keys that share a spelling across different
// sub-objects (fBrightness in the light vs celestial property blocks) are kept
// as separate constants so each context can change independently. The
// structural "Children" key is not here -- it is not a wire field, so it lives
// with the branch walker that recurses the tree.
#define NODE_KEY_HEAD                   "Head"
#define NODE_KEY_HEAD_SELF              "Self"
#define NODE_KEY_HEAD_EVENT             "Event"
#define NODE_KEY_NAME                   "Name"
#define NODE_KEY_TYPE                   "Type"
#define NODE_KEY_TYPE_TYPE              "bType"
#define NODE_KEY_TYPE_SUBTYPE           "bSubtype"
#define NODE_KEY_TYPE_FICTION           "bFiction"
#define NODE_KEY_OWNER                  "Owner"
#define NODE_KEY_RESOURCE               "Resource"
#define NODE_KEY_RESOURCE_HANDLE        "qwResource"
#define NODE_KEY_RESOURCE_NAME          "sName"
#define NODE_KEY_RESOURCE_REFERENCE     "sReference"
#define NODE_KEY_TRANSFORM              "Transform"
#define NODE_KEY_TRANSFORM_POSITION     "Position"
#define NODE_KEY_TRANSFORM_ROTATION     "Rotation"
#define NODE_KEY_TRANSFORM_SCALE        "Scale"
#define NODE_KEY_ORBIT                  "Orbit"
#define NODE_KEY_ORBIT_PERIOD           "tmPeriod"
#define NODE_KEY_ORBIT_ORIGIN           "tmOrigin"
#define NODE_KEY_ORBIT_A                "dA"
#define NODE_KEY_ORBIT_B                "dB"
#define NODE_KEY_BOUND                  "Bound"
#define NODE_KEY_BOUND_MAX              "Max"
#define NODE_KEY_PROPERTIES             "Properties"
#define NODE_KEY_LIGHT_BRIGHTNESS       "fBrightness"
#define NODE_KEY_LIGHT_ANGLE_OPENING    "fAngleOpening"
#define NODE_KEY_LIGHT_ANGLE_FALLOFF    "fAngleFalloff"
#define NODE_KEY_CELESTIAL_MASS         "fMass"
#define NODE_KEY_CELESTIAL_GRAVITY      "fGravity"
#define NODE_KEY_CELESTIAL_BRIGHTNESS   "fBrightness"
#define NODE_KEY_CELESTIAL_REFLECTIVITY "fReflectivity"
#define NODE_KEY_PROPERTIES_COLOR       "fColor"

namespace SNEEZE
{

// ComposeFromId — turn a human "<class>-<index>" id (e.g. "P-5039") into a
// composed OBJECTIX. Class letters: R root, C celestial, T terrestrial,
// P physical, L light. A "?" index (e.g. "P-?") means "assign me the next
// free index in this container" -- it composes the OBJECTIX_IDENTITY sentinel,
// which Node_Create resolves to an allocated index.
static uint64_t ComposeFromId (const std::string& sId)
{
   uint64_t twResult = 0;
   size_t   nDash    = sId.find ('-');

   if (nDash != std::string::npos)
   {
      char        cClass = sId[0];
      const char* pIndex = sId.c_str () + nDash + 1;
      uint64_t    nIndex = (*pIndex == '?') ? OBJECTIX_IDENTITY : strtoull (pIndex, nullptr, 10);

      MAP_OBJECT::MAP_OBJECT_CLASS eClass = MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL;
      if      (cClass == 'R') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_ROOT;
      else if (cClass == 'C') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_CELESTIAL;
      else if (cClass == 'T') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_TERRESTRIAL;
      else if (cClass == 'P') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL;
      else if (cClass == 'L') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT;

      twResult = OBJECTIX_COMPOSE (eClass, nIndex);
   }

   return twResult;
}

void RmcObject_Init (RMCOBJECT& RMCObject)
{
   memset (&RMCObject, 0, sizeof (RMCOBJECT));
   RMCObject.Transform.d4Rotation[3] = 1.0;
   RMCObject.Transform.d3Scale[0]    = 1.0;
   RMCObject.Transform.d3Scale[1]    = 1.0;
   RMCObject.Transform.d3Scale[2]    = 1.0;
}

void RmcObject_FromJson (const nlohmann::json& j, RMCOBJECT* pObject)
{
   *pObject = RMCOBJECT {};

   // Sensible decode defaults for omitted transform fields: identity orientation and unit scale 
   // (a zero quaternion / zero scale would be degenerate). Present fields below overwrite these.
   pObject->Transform.d4Rotation[3] = 1.0;
   pObject->Transform.d3Scale[0]    = 1.0;
   pObject->Transform.d3Scale[1]    = 1.0;
   pObject->Transform.d3Scale[2]    = 1.0;

   auto Vec = [] (const nlohmann::json& a, double* pd, int n)
   {
      if (a.is_array ())
      {
         for (int i = 0; i < n  &&  i < static_cast<int> (a.size ()); i++)
            pd[i] = a[i].get<double> ();
      }
   };

   auto Str = [] (const nlohmann::json& v, char* pDst, size_t nMax)
   {
      if (v.is_string ())
      {
         std::string s = v.get<std::string> ();
         size_t nLen = s.size () < nMax - 1 ? s.size () : nMax - 1;
         memcpy (pDst, s.data (), nLen);
      }
   };

   if (j.contains (NODE_KEY_HEAD))
   {
      const auto& h = j[NODE_KEY_HEAD];

      // Self accepts the human "class:index" id (preferred) or a raw composed
      // integer. Parent is never read (parentage comes from the node tree), so
      // it is ignored when absent.
      if (h.contains (NODE_KEY_HEAD_SELF))
      {
         if (h[NODE_KEY_HEAD_SELF].is_string ())
            pObject->Head.Self.qwComposed = ComposeFromId (h[NODE_KEY_HEAD_SELF].get<std::string> ());
         else
            pObject->Head.Self.qwComposed = h[NODE_KEY_HEAD_SELF].get<uint64_t> ();
      }

      pObject->Head.qwEvent = h.value (NODE_KEY_HEAD_EVENT, static_cast<uint64_t> (0));
   }

   if (j.contains (NODE_KEY_NAME)  &&  j[NODE_KEY_NAME].is_string ())
   {
      std::string s = j[NODE_KEY_NAME].get<std::string> ();
      int i = 0;
      for (unsigned char c : s)
      {
         if (i >= 48)
            break;
         pObject->Name.wsName[i++] = static_cast<uint16_t> (c);
      }
   }

   if (j.contains (NODE_KEY_TYPE))
   {
      const auto& t = j[NODE_KEY_TYPE];
      pObject->Type.bType    = static_cast<uint8_t> (t.value (NODE_KEY_TYPE_TYPE,    0));
      pObject->Type.bSubtype = static_cast<uint8_t> (t.value (NODE_KEY_TYPE_SUBTYPE, 0));
      pObject->Type.bFiction = static_cast<uint8_t> (t.value (NODE_KEY_TYPE_FICTION, 0));
   }

   pObject->Owner.twOwner = j.value (NODE_KEY_OWNER, static_cast<uint64_t> (0));

   if (j.contains (NODE_KEY_RESOURCE))
   {
      const auto& r = j[NODE_KEY_RESOURCE];
      pObject->Resource.qwResource = r.value (NODE_KEY_RESOURCE_HANDLE, static_cast<uint64_t> (0));
      if (r.contains (NODE_KEY_RESOURCE_NAME))      Str (r[NODE_KEY_RESOURCE_NAME],      pObject->Resource.sName,      sizeof (pObject->Resource.sName));
      if (r.contains (NODE_KEY_RESOURCE_REFERENCE)) Str (r[NODE_KEY_RESOURCE_REFERENCE], pObject->Resource.sReference, sizeof (pObject->Resource.sReference));
   }

   if (j.contains (NODE_KEY_TRANSFORM))
   {
      const auto& tr = j[NODE_KEY_TRANSFORM];
      if (tr.contains (NODE_KEY_TRANSFORM_POSITION)) Vec (tr[NODE_KEY_TRANSFORM_POSITION], pObject->Transform.d3Position, 3);
      if (tr.contains (NODE_KEY_TRANSFORM_ROTATION)) Vec (tr[NODE_KEY_TRANSFORM_ROTATION], pObject->Transform.d4Rotation, 4);
      if (tr.contains (NODE_KEY_TRANSFORM_SCALE))    Vec (tr[NODE_KEY_TRANSFORM_SCALE],    pObject->Transform.d3Scale,    3);
   }

   if (j.contains (NODE_KEY_ORBIT))
   {
      const auto& o = j[NODE_KEY_ORBIT];
      pObject->Orbit.Celestial.tmPeriod = o.value (NODE_KEY_ORBIT_PERIOD, static_cast<int64_t> (0));
      pObject->Orbit.Celestial.tmOrigin = o.value (NODE_KEY_ORBIT_ORIGIN, static_cast<int64_t> (0));
      pObject->Orbit.Celestial.dA       = o.value (NODE_KEY_ORBIT_A, 0.0);
      pObject->Orbit.Celestial.dB       = o.value (NODE_KEY_ORBIT_B, 0.0);
   }

   if (j.contains (NODE_KEY_BOUND)  &&  j[NODE_KEY_BOUND].contains (NODE_KEY_BOUND_MAX))
      Vec (j[NODE_KEY_BOUND][NODE_KEY_BOUND_MAX], pObject->Bound.d3Max, 3);

   if (j.contains (NODE_KEY_PROPERTIES))
   {
      const auto& p = j[NODE_KEY_PROPERTIES];

      // The 32-byte Properties region is class-tagged (celestial vs light), so
      // parse into the member the node's class actually owns.
      MAP_OBJECT::MAP_OBJECT_CLASS eClass = pObject->Head.Self.Class ();

      if (eClass == MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT)
      {
         pObject->Properties.Light.fBrightness   = p.value (NODE_KEY_LIGHT_BRIGHTNESS,    0.0f);
         pObject->Properties.Light.fOpeningAngle = p.value (NODE_KEY_LIGHT_ANGLE_OPENING, 0.0f);
         pObject->Properties.Light.fFalloffAngle = p.value (NODE_KEY_LIGHT_ANGLE_FALLOFF, 0.0f);
      }
      else
      {
         pObject->Properties.Celestial.fMass         = p.value (NODE_KEY_CELESTIAL_MASS,         0.0f);
         pObject->Properties.Celestial.fGravity      = p.value (NODE_KEY_CELESTIAL_GRAVITY,      0.0f);
         pObject->Properties.Celestial.fBrightness   = p.value (NODE_KEY_CELESTIAL_BRIGHTNESS,   0.0f);
         pObject->Properties.Celestial.fReflectivity = p.value (NODE_KEY_CELESTIAL_REFLECTIVITY, 0.0f);
      }

      // fColor is authored as an ordinary 0xRRGGBB colour -- a decimal integer
      // (e.g. 3368601) or a hex string ("0x336699" or "#336699"). Its 24 bits
      // are stored verbatim into the float field, because the engine reads
      // fColor's bits (not its numeric value) as the colour. Absent leaves it 0,
      // which the light path treats as "default white". fColor sits at the same
      // offset in both members, so either alias writes the right bytes.
      uint32_t nColor = 0;

      if (p.contains (NODE_KEY_PROPERTIES_COLOR))
      {
         const auto& c = p[NODE_KEY_PROPERTIES_COLOR];

         if (c.is_string ())
         {
            std::string sColor  = c.get<std::string> ();
            size_t      nOffset = 0;

            if (!sColor.empty ()  &&  sColor[0] == '#')
               nOffset = 1;
            else if (sColor.size () >= 2  &&  sColor[0] == '0'  &&  (sColor[1] == 'x'  ||  sColor[1] == 'X'))
               nOffset = 2;

            nColor = static_cast<uint32_t> (strtoul (sColor.c_str () + nOffset, nullptr, 16));
         }
         else if (c.is_number ())
         {
            nColor = static_cast<uint32_t> (c.get<double> ());
         }
      }

      memcpy (&pObject->Properties.Celestial.fColor, &nColor, sizeof (float));
   }
}

}
