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

#ifndef SNEEZE_SOM_MAP_OBJECT_H
#define SNEEZE_SOM_MAP_OBJECT_H

#include "sneeze/Types.h"

namespace SNEEZE
{
   class ENGINE;

   struct GLTF_RENDER_MODEL;

   namespace DEP
   {
      class UI_PANEL;
   }

   // Compose a class discriminator and a 48-bit object index into one OBJECTIX value. Use this instead of hardcoding opaque 64-bit literals.
   #define OBJECTIX_COMPOSE(eClass, twObjectIx)      ((static_cast<uint64_t> (eClass) << 48)  |  (static_cast<uint64_t> (twObjectIx) & 0x0000FFFFFFFFFFFFull))

   // ---------------------------------------------------------------------------
   // MAP_OBJECT — base class for all 3D objects referenced by SOM::NODEs.
   // All spatial properties (position, orientation, scale, bounding volume,
   // visual appearance) belong here, not on the NODE itself.
   // ---------------------------------------------------------------------------

   class MAP_OBJECT
   {
   public:
      enum MAP_OBJECT_CLASS : uint16_t
      {
         MAP_OBJECT_CLASS_ROOT        = 70,
         MAP_OBJECT_CLASS_CELESTIAL   = 71,
         MAP_OBJECT_CLASS_TERRESTRIAL = 72,
         MAP_OBJECT_CLASS_PHYSICAL    = 73,
         MAP_OBJECT_CLASS_PANEL       = 74,
         MAP_OBJECT_CLASS_LIGHT       = 75,
      };

      struct OBJECTIX
      {
         uint64_t              qwComposed;

         uint64_t              ObjectIx () const   { return qwComposed & 0x0000FFFFFFFFFFFFull; }
         MAP_OBJECT_CLASS      Class    () const   { return static_cast<MAP_OBJECT_CLASS> (qwComposed >> 48); }
      };

      struct OBJECT_HEAD
      {
         OBJECTIX              Parent;
         OBJECTIX              Self;
         uint64_t              qwEvent;
      };

      struct MAP_OBJECT_NAME
      {
         uint16_t              wsName[48];
      };

      struct MAP_OBJECT_TYPE
      {
         uint8_t               bType;
         uint8_t               bSubtype;
         uint8_t               bFiction;
         uint8_t               abReserved[5];
      };

      struct MAP_OBJECT_OWNER
      {
         uint64_t              twOwner;
      };

      struct MAP_OBJECT_RESOURCE
      {
         uint64_t              qwResource;
         char                  sName[64];
         char                  sReference[128];
      };

      struct MAP_OBJECT_TRANSFORM
      {
         double                d3Position[3];
         double                d4Rotation[4];
         double                d3Scale[3];
      };

      struct MAP_OBJECT_ORBIT_CELESTIAL
      {
         int64_t               tmPeriod;
         int64_t               tmOrigin;
         double                dA;
         double                dB;
      };

      // The 32-byte orbit region is class-tagged: only celestial objects use it.
      // Other classes leave it reserved. The active member is chosen by the node's
      // MAP_OBJECT_CLASS; the wire size never changes.
      union MAP_OBJECT_ORBIT
      {
         MAP_OBJECT_ORBIT_CELESTIAL   Celestial;
         uint8_t                      abReserved[32];
      };

      struct MAP_OBJECT_BOUND
      {
         uint8_t               abReserved[24];
         double                d3Max[3];
      };

      struct MAP_OBJECT_PROPERTIES_CELESTIAL
      {
         float                 fMass;
         float                 fGravity;
         float                 fColor;
         float                 fBrightness;
         float                 fReflectivity;
         uint8_t               abReserved[12];
      };

      // A light keeps fColor (0xRRGGBB packed into the float's bits) and
      // fBrightness at the same offsets as the celestial fields, so the shared
      // ColorToU32 accessor works for any class. The leading 8 bytes -- fMass and
      // fGravity on a celestial object -- carry the spot-light cone angles instead
      // (degrees). Point/ambient/directional lights ignore both angles.
      struct MAP_OBJECT_PROPERTIES_LIGHT
      {
         float                 fOpeningAngle;
         float                 fFalloffAngle;
         float                 fColor;
         float                 fBrightness;
         uint8_t               abReserved[16];
      };

