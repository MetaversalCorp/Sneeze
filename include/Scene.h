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

#include "sneeze/Types.h"

#ifndef SNEEZE_SCENE_H
#define SNEEZE_SCENE_H

namespace SNEEZE
{
   class CONTAINER;
   class MSF;
   class NETWORK;
   class SCENE;
   class FABRIC;

   // Forward declaration: defined in gltf.h
   struct GLTF_RENDER_MODEL;

   // ---------------------------------------------------------------------------
   // RMAP Object Index constants
   //
   // Object indices are 48-bit values (TWORD) stored in a uint64_t. The upper
   // 16 bits of the containing QWORD may carry a packed class discriminator
   // (OBJECTIX union), but the object index itself is always in the low 48.
   // ---------------------------------------------------------------------------

   static constexpr uint64_t TWORD_MAX        = 0x0000FFFFFFFFFFFFull;
   static constexpr uint64_t OBJECTIX_MAX     = 0x0000FFFFFFFFFFFCull;
   static constexpr uint64_t OBJECTIX_LAST    = 0x0000FFFFFFFFFFFDull;
   static constexpr uint64_t OBJECTIX_ERROR   = 0x0000FFFFFFFFFFFeull;
   static constexpr uint64_t OBJECTIX_INVALID = 0x0000FFFFFFFFFFFFull;
   static constexpr uint64_t OBJECTIX_IDENTITY= 0x0000FFFFFFFFFFFFull;
   static constexpr uint64_t OBJECTIX_NULL    = 0x0000000000000000ull;

   // ---------------------------------------------------------------------------
   // NODE -- structural element in the scene.
   //
   // Each node participates in a tree owned by a single FABRIC. When a
   // MAP_OBJECT with a non-empty Resource.sReference is assigned, the node
   // fetches it and dispatches by content (glTF model on the node, or image
   // texture on the map object).
   // ---------------------------------------------------------------------------

   class NODE
   {
   public:
      NODE  (FABRIC* pFabric, NODE* pNode_Parent, RMAP::MAP::MAP_OBJECT* pMap_Object);
      ~NODE ();

      bool               Initialize        ();

      // Accessors
      uint64_t                   ObjectIx          () const;
      uint64_t                   Handle            () const;  // composed OBJECTIX — CONTAINER table key
      std::string                Name              () const;
      std::string                ClassName         () const;  // "celestial", "terrestrial", ...
      std::string                TypeName          () const;  // "starsystem", "star", ... (class-specific)
      int                        Subtype           () const;  // raw subtype discriminator
      RMAP::MAP::MAP_OBJECT*     Map_Object        () const;
      FABRIC*                    Fabric            () const;
      FABRIC*                    Fabric_Attachment () const;
      NODE*                      Parent            () const;
      NODE*                      Child             (int nPosition) const;
      int                        Node_Count        () const;
      bool                       IsPrivate         () const;

      // Mutators
      void               Private           (bool bPrivate);
      void               Fabric_Add        (FABRIC* pFabric_Child);
      void               Fabric_Remove     (FABRIC* pFabric_Child);

      // Methods
      void               Node_Add          (NODE* pNode_Child);
      void               Node_Remove       (NODE* pNode_Child);

      const GLTF_RENDER_MODEL* Gltf_Render_Model () const;
      void                     Gltf_Render_Model (GLTF_RENDER_MODEL* pModel);

      void           Source (const std::string& sSource);
      bool           Render (ENGINE* pEngine, int nWidth, int nHeight);
      const uint8_t* Pixels ()                                             const;
      int            Width ()                                              const;
      int            Height ()                                             const;

   private:
      class Impl;
      Impl*              m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // FABRIC -- a spatial fabric's branch in the scene graph.
   //
   // Each fabric owns a tree of NODEs rooted at Node_Root(). Fabrics form their
   // own hierarchy (parent/child) mirroring the attachment relationships in the
   // SOM tree. The attaching node is the NODE in the parent fabric's tree that
   // serves as the attachment point for this fabric.
   // ---------------------------------------------------------------------------

   class FABRIC
   {
   public:
      FABRIC  (SCENE* pScene, CONTAINER* pContainer, uint64_t twFabricIx, NODE* pNode_Attach, MSF* pMsf);
      ~FABRIC ();

      bool       Initialize     (const std::string& sUrl);

      // Accessors
      SCENE*             Scene          () const;
      FABRIC*            Fabric_Parent  () const;
      NODE*              Node_Root      () const;
      NODE*              Node_Attach    () const;
      CONTAINER*         Container      () const;
      uint64_t           FabricIx       () const;
      MSF*               Msf            () const;
      const std::string& Url            () const;
      std::string        Resolve        (const std::string& sReference) const;

