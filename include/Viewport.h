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
         float m_dDistance = 10.0f;
         float m_dTargetX  = 0.0f;
         float m_dTargetY  = 0.0f;
         float m_dTargetZ  = 0.0f;

         void Update (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight);
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
      void  Input_Key     (bool bKeySpace, bool bKeyPlus, bool bKeyMinus);
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
      void Diagnostics ();

      std::chrono::steady_clock::time_point     m_tpLastFrame;
      int64_t                                   m_tmNow;

      int    m_nFrameCount;
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
