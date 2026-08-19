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

#include "AnariRenderer.h"
#include "sneeze/control/Control.h"

#include <cmath>

using namespace SNEEZE;

static constexpr float MOUSE_SENSITIVITY = 0.002f;
static constexpr float SCROLL_FACTOR = 1.05f;
static constexpr float MIN_DISTANCE = 0.001f;
static constexpr float MAX_DISTANCE = 1e14f;
static constexpr float PI_F = 3.14159265358979f;
static constexpr float KEY_PAN_FRACTION = 0.02f;   // fraction of orbit distance per held frame at 60 Hz
static constexpr float KEY_PAN_REFERENCE_HZ = 60.0f;

// ===========================================================================
// Impl
// ===========================================================================

class VIEWPORT::Impl
{
public:
   Impl (VIEWPORT* pViewport, CONTEXT* pContext) :
      m_pViewport        (pViewport),
      m_pContext         (pContext),
      m_pHost            (nullptr),
      m_pJob_Compositor  (nullptr),
      m_pRenderer        (nullptr),
      m_bScene_Invalidate (false),
      m_nFbWidth         (0),
      m_nFbHeight        (0),
      m_nWidth           (0),
      m_nHeight          (0)
   {
   }

   bool Initialize ()
   {
      return true;
   }

   ~Impl ()
   {
      Deactivate ();
   }

   void Activate (IVIEWPORT* pHost)
   {
      std::lock_guard<std::mutex> guard (m_mxViewport);

      if (!m_pHost  &&  pHost)
      {
         m_pHost = pHost;

         m_pJob_Compositor = new JOB_COMPOSITOR (m_pViewport);
         m_pContext->Engine ()->Queue_Post_Compositor (m_pJob_Compositor);
      }
   }

   void Deactivate ()
   {
      std::lock_guard<std::mutex> guard (m_mxViewport);

      if (m_pHost)
      {
         m_pJob_Compositor->Cancel (); // blocks until renderer is destroyed
         
         delete m_pJob_Compositor;
         m_pJob_Compositor = nullptr;

         m_pHost = nullptr;
      }
   }

   // ---------------------------------------------------------------------------
   // Renderer
   // ---------------------------------------------------------------------------

   bool Renderer_Initialize ()
   {
      // Filament requires that the thread that creates the rendering device is the
      // same thread that renders with it. This method is called from the compositor
      // thread rather than during Viewport::Initialize() to satisfy that constraint.

      if (!m_pRenderer)
      {
         if (m_pHost)
         {
            std::string sLibrary = m_pContext->Engine ()->Host ()->sRenderer ();
   
            if (!sLibrary.empty ())
            {
               auto* pRenderer = new RENDERER::ANARI (m_pContext->Engine (), sLibrary);
   
               void* pNativeWindow = m_pHost->FrameWindow ();
               if (pNativeWindow)
                  pRenderer->SetNativeWindow (pNativeWindow);
   
               if (pRenderer->Initialize (m_nWidth, m_nHeight))
               {
                  m_pRenderer = pRenderer;

                  m_pViewport->m_tpLastFrame = std::chrono::steady_clock::now ();
                  m_pViewport->m_tpLastCameraUpdate = m_pViewport->m_tpLastFrame;

                  m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "VIEWPORT", "Renderer initialized on compositor thread");
               }
               else
               {
                  delete pRenderer;

                  m_pContext->Engine ()->Log (IENGINE::kLOGLEVEL_Error, "VIEWPORT", "Failed to initialize renderer");
               }
            }
         }
      }

