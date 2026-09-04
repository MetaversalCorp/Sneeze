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

#ifndef SNEEZE_VIEWPORT_H
#define SNEEZE_VIEWPORT_H

namespace SNEEZE
{
   class VIEWPORT
   {
   public:
      class RENDERER;

   public:

      // --- Camera orbit state ---

      class VIEW
      {
      public:
         float m_dTheta    = 0.3f;
         float m_dPhi      = 0.4f;
         float m_dDistance = 5.0f;
         RMAP::MAP::MAP_OBJECT::VEC3  m_vTarget   = { 0.0, 0.0, 0.0 };

         // dMoveScale is the host-supplied WASD travel multiplier (1.0 = default).
         // The application owns the user preference; the engine just applies it on
         // top of the distance-relative KEY_PAN_FRACTION step.
         // dDeltaSeconds is the elapsed time since the last compositor camera update;
         // movement is scaled to a 60 Hz reference so refresh rate does not change speed.
         void Update (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight,
                      bool bKeyA, bool bKeyS, bool bKeyD, bool bKeyW,
                      bool bKeySpace, bool bKeyCtrl, float dMoveScale, float dDeltaSeconds);
      };

      // --- Camera absolute world pose ---
      //
      // The long-term camera model: an absolute world position (metres) plus an
      // orientation quaternion (x, y, z, w). The orbit VIEW above is a temporary
      // interaction stop-gap; the compositor seeds it from this pose on change.

      struct CAMERA
      {
         double aPosition[3] = { 0.0, 0.0, 0.0 };
         double aRotation[4] = { 0.0, 0.0, 0.0, 1.0 };
      };

      // --- Per-frame input state ---

      struct INPUT
      {
         int   nMouseDX    = 0;
         int   nMouseDY    = 0;
         float dScrollY    = 0.0f;
         bool  bMouseLeft  = false;
         bool  bMouseRight = false;

         bool  bKeySpace   = false;
         bool  bKeyPlus    = false;
         bool  bKeyMinus   = false;
         bool  bKeyA       = false;
         bool  bKeyS       = false;
         bool  bKeyD       = false;
         bool  bKeyW       = false;
         bool  bKeyCtrl    = false;

         // Host-supplied WASD travel multiplier (application-owned preference).
         // A level, not a delta -- it persists across Input_Consume().
         float dMoveScale  = 1.0f;
      };

      // ------------------------------------------------------------------------

      explicit VIEWPORT (CONTEXT* pContext);
      ~VIEWPORT ();

      bool Initialize ();
      bool Renderer_Initialize ();
      void Renderer_Shutdown ();

      void Activate (IVIEWPORT* pHost);
      void Deactivate ();

      ENGINE*              Engine   () const;
      CONTEXT*             Context  () const;
      IVIEWPORT*           Host     () const;
      SCENE*               Scene    () const;
      bool                 IsActive () const;

      // --- Input (called by application) ---

      void  Input_Mouse   (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight);
      void  Input_Key     (bool bKeySpace, bool bKeyPlus, bool bKeyMinus,
                           bool bKeyA, bool bKeyS, bool bKeyD, bool bKeyW, bool bKeyCtrl);
      void  Input_MoveScale (float dScale);
      INPUT Input_Consume ();

      // --- Framebuffer ---

      void            FrameBuffer_Write   (const uint32_t* pPixels, int nWidth, int nHeight);
      const uint32_t* FrameBuffer_Capture (int& nWidth, int& nHeight);
      void            FrameBuffer_Release ();

      // --- Dimensions ---

      void Size   (int& nWidth, int& nHeight);
      void Resize (int  nWidth, int  nHeight);

      // --- Camera ---

      VIEW& View ();

      // Absolute world pose. Setting it (any thread) makes the pose "active": the
      // compositor re-seeds the orbit VIEW from it every frame (so it self-corrects
      // while the scene streams in) until the user interacts, which deactivates it.
      void   Camera            (const CAMERA& Camera);
      CAMERA Camera            () const;
      bool   Camera_Active     (CAMERA& Camera) const;
      void   Camera_Deactivate ();

      // --- Renderer ---

      RENDERER* Renderer () const;

      // Passthrough (AR): the compositor clears with a transparent backdrop so
      // a host-provided video feed shows through instead of rgbBackground.
      // Android: phone camera under a translucent native surface.
      // OpenXR (later): XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND + runtime
      // passthrough (e.g. XR_FB_passthrough).
      void Passthrough (bool bPassthrough);
      bool Passthrough () const;

      // --- Scene invalidation (set from any thread, consumed by compositor) ---

      void Scene_Invalidate         ();
      bool Scene_Invalidate_Consume ();

      // --- Frame timing (written by compositor, per-viewport) ---

      enum eACCUMULATE
      {
         kACCUMULATE_INPUT,
         kACCUMULATE_SCENE,
         kACCUMULATE_SUBMIT,
         kACCUMULATE_RENDER,
         kACCUMULATE_PUBLISH,
      };

      void Accumulate  (eACCUMULATE eType, std::chrono::steady_clock::time_point tpStart);
      void Accumulate  (eACCUMULATE eType, double dSeconds);
      void Diagnostics (bool bPresented);

      std::chrono::steady_clock::time_point     m_tpLastFrame;
      std::chrono::steady_clock::time_point     m_tpLastCameraUpdate;
      int64_t                                   m_tmNow;

      int    m_nFrameCount;
      int    m_nPresentCount;
      double m_dFpsAccum;
      double m_dAccumInput;
      double m_dAccumScene;
      double m_dAccumSubmit;
      double m_dAccumRender;
      double m_dAccumPublish;

   private:
      class Impl;
      Impl* m_pImpl;
   };
}
#endif // SNEEZE_VIEWPORT_H