      // The 32-byte properties region is class-tagged. The active member is chosen
      // by the node's MAP_OBJECT_CLASS; the wire size never changes.
      union MAP_OBJECT_PROPERTIES
      {
         MAP_OBJECT_PROPERTIES_CELESTIAL  Celestial;
         MAP_OBJECT_PROPERTIES_LIGHT      Light;
         uint8_t                          abReserved[32];
      };

   public:
      OBJECT_HEAD                   Head        = {};
      MAP_OBJECT_NAME               Name        = {};
      MAP_OBJECT_TYPE               Type        = {};
      MAP_OBJECT_OWNER              Owner       = {};
      MAP_OBJECT_RESOURCE           Resource    = {};
      MAP_OBJECT_TRANSFORM          Transform   = {};
      MAP_OBJECT_ORBIT              Orbit       = {};
      MAP_OBJECT_BOUND              Bound       = {};
      MAP_OBJECT_PROPERTIES         Properties  = {};

   public:
      explicit MAP_OBJECT (OBJECT_HEAD Head);
      virtual ~MAP_OBJECT ();

      MAP_OBJECT_CLASS   Class ()      const;

      static const char* ClassName (MAP_OBJECT_CLASS eType);

      void        Scale            (double& dX, double& dY, double& dZ)   const;
      void        Scale            (VEC3& vScale)                         const;
      double      Radius           ()                                     const;
      uint32_t    ColorToU32       ()                                     const;
      uint32_t    ColorDimToU32    ()                                     const;
      uint32_t    ColorBrightToU32 ()                                     const;

      bool GetTexture (const uint8_t*& pTex, int& nTexW, int& nTexH); // WRONG, shouldn't return pointer to pTex
      void SetTexture (const uint8_t* pTex, int nTexW, int nTexH);

      // The object's loaded glTF/GLB model (its drawable geometry), or null until
      // the resource has been fetched and built. The map object takes ownership
      // of the model handed to the setter and frees it on destruction.
      const GLTF_RENDER_MODEL* Gltf_Render_Model () const;
      void                     Gltf_Render_Model (GLTF_RENDER_MODEL* pModel);

      virtual void Position (int64_t tmNow, double& dX, double& dY, double& dZ)                 const;
      virtual void Rotation (int64_t tmNow, double& dQx, double& dQy, double& dQz, double& dQw) const;

      void         Position (int64_t tmNow, VEC3& vPosition)                                    const;
      void         Rotation (int64_t tmNow, QUAT& qRotation)                                    const;

   private:
      class Impl;
      Impl* m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // Derived map object types
   // ---------------------------------------------------------------------------

   class MAP_OBJECT_ROOT : public MAP_OBJECT
   {
   public:
      explicit MAP_OBJECT_ROOT (OBJECT_HEAD Head);
   };

   class MAP_OBJECT_CELESTIAL : public MAP_OBJECT
   {
   public:
      struct ORBIT_POSITION
      {
         double dX;
         double dY;
         double dZ;
         double dE;
      };

      enum MAP_OBJECT_TYPE_TYPE_CELESTIAL
      {
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_NONE           = 0,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_UNIVERSE       = 1,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_SUPERCLUSTER   = 2,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_GALAXYCLUSTER  = 3,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_GALAXY         = 4,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_SECTOR         = 5,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_NEBULA         = 6,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARCLUSTER    = 7,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_BLACKHOLE      = 8,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM     = 9,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_STAR           = 10,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANETSYSTEM   = 11,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANET         = 12,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOONSYSTEM     = 125,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOON           = 13,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRISSYSTEM   = 135,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRIS         = 14,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_SATELLITE      = 15,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_TRANSPORT      = 16,
         MAP_OBJECT_TYPE_TYPE_CELESTIAL_SURFACE        = 17,
      };

   public:
      explicit MAP_OBJECT_CELESTIAL (OBJECT_HEAD Head);

      static const char* GetTypeName (MAP_OBJECT_TYPE_TYPE_CELESTIAL eType);

      bool HasOrbit () const;