      return (m_pRenderer != nullptr);
   }

   void Renderer_Shutdown ()
   {
      if (m_pRenderer)
      {
         delete m_pRenderer;
         m_pRenderer = nullptr;
      }
   }

   // ---------------------------------------------------------------------------
   // Input
   // ---------------------------------------------------------------------------

   void Input_Mouse (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight)
   {
      std::lock_guard<std::mutex> guard (m_mxInput);

      m_Input.nMouseDX += nDX;
      m_Input.nMouseDY += nDY;
      m_Input.dScrollY += dScrollY;
      m_Input.bMouseLeft = bMouseLeft;
      m_Input.bMouseRight = bMouseRight;
   }

   void Input_Key (bool bKeySpace, bool bKeyPlus, bool bKeyMinus,
                   bool bKeyA, bool bKeyS, bool bKeyD, bool bKeyW, bool bKeyCtrl)
   {
      std::lock_guard<std::mutex> guard (m_mxInput);

      m_Input.bKeySpace = bKeySpace;
      m_Input.bKeyPlus  = bKeyPlus;
      m_Input.bKeyMinus = bKeyMinus;
      m_Input.bKeyA     = bKeyA;
      m_Input.bKeyS     = bKeyS;
      m_Input.bKeyD     = bKeyD;
      m_Input.bKeyW     = bKeyW;
      m_Input.bKeyCtrl  = bKeyCtrl;
   }

   void Input_MoveScale (float dScale)
   {
      std::lock_guard<std::mutex> guard (m_mxInput);

      m_Input.dMoveScale = dScale;
   }

   INPUT Input_Consume ()
   {
      std::lock_guard<std::mutex> guard (m_mxInput);

      INPUT Input = m_Input;
      m_Input.nMouseDX = 0;
      m_Input.nMouseDY = 0;
      m_Input.dScrollY = 0.0f;

      return Input;
   }

   // ---------------------------------------------------------------------------
   // Framebuffer
   // ---------------------------------------------------------------------------

   void FrameBuffer_Write (const uint32_t* pPixels, int nWidth, int nHeight)
   {
      std::lock_guard<std::mutex> guard (m_mxFrameBuffer);

      int nSize = nWidth * nHeight;
      m_aFrameBuffer.resize (nSize);

      std::memcpy (m_aFrameBuffer.data (), pPixels, nSize * sizeof (uint32_t));

      m_nFbWidth = nWidth;
      m_nFbHeight = nHeight;
   }

   const uint32_t* FrameBuffer_Capture (int& nWidth, int& nHeight)
   {
      m_mxFrameBuffer.lock ();

      nWidth = m_nFbWidth;
      nHeight = m_nFbHeight;
      
      return m_aFrameBuffer.empty () ? nullptr : m_aFrameBuffer.data ();
   }

   void FrameBuffer_Release ()
   {
      m_mxFrameBuffer.unlock ();
   }

public:
   VIEWPORT*               m_pViewport;
   CONTEXT*                m_pContext;
   IVIEWPORT*              m_pHost;
   JOB_COMPOSITOR*         m_pJob_Compositor;
   RENDERER*               m_pRenderer;
   std::atomic<bool>       m_bScene_Invalidate;
   std::mutex              m_mxViewport;

   // Input
   std::mutex              m_mxInput;
   INPUT                   m_Input;

   // Framebuffer
   std::mutex              m_mxFrameBuffer;
   std::vector<uint32_t>   m_aFrameBuffer;
   int                     m_nFbWidth;
   int                     m_nFbHeight;

   // Dimensions
   int                     m_nWidth;
   int                     m_nHeight;

   // Camera
   VIEW                    m_View;

   // Absolute world pose (set from any thread; applied by the compositor while
   // active, until the user interacts)
   std::mutex              m_mxCamera;
   CAMERA                  m_Camera;
   bool                    m_bCamera_Active = false;
};


// ===========================================================================
// VIEWPORT
// ===========================================================================

VIEWPORT::VIEWPORT (CONTEXT* pContext) :
   m_pImpl         (new Impl (this, pContext)),
   m_tmNow         (0),
   m_nFrameCount   (0),
   m_dFpsAccum     (0.0),
   m_dAccumInput   (0.0),
   m_dAccumScene   (0.0),
   m_dAccumSubmit  (0.0),
   m_dAccumRender  (0.0),
   m_dAccumPublish (0.0)
{
}

