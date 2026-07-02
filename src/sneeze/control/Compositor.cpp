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

// ---------------------------------------------------------------------------
// THREAD AFFINITY WORKAROUND (Halogen / Filament)
//
// Problem:
//   Filament requires its Engine to be created and destroyed on the same
//   thread. Halogen (our ANARI implementation backed by Filament) does not
//   expose Filament's adoptCommandStream() or any thread-transfer API.
//   This means we cannot create the renderer on one thread and destroy it
//   on another without triggering:
//     "Precondition: shutdown() called from the wrong thread!"
//
// Mitigation:
//   Compositor agent index 0 is the designated lifecycle thread. All
//   JOB_COMPOSITOR jobs in kSTATE_CREATE or kSTATE_DESTROY are routed
//   exclusively to agent 0 by POOL_CYCLE::Grab(). This guarantees
//   renderer creation and destruction happen on the same OS thread.
//
// Preferred fix (Halogen):
//   Expose a thread-transfer API on the ANARI device (equivalent to
//   Filament's adoptCommandStream or unprotect) so that
//   ownership can be handed from the compositor thread back to the main
//   thread before destruction.
// ---------------------------------------------------------------------------

#include "Control.h"
#include "Types.h"
#include "context/viewport/Viewport.h"
#include <cmath>
#include <cstring>
#include <functional>

using namespace SNEEZE;

// ===========================================================================
// JOB_COMPOSITOR
// ===========================================================================

JOB_COMPOSITOR::JOB_COMPOSITOR (VIEWPORT* pViewport) :
   m_pViewport    (pViewport),
   m_eState       (kSTATE_CREATE),
   m_bBusy        (false),
   m_bCancelled   (false),
   m_nLastFrame   (0),
   m_dRenderScale (0.0f)
{
}

JOB_COMPOSITOR::eSTATE JOB_COMPOSITOR::State () const
{
   return m_eState;
}

VIEWPORT* JOB_COMPOSITOR::Viewport () const
{
   return m_pViewport;
}

bool JOB_COMPOSITOR::Busy ()
{
   // this function is called exclusively by POOL_CYCLE::Grab () to lock a job

   bool bBusy = !m_bBusy;

   if (bBusy)
   {
      m_mxJob.lock ();
      {
         m_bBusy = true;
      }
   }


   return bBusy;
}

void JOB_COMPOSITOR::Idle ()
{
   // this function is called exclusively by POOL_CYCLE::Grab () on a locked job

   {
      m_bBusy = false;
   }
   m_mxJob.unlock ();
}

void JOB_COMPOSITOR::Unlock ()
{
   // this function is called exclusively by POOL_CYCLE::Grab () on a locked job

   {
   }
   m_mxJob.unlock ();
}

void JOB_COMPOSITOR::Return (eSTATE eState)
{
   // this function is called exclusively by AGENT::COMPOSITOR::Job () on a grabbed job

   m_mxJob.lock ();
   {
      if (m_eState != kSTATE_DESTROY)
         m_eState = eState;

      m_bBusy  = false;
   }
   m_mxJob.unlock ();
}

void JOB_COMPOSITOR::Cancel ()
{
   std::unique_lock<std::recursive_mutex> lock (m_mxCancel);

   m_mxJob.lock ();
   {
      m_eState = kSTATE_DESTROY;
   }
   m_mxJob.unlock ();

   m_cvCancel.wait (lock, [this] { return m_bCancelled; });
}

void JOB_COMPOSITOR::Complete ()
{
   {
      std::lock_guard<std::recursive_mutex> guard (m_mxCancel);

      m_bCancelled = true;
   }
   m_cvCancel.notify_one ();
}

void JOB_COMPOSITOR::Complete_Deliver ()
{
}

// ===========================================================================
// AGENT::COMPOSITOR
// ===========================================================================

static constexpr float MIN_SPHERE_RADIUS = 0.0f;

// Bodies are far too small to see at honest scene scale, so a body's VISUAL
// radius is a compressed magnification of its true radius (already in render
// space): BODY_MAG * (radius_render ^ BODY_EXP). BODY_EXP < 1 squashes the huge
// range of real radii; raise it toward 1.0 for more size variation between
// bodies. These are the only "art" knobs -- positions stay 1:1 (scaled).
static constexpr double BODY_MAG = 1.25;
static constexpr double BODY_EXP = 0.7;

// The one celestial kludge (moons only): a moon orbits farther out so it clears
// its magnified planet, and renders smaller than the planet magnification.
static constexpr double MOON_ORBIT_BOOST = 5.0;
static constexpr double MOON_SIZE_FACTOR = 1.0;

static constexpr int    TRAIL_SEGMENTS   = 128;
static constexpr double TRAIL_FRACTION   = 0.75;
static constexpr float  TRAIL_RADIUS_PLANET = 0.0002f;
static constexpr float  TRAIL_RADIUS_MOON   = 0.00005f;

static void ColorFromU32 (uint32_t nColor, float& r, float& g, float& b)
{
   r = static_cast<float> ((nColor >> 16) & 0xFF) / 255.0f;
   g = static_cast<float> ((nColor >> 8)  & 0xFF) / 255.0f;
   b = static_cast<float> (nColor & 0xFF) / 255.0f;
}

static void ColorFromPropertyFloat (float fColor, float& r, float& g, float& b)
{
   uint32_t nColor;
   memcpy (&nColor, &fColor, 4);
   ColorFromU32 (nColor & 0x00FFFFFF, r, g, b);
}

// Stable per-object color so adjacent boxes are visually distinct.
static void ColorFromIndex (uint64_t nIx, float& r, float& g, float& b)
{
   uint32_t h = static_cast<uint32_t> (nIx * 2654435761ull);
   r = 0.45f + 0.45f * (static_cast<float> ( h        & 0xFF) / 255.0f);
   g = 0.45f + 0.45f * (static_cast<float> ((h >> 8)  & 0xFF) / 255.0f);
   b = 0.45f + 0.45f * (static_cast<float> ((h >> 16) & 0xFF) / 255.0f);
}

// --- Double-precision 4x4 transforms (column-major, translation in d[12..14]) ---