      // The PERFORMANCE (monotonic) origin, captured at fabric load — the SDK's
      // per-fabric analog of performance.timeOrigin. Steady is the monotonic
      // count (100 ns) at t0; Wall is the wall-clock anchor (100 ns since 1601 UTC).
      int64_t            Performance_Origin_Steady () const;
      int64_t            Performance_Origin_Wall   () const;

      // Mutators
      void               Node_Root      (NODE* pNode_Root);

      // Methods
      void               Fabric_Add     (FABRIC* pFabric_Child);
      void               Fabric_Remove  (FABRIC* pFabric_Child);

      // Scene leftover teardown: drop the attach-node back-pointer without
      // Fabric_Remove (the attach node is already gone).
      void               Parent_Clear   ();
      void               OnWasmReady    (FILE* pFile, const std::string& sUrl, const std::string& sHash);
      void               OnWasmFailed   (FILE* pFile, const std::string& sUrl);

   protected:
      class Impl;
      Impl*              m_pImpl;
   };

   // ---------------------------------------------------------------------------
   // SCENE_LIGHT -- a scene-global light held by the SCENE and authored in the
   // primary fabric's "Primary" block, not a node in the graph. Ambient uses
   // rgbColor + fIntensity; the primary directional ("sun") additionally uses
   // vDirection (the unit vector the light travels along, world space). That
   // vector is authored the same way a spot node is aimed -- as a rotation of
   // the identity forward (+X), so the default (identity rotation) travels +X.
   // An fIntensity of 0 is simply an off light and is fully authorable.
   // ---------------------------------------------------------------------------

   struct SCENE_LIGHT
   {
      float                         fIntensity = 0.0f;
      RGB                           rgbColor   = { 1.0f, 1.0f, 1.0f };
      RMAP::MAP::MAP_OBJECT::VEC3   vDirection = { 1.0, 0.0, 0.0 };
   };

   // ---------------------------------------------------------------------------
   // SCENE -- root container for the scene object model.
   //
   // Owned by CONTEXT. Every FABRIC in the scene holds a back-pointer to
   // the SCENE, giving any NODE a path to engine services:
   //     NODE -> FABRIC -> SCENE -> Engine() / Network()
   // ---------------------------------------------------------------------------

   class SCENE
   {
   public:
      explicit SCENE (CONTEXT* pContext);
      ~SCENE ();

      bool               Initialize         (const std::string& sUrl);

      // Accessors
      ENGINE*            Engine             () const;
      CONTEXT*           Context            () const;
      NETWORK*           Network            () const;
      FABRIC*            Fabric_Root        () const;
      FABRIC*            Fabric_Primary     () const;
      RGBA               Background         () const;
      SCENE_LIGHT        Ambient            () const;
      SCENE_LIGHT        Directional        () const;

      // Mutators
      bool               Url                (const std::string& sUrl);
      void               Background         (const RGBA& rgbaBackground);
      void               Ambient            (const SCENE_LIGHT& Light);
      void               Directional        (const SCENE_LIGHT& Light);

      // Methods
      //
      // Loads a standalone glTF/GLB (raw bytes) as the scene's sole renderable,
      // attaching the built model to the primary node and seeding default
      // preview lighting. Intended for host-driven asset previews (e.g. the
      // browser's inspector) on a context opened with an empty URL -- no fabric,
      // no network fetch. Returns true when at least one drawable primitive was
      // produced. Call on the thread that owns the context; the compositor picks
      // the model up on its next traversal.
      bool               Gltf_Preview       (const uint8_t* pData, size_t nLen);

      // Internal functions
      bool               Background_Consume (RGBA& rgbaBackground);
      bool               Frame_Consume      ();
      void               Camera_Flush       ();
      void               Fabric_Spawn       (NODE* pNode_Attach, const std::string& sUrl);
      FABRIC*            Fabric_Open        (NODE* pNode_Attach, MSF* pMsf, const std::string& sUrl);
      FABRIC*            Fabric_Close       (FABRIC* pFabric);
      FABRIC*            Fabric_Find        (uint64_t twFabricIx) const;

      // Internal callbacks (used by file-local MSF_FETCH)
      void               OnMsfReady         (NODE* pNode_Attach, FILE* pFile);
      void               OnMsfFailed        (NODE* pNode_Attach, FILE* pFile);

   private:
      class Impl;
      Impl*              m_pImpl;
   };
}
#endif // SNEEZE_SCENE_H