bool VIEWPORT::Initialize ()
{
   return m_pImpl->Initialize ();
}

VIEWPORT::~VIEWPORT ()
{
   delete m_pImpl;
}

void VIEWPORT::Activate (IVIEWPORT* pHost)
{
   m_pImpl->Activate (pHost);
}

void VIEWPORT::Deactivate ()
{
   m_pImpl->Deactivate ();
}

bool VIEWPORT::Renderer_Initialize ()
{
   return m_pImpl->Renderer_Initialize ();
}

void VIEWPORT::Renderer_Shutdown ()
{
   m_pImpl->Renderer_Shutdown ();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

SNEEZE::ENGINE*      VIEWPORT::Engine          () const { return m_pImpl->m_pContext->Engine (); }
SNEEZE::CONTEXT*     VIEWPORT::Context         () const { return m_pImpl->m_pContext;            }
IVIEWPORT*           VIEWPORT::Host            () const { return m_pImpl->m_pHost;               }
SNEEZE::SCENE*       VIEWPORT::Scene           () const { return m_pImpl->m_pContext->Scene ();  }
bool                 VIEWPORT::IsActive        () const { return m_pImpl->m_pHost != nullptr;     }
VIEWPORT::VIEW&      VIEWPORT::View            ()       { return m_pImpl->m_View;                }
VIEWPORT::RENDERER*  VIEWPORT::Renderer        () const { return m_pImpl->m_pRenderer;           }

// ---------------------------------------------------------------------------
// Camera absolute world pose
// ---------------------------------------------------------------------------

void VIEWPORT::Camera (const CAMERA& Camera)
{
   std::lock_guard<std::mutex> guard (m_pImpl->m_mxCamera);

   m_pImpl->m_Camera        = Camera;
   m_pImpl->m_bCamera_Active = true;
}

VIEWPORT::CAMERA VIEWPORT::Camera () const
{
   std::lock_guard<std::mutex> guard (m_pImpl->m_mxCamera);

   return m_pImpl->m_Camera;
}

bool VIEWPORT::Camera_Active (CAMERA& Camera) const
{
   std::lock_guard<std::mutex> guard (m_pImpl->m_mxCamera);

   if (m_pImpl->m_bCamera_Active)
      Camera = m_pImpl->m_Camera;

   return m_pImpl->m_bCamera_Active;
}

void VIEWPORT::Camera_Deactivate ()
{
   std::lock_guard<std::mutex> guard (m_pImpl->m_mxCamera);

   m_pImpl->m_bCamera_Active = false;
}

void VIEWPORT::Scene_Invalidate ()
{
   m_pImpl->m_bScene_Invalidate.store (true);
}

bool VIEWPORT::Scene_Invalidate_Consume ()
{
   return m_pImpl->m_bScene_Invalidate.exchange (false);
}

void VIEWPORT::Size (int& nWidth, int& nHeight)
{
   nWidth  = m_pImpl->m_nWidth;
   nHeight = m_pImpl->m_nHeight;
}

void VIEWPORT::Resize (int nWidth, int nHeight)
{
   m_pImpl->m_nWidth  = nWidth;
   m_pImpl->m_nHeight = nHeight;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void VIEWPORT::Input_Mouse (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight)
{
   m_pImpl->Input_Mouse (nDX, nDY, dScrollY, bMouseLeft, bMouseRight);
}

void VIEWPORT::Input_Key (bool bKeySpace, bool bKeyPlus, bool bKeyMinus,
                          bool bKeyA, bool bKeyS, bool bKeyD, bool bKeyW, bool bKeyCtrl)
{
   m_pImpl->Input_Key (bKeySpace, bKeyPlus, bKeyMinus, bKeyA, bKeyS, bKeyD, bKeyW, bKeyCtrl);
}

void VIEWPORT::Input_MoveScale (float dScale)
{
   m_pImpl->Input_MoveScale (dScale);
}

VIEWPORT::INPUT VIEWPORT::Input_Consume ()
{
   return m_pImpl->Input_Consume ();
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

void VIEWPORT::FrameBuffer_Write (const uint32_t* pPixels, int nWidth, int nHeight)
{
   m_pImpl->FrameBuffer_Write (pPixels, nWidth, nHeight);
}

const uint32_t* VIEWPORT::FrameBuffer_Capture (int& nWidth, int& nHeight)
{
   return m_pImpl->FrameBuffer_Capture (nWidth, nHeight);
}

void VIEWPORT::FrameBuffer_Release ()
{
   m_pImpl->FrameBuffer_Release ();
}

void VIEWPORT::Accumulate (eACCUMULATE eType, std::chrono::steady_clock::time_point tpStart)
{
   double dDuration = std::chrono::duration<double> (std::chrono::steady_clock::now () - tpStart).count ();

   switch (eType)
   {
      case kACCUMULATE_INPUT:   m_dAccumInput   += dDuration;  break;
      case kACCUMULATE_SCENE:   m_dAccumScene   += dDuration;  break;
      case kACCUMULATE_SUBMIT:  m_dAccumSubmit  += dDuration;  break;
      case kACCUMULATE_RENDER:  m_dAccumRender  += dDuration;  break;
      case kACCUMULATE_PUBLISH: m_dAccumPublish += dDuration;  break;
   }
}

void VIEWPORT::Accumulate (eACCUMULATE eType, double dSeconds)
{
   switch (eType)
   {
      case kACCUMULATE_INPUT:   m_dAccumInput   += dSeconds;  break;
      case kACCUMULATE_SCENE:   m_dAccumScene   += dSeconds;  break;
      case kACCUMULATE_SUBMIT:  m_dAccumSubmit  += dSeconds;  break;
      case kACCUMULATE_RENDER:  m_dAccumRender  += dSeconds;  break;
      case kACCUMULATE_PUBLISH: m_dAccumPublish += dSeconds;  break;
   }
}

void VIEWPORT::Diagnostics ()
{
   auto tpNow  = std::chrono::steady_clock::now ();

   m_nFrameCount++;
   
   m_dFpsAccum += std::chrono::duration<double> (tpNow - m_tpLastFrame).count ();

   if (m_dFpsAccum >= 1.0)
   {
      double dAvgInput   = m_dAccumInput   / m_nFrameCount * 1000.0;
      double dAvgScene   = m_dAccumScene   / m_nFrameCount * 1000.0;
      double dAvgSubmit  = m_dAccumSubmit  / m_nFrameCount * 1000.0;
      double dAvgRender  = m_dAccumRender  / m_nFrameCount * 1000.0;
      double dAvgPublish = m_dAccumPublish / m_nFrameCount * 1000.0;
      double dAvgFrame   = m_dFpsAccum     / m_nFrameCount * 1000.0;

      char szFps[256];
      std::snprintf (szFps, sizeof (szFps), "%d  (frame %.1f ms | input %.1f ms | scene %.1f ms | submit %.1f ms | render %.1f ms | publish %.1f ms)", m_nFrameCount, dAvgFrame, dAvgInput, dAvgScene, dAvgSubmit, dAvgRender, dAvgPublish);

      Engine ()->Log (IENGINE::kLOGLEVEL_Trace, "FPS", std::string (szFps));

      m_nFrameCount   = 0;

      m_dFpsAccum    -= 1.0;

      m_dAccumInput   = 0.0;
      m_dAccumScene   = 0.0;
      m_dAccumSubmit  = 0.0;
      m_dAccumRender  = 0.0;
      m_dAccumPublish = 0.0;
   }

   m_tpLastFrame = tpNow;
}

// ---------------------------------------------------------------------------
// VIEW (camera orbit)
// ---------------------------------------------------------------------------

void VIEWPORT::VIEW::Update (int nDX, int nDY, float dScrollY, bool bMouseLeft, bool bMouseRight,
                             bool bKeyA, bool bKeyS, bool bKeyD, bool bKeyW,
                             bool bKeySpace, bool bKeyCtrl, float dMoveScale, float dDeltaSeconds)
{
   if (bMouseLeft)
   {
      // First-person look: hold the eye fixed and swing the view direction, so
      // the target orbits the eye (not the eye orbiting the target). Mouse-left
      // looks left, mouse-right looks right; a full sweep returns to the start.
      float dCosPhi = std::cos (m_dPhi);
      RMAP::MAP::MAP_OBJECT::VEC3  vEye;
      vEye.dX = m_vTarget.dX + m_dDistance * dCosPhi * std::cos (m_dTheta);
      vEye.dY = m_vTarget.dY + m_dDistance * dCosPhi * std::sin (m_dTheta);
      vEye.dZ = m_vTarget.dZ + m_dDistance * std::sin (m_dPhi);

      m_dTheta -= nDX * MOUSE_SENSITIVITY;
      m_dPhi   += nDY * MOUSE_SENSITIVITY;
      m_dPhi = std::max (-PI_F * 0.49f, std::min (PI_F * 0.49f, m_dPhi));

      dCosPhi = std::cos (m_dPhi);
      m_vTarget.dX = vEye.dX - m_dDistance * dCosPhi * std::cos (m_dTheta);
      m_vTarget.dY = vEye.dY - m_dDistance * dCosPhi * std::sin (m_dTheta);
      m_vTarget.dZ = vEye.dZ - m_dDistance * std::sin (m_dPhi);
   }

   if (dScrollY != 0.0f)
   {
      float dFactor = (dScrollY > 0.0f) ? (1.0f / SCROLL_FACTOR) : SCROLL_FACTOR;
      m_dDistance *= dFactor;
      m_dDistance = std::max (MIN_DISTANCE, std::min (MAX_DISTANCE, m_dDistance));
   }

   // WASD pans the orbit target on the XY ground plane, relative to camera
   // facing (azimuth). Space / Ctrl move along +Z / -Z. Speed scales with
   // orbit distance.
   if (bKeyA  ||  bKeyS  ||  bKeyD  ||  bKeyW  ||  bKeySpace  ||  bKeyCtrl)
   {
      float dCos = std::cos (m_dTheta);
      float dSin = std::sin (m_dTheta);
      float dFwdX = -dCos;
      float dFwdY = -dSin;
      float dRightX = -dSin;
      float dRightY =  dCos;
      float dStep = m_dDistance * KEY_PAN_FRACTION * dMoveScale * dDeltaSeconds * KEY_PAN_REFERENCE_HZ;
      float dMoveX = 0.0f;
      float dMoveY = 0.0f;
      float dMoveZ = 0.0f;

      if (bKeyW)     { dMoveX += dFwdX;   dMoveY += dFwdY; }
      if (bKeyS)     { dMoveX -= dFwdX;   dMoveY -= dFwdY; }
      if (bKeyD)     { dMoveX += dRightX; dMoveY += dRightY; }
      if (bKeyA)     { dMoveX -= dRightX; dMoveY -= dRightY; }
      if (bKeySpace) { dMoveZ += 1.0f; }
      if (bKeyCtrl)  { dMoveZ -= 1.0f; }

      float dLen = std::sqrt (dMoveX * dMoveX + dMoveY * dMoveY + dMoveZ * dMoveZ);
      if (dLen > 1e-6f)
      {
         m_vTarget.dX += static_cast<double> (dMoveX / dLen * dStep);
         m_vTarget.dY += static_cast<double> (dMoveY / dLen * dStep);
         m_vTarget.dZ += static_cast<double> (dMoveZ / dLen * dStep);
      }
   }
}