static MAT4 Mat4_Identity ()
{
   MAT4 m = {};
   m.d[0] = m.d[5] = m.d[10] = m.d[15] = 1.0;
   return m;
}

static MAT4 Mat4_Multiply (const MAT4& a, const MAT4& b)
{
   MAT4 c = {};

   for (int j = 0; j < 4; j++)
   {
      for (int i = 0; i < 4; i++)
      {
         double dSum = 0.0;
         for (int k = 0; k < 4; k++)
            dSum += a.d[k * 4 + i] * b.d[j * 4 + k];
         c.d[j * 4 + i] = dSum;
      }
   }

   return c;
}

static MAT4 Mat4_FromTRS (double tx, double ty, double tz,
                          double qx, double qy, double qz, double qw,
                          double sx, double sy, double sz)
{
   double r00 = 1.0 - 2.0 * (qy * qy + qz * qz);
   double r01 =       2.0 * (qx * qy - qw * qz);
   double r02 =       2.0 * (qx * qz + qw * qy);
   double r10 =       2.0 * (qx * qy + qw * qz);
   double r11 = 1.0 - 2.0 * (qx * qx + qz * qz);
   double r12 =       2.0 * (qy * qz - qw * qx);
   double r20 =       2.0 * (qx * qz - qw * qy);
   double r21 =       2.0 * (qy * qz + qw * qx);
   double r22 = 1.0 - 2.0 * (qx * qx + qy * qy);

   MAT4 m = {};
   m.d[0]  = r00 * sx;  m.d[1]  = r10 * sx;  m.d[2]  = r20 * sx;  m.d[3]  = 0.0;
   m.d[4]  = r01 * sy;  m.d[5]  = r11 * sy;  m.d[6]  = r21 * sy;  m.d[7]  = 0.0;
   m.d[8]  = r02 * sz;  m.d[9]  = r12 * sz;  m.d[10] = r22 * sz;  m.d[11] = 0.0;
   m.d[12] = tx;        m.d[13] = ty;        m.d[14] = tz;        m.d[15] = 1.0;
   return m;
}

// Transform a point (w = 1) by a column-major MAT4.
static void Mat4_Point (const MAT4& m, double x, double y, double z, double& ox, double& oy, double& oz)
{
   ox = m.d[0] * x + m.d[4] * y + m.d[8]  * z + m.d[12];
   oy = m.d[1] * x + m.d[5] * y + m.d[9]  * z + m.d[13];
   oz = m.d[2] * x + m.d[6] * y + m.d[10] * z + m.d[14];
}

// A body's visible radius from its true radius (metres) and the scene scale.
static float MagnifyRadius (double dRadiusM, double dScale, bool bMoon)
{
   double dRender = BODY_MAG * std::pow (dRadiusM * dScale, BODY_EXP);
   if (bMoon) dRender *= MOON_SIZE_FACTOR;
   if (dRender < MIN_SPHERE_RADIUS) dRender = MIN_SPHERE_RADIUS;
   return static_cast<float> (dRender);
}

// Scene is sized so its root-anchored bounding sphere maps to this many render
// units, keeping coordinates float-friendly and framed by the default camera.
static constexpr double TARGET_EXTENT = 5.0;
static constexpr double MIN_REACH     = 1e-6;

static int64_t s_nGlobalFrameSeq = 0;

// ---------------------------------------------------------------------------

AGENT::COMPOSITOR::COMPOSITOR (POOL* pPool, int nAgentIz) : AGENT (pPool, nAgentIz)
{
}

AGENT::COMPOSITOR::~COMPOSITOR ()
{
   Join ();
}

void AGENT::COMPOSITOR::Main ()
{
   Ready ();

   Wait ([this] { return Job (); });
}

bool AGENT::COMPOSITOR::Job ()
{
   auto* pPool_Cycle = static_cast<POOL_CYCLE*> (m_pPool);

   bool bResult, bJob;
   JOB_COMPOSITOR* pJob_Compositor = nullptr;

   while (true)
   {
      bResult = IsShutdown ();
      bJob    = pPool_Cycle->Grab (pJob_Compositor, m_nAgentIz);

      m_bBusy.store (bJob, std::memory_order_release);

      if (bJob)
      {
         switch (pJob_Compositor->State ())
         {
            case JOB_COMPOSITOR::kSTATE_CREATE:   Execute_Create  (pJob_Compositor);  break;
            case JOB_COMPOSITOR::kSTATE_RENDER:   Execute_Render  (pJob_Compositor);  break;
            case JOB_COMPOSITOR::kSTATE_PRESENT:  Execute_Present (pJob_Compositor);  break;
            case JOB_COMPOSITOR::kSTATE_DESTROY:  Execute_Destroy (pJob_Compositor);  break;
         }
      }
      else break;
   }

   return bResult;
}

void AGENT::COMPOSITOR::Execute_Create (JOB_COMPOSITOR* pJob_Compositor)
{
   VIEWPORT*           pViewport = pJob_Compositor->Viewport();
   IVIEWPORT*          pHost     = pViewport->Host();

   int nHostW, nHostH;

   if (m_nAgentIz == 0)
   {
      VIEWPORT* pViewport = pJob_Compositor->Viewport ();

      pViewport->Size (nHostW, nHostH);

      pHost->FrameSize  (nHostW, nHostH);
      pViewport->Resize (nHostW, nHostH);

      pViewport->Renderer_Initialize ();

      pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_RENDER);
   }      
   else pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_CREATE);
}

void AGENT::COMPOSITOR::Execute_Destroy (JOB_COMPOSITOR* pJob_Compositor)
{
   auto* pPool_Cycle = static_cast<POOL_CYCLE*> (m_pPool);

   if (m_nAgentIz == 0)
   {
      VIEWPORT* pViewport = pJob_Compositor->Viewport ();

      pViewport->Renderer_Shutdown ();

      pPool_Cycle->Remove (pJob_Compositor);

      pJob_Compositor->Complete ();
   }
   else pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_DESTROY);
}

// Carried parent->child during traversal. mWorld is the accumulated world
// transform in SI metres (double); the render scale is applied later, at the
// single flatten seam, so the SOM stays meters end to end. dRadius/fColor/bStar
// hand celestial body appearance from a system/body node down to its surface.
struct WORLD_FRAME
{
   MAT4   mWorld  = { { 1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0 } };
   double dRadius = 0.0;
   float  fColor  = 0.0f;
   bool   bStar   = false;
   bool   bMoon   = false;
};