      void Position (int64_t tmNow, double& dX, double& dY, double& dZ)                 const override;
      void Rotation (int64_t tmNow, double& dQx, double& dQy, double& dQz, double& dQw) const override;

      bool PositionAtTick (int64_t tmNow, ORBIT_POSITION& out) const;
      VEC3 OrbitTrailPoint (double dE, int64_t tmElapsed)      const;
   };

   class MAP_OBJECT_TERRESTRIAL : public MAP_OBJECT
   {
   public:
      explicit MAP_OBJECT_TERRESTRIAL (OBJECT_HEAD Head);
   };

   class MAP_OBJECT_PHYSICAL : public MAP_OBJECT
   {
   public:
      explicit MAP_OBJECT_PHYSICAL (OBJECT_HEAD Head);
   };

   // A scene light. Its world placement comes from the node's TRS like any other
   // map object; the light reads Properties.Light -- colour from fColor (0xRRGGBB),
   // intensity from fBrightness. A spot light additionally aims down the node's
   // local +X axis (identity forward in the Z-up world, rotated by its TRS) and reads
   // its cone from fOpeningAngle / fFalloffAngle (degrees). The subtype selects the
   // ANARI light kind.
   class MAP_OBJECT_LIGHT : public MAP_OBJECT
   {
   public:
      // A light node is a placed light only -- point or spot. Ambient and
      // directional lighting are scene-global properties (set via the primary
      // fabric), never nodes. Values mirror LIGHT_DATA::eTYPE: the deprecated
      // 3/4 remain accepted because existing fabrics authored point/spot there.
      enum MAP_OBJECT_TYPE_TYPE_LIGHT
      {
         MAP_OBJECT_TYPE_TYPE_LIGHT_NONE              = 0,
         MAP_OBJECT_TYPE_TYPE_LIGHT_POINT             = 1,
         MAP_OBJECT_TYPE_TYPE_LIGHT_SPOT              = 2,
         MAP_OBJECT_TYPE_TYPE_LIGHT_POINT__DEPRECATED = 3,
         MAP_OBJECT_TYPE_TYPE_LIGHT_SPOT__DEPRECATED  = 4,
      };

   public:
      explicit MAP_OBJECT_LIGHT (OBJECT_HEAD Head);
   };

   // An in-scene UI panel (RmlUi RML+CSS rasterized to a textured quad). Owns
   // its own UI surface; the panel's world size comes from Bound.d3Max[0,1] and
   // its placement from the node's TRS, so it flows through the compositor and
   // per-scene render scale exactly like any other node.
   class MAP_OBJECT_PANEL : public MAP_OBJECT
   {
   public:
      explicit MAP_OBJECT_PANEL (OBJECT_HEAD Head);
      ~MAP_OBJECT_PANEL () override;

      // Set the panel's RML+CSS document source. Marks the UI dirty so the next
      // Render rasterizes it. If never set, the panel's built-in default
      // document is used.
      void Source (const std::string& sSource);

      // Rasterize the panel's UI into its canvas (call on the render thread).
      // Cheap when unchanged. Returns true if Pixels() is valid.
      bool Render (ENGINE* pEngine, int nWidth, int nHeight);

      const uint8_t* Pixels () const;
      int            Width  () const;
      int            Height () const;

   private:
      DEP::UI_PANEL* m_pPanel;
   };

//-------------------------
   struct RMCOBJECT
   {
      MAP_OBJECT::OBJECT_HEAD           Head;
      MAP_OBJECT::MAP_OBJECT_NAME       Name;
      MAP_OBJECT::MAP_OBJECT_TYPE       Type;
      MAP_OBJECT::MAP_OBJECT_OWNER      Owner;
      MAP_OBJECT::MAP_OBJECT_RESOURCE   Resource;
      MAP_OBJECT::MAP_OBJECT_TRANSFORM  Transform;
      MAP_OBJECT::MAP_OBJECT_ORBIT      Orbit;
      MAP_OBJECT::MAP_OBJECT_BOUND      Bound;
      MAP_OBJECT::MAP_OBJECT_PROPERTIES Properties;
   };

}
#endif // SNEEZE_SOM_MAP_OBJECT_H