// Everything is gathered during traversal in SI metres (double); the single
// per-scene render scale is applied once, at the flatten seam, when these are
// turned into the renderer's float structures. No AU, no per-emit scaling.
struct BOX_BUILD
{
   MAT4  mWorld;                 // metres
   float r, g, b;
};

struct SPHERE_BUILD
{
   double dx, dy, dz;            // metres
   double dRadiusM;             // true body radius, metres
   bool   bMoon;
   bool   bEmissive;
   float  r, g, b;
   const uint8_t* pTex = nullptr;
   int    nTexW = 0;
   int    nTexH = 0;
};

struct CURVE_BUILD
{
   std::vector<CURVE_POINT> aPoints;   // x/y/z metres; dRadius is render-space
   float r, g, b;
};

struct LIGHT_BUILD
{
   double dx = 0.0, dy = 0.0, dz = 0.0;   // metres (point) / direction (directional)
   float  r = 1.0f, g = 1.0f, b = 0.95f;  // colour; defaults match a warm star key light
   float  dIntensity = 4.0f;
   double dWorldScale = 1.0;              // accumulated linear scale at the light's node frame
   bool   bCompensate = true;            // apply (worldScale * renderScale)^2 invariance to a point light
   int    eType = LIGHT_DATA::kPOINT;
};

struct PANEL_BUILD
{
   const uint8_t* pPixels;       // straight-alpha RGBA8, top-down (owned by the panel node)
   int            nW, nH;
   double         dAspect;       // panel width / height (quad shape only)
   double         dWx, dWy, dWz; // node world position (metres)
};

// One glTF/GLB draw gathered during traversal. mWorld is the draw's full world
// transform in metres (node world * the model-internal draw transform); pSrc
// borrows the node-owned MESH_DATA for its vertex streams and material, which
// are copied through unchanged at the flatten seam (only m16 is rescaled).
struct MESH_BUILD
{
   MAT4             mWorld;      // metres
   const MESH_DATA* pSrc;
};

static void TraverseNode (NODE* pNode, const WORLD_FRAME& frame, int64_t tmNow, SNEEZE::ENGINE* pEngine, std::vector<SPHERE_BUILD>& aSphere, std::vector<CURVE_BUILD>& aCurve, std::vector<LIGHT_BUILD>& aLight, std::vector<BOX_BUILD>& aBox, std::vector<PANEL_BUILD>& aPanel, std::vector<MESH_BUILD>& aMesh, double& dMaxReach)
{
   MAP_OBJECT* pObj = pNode->Map_Object ();
   WORLD_FRAME childFrame = frame;

   if (pObj)
   {
      // Universal TRS: every node, root to leaf, composes its local
      // translation/rotation/scale onto its parent's world transform. No class
      // is exempt -- celestial, terrestrial and physical all inherit identically.
      double dPosX, dPosY, dPosZ;
      double dQx, dQy, dQz, dQw;
      double dSx, dSy, dSz;
      pObj->Position (tmNow, dPosX, dPosY, dPosZ);
      pObj->Rotation (tmNow, dQx, dQy, dQz, dQw);
      pObj->Scale (dSx, dSy, dSz);

      // The one celestial kludge: a moon system's orbit is pushed outward so the
      // moon clears its magnified planet. Everything else stays 1:1 (metres).
      bool bMoonSystem = (pObj->Class () == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CLASS_CELESTIAL
                       &&  static_cast<MAP_OBJECT_CELESTIAL*> (pObj)->Type.bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOONSYSTEM);
      if (bMoonSystem)
      {
         dPosX *= MOON_ORBIT_BOOST;
         dPosY *= MOON_ORBIT_BOOST;
         dPosZ *= MOON_ORBIT_BOOST;
      }

      MAT4 mLocal = Mat4_FromTRS (dPosX, dPosY, dPosZ, dQx, dQy, dQz, dQw, dSx, dSy, dSz);
      childFrame.mWorld = Mat4_Multiply (frame.mWorld, mLocal);

      double dWx = childFrame.mWorld.d[12];
      double dWy = childFrame.mWorld.d[13];
      double dWz = childFrame.mWorld.d[14];

      // Every node's world position contributes to the scene's metre extent, so
      // the single render scale frames the whole thing. Lights are excluded:
      // they illuminate the scene but must not reframe it (a light placed out
      // beyond the geometry would otherwise shrink everything else).
      if (pObj->Class () != MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT)
      {
         double dReach = std::sqrt (dWx * dWx + dWy * dWy + dWz * dWz);
         if (dReach > dMaxReach) dMaxReach = dReach;
      }

      // A loaded glTF/GLB renders its own geometry regardless of the node's class
      // -- a model can sit at the celestial, terrestrial, or physical level. Nodes
      // whose resource is an "action:" reference (e.g. colliders) are invisible
      // logic volumes and never carry geometry.
      const GLTF_RENDER_MODEL* pModel = nullptr;
      if (std::strncmp (pObj->Resource.sReference, "action:", 7) != 0)
         pModel = pObj->Gltf_Render_Model ();

      if (pModel)
      {
         // Each draw's model-internal transform composes under this node's world
         // frame; the streams/material ride through untouched.
         for (const MESH_DATA& draw : pModel->aMesh)
         {
            MAT4 mLocal;
            for (int j = 0; j < 16; j++)
               mLocal.d[j] = draw.m16[j];

            MESH_BUILD mesh;
            mesh.mWorld = Mat4_Multiply (childFrame.mWorld, mLocal);
            mesh.pSrc   = &draw;
            aMesh.push_back (mesh);
         }

         // The model's bounding sphere (center carried through the node frame,
         // radius scaled by the frame's average axis scale) extends the scene
         // reach so the single render scale frames it like everything else.
         double dCx, dCy, dCz;
         Mat4_Point (childFrame.mWorld, pModel->aCenter[0], pModel->aCenter[1], pModel->aCenter[2], dCx, dCy, dCz);
         double dCol0 = std::sqrt (childFrame.mWorld.d[0] * childFrame.mWorld.d[0] + childFrame.mWorld.d[1] * childFrame.mWorld.d[1] + childFrame.mWorld.d[2]  * childFrame.mWorld.d[2]);
         double dCol1 = std::sqrt (childFrame.mWorld.d[4] * childFrame.mWorld.d[4] + childFrame.mWorld.d[5] * childFrame.mWorld.d[5] + childFrame.mWorld.d[6]  * childFrame.mWorld.d[6]);
         double dCol2 = std::sqrt (childFrame.mWorld.d[8] * childFrame.mWorld.d[8] + childFrame.mWorld.d[9] * childFrame.mWorld.d[9] + childFrame.mWorld.d[10] * childFrame.mWorld.d[10]);
         double dScale = (dCol0 + dCol1 + dCol2) / 3.0;
         double dMeshReach = std::sqrt (dCx * dCx + dCy * dCy + dCz * dCz) + pModel->dRadius * dScale;
         if (dMeshReach > dMaxReach) dMaxReach = dMeshReach;
      }

      if (pObj->Class () == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CLASS_CELESTIAL)
      {
         auto* pCelestial = static_cast<MAP_OBJECT_CELESTIAL*> (pObj);
         uint8_t bType = pCelestial->Type.bType;

         if (bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM
         ||  bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANETSYSTEM
         ||  bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOONSYSTEM
         ||  bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRISSYSTEM)
         {
            if (pCelestial->HasOrbit ())
            {
               float dTrailRadius = (bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOONSYSTEM  || bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRISSYSTEM) ? TRAIL_RADIUS_MOON : TRAIL_RADIUS_PLANET;

               CURVE_BUILD curve;
               ColorFromPropertyFloat (pCelestial->Properties.fColor, curve.r, curve.g, curve.b);
               curve.r *= 0.4f;
               curve.g *= 0.4f;
               curve.b *= 0.4f;

               int nTrailPoints = static_cast<int> (TRAIL_SEGMENTS * TRAIL_FRACTION);

               CURVE_POINT cpHead;
               cpHead.x       = static_cast<float> (dWx);
               cpHead.y       = static_cast<float> (dWy);
               cpHead.z       = static_cast<float> (dWz);
               cpHead.dRadius = dTrailRadius;
               curve.aPoints.push_back (cpHead);

               MAP_OBJECT_CELESTIAL::ORBIT_POSITION pos;

               if (pCelestial->PositionAtTick (tmNow, pos))
               {
                  double dE_planet = pos.dE;
                  for (int i = 1; i <= nTrailPoints; i++)
                  {
                     double dE = dE_planet - (static_cast<double> (i) / TRAIL_SEGMENTS) * TWO_PI;
                     VEC3 vPt = pCelestial->OrbitTrailPoint (dE, tmNow);

                     if (bMoonSystem)
                     {
                        vPt.x *= MOON_ORBIT_BOOST;
                        vPt.y *= MOON_ORBIT_BOOST;
                        vPt.z *= MOON_ORBIT_BOOST;
                     }

                     // The trail lives in the parent frame; carry it through the
                     // parent's full world transform, same basis the node inherits.
                     double dTx, dTy, dTz;
                     Mat4_Point (frame.mWorld, vPt.x, vPt.y, vPt.z, dTx, dTy, dTz);

                     float dTaper = 1.0f - static_cast<float> (i) / static_cast<float> (nTrailPoints);

                     CURVE_POINT cp;
                     cp.x       = static_cast<float> (dTx);
                     cp.y       = static_cast<float> (dTy);
                     cp.z       = static_cast<float> (dTz);
                     cp.dRadius = dTrailRadius * dTaper;
                     curve.aPoints.push_back (cp);
                  }
               }

               aCurve.push_back (std::move (curve));
            }
         }
         else if (bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_STAR
              ||  bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANET
              ||  bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOON
              ||  bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRIS)
         {
            childFrame.dRadius = pCelestial->Radius ();
            childFrame.fColor  = pCelestial->Properties.fColor;
            childFrame.bStar   = (bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_STAR);
            childFrame.bMoon   = (bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOON);

            // A body's own radius extends the scene's reach, so a single body
            // centred at the origin still yields a sane scale (no divide-by-zero
            // fallback to 1.0 that would render it at near-true metres).
            double dBodyReach = std::sqrt (dWx * dWx + dWy * dWy + dWz * dWz) + childFrame.dRadius;
            if (dBodyReach > dMaxReach) dMaxReach = dBodyReach;

            if (childFrame.bStar)
            {
               LIGHT_BUILD light;
               light.dx = dWx;
               light.dy = dWy;
               light.dz = dWz;
               // A star's point light uses 1/r^2 falloff, and at solar render
               // scale a body sits very close to the star in render units -- the
               // default intensity blows surfaces out. Dim it so the lit limb
               // reads with detail instead of clipping to white. This is an
               // engine-generated light already tuned at render scale, so it
               // opts out of the unit-scale intensity invariance below.
               light.dIntensity = 5.6f; //0.09f;
               light.bCompensate = false;
               aLight.push_back (light);
            }
         }
         else if (bType == MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_TYPE_CELESTIAL_SURFACE)
         {
            SPHERE_BUILD sphere;
            sphere.dx        = dWx;
            sphere.dy        = dWy;
            sphere.dz        = dWz;
            sphere.dRadiusM  = childFrame.dRadius;
            sphere.bMoon     = childFrame.bMoon;
            sphere.bEmissive = childFrame.bStar;
            ColorFromPropertyFloat (childFrame.fColor, sphere.r, sphere.g, sphere.b);

            pCelestial->GetTexture (sphere.pTex, sphere.nTexW, sphere.nTexH);

            aSphere.push_back (sphere);
         }
      }
      else if (pObj->Class () == MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL
           &&  std::strncmp (pObj->Resource.sReference, "action:", 7) != 0
           &&  !pModel)
      {
         // A model-less physical node falls back to a grounded box from its bound
         // so it stays visible (any glTF/GLB it carries was already emitted above).
         // Nodes whose resource is an "action:" reference (e.g. colliders) are
         // invisible logic volumes, never geometry.
         double dW = pObj->Bound.d3Max[0];
         double dH = pObj->Bound.d3Max[1];
         double dD = pObj->Bound.d3Max[2];

         if (dW > 0.0  ||  dH > 0.0  ||  dD > 0.0)
         {
            // Map the centered unit cube to a grounded box (base at y=0, rises by dH).
            MAT4 mBound = Mat4_Identity ();
            mBound.d[0]  = dW;
            mBound.d[5]  = dH;
            mBound.d[10] = dD;
            mBound.d[13] = dH * 0.5;

            BOX_BUILD box;
            box.mWorld = Mat4_Multiply (childFrame.mWorld, mBound);
            ColorFromIndex (pObj->Head.Self.ObjectIx (), box.r, box.g, box.b);
            aBox.push_back (box);

            double dCenterX = box.mWorld.d[12];
            double dCenterY = box.mWorld.d[13];
            double dCenterZ = box.mWorld.d[14];
            double dCol0 = std::sqrt (box.mWorld.d[0]  * box.mWorld.d[0]  + box.mWorld.d[1]  * box.mWorld.d[1]  + box.mWorld.d[2]  * box.mWorld.d[2]);
            double dCol1 = std::sqrt (box.mWorld.d[4]  * box.mWorld.d[4]  + box.mWorld.d[5]  * box.mWorld.d[5]  + box.mWorld.d[6]  * box.mWorld.d[6]);
            double dCol2 = std::sqrt (box.mWorld.d[8]  * box.mWorld.d[8]  + box.mWorld.d[9]  * box.mWorld.d[9]  + box.mWorld.d[10] * box.mWorld.d[10]);
            double dHalf = 0.5 * (dCol0 + dCol1 + dCol2);
            double dBoxReach = std::sqrt (dCenterX * dCenterX + dCenterY * dCenterY + dCenterZ * dCenterZ) + dHalf;
            if (dBoxReach > dMaxReach) dMaxReach = dBoxReach;
         }
      }
      else if (pObj->Class () == MAP_OBJECT::MAP_OBJECT_CLASS_PANEL)
      {
         // A panel manifests as a flat, textured quad. The UI is rasterized here,
         // on the compositor thread (the only thread that touches both RmlUi and
         // the renderer); the cost is paid once and cached, so re-traversal each
         // frame is cheap. Panels are chrome, not scene geometry: they do NOT
         // contribute to dMaxReach, so a panel never changes how the 3D content
         // is framed. Bound.d3Max[0,1] gives only the quad's aspect ratio; the
         // panel's world position rides the node's TRS (captured here) and its
         // on-screen size is resolved at the flatten seam (below).
         auto* pPanel = static_cast<MAP_OBJECT_PANEL*> (pObj);
         double dPanelW = pObj->Bound.d3Max[0];
         double dPanelH = pObj->Bound.d3Max[1];

         if (dPanelW > 0.0  &&  dPanelH > 0.0  &&  pPanel->Render (pEngine, 512, 512)  &&  pPanel->Pixels ())
         {
            PANEL_BUILD panel;
            panel.pPixels = pPanel->Pixels ();
            panel.nW      = pPanel->Width ();
            panel.nH      = pPanel->Height ();
            panel.dAspect = dPanelW / dPanelH;
            panel.dWx     = dWx;
            panel.dWy     = dWy;
            panel.dWz     = dWz;
            aPanel.push_back (panel);
         }
      }
      else if (pObj->Class () == MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT)
      {
         // A light node contributes an ANARI light at its world placement. Colour
         // comes from Properties.fColor (0xRRGGBB), intensity from fBrightness,
         // and the subtype selects point / ambient / directional.
         LIGHT_BUILD light;
         light.dx = dWx;
         light.dy = dWy;
         light.dz = dWz;
         ColorFromPropertyFloat (pObj->Properties.fColor, light.r, light.g, light.b);
         light.dIntensity = pObj->Properties.fBrightness;

         // Accumulated linear scale of this light's world frame (average of the
         // upper-3x3 column norms). A point light authored at unit scale is
         // intensity-compensated by (worldScale * renderScale)^2 at the flatten
         // seam so its illumination is invariant to both the embed scale and the
         // per-scene render scale -- author once at unit scale, drop in anywhere.
         {
            double dSc0 = std::sqrt (childFrame.mWorld.d[0] * childFrame.mWorld.d[0] + childFrame.mWorld.d[1] * childFrame.mWorld.d[1] + childFrame.mWorld.d[2]  * childFrame.mWorld.d[2]);
            double dSc1 = std::sqrt (childFrame.mWorld.d[4] * childFrame.mWorld.d[4] + childFrame.mWorld.d[5] * childFrame.mWorld.d[5] + childFrame.mWorld.d[6]  * childFrame.mWorld.d[6]);
            double dSc2 = std::sqrt (childFrame.mWorld.d[8] * childFrame.mWorld.d[8] + childFrame.mWorld.d[9] * childFrame.mWorld.d[9] + childFrame.mWorld.d[10] * childFrame.mWorld.d[10]);
            light.dWorldScale = (dSc0 + dSc1 + dSc2) / 3.0;
         }

         switch (pObj->Type.bType)
         {
            case MAP_OBJECT_LIGHT::MAP_OBJECT_TYPE_TYPE_LIGHT_AMBIENT:     light.eType = LIGHT_DATA::kAMBIENT;     break;
            case MAP_OBJECT_LIGHT::MAP_OBJECT_TYPE_TYPE_LIGHT_DIRECTIONAL: light.eType = LIGHT_DATA::kDIRECTIONAL; break;
            default:                                                       light.eType = LIGHT_DATA::kPOINT;       break;
         }

         aLight.push_back (light);
      }
   }

   for (int i = 0; i < pNode->Node_Count (); i++)
   {
      NODE* pChild = pNode->Child (i);
      if (pChild)
         TraverseNode (pChild, childFrame, tmNow, pEngine, aSphere, aCurve, aLight, aBox, aPanel, aMesh, dMaxReach);
   }

   // An attachment point spawns a child fabric; traverse it in this node's own
   // accumulated frame so the secondary fabric inherits this node's transform.
   FABRIC* pAttached = pNode->Fabric_Attachment ();
   if (pAttached  &&  pAttached->Node_Root ())
      TraverseNode (pAttached->Node_Root (), childFrame, tmNow, pEngine, aSphere, aCurve, aLight, aBox, aPanel, aMesh, dMaxReach);
}

void AGENT::COMPOSITOR::Execute_Render (JOB_COMPOSITOR* pJob_Compositor)
{
   VIEWPORT*           pViewport = pJob_Compositor->Viewport ();
   VIEWPORT::RENDERER* pRenderer = pViewport->Renderer ();
   IVIEWPORT*          pHost     = pViewport->Host ();
   int64_t             tmNow     = pViewport->m_tmNow;

   int nHostW, nHostH;

   if (pRenderer  &&  pHost)
   {
      auto tpLoopStart = std::chrono::steady_clock::now ();

      pViewport->Size (nHostW, nHostH);

      if (pHost->FrameSize (nHostW, nHostH))
      {
         pViewport->Resize (nHostW, nHostH);
         pRenderer->Resize (nHostW, nHostH);
      }

      VIEWPORT::INPUT Input = pViewport->Input_Consume ();
      VIEWPORT::VIEW& View = pViewport->View ();

      // Any camera interaction releases an active scene-driven pose so the user
      // takes over the orbit from wherever the pose last placed it.
      if (Input.nMouseDX != 0  ||  Input.nMouseDY != 0  ||  Input.dScrollY != 0.0f)
         pViewport->Camera_Deactivate ();

      View.Update (Input.nMouseDX, Input.nMouseDY, Input.dScrollY, Input.bMouseLeft, Input.bMouseRight);

      float dCamX = View.m_dTargetX + View.m_dDistance * std::cos (View.m_dPhi) * std::cos (View.m_dTheta);
      float dCamY = View.m_dTargetY + View.m_dDistance * std::sin (View.m_dPhi);
      float dCamZ = View.m_dTargetZ + View.m_dDistance * std::cos (View.m_dPhi) * std::sin (View.m_dTheta);

      CAMERA_DATA Camera;
      Camera.dPosX = dCamX;
      Camera.dPosY = dCamY;
      Camera.dPosZ = dCamZ;
      Camera.dDirX = View.m_dTargetX - dCamX;
      Camera.dDirY = View.m_dTargetY - dCamY;
      Camera.dDirZ = View.m_dTargetZ - dCamZ;
      Camera.dUpX  = 0.0f;
      Camera.dUpY  = 1.0f;
      Camera.dUpZ  = 0.0f;

      int nW = pRenderer->GetWidth ();
      int nH = pRenderer->GetHeight ();

#if (1)
      float dBaseFovY = 60.0f * 3.14159265f / 180.0f;
      int   nRefH     = 1080;
      Camera.dFovY    = 2.0f * std::atan (std::tan (dBaseFovY * 0.5f) * static_cast<float> (nH) / static_cast<float> (nRefH));
#else
      Camera.dFovY    = 60.0f * 3.14159265f / 180.0f;
#endif
      Camera.dAspect  = (nW > 0  &&  nH > 0) ? static_cast<float> (nW) / static_cast<float> (nH) : 1.0f;
      Camera.dNear    = 0.00001f;
      Camera.dFar     = 1000.0f;

      pRenderer->SetCamera (Camera);

      pViewport->Accumulate (VIEWPORT::kACCUMULATE_INPUT, tpLoopStart);

      auto tpSceneStart = std::chrono::steady_clock::now ();

      std::vector<SPHERE_BUILD> aSphereBuild;
      std::vector<CURVE_BUILD>  aCurveBuild;
      std::vector<BOX_BUILD>    aBoxBuild;
      std::vector<LIGHT_BUILD>  aLightBuild;
      std::vector<PANEL_BUILD>  aPanelBuild;
      std::vector<MESH_BUILD>   aMeshBuild;

      SCENE* pScene = pViewport->Scene ();
      FABRIC* pFabric_Root = pScene ? pScene->Fabric_Root () : nullptr;
      NODE* pSomRoot = pFabric_Root ? pFabric_Root->Node_Root () : nullptr;
      SNEEZE::ENGINE* pEngine = pViewport->Engine ();

      double dMaxReach = 0.0;

      if (pSomRoot)
      {
         WORLD_FRAME rootFrame;
         TraverseNode (pSomRoot, rootFrame, tmNow, pEngine, aSphereBuild, aCurveBuild, aLightBuild, aBoxBuild, aPanelBuild, aMeshBuild, dMaxReach);
      }

      // One uniform per-scene render scale, applied at this single flatten seam:
      // metres (double) -> render units (float). Sized so the root-anchored
      // bounding sphere fills TARGET_EXTENT and the default camera frames it.
      // Every renderable -- celestial and physical alike -- rides this one scale.
      double dRenderScale = (dMaxReach > MIN_REACH) ? (TARGET_EXTENT / dMaxReach) : 1.0;
      pJob_Compositor->m_dRenderScale = static_cast<float> (dRenderScale);

      // Seed the temporary orbit camera from an absolute world pose, if one was
      // set (initial pose from the primary fabric, or a future wasm call). The
      // world position is metres and rides this same render scale; the rotation
      // quaternion supplies the look direction. The seed reproduces eye+direction
      // exactly -- the chosen orbit distance only affects later mouse pivoting.
      // While a scene-driven pose is active, re-seed the orbit VIEW from it every
      // frame. The node data injects asynchronously (wasm) and streams in, so the
      // render scale settles over several frames; re-seeding each frame lets the
      // pose self-correct as dMaxReach grows, instead of locking in an early
      // partial-scene scale. Guarded on real extent so a metre-scale pose is never
      // applied against an empty scene (which would fling the camera past the far
      // plane). User interaction deactivates it (above).
      VIEWPORT::CAMERA CameraPose;
      if (dMaxReach > MIN_REACH  &&  pViewport->Camera_Active (CameraPose))
      {
         double ex = CameraPose.aPosition[0] * dRenderScale;
         double ey = CameraPose.aPosition[1] * dRenderScale;
         double ez = CameraPose.aPosition[2] * dRenderScale;

         double qx = CameraPose.aRotation[0], qy = CameraPose.aRotation[1], qz = CameraPose.aRotation[2], qw = CameraPose.aRotation[3];
         double tx = 2.0 * (qy * (-1.0) - qz * 0.0);
         double ty = 2.0 * (qz * 0.0 - qx * (-1.0));
         double tz = 2.0 * (qx * 0.0 - qy * 0.0);
         double dx = 0.0  + qw * tx + (qy * tz - qz * ty);
         double dy = 0.0  + qw * ty + (qz * tx - qx * tz);
         double dz = -1.0 + qw * tz + (qx * ty - qy * tx);
         double dLen = std::sqrt (dx * dx + dy * dy + dz * dz);
         if (dLen > 1e-9) { dx /= dLen; dy /= dLen; dz /= dLen; }

         double D = std::sqrt (ex * ex + ey * ey + ez * ez);
         if (D < 1e-6) D = 10.0;

         VIEWPORT::VIEW& Seed = pViewport->View ();
         Seed.m_dTargetX  = static_cast<float> (ex + dx * D);
         Seed.m_dTargetY  = static_cast<float> (ey + dy * D);
         Seed.m_dTargetZ  = static_cast<float> (ez + dz * D);
         Seed.m_dDistance = static_cast<float> (D);
         Seed.m_dPhi      = static_cast<float> (std::asin (std::max (-1.0, std::min (1.0, -dy))));
         Seed.m_dTheta    = static_cast<float> (std::atan2 (-dz, -dx));
      }

      std::vector<SPHERE_DATA> aSphere_Data;
      aSphere_Data.reserve (aSphereBuild.size ());
      for (const auto& sb : aSphereBuild)
      {
         SPHERE_DATA sphere;
         sphere.x         = static_cast<float> (sb.dx * dRenderScale);
         sphere.y         = static_cast<float> (sb.dy * dRenderScale);
         sphere.z         = static_cast<float> (sb.dz * dRenderScale);
         sphere.dRadius   = MagnifyRadius (sb.dRadiusM, dRenderScale, sb.bMoon);
         sphere.bEmissive = sb.bEmissive;
         sphere.r = sb.r;
         sphere.g = sb.g;
         sphere.b = sb.b;
         sphere.pTexturePixels = sb.pTex;
         sphere.nTextureWidth  = sb.nTexW;
         sphere.nTextureHeight = sb.nTexH;
         aSphere_Data.push_back (sphere);
      }

      std::vector<CURVE_DATA> aCurve_Data;
      aCurve_Data.reserve (aCurveBuild.size ());
      for (const auto& cb : aCurveBuild)
      {
         CURVE_DATA curve;
         curve.r = cb.r;
         curve.g = cb.g;
         curve.b = cb.b;
         curve.aPoints.reserve (cb.aPoints.size ());
         for (const auto& p : cb.aPoints)
         {
            CURVE_POINT cp;
            cp.x       = static_cast<float> (p.x * dRenderScale);
            cp.y       = static_cast<float> (p.y * dRenderScale);
            cp.z       = static_cast<float> (p.z * dRenderScale);
            cp.dRadius = p.dRadius;
            curve.aPoints.push_back (cp);
         }
         aCurve_Data.push_back (std::move (curve));
      }

      std::vector<LIGHT_DATA> aLight;
      aLight.reserve (aLightBuild.size ());
      for (const auto& lb : aLightBuild)
      {
         LIGHT_DATA light;
         // A point light's position rides the per-scene render scale like any
         // world point; a directional light's x/y/z is a direction (left as-is),
         // and an ambient light has no position at all.
         double dScale = (lb.eType == LIGHT_DATA::kPOINT) ? dRenderScale : 1.0;
         light.x          = static_cast<float> (lb.dx * dScale);
         light.y          = static_cast<float> (lb.dy * dScale);
         light.z          = static_cast<float> (lb.dz * dScale);
         light.r          = lb.r;
         light.g          = lb.g;
         light.b          = lb.b;
         // Distances from authored local space to render space scale by
         // (worldScale * renderScale); a point light's 1/r^2 falloff therefore
         // needs intensity multiplied by that factor squared to keep illumination
         // invariant. Directional/ambient lights have no falloff, so they pass
         // through unscaled.
         if (lb.eType == LIGHT_DATA::kPOINT  &&  lb.bCompensate)
         {
            double dFull = lb.dWorldScale * dRenderScale;
            light.dIntensity = static_cast<float> (lb.dIntensity * dFull * dFull);
         }
         else
            light.dIntensity = lb.dIntensity;
         light.eType      = lb.eType;
         aLight.push_back (light);
      }

      std::vector<BOX_DATA> aBox_Data;
      aBox_Data.reserve (aBoxBuild.size ());
      for (const auto& bb : aBoxBuild)
      {
         BOX_DATA box;
         for (int j = 0; j < 4; j++)
         {
            box.m16[j * 4 + 0] = static_cast<float> (bb.mWorld.d[j * 4 + 0] * dRenderScale);
            box.m16[j * 4 + 1] = static_cast<float> (bb.mWorld.d[j * 4 + 1] * dRenderScale);
            box.m16[j * 4 + 2] = static_cast<float> (bb.mWorld.d[j * 4 + 2] * dRenderScale);
            box.m16[j * 4 + 3] = static_cast<float> (bb.mWorld.d[j * 4 + 3]);
         }
         box.r = bb.r;
         box.g = bb.g;
         box.b = bb.b;
         aBox_Data.push_back (box);
      }

      // A panel's on-screen size still rides the framed scene (its Bound carries
      // only the quad's aspect ratio, so one absolute metre size need not suit
      // both a planetary system and a city block), but its PLACEMENT comes from
      // the node's world transform, flattened through the same dRenderScale as
      // every other renderable. The panel is billboarded toward the camera (its
      // +Z normal tracks the eye) so it stays readable from any orbit angle
      // instead of being seen edge-on. Billboarding per node is a future panel
      // property.
      std::vector<PANEL_DATA> aPanel_Data;
      aPanel_Data.reserve (aPanelBuild.size ());
      for (const auto& pb : aPanelBuild)
      {
         double dHeight = 0.26 * TARGET_EXTENT;          // quad height, render units
         double dWidth  = dHeight * pb.dAspect;

         double dAnchorX = pb.dWx * dRenderScale;
         double dAnchorY = pb.dWy * dRenderScale;
         double dAnchorZ = pb.dWz * dRenderScale;

         // Billboard basis: +Z (panel normal) points at the eye; +Y stays world-up.
         double dNx = dCamX - dAnchorX, dNy = dCamY - dAnchorY, dNz = dCamZ - dAnchorZ;
         double dNLen = std::sqrt (dNx * dNx + dNy * dNy + dNz * dNz);
         if (dNLen < 1e-9) { dNx = 0.0; dNy = 0.0; dNz = 1.0; dNLen = 1.0; }
         dNx /= dNLen; dNy /= dNLen; dNz /= dNLen;

         // right = normalize(worldUp x normal); worldUp = (0,1,0)
         double dRx = dNz, dRy = 0.0, dRz = -dNx;
         double dRLen = std::sqrt (dRx * dRx + dRz * dRz);
         if (dRLen < 1e-9) { dRx = 1.0; dRy = 0.0; dRz = 0.0; dRLen = 1.0; }
         dRx /= dRLen; dRz /= dRLen;

         // up = normal x right
         double dUx = dNy * dRz - dNz * dRy;
         double dUy = dNz * dRx - dNx * dRz;
         double dUz = dNx * dRy - dNy * dRx;

         PANEL_DATA panel;
         panel.m16[0]  = static_cast<float> (dRx * dWidth);
         panel.m16[1]  = static_cast<float> (dRy * dWidth);
         panel.m16[2]  = static_cast<float> (dRz * dWidth);
         panel.m16[3]  = 0.0f;
         panel.m16[4]  = static_cast<float> (dUx * dHeight);
         panel.m16[5]  = static_cast<float> (dUy * dHeight);
         panel.m16[6]  = static_cast<float> (dUz * dHeight);
         panel.m16[7]  = 0.0f;
         panel.m16[8]  = static_cast<float> (dNx);
         panel.m16[9]  = static_cast<float> (dNy);
         panel.m16[10] = static_cast<float> (dNz);
         panel.m16[11] = 0.0f;
         panel.m16[12] = static_cast<float> (dAnchorX);
         panel.m16[13] = static_cast<float> (dAnchorY);
         panel.m16[14] = static_cast<float> (dAnchorZ);
         panel.m16[15] = 1.0f;

         panel.pPixels = pb.pPixels;
         panel.nWidth  = pb.nW;
         panel.nHeight = pb.nH;
         aPanel_Data.push_back (panel);
      }

      // glTF/GLB draws ride the same single render scale as boxes: the world
      // transform's linear part and translation are scaled to render units while
      // the homogeneous row is preserved. Vertex streams and the material (with
      // any decoded base-color texture) are copied through from the node-owned
      // source unchanged.
      std::vector<MESH_DATA> aMesh_Data;
      aMesh_Data.reserve (aMeshBuild.size ());
      for (const auto& mb : aMeshBuild)
      {
         MESH_DATA mesh = *mb.pSrc;
         for (int j = 0; j < 4; j++)
         {
            mesh.m16[j * 4 + 0] = static_cast<float> (mb.mWorld.d[j * 4 + 0] * dRenderScale);
            mesh.m16[j * 4 + 1] = static_cast<float> (mb.mWorld.d[j * 4 + 1] * dRenderScale);
            mesh.m16[j * 4 + 2] = static_cast<float> (mb.mWorld.d[j * 4 + 2] * dRenderScale);
            mesh.m16[j * 4 + 3] = static_cast<float> (mb.mWorld.d[j * 4 + 3]);
         }
         aMesh_Data.push_back (mesh);
      }

      pRenderer->SetLights (aLight);

      pViewport->Accumulate (VIEWPORT::kACCUMULATE_SCENE, tpSceneStart);

      if (pViewport->Scene_Invalidate_Consume ())
         pRenderer->InvalidateScene ();

      float aBackground[4];
      if (pScene  &&  pScene->Backdrop_Consume (aBackground))
         pRenderer->SetBackground (aBackground[0], aBackground[1], aBackground[2], aBackground[3]);

      pRenderer->BeginFrame ();
      pRenderer->SubmitSpheres (aSphere_Data);
      pRenderer->SubmitCurves (aCurve_Data);
      pRenderer->SubmitBoxes (aBox_Data);
      pRenderer->SubmitPanels (aPanel_Data);
      pRenderer->SubmitMeshes (aMesh_Data);
      pRenderer->EndFrame ();

      pViewport->Accumulate (VIEWPORT::kACCUMULATE_SUBMIT, pRenderer->GetLastSubmitSeconds ());
      pViewport->Accumulate (VIEWPORT::kACCUMULATE_RENDER, pRenderer->GetLastRenderSeconds ());

      pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_PRESENT);
   }
   else pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_RENDER);
}

void AGENT::COMPOSITOR::Execute_Present (JOB_COMPOSITOR* pJob_Compositor)
{
   VIEWPORT*           pViewport = pJob_Compositor->Viewport ();
   VIEWPORT::RENDERER* pRenderer = pViewport->Renderer ();
   IVIEWPORT*          pHost     = pViewport->Host ();

   auto tpPublishStart = std::chrono::steady_clock::now ();

   if (pRenderer  &&  !pRenderer->IsRenderingToNativeSurface ())
   {
      const uint32_t* puPixels = pRenderer->GetFrameBuffer ();

      if (puPixels)
      {
         pViewport->FrameBuffer_Write (puPixels, pRenderer->GetWidth (), pRenderer->GetHeight ());

         if (pHost)
         {
            int nFbW, nFbH;
            const uint32_t* puFB = pViewport->FrameBuffer_Capture (nFbW, nFbH);

            if (puFB != nullptr)
               pHost->OnFrameReady (puFB, nFbW, nFbH);

            pViewport->FrameBuffer_Release ();
         }
      }
   }

   pViewport->Accumulate (VIEWPORT::kACCUMULATE_PUBLISH, tpPublishStart);
   pViewport->Diagnostics ();

   pJob_Compositor->m_nLastFrame = ++s_nGlobalFrameSeq;

   pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_RENDER);
}
