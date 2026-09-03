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
#include "Container.h"
#include "context/viewport/Viewport.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

static constexpr float  MIN_SPHERE_RADIUS   = 0.0f;

// Bodies are far too small to see at honest scene scale, so a body's VISUAL
// radius is a compressed magnification of its true radius (already in render
// space): BODY_MAG * (radius_render ^ BODY_EXP). BODY_EXP < 1 squashes the huge
// range of real radii; raise it toward 1.0 for more size variation between
// bodies. These are the only "art" knobs -- positions stay 1:1 (scaled).
static constexpr double BODY_MAG            = 1.25;
static constexpr double BODY_EXP            = 0.7;

// The one celestial kludge (moons only): a moon orbits farther out so it clears
// its magnified planet, and renders smaller than the planet magnification.
static constexpr double MOON_ORBIT_BOOST    = 5.0;
static constexpr double MOON_SIZE_FACTOR    = 1.0;

static constexpr int    TRAIL_SEGMENTS      = 128;
static constexpr double TRAIL_FRACTION      = 0.75;
static constexpr float  TRAIL_RADIUS_PLANET = 0.0002f;
static constexpr float  TRAIL_RADIUS_MOON   = 0.00005f;
static constexpr float  ORBIT_TRAIL_DIM     = 0.4f;      // orbit-trail colour dim factor

// Default camera frustum. The base vertical FOV assumes a reference viewport
// height and is rescaled to the actual height. Clip planes are floored in world
// metres (then scaled to render units) so far/near stays within the depth
// buffer's resolvable range.
static constexpr double DEFAULT_FOVY_DEG    = 60.0;      // base vertical field of view, degrees
static constexpr int    REFERENCE_HEIGHT_PX = 1080;      // viewport height the base FOV assumes
static constexpr double FAR_PLANE_MIN       = 20000.0;   // far-plane floor, world metres (~20 km)
static constexpr double NEAR_FAR_RATIO      = 1.0e6;     // near = far / ratio (depth-buffer budget)
static constexpr double NEAR_PLANE_MIN      = 0.05;      // near-plane floor, world metres (~5 cm)

static void ColorFromU32 (uint32_t nColor, RGB& rgb)
{
   rgb.fR = static_cast<float> ((nColor >> 16) & 0xFF) / 255.0f;
   rgb.fG = static_cast<float> ((nColor >> 8)  & 0xFF) / 255.0f;
   rgb.fB = static_cast<float> ( nColor        & 0xFF) / 255.0f;
}

static void ColorFromPropertyFloat (float fColor, RGB& rgb)
{
   uint32_t nColor;
   memcpy (&nColor, &fColor, 4);
   ColorFromU32 (nColor & 0x00FFFFFF, rgb);
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

static MAT4 Mat4_FromTRS (const RMAP::MAP::MAP_OBJECT::VEC3& vTranslate, const RMAP::MAP::MAP_OBJECT::QUAT& qRotate, const RMAP::MAP::MAP_OBJECT::VEC3& vScale)
{
   double r00 = 1.0 - 2.0 * (qRotate.dY * qRotate.dY + qRotate.dZ * qRotate.dZ);
   double r01 =       2.0 * (qRotate.dX * qRotate.dY - qRotate.dW * qRotate.dZ);
   double r02 =       2.0 * (qRotate.dX * qRotate.dZ + qRotate.dW * qRotate.dY);
   double r10 =       2.0 * (qRotate.dX * qRotate.dY + qRotate.dW * qRotate.dZ);
   double r11 = 1.0 - 2.0 * (qRotate.dX * qRotate.dX + qRotate.dZ * qRotate.dZ);
   double r12 =       2.0 * (qRotate.dY * qRotate.dZ - qRotate.dW * qRotate.dX);
   double r20 =       2.0 * (qRotate.dX * qRotate.dZ - qRotate.dW * qRotate.dY);
   double r21 =       2.0 * (qRotate.dY * qRotate.dZ + qRotate.dW * qRotate.dX);
   double r22 = 1.0 - 2.0 * (qRotate.dX * qRotate.dX + qRotate.dY * qRotate.dY);

   MAT4 m = {};
   m.d[0]  = r00 * vScale.dX;  m.d[1]  = r10 * vScale.dX;  m.d[2]  = r20 * vScale.dX;  m.d[3]  = 0.0;
   m.d[4]  = r01 * vScale.dY;  m.d[5]  = r11 * vScale.dY;  m.d[6]  = r21 * vScale.dY;  m.d[7]  = 0.0;
   m.d[8]  = r02 * vScale.dZ;  m.d[9]  = r12 * vScale.dZ;  m.d[10] = r22 * vScale.dZ;  m.d[11] = 0.0;
   m.d[12] = vTranslate.dX;    m.d[13] = vTranslate.dY;    m.d[14] = vTranslate.dZ;    m.d[15] = 1.0;
   return m;
}

// Transform a point (w = 1) by a column-major MAT4.
static RMAP::MAP::MAP_OBJECT::VEC3 Mat4_Point (const MAT4& m, const RMAP::MAP::MAP_OBJECT::VEC3& v)
{
   RMAP::MAP::MAP_OBJECT::VEC3 vOut;
   vOut.dX = m.d[0] * v.dX + m.d[4] * v.dY + m.d[8]  * v.dZ + m.d[12];
   vOut.dY = m.d[1] * v.dX + m.d[5] * v.dY + m.d[9]  * v.dZ + m.d[13];
   vOut.dZ = m.d[2] * v.dX + m.d[6] * v.dY + m.d[10] * v.dZ + m.d[14];
   return vOut;
}

// The frame's average axis scale: the mean length of its three basis columns.
static double Mat4_Scale (const MAT4& m)
{
   double dCol0 = std::sqrt (m.d[0] * m.d[0] + m.d[1] * m.d[1] + m.d[2]  * m.d[2]);
   double dCol1 = std::sqrt (m.d[4] * m.d[4] + m.d[5] * m.d[5] + m.d[6]  * m.d[6]);
   double dCol2 = std::sqrt (m.d[8] * m.d[8] + m.d[9] * m.d[9] + m.d[10] * m.d[10]);
   return (dCol0 + dCol1 + dCol2) / 3.0;
}

// A body's visible radius from its true radius (metres) and the scene scale.
static float MagnifyRadius (double dRadiusM, double dScale, bool bMoon)
{
   double dRender = BODY_MAG * std::pow (dRadiusM * dScale, BODY_EXP);

   if (bMoon)
      dRender *= MOON_SIZE_FACTOR;
   if (dRender < MIN_SPHERE_RADIUS)
      dRender = MIN_SPHERE_RADIUS;

   return static_cast<float> (dRender);
}

// Scene is sized so its root-anchored bounding sphere maps to this many render
// units, keeping coordinates float-friendly and framed by the default camera.
static constexpr double TARGET_EXTENT = 5.0;
static constexpr double MIN_REACH     = 1e-6;

// Proximity-driven lazy loading (screen-space LOD): a map-managed node loads its
// next child tier once its apparent angular size -- node radius (metres) divided
// by camera distance (metres) -- exceeds this ratio. Scale-independent: works for
// a room, a planet, or a solar system without a magic absolute distance.
// ~0.10 => expand when the node's radius spans roughly a tenth of its distance
// (angular radius ~5.7 deg). Tune to taste.
static constexpr double PROXIMITY_LOAD_ANGULAR_RATIO = 0.15;

// Hysteresis band below the load ratio: a previously expanded node unloads its
// children only once its angular size drops under this. Same threshold as load
// would flicker Expand/Collapse at the boundary every frame.
//
// Kept far below the load ratio, not just under it. A node's children are often
// its ONLY renderable form -- a planet is drawn by its tile subtree, not by the
// node itself -- so collapsing does not coarsen that node, it erases it. At 0.10
// (a 5.7 deg angular radius, so ~11 deg of screen) content was being erased
// while still plainly visible, which reads as "the fabric is empty". 0.01 puts
// the cutoff at roughly half a degree, by which point there is genuinely nothing
// to see, and the wide band leaves ample room for camera jitter.
static constexpr double PROXIMITY_UNLOAD_ANGULAR_RATIO = 0.01;

// Bounding-box overlay: draw each map node's bounding box as a solid instanced
// box so node positions and extents are visible while navigating. Toggled at
// runtime via the engine CONFIG flag bBoundingBox (read once per frame in
// Execute_Render and threaded into TraverseNode), not a compile switch.
//
// Bounding boxes are emitted on the SOLID instanced BOX path (one shared unit-box
// vertex buffer, one transform per node). The curve/tube path was tried first but
// commitCurve() does a per-strand vertex-buffer upload (fillDefaultAttributes),
// and 12 strands x hundreds of nodes overflows Filament's fixed command buffer
// (CircularBuffer::allocate -> panic). The box path uploads geometry once.
//
// A box is drawn only when its bounding radius is at most this fraction of the
// eye distance. Enclosing ancestor regions (whole map / whole body) and any box
// the eye is inside or right beside subtend a huge angle and fill the screen with
// solid faces; capping the on-screen size leaves only discrete cubes ahead.
static constexpr double BOUND_BOX_MAX_RATIO = 0.25; // 0.5;

// Restrict the debug boxes to a single (class, type) so the view isn't buried
// under every descendant node. With this on, only continents draw -- the 7 big
// opaque azure boxes -- and their loaded children (regions/features) are hidden.
// Set BOUND_BOX_ONLY_TYPE to 0 to draw every node's box (colored by type).
#define BOUND_BOX_ONLY_TYPE 0
static constexpr uint32_t BOUND_BOX_FILTER_CLASS = 72;   // terrestrial
static constexpr uint32_t BOUND_BOX_FILTER_TYPE  = 3;    // continent

// Give each map-object (class, type) pair its own stable, distinct color so the
// debug boxes read as different colors per node type. A golden-ratio hue hash
// spreads adjacent keys far apart on the color wheel; HSV->RGB at full value.
static RGB BoundBoxColorForType (uint32_t nClass, uint32_t nType)
{
   uint32_t nKey  = nClass * 131u + nType;
   double   dHue  = std::fmod (static_cast<double> (nKey) * 0.61803398875, 1.0) * 6.0;
   double   dSat  = 0.85;
   double   dVal  = 1.0;
   int      nSeg  = static_cast<int> (dHue);
   double   dFrac = dHue - nSeg;
   double   dP    = dVal * (1.0 - dSat);
   double   dQ    = dVal * (1.0 - dSat * dFrac);
   double   dT    = dVal * (1.0 - dSat * (1.0 - dFrac));
   double   dR    = 0.0;
   double   dG    = 0.0;
   double   dB    = 0.0;

   switch (nSeg % 6)
   {
      case 0:  dR = dVal; dG = dT;   dB = dP;   break;
      case 1:  dR = dQ;   dG = dVal; dB = dP;   break;
      case 2:  dR = dP;   dG = dVal; dB = dT;   break;
      case 3:  dR = dP;   dG = dQ;   dB = dVal; break;
      case 4:  dR = dT;   dG = dP;   dB = dVal; break;
      default: dR = dVal; dG = dP;   dB = dQ;   break;
   }

   RGB rgb;
   rgb.fR = static_cast<float> (dR);
   rgb.fG = static_cast<float> (dG);
   rgb.fB = static_cast<float> (dB);
   return rgb;
}

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
      if (bResult)
         break;

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
   RGB   rgbColor;
};

struct SPHERE_BUILD
{
   RMAP::MAP::MAP_OBJECT::VEC3   vPosition;             // metres
   double                        dRadiusM;             // true body radius, metres
   bool                          bMoon;
   bool                          bEmissive;
   RGB                           rgbColor;
   const uint8_t*                pTex = nullptr;
   DIM2                          dimTexture = { 0, 0 };
};

struct CURVE_BUILD
{
   std::vector<CURVE_POINT> aPoints;   // x/y/z metres; dRadius is render-space
   RGB   rgbColor;
};

struct LIGHT_BUILD
{
   RMAP::MAP::MAP_OBJECT::VEC3   vPosition     = { 0.0, 0.0, 0.0 };      // metres (point / spot world position)
   RMAP::MAP::MAP_OBJECT::VEC3   vDirection    = { 1.0, 0.0, 0.0 };      // spot aim (world, unit); identity forward = +X
   RGB                           rgbColor      = { 1.0f, 1.0f, 0.95f };  // defaults match a warm star key light
   float                         fIntensity    = 4.0f;
   double                        dWorldScale   = 1.0;                    // accumulated linear scale at the light's node frame
   bool                          bCompensate   = true;                   // apply (worldScale * renderScale)^2 invariance to a point/spot light
   int                           eType         = LIGHT_DATA::kPOINT;
   float                         fOpeningAngle = 0.0f;                   // spot cone opening, radians
   float                         fFalloffAngle = 0.0f;                   // spot cone penumbra, radians
};

struct PANEL_BUILD
{
   const uint8_t*                pbPixels;      // straight-alpha RGBA8, top-down (owned by the panel node)
   DIM2                          dim;           // pixel buffer dimensions
   double                        dAspect;       // panel width / height (quad shape only)
   RMAP::MAP::MAP_OBJECT::VEC3   vWorld;        // node world position (metres)
};

// One glTF/GLB draw gathered during traversal. mWorld is the draw's full world
// transform in metres (node world * the model-internal draw transform); pSrc
// borrows the node-owned MESH_DATA for its vertex streams and material, which
// are copied through unchanged at the flatten seam (only m16 is rescaled).
struct MESH_BUILD
{
   MAT4             mWorld;           // metres
   const MESH_DATA* pSrc;
   const void*      pInstanceOwner;   // NODE* that placed this draw
   uint32_t         nDrawIx;
};

// A node's declared bound does not necessarily enclose its own subtree. The map's
// "Earth" node declares a 1 m cube (Bound.d3Max = 1,1,1) while its eight continent
// children sit 3,165 km out and are 8,018 km across. Gating on the declared bound
// alone therefore treats Earth as a one-metre object: it unloads from any realistic
// viewing distance, and the only frames that keep it alive are ones where a
// mis-scaled camera happens to fall inside that 1 m sphere.
//
// Measuring the children that are already resident gives the gate a size that
// reflects what the node actually draws. Only the immediate children are walked --
// enough to recover Earth's true 11,183 km reach, and it keeps the cost proportional
// to the fan-out of the node being tested rather than to the whole subtree.
static double Node_ExtentMeasured (NODE* pNode, const MAT4& mWorld, const RMAP::MAP::MAP_OBJECT::VEC3& vWorld, double dExtentOwn, int64_t tmNow)
{
   double dExtent = dExtentOwn;

   for (int i = 0; i < pNode->Node_Count (); i++)
   {
      NODE*                  pChild    = pNode->Child (i);
      RMAP::MAP::MAP_OBJECT* pChildObj = pChild ? pChild->Map_Object () : nullptr;

      if (pChildObj)
      {
         RMAP::MAP::MAP_OBJECT_POD ChildPod;
         pChildObj->GetPOD (ChildPod);

         RMAP::MAP::MAP_OBJECT::VEC3 vPosition;
         RMAP::MAP::MAP_OBJECT::QUAT qRotation;
         RMAP::MAP::MAP_OBJECT::VEC3 vScale;

         pChildObj->Position (tmNow, vPosition);
         pChildObj->Rotation (tmNow, qRotation);
         pChildObj->Scale (vScale);

         MAT4 mChild = Mat4_Multiply (mWorld, Mat4_FromTRS (vPosition, qRotation, vScale));

         double dOffsetX = mChild.d[12] - vWorld.dX;
         double dOffsetY = mChild.d[13] - vWorld.dY;
         double dOffsetZ = mChild.d[14] - vWorld.dZ;
         double dOffset  = std::sqrt (dOffsetX * dOffsetX + dOffsetY * dOffsetY + dOffsetZ * dOffsetZ);

         double dChildX     = ChildPod.Bound.d3Max[0];
         double dChildY     = ChildPod.Bound.d3Max[1];
         double dChildZ     = ChildPod.Bound.d3Max[2];
         double dChildExtent = 0.5 * std::sqrt (dChildX * dChildX + dChildY * dChildY + dChildZ * dChildZ);

         if (pChildObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL)
         {
            double dRadius = static_cast<RMAP::MAP::MAP_OBJECT_CELESTIAL*> (pChildObj)->Radius ();
            if (dRadius > dChildExtent)
               dChildExtent = dRadius;
         }

         if (dOffset + dChildExtent > dExtent)
            dExtent = dOffset + dChildExtent;
      }
   }

   return dExtent;
}

static void TraverseNode (NODE* pNode, const WORLD_FRAME& frame, int64_t tmNow, SNEEZE::ENGINE* pEngine, std::vector<SPHERE_BUILD>& aSphere, std::vector<CURVE_BUILD>& aCurve_Build, std::vector<LIGHT_BUILD>& aLight, std::vector<BOX_BUILD>& aBox, std::vector<PANEL_BUILD>& aPanel, std::vector<MESH_BUILD>& aMesh, double& dMaxReach, const RMAP::MAP::MAP_OBJECT::VEC3& vEyeMetre, double dAngularRatio, std::vector<std::pair<CONTAINER*, uint64_t>>& aExpand, std::vector<std::pair<CONTAINER*, uint64_t>>& aCollapse, bool bBoundingBox, std::unordered_map<uint64_t, double>& mapExtent)
{
   RMAP::MAP::MAP_OBJECT* pObj = pNode->Map_Object ();
   WORLD_FRAME wfChild = frame;
   bool        bSkipChildren = false;

   if (pObj)
   {
      RMAP::MAP::MAP_OBJECT_POD Pod;

      pObj->GetPOD (Pod);

      // Universal TRS: every node, root to leaf, composes its local
      // translation/rotation/scale onto its parent's world transform. No class
      // is exempt -- celestial, terrestrial and physical all inherit identically.
      RMAP::MAP::MAP_OBJECT::VEC3 vPosition;
      RMAP::MAP::MAP_OBJECT::QUAT qRotation;
      RMAP::MAP::MAP_OBJECT::VEC3 vScale;

      pObj->Position (tmNow, vPosition);
      pObj->Rotation (tmNow, qRotation);
      pObj->Scale (vScale);

      // The one celestial kludge: a moon system's orbit is pushed outward so the
      // moon clears its magnified planet. Everything else stays 1:1 (metres).
      bool bMoonSystem = (pObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL && Pod.Type.bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_MOONSYSTEM);
      if (bMoonSystem)
      {
         vPosition = vPosition * MOON_ORBIT_BOOST;
      }

      MAT4 mLocal = Mat4_FromTRS (vPosition, qRotation, vScale);
      wfChild.mWorld = Mat4_Multiply (frame.mWorld, mLocal);

      RMAP::MAP::MAP_OBJECT::VEC3 vWorld = { wfChild.mWorld.d[12], wfChild.mWorld.d[13], wfChild.mWorld.d[14] };

      // Proximity-driven lazy loading (read-only detection): screen-space LOD.
      // When this node's apparent angular size (its radius over its distance from
      // the camera, both in metres) exceeds the load ratio, record a request to
      // stream its children in; when it falls under the unload ratio AND the node
      // has children, record a request to collapse them. Children of a collapsing
      // node are not descended -- their draws must not be submitted this frame
      // because Collapse (after EndFrame) will free the host buffers ANARI shares.
      // Node_Open / Node_Close happen after the walk so the tree is never mutated
      // mid-walk. Requests are idempotent -- MapSvc dedups, and non-map containers
      // no-op.
      {
         double dEyeX = vWorld.dX - vEyeMetre.dX;
         double dEyeY = vWorld.dY - vEyeMetre.dY;
         double dEyeZ = vWorld.dZ - vEyeMetre.dZ;
         double dDist = std::sqrt (dEyeX * dEyeX + dEyeY * dEyeY + dEyeZ * dEyeZ);

         // Node radius in metres. Bound.d3Max carries the node's extents; celestial
         // bodies instead carry their size in Radius(). Take the larger so both
         // geometry-bearing and celestial nodes get a meaningful size.
         double dExtX   = Pod.Bound.d3Max[0];
         double dExtY   = Pod.Bound.d3Max[1];
         double dExtZ   = Pod.Bound.d3Max[2];
         double dExtent = 0.5 * std::sqrt (dExtX * dExtX + dExtY * dExtY + dExtZ * dExtZ);

         if (pObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL)
         {
            double dRadius = static_cast<RMAP::MAP::MAP_OBJECT_CELESTIAL*> (pObj)->Radius ();
            if (dRadius > dExtent)
               dExtent = dRadius;
         }

         // Gate on the node's measured size, not its declared bound, and remember
         // the largest value ever seen for it. The measurement only works while the
         // children are resident, so without the memo collapsing a node would throw
         // its true size away: the next frame would measure the 1 m declared bound
         // again, the render scale (derived from this same extent) would snap by
         // seven orders of magnitude, the eye-in-metres computed from that scale
         // would land inside the 1 m sphere, and the node would re-expand -- one
         // frame on, one frame off, indefinitely. A node's real size does not
         // change, so once learned it is kept.
         double dExtentGate = Node_ExtentMeasured (pNode, wfChild.mWorld, vWorld, dExtent, tmNow);

         {
            uint64_t qwExtentKey = OBJECTIX_COMPOSE (pObj->m_wClass, pNode->ObjectIx ());
            double&  dLearned    = mapExtent[qwExtentKey];

            if (dExtentGate > dLearned)
               dLearned = dExtentGate;

            dExtentGate = dLearned;
         }

         // Every node's measured size anchors the scene's metre extent, so the one
         // render scale frames the whole thing regardless of what is expanded right
         // now. Lights are excluded for the same reason as below: they illuminate
         // the scene but must not reframe it.
         if (dExtentGate > 0.0  &&  pObj->m_wClass != RMAP::MAP::MAP_OBJECT_CLASS_LIGHT)
         {
            double dNodeReach = vWorld.Length () + dExtentGate;
            if (dNodeReach > dMaxReach)
               dMaxReach = dNodeReach;
         }

         // Visible when the eye is inside the node's radius, or the node subtends
         // more than the load ratio. Unload only when outside the radius AND under
         // the (lower) unload ratio -- the band between them is hysteresis. A node
         // with no known size never triggers (avoids cascading boundless nodes).
         bool bVisible = (dExtentGate > 0.0)  &&  (dDist <= dExtentGate  ||  (dExtentGate / dDist) > dAngularRatio);
         bool bUnload  = (dExtentGate > 0.0)  &&  (dDist > dExtentGate)   &&  ((dExtentGate / dDist) < PROXIMITY_UNLOAD_ANGULAR_RATIO);

         if (bVisible  ||  bUnload)
         {
            FABRIC*    pFabric    = pNode->Fabric ();
            CONTAINER* pContainer = pFabric ? pFabric->Container () : nullptr;

            // MapSvc's registry is keyed by the COMPOSED handle ((class<<N)|objectix,
            // as returned by Node_Open), but NODE::ObjectIx() reports only the raw
            // object index. Compose it here so Expand/Collapse registry lookup matches.
            if (pContainer)
            {
               uint64_t qwComposed = OBJECTIX_COMPOSE (pObj->m_wClass, pNode->ObjectIx ());

               // Proximity streaming is a map-service feature: only nodes the map
               // service opened (present in its registry) may stream in or out.
               // WASM-injected and static-MSF nodes are authored by their fabric
               // and must never be proximity-removed -- they stay resident and
               // visible at any camera distance.
               if (pContainer->Node_IsMapManaged (qwComposed))
               {
                  if (bVisible)
                     aExpand.push_back ({ pContainer, qwComposed });
                  else if (pNode->Node_Count () > 0)
                  {
                     aCollapse.push_back ({ pContainer, qwComposed });
                     bSkipChildren = true;
                  }
               }
            }
         }
      }

      // Bounding-box overlay: emit this node's box as one solid instanced box on
      // the shared unit-box path when the engine CONFIG flag bBoundingBox is set.
      // Full extent comes from Bound.d3Max, and for celestial bodies from Radius().
      // mWorld is a metres-space TRS: the unit box spans [-0.5,0.5] so the diagonal
      // scale is the FULL extent (2 x half-extent); translation is the node world
      // position. The flatten seam rescales the whole matrix by dRenderScale.
      if (bBoundingBox)
      {
         double dHx = 0.5 * Pod.Bound.d3Max[0];
         double dHy = 0.5 * Pod.Bound.d3Max[1];
         double dHz = 0.5 * Pod.Bound.d3Max[2];

         if (pObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL)
         {
            double dRadius = static_cast<RMAP::MAP::MAP_OBJECT_CELESTIAL*> (pObj)->Radius ();
            if (dRadius > dHx) dHx = dRadius;
            if (dRadius > dHy) dHy = dRadius;
            if (dRadius > dHz) dHz = dRadius;
         }

         // Cull boxes that would swamp the view. The enclosing ancestor regions
         // (and any box the eye is inside or right beside) have a bounding radius
         // that dwarfs the eye distance, so their solid faces fill the screen.
         // Draw a box only when it reads as a discrete cube ahead: bounding radius
         // no larger than BOUND_BOX_MAX_RATIO x the eye-to-centre distance.
         double dCx        = vWorld.dX - vEyeMetre.dX;
         double dCy        = vWorld.dY - vEyeMetre.dY;
         double dCz        = vWorld.dZ - vEyeMetre.dZ;
         double dEyeDist   = std::sqrt (dCx * dCx + dCy * dCy + dCz * dCz);
         double dBoxRadius = std::sqrt (dHx * dHx + dHy * dHy + dHz * dHz);

         bool bHasSize    = (dHx > 0.0  ||  dHy > 0.0  ||  dHz > 0.0);

#if BOUND_BOX_ONLY_TYPE
         // Continents-only mode: draw just the filtered (class, type). Skip the
         // angular size cap (continents are meant to be large and opaque); only
         // guard against a box the eye is inside, which would fill the screen.
         bool bTypeMatch = (pObj->m_wClass == BOUND_BOX_FILTER_CLASS  &&  Pod.Type.bType == BOUND_BOX_FILTER_TYPE);
         bool bEyeInside = (dEyeDist <= dBoxRadius);
         bool bDrawBox   = (bHasSize  &&  bTypeMatch  &&  !bEyeInside);
#else
         // All-nodes mode: draw every node's box, capping on-screen size so
         // enclosing ancestors don't swamp the view.
         bool bDiscrete  = (dBoxRadius <= dEyeDist * BOUND_BOX_MAX_RATIO);
         bool bDrawBox   = (bHasSize  &&  bDiscrete);
#endif

         if (bDrawBox)
         {
            BOX_BUILD Box_Build;
            Box_Build.rgbColor = BoundBoxColorForType (pObj->m_wClass, Pod.Type.bType);   // color by (class, type)

            // Build the box in the node's LOCAL space (unit cube [-0.5,0.5]
            // scaled to the full bound extent, centered on the node origin), then
            // ride the node's full world matrix -- the same wfChild.mWorld the
            // meshes use. This carries the node's rotation and position, so each
            // continent box orients tangent to the sphere instead of collapsing
            // to an axis-aligned slab at the origin.
            MAT4 mBoxLocal;
            for (int i = 0; i < 16; ++i)
               mBoxLocal.d[i] = 0.0;

            mBoxLocal.d[0]  = 2.0 * dHx;                   // scale X (full extent)
            mBoxLocal.d[5]  = 2.0 * dHy;                   // scale Y
            mBoxLocal.d[10] = 2.0 * dHz;                   // scale Z
            mBoxLocal.d[15] = 1.0;

            Box_Build.mWorld = Mat4_Multiply (wfChild.mWorld, mBoxLocal);

            aBox.push_back (Box_Build);
         }
      }

      // Every node's world position contributes to the scene's metre extent, so
      // the single render scale frames the whole thing. Lights are excluded:
      // they illuminate the scene but must not reframe it (a light placed out
      // beyond the geometry would otherwise shrink everything else).
      if (pObj->m_wClass != RMAP::MAP::MAP_OBJECT_CLASS_LIGHT)
      {
         double dReach = vWorld.Length ();
         if (dReach > dMaxReach)
            dMaxReach = dReach;
      }
      // A loaded glTF/GLB renders its own geometry regardless of the node's class
      // -- a model can sit at the celestial, terrestrial, or physical level. Nodes
      // whose resource is an "action:" reference (e.g. colliders) are invisible
      // logic volumes and never carry geometry.
      const GLTF_RENDER_MODEL* pModel = nullptr;
      if (std::strncmp (Pod.Resource.sReference, "action:", 7) != 0)
         pModel = pNode->Gltf_Render_Model ();

      if (pModel)
      {
         // Each draw's model-internal transform composes under this node's world
         // frame; the streams/material ride through untouched.
         uint32_t nDrawIx = 0;
         for (const MESH_DATA& draw : pModel->aMesh)
         {
            MAT4 mLocal;
            for (int j = 0; j < 16; j++)
               mLocal.d[j] = draw.mWorld.f[j];

            MESH_BUILD mb;
            mb.mWorld          = Mat4_Multiply (wfChild.mWorld, mLocal);
            mb.pSrc            = &draw;
            mb.pInstanceOwner  = pNode;
            mb.nDrawIx         = nDrawIx;
            aMesh.push_back (mb);
            nDrawIx++;
         }

         // The model's bounding sphere (center carried through the node frame,
         // radius scaled by the frame's average axis scale) extends the scene
         // reach so the single render scale frames it like everything else.
         RMAP::MAP::MAP_OBJECT::VEC3 vCenter = Mat4_Point (wfChild.mWorld, pModel->vCenter);
         double dScale = Mat4_Scale (wfChild.mWorld);
         double dMeshReach = vCenter.Length () + pModel->dRadius * dScale;
         if (dMeshReach > dMaxReach) dMaxReach = dMeshReach;
      }

      if (pObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_CELESTIAL)
      {
         RMAP::MAP::MAP_OBJECT_CELESTIAL* pCelestial = static_cast<RMAP::MAP::MAP_OBJECT_CELESTIAL*> (pObj);
         uint8_t bType = Pod.Type.bType;

         if (bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_STARSYSTEM
         ||  bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_PLANETSYSTEM
         ||  bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_MOONSYSTEM
         ||  bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_DEBRISSYSTEM)
         {
            if (pCelestial->HasOrbit ())
            {
               float dTrailRadius = (bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_MOONSYSTEM  || bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_DEBRISSYSTEM) ? TRAIL_RADIUS_MOON : TRAIL_RADIUS_PLANET;

               CURVE_BUILD Curve_Build;
               ColorFromPropertyFloat (Pod.Properties.Celestial.fColor, Curve_Build.rgbColor);
               Curve_Build.rgbColor = Curve_Build.rgbColor * ORBIT_TRAIL_DIM;

               int nTrailPoints = static_cast<int> (TRAIL_SEGMENTS * TRAIL_FRACTION);

               CURVE_POINT Curve_Point_Head;
               Curve_Point_Head.vPosition = vWorld;
               Curve_Point_Head.fRadius   = dTrailRadius;
               Curve_Build.aPoints.push_back (Curve_Point_Head);

               RMAP::MAP::MAP_OBJECT_CELESTIAL::ORBIT_POSITION pos;

               if (pCelestial->PositionAtTick (tmNow, pos))
               {
                  double dE_planet = pos.dE;
                  for (int i = 1; i <= nTrailPoints; i++)
                  {
                     double dE = dE_planet - (static_cast<double> (i) / TRAIL_SEGMENTS) * TWO_PI;
                     RMAP::MAP::MAP_OBJECT::VEC3 vPt = pCelestial->OrbitTrailPoint (dE, tmNow);

                     if (bMoonSystem)
                     {
                        vPt = vPt * MOON_ORBIT_BOOST;
                     }

                     // The trail lives in the parent frame; carry it through the
                     // parent's full world transform, same basis the node inherits.
                     RMAP::MAP::MAP_OBJECT::VEC3 vTrail = Mat4_Point (frame.mWorld, vPt);

                     float dTaper = 1.0f - static_cast<float> (i) / static_cast<float> (nTrailPoints);

                     CURVE_POINT Curve_Point;
                     Curve_Point.vPosition = vTrail;
                     Curve_Point.fRadius   = dTrailRadius * dTaper;
                     Curve_Build.aPoints.push_back (Curve_Point);
                  }
               }

               aCurve_Build.push_back (std::move (Curve_Build));
            }
         }
         else if (bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_STAR
              ||  bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_PLANET
              ||  bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_MOON
              ||  bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_DEBRIS)
         {
            wfChild.dRadius = pCelestial->Radius ();
            wfChild.fColor  = Pod.Properties.Celestial.fColor;
            wfChild.bStar   = (bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_STAR);
            wfChild.bMoon   = (bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_MOON);

            // A body's own radius extends the scene's reach, so a single body
            // centred at the origin still yields a sane scale (no divide-by-zero
            // fallback to 1.0 that would render it at near-true metres).
            double dBodyReach = vWorld.Length () + wfChild.dRadius;
            if (dBodyReach > dMaxReach) dMaxReach = dBodyReach;

            if (wfChild.bStar)
            {
               LIGHT_BUILD Light_Build;
               Light_Build.vPosition = vWorld;
               // A star's point light uses 1/r^2 falloff, and at solar render
               // scale a body sits very close to the star in render units -- the
               // default intensity blows surfaces out. Dim it so the lit limb
               // reads with detail instead of clipping to white. This is an
               // engine-generated light already tuned at render scale, so it
               // opts out of the unit-scale intensity invariance below.
               Light_Build.fIntensity  = 0.09f;
               Light_Build.bCompensate = false;
               aLight.push_back (Light_Build);
            }
         }
         else if (bType == RMAP::MAP::MAP_OBJECT_CELESTIAL::MAP_OBJECT_TYPE_CELESTIAL_SURFACE)
         {
            SPHERE_BUILD sphere;
            sphere.vPosition = vWorld;
            sphere.dRadiusM  = wfChild.dRadius;
            sphere.bMoon     = wfChild.bMoon;
            sphere.bEmissive = wfChild.bStar;
            ColorFromPropertyFloat (wfChild.fColor, sphere.rgbColor);

            pCelestial->GetTexture (sphere.pTex, sphere.dimTexture.nW, sphere.dimTexture.nH);

            aSphere.push_back (sphere);
         }
      }
      else if (pObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_PANEL)
      {
         // A panel manifests as a flat, textured quad. The UI is rasterized here,
         // on the compositor thread (the only thread that touches both RmlUi and
         // the renderer); the cost is paid once and cached, so re-traversal each
         // frame is cheap. Panels are chrome, not scene geometry: they do NOT
         // contribute to dMaxReach, so a panel never changes how the 3D content
         // is framed. Bound.d3Max[0,1] gives only the quad's aspect ratio; the
         // panel's world position rides the node's TRS (captured here) and its
         // on-screen size is resolved at the flatten seam (below).
         double dPanelW = Pod.Bound.d3Max[0];
         double dPanelH = Pod.Bound.d3Max[1];

         if (dPanelW > 0.0  &&  dPanelH > 0.0  &&  pNode->Render (pEngine, 512, 512)  && pNode->Pixels ())
         {
            PANEL_BUILD panel;
            panel.pbPixels  = pNode->Pixels ();
            panel.dim.nW    = pNode->Width ();
            panel.dim.nH    = pNode->Height ();
            panel.dAspect   = dPanelW / dPanelH;
            panel.vWorld    = vWorld;
            aPanel.push_back (panel);
         }
      }
      else if (pObj->m_wClass == RMAP::MAP::MAP_OBJECT_CLASS_LIGHT)
      {
         // A light node contributes an ANARI light at its world placement. Colour
         // comes from Properties.Light.fColor (0xRRGGBB), intensity from
         // fBrightness, and the subtype selects point / ambient / directional /
         // spot.
         LIGHT_BUILD Light_Build;
         Light_Build.vPosition = vWorld;
         ColorFromPropertyFloat (Pod.Properties.Light.fColor, Light_Build.rgbColor);

         // A light with no authored colour (fColor unset -> bits 0 -> black)
         // defaults to white, so brightness alone yields a visible white light.
         if (Light_Build.rgbColor.fR == 0.0f  &&  Light_Build.rgbColor.fG == 0.0f  &&  Light_Build.rgbColor.fB == 0.0f)
            Light_Build.rgbColor = { 1.0f, 1.0f, 1.0f };

         Light_Build.fIntensity = Pod.Properties.Light.fBrightness;

         // A spot light aims down the node's local +X axis (identity forward in the
         // Z-up world), rotated into world space by the frame's upper-3x3 (column 0).
         // Scale in the columns cancels under normalisation. Its cone comes from the
         // authored degrees.
         {
            RMAP::MAP::MAP_OBJECT::VEC3   vAim = { wfChild.mWorld.d[0], wfChild.mWorld.d[1], wfChild.mWorld.d[2] };
            double dLen = vAim.Length ();
            if (dLen > 0.0)
               Light_Build.vDirection = vAim * (1.0 / dLen);
            Light_Build.fOpeningAngle = static_cast<float> (Pod.Properties.Light.fOpeningAngle * DEG_TO_RAD);
            Light_Build.fFalloffAngle = static_cast<float> (Pod.Properties.Light.fFalloffAngle * DEG_TO_RAD);
         }

         // Accumulated linear scale of this light's world frame (average of the
         // upper-3x3 column norms). A point light authored at unit scale is
         // intensity-compensated by (worldScale * renderScale)^2 at the flatten
         // seam so its illumination is invariant to both the embed scale and the
         // per-scene render scale -- author once at unit scale, drop in anywhere.
         Light_Build.dWorldScale = Mat4_Scale (wfChild.mWorld);

         // Light nodes are placed lights only -- point or spot. Ambient and
         // directional are scene-global properties, never nodes.
         switch (Pod.Type.bType)
         {
         case RMAP::MAP::MAP_OBJECT_LIGHT::MAP_OBJECT_TYPE_LIGHT_SPOT:
         case RMAP::MAP::MAP_OBJECT_LIGHT::MAP_OBJECT_TYPE_LIGHT_SPOT__DEPRECATED:
            Light_Build.eType = LIGHT_DATA::kSPOT;
            break;
         case RMAP::MAP::MAP_OBJECT_LIGHT::MAP_OBJECT_TYPE_LIGHT_POINT:
         case RMAP::MAP::MAP_OBJECT_LIGHT::MAP_OBJECT_TYPE_LIGHT_POINT__DEPRECATED:
            Light_Build.eType = LIGHT_DATA::kPOINT;
            break;
         default:
            Light_Build.eType = LIGHT_DATA::kNONE;
            break;
         }

         // An unrecognised subtype is not a light -- drop it rather than carry a
         // kNONE entry the renderer would ignore anyway.
         if (Light_Build.eType != LIGHT_DATA::kNONE)
            aLight.push_back (Light_Build);
      }
   }

   if (!bSkipChildren)
   {
      for (int i = 0; i < pNode->Node_Count (); i++)
      {
         NODE* pChild = pNode->Child (i);
         if (pChild)
            TraverseNode (pChild, wfChild, tmNow, pEngine, aSphere, aCurve_Build, aLight, aBox, aPanel, aMesh, dMaxReach, vEyeMetre, dAngularRatio, aExpand, aCollapse, bBoundingBox, mapExtent);
      }

      // An attachment point spawns a child fabric; traverse it in this node's own
      // accumulated frame so the secondary fabric inherits this node's transform.
      FABRIC* pAttached = pNode->Fabric_Attachment ();

      if (pAttached  &&  pAttached->Node_Root ())
         TraverseNode (pAttached->Node_Root (), wfChild, tmNow, pEngine, aSphere, aCurve_Build, aLight, aBox, aPanel, aMesh, dMaxReach, vEyeMetre, dAngularRatio, aExpand, aCollapse, bBoundingBox, mapExtent);
   }
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

      auto tpNow = std::chrono::steady_clock::now ();
      float dDeltaSeconds = std::chrono::duration<float> (tpNow - pViewport->m_tpLastCameraUpdate).count ();
      pViewport->m_tpLastCameraUpdate = tpNow;

      if (dDeltaSeconds <= 0.0f  ||  dDeltaSeconds > 0.25f)
         dDeltaSeconds = 1.0f / 60.0f;

      // Any camera interaction releases an active scene-driven pose so the user
      // takes over the orbit from wherever the pose last placed it.
      if (Input.nMouseDX != 0  ||  Input.nMouseDY != 0  ||  Input.dScrollY != 0.0f
          ||  Input.bKeyA  ||  Input.bKeyS  ||  Input.bKeyD  ||  Input.bKeyW
          ||  Input.bKeySpace  ||  Input.bKeyCtrl)
         pViewport->Camera_Deactivate ();

      View.Update (Input.nMouseDX, Input.nMouseDY, Input.dScrollY, Input.bMouseLeft, Input.bMouseRight,
                   Input.bKeyA, Input.bKeyS, Input.bKeyD, Input.bKeyW,
                   Input.bKeySpace, Input.bKeyCtrl, Input.dMoveScale, dDeltaSeconds);

      // Z-up orbit: azimuth (Theta) sweeps the XY ground plane (0 = +X east,
      // 90 deg = +Y north) and elevation (Phi) lifts the eye toward +Z up.
      RMAP::MAP::MAP_OBJECT::VEC3 vEye;
      vEye.dX = View.m_vTarget.dX + View.m_dDistance * std::cos (View.m_dPhi) * std::cos (View.m_dTheta);
      vEye.dY = View.m_vTarget.dY + View.m_dDistance * std::cos (View.m_dPhi) * std::sin (View.m_dTheta);
      vEye.dZ = View.m_vTarget.dZ + View.m_dDistance * std::sin (View.m_dPhi);

      CAMERA_DATA Camera;
      Camera.vPosition  = vEye;
      Camera.vDirection = { View.m_vTarget.dX - vEye.dX, View.m_vTarget.dY - vEye.dY, View.m_vTarget.dZ - vEye.dZ };
      Camera.vUp        = { 0.0, 0.0, 1.0 };

      int nW = pRenderer->GetWidth ();
      int nH = pRenderer->GetHeight ();

#if (1)
      float dBaseFovY = static_cast<float> (DEFAULT_FOVY_DEG * DEG_TO_RAD);
      int   nRefH     = REFERENCE_HEIGHT_PX;
      Camera.fFovY    = 2.0f * std::atan (std::tan (dBaseFovY * 0.5f) * static_cast<float> (nH) / static_cast<float> (nRefH));
#else
      Camera.fFovY    = static_cast<float> (DEFAULT_FOVY_DEG * DEG_TO_RAD);
#endif
      Camera.fAspect  = (nW > 0  &&  nH > 0) ? static_cast<float> (nW) / static_cast<float> (nH) : 1.0f;

      // Camera.fNear / fFar are set below, once dRenderScale is known, so the
      // clip range can be expressed in rational world metres and the camera
      // committed there.

      pViewport->Accumulate (VIEWPORT::kACCUMULATE_INPUT, tpLoopStart);

      auto tpSceneStart = std::chrono::steady_clock::now ();

      std::vector<SPHERE_BUILD> aSphereBuild;
      std::vector<CURVE_BUILD>  aCurve_Build;
      std::vector<BOX_BUILD>    aBoxBuild;
      std::vector<LIGHT_BUILD>  aLightBuild;
      std::vector<PANEL_BUILD>  aPanelBuild;
      std::vector<MESH_BUILD>   aMeshBuild;

      // Proximity-driven lazy loading: nodes whose children should stream in, or
      // out, this frame (collected read-only during traversal, drained after it).
      std::vector<std::pair<CONTAINER*, uint64_t>> aExpand;
      std::vector<std::pair<CONTAINER*, uint64_t>> aCollapse;

      SCENE* pScene = pViewport->Scene ();
      FABRIC* pFabric_Root = pScene ? pScene->Fabric_Root () : nullptr;
      NODE* pSomRoot = pFabric_Root ? pFabric_Root->Node_Root () : nullptr;
      SNEEZE::ENGINE* pEngine = pViewport->Engine ();

      // Read the engine config once per frame (GetConfig locks) and thread the
      // bounding-box overlay flag into the traversal, rather than locking per node.
      SNEEZE::ENGINE::CONFIG Config = {};
      if (pEngine)
         pEngine->GetConfig (Config);
      bool bBoundingBox = Config.bBoundingBox;

      // A standalone preview asks (once, after loading a model) that the orbit
      // camera be re-framed to fit. All geometry is normalised to within
      // TARGET_EXTENT of the origin, so the distance that just fits it is
      // TARGET_EXTENT / tan(fovy/2), with a small margin. fovy scales with
      // viewport height, so this must be computed here, not at request time.
      // Only the distance/target are seeded; the user's subsequent orbit and
      // zoom are left alone (the flag is one-shot). Applies next frame's vEye.
      if (pScene  &&  pScene->Frame_Consume ())
      {
         float dTanHalfFovy = std::tan (Camera.fFovY * 0.5f);
         if (dTanHalfFovy > 1e-4f)
            View.m_dDistance = static_cast<float> (TARGET_EXTENT / dTanHalfFovy * 1.15);

         View.m_vTarget = { 0.0, 0.0, 0.0 };
         View.m_dTheta  = 0.3f;
         View.m_dPhi    = 0.4f;
      }

      double dMaxReach = 0.0;

      // Camera position in world metres for the proximity gate. vEye is in render
      // units; convert back with the previous frame's render scale (this frame's
      // is not known until after traversal). A one-frame lag is harmless for a
      // proximity trigger. The scale is 0 before the first completed frame.
      double dScalePrev = (pJob_Compositor->m_dRenderScale > 0.0f) ? static_cast<double> (pJob_Compositor->m_dRenderScale) : 1.0;
      RMAP::MAP::MAP_OBJECT::VEC3 vEyeMetre = { vEye.dX / dScalePrev, vEye.dY / dScalePrev, vEye.dZ / dScalePrev };

      // Learned node extents live on this compositor job so they survive
      // collapse within a session (a node's measured size is what keeps the
      // render scale from snapping when children stream out) but die with the
      // job when the URL bar tears the context down. They are keyed only by
      // composed OBJECTIX, which collides across fabrics -- a 100-mesh Tester01
      // load must not leave AU-scale or city-scale metres on Earth's keys.
      if (pViewport->Scene_Invalidate_Consume ())
      {
         pJob_Compositor->m_mapExtent.clear ();
         pRenderer->InvalidateScene ();
      }

      if (pSomRoot)
      {
         WORLD_FRAME rootFrame;
         TraverseNode (pSomRoot, rootFrame, tmNow, pEngine, aSphereBuild, aCurve_Build, aLightBuild, aBoxBuild, aPanelBuild, aMeshBuild, dMaxReach, vEyeMetre, PROXIMITY_LOAD_ANGULAR_RATIO, aExpand, aCollapse, bBoundingBox, pJob_Compositor->m_mapExtent);
      }

      // Collapse/Expand drain after EndFrame. Collapsed meshes are omitted from
      // this frame's submit list so incremental ANARI sync releases their
      // objects (and helium copies Array2D host pixels) before Node_Close frees
      // the CPU buffers. Expanding after collapse avoids opening a child that
      // this frame's collapse is about to close.

      // One uniform per-scene render scale, applied at this single flatten seam:
      // metres (double) -> render units (float). Sized so the root-anchored
      // bounding sphere fills TARGET_EXTENT and the default camera frames it.
      // Every renderable -- celestial and physical alike -- rides this one scale.
      double dRenderScale = (dMaxReach > MIN_REACH) ? (TARGET_EXTENT / dMaxReach) : 1.0;
      pJob_Compositor->m_dRenderScale = static_cast<float> (dRenderScale);

      // Clip planes are chosen as rational WORLD distances, then converted to
      // render units (x dRenderScale) so the viewing range no longer rides the
      // scene scale: a sub-metre prop and a city block alike see from ~5 cm out
      // to at least 20 km. The far plane widens to enclose a scene larger than
      // that floor, and the near plane is floored so far/near stays within what
      // the depth buffer can resolve -- an astronomical scene trades away near
      // detail rather than going black.
      double dFarWorld  = std::max (FAR_PLANE_MIN, 2.0 * dMaxReach);
      double dNearWorld = std::max (NEAR_PLANE_MIN, dFarWorld / NEAR_FAR_RATIO);
      Camera.fNear = static_cast<float> (dNearWorld * dRenderScale);
      Camera.fFar  = static_cast<float> (dFarWorld * dRenderScale);

      pRenderer->SetCamera (Camera);

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
         RMAP::MAP::MAP_OBJECT::VEC3 vEye = { CameraPose.aPosition[0] * dRenderScale, CameraPose.aPosition[1] * dRenderScale, CameraPose.aPosition[2] * dRenderScale };

         // Identity-forward is +X (Z-up world), so the look direction is the pose
         // quaternion applied to (1,0,0). Orbit angles are then extracted with Z as
         // the elevation axis and the XY plane as azimuth (matching the orbit above).
         RMAP::MAP::MAP_OBJECT::QUAT qPose = { CameraPose.aRotation[0], CameraPose.aRotation[1], CameraPose.aRotation[2], CameraPose.aRotation[3] };
         RMAP::MAP::MAP_OBJECT::VEC3 vT;

         vT.dX = 2.0 * (qPose.dY * 0.0 - qPose.dZ * 0.0);
         vT.dY = 2.0 * (qPose.dZ * 1.0 - qPose.dX * 0.0);
         vT.dZ = 2.0 * (qPose.dX * 0.0 - qPose.dY * 1.0);

         RMAP::MAP::MAP_OBJECT::VEC3 vDir;

         vDir.dX = 1.0 + qPose.dW * vT.dX + (qPose.dY * vT.dZ - qPose.dZ * vT.dY);
         vDir.dY = 0.0 + qPose.dW * vT.dY + (qPose.dZ * vT.dX - qPose.dX * vT.dZ);
         vDir.dZ = 0.0 + qPose.dW * vT.dZ + (qPose.dX * vT.dY - qPose.dY * vT.dX);
         double dLen = vDir.Length ();
         if (dLen > 1e-9) { vDir.dX /= dLen; vDir.dY /= dLen; vDir.dZ /= dLen; }

         double dDistance = vEye.Length ();
         if (dDistance < 1e-6) dDistance = 10.0;

         View.m_vTarget    = vEye + vDir * dDistance;
         View.m_dDistance  = static_cast<float> (dDistance);
         View.m_dPhi      = static_cast<float> (std::asin (std::max (-1.0, std::min (1.0, -vDir.dZ))));
         View.m_dTheta    = static_cast<float> (std::atan2 (-vDir.dY, -vDir.dX));
      }

      std::vector<SPHERE_DATA> aSphere_Data;
      aSphere_Data.reserve (aSphereBuild.size ());
      for (const auto& sb : aSphereBuild)
      {
         SPHERE_DATA Sphere_Data;
         Sphere_Data.vPosition       = sb.vPosition * dRenderScale;
         Sphere_Data.fRadius         = MagnifyRadius (sb.dRadiusM, dRenderScale, sb.bMoon);
         Sphere_Data.bEmissive       = sb.bEmissive;
         Sphere_Data.rgbColor        = sb.rgbColor;
         Sphere_Data.pbTexturePixels = sb.pTex;
         Sphere_Data.dimTexture      = sb.dimTexture;
         aSphere_Data.push_back (Sphere_Data);
      }

      std::vector<CURVE_DATA> aCurve_Data;
      aCurve_Data.reserve (aCurve_Build.size ());
      for (const auto& cb : aCurve_Build)
      {
         CURVE_DATA Curve_Data;
         Curve_Data.rgbColor = cb.rgbColor;
         Curve_Data.aPoints.reserve (cb.aPoints.size ());
         for (const auto& p : cb.aPoints)
         {
            CURVE_POINT Curve_Point;
            Curve_Point.vPosition    = p.vPosition * dRenderScale;
            Curve_Point.fRadius      = p.fRadius;
            Curve_Data.aPoints.push_back (Curve_Point);
         }
         aCurve_Data.push_back (std::move (Curve_Data));
      }

      std::vector<LIGHT_DATA> aLight;
      aLight.reserve (aLightBuild.size ());
      for (const LIGHT_BUILD& Light_Build : aLightBuild)
      {
         LIGHT_DATA Light_Data;
         Light_Data.eType = Light_Build.eType;

         // A point/spot light's position rides the per-scene render scale like any
         // world point; a directional light has no position, and its direction is
         // left unscaled. An ambient light uses neither.
         bool   bPositional = (Light_Build.eType == LIGHT_DATA::kPOINT  ||  Light_Build.eType == LIGHT_DATA::kSPOT  ||  Light_Build.eType == LIGHT_DATA::kPOINT__DEPRECATED  ||  Light_Build.eType == LIGHT_DATA::kSPOT__DEPRECATED);
         double dScale      = bPositional ? dRenderScale : 1.0;
         Light_Data.vPosition   = Light_Build.vPosition * dScale;
         Light_Data.vDirection  = Light_Build.vDirection;
         Light_Data.rgbColor    = Light_Build.rgbColor;

         // Distances from authored local space to render space scale by
         // (worldScale * renderScale); a point/spot light's 1/r^2 falloff therefore
         // needs intensity multiplied by that factor squared to keep illumination
         // invariant. Directional/ambient lights have no falloff, so they pass
         // through unscaled.
         if (bPositional  &&  Light_Build.bCompensate)
         {
            double dFull = Light_Build.dWorldScale * dRenderScale;
            Light_Data.fIntensity = static_cast<float> (Light_Build.fIntensity * dFull * dFull);
         }
         else
            Light_Data.fIntensity = Light_Build.fIntensity;

         Light_Data.fOpeningAngle = Light_Build.fOpeningAngle;
         Light_Data.fFalloffAngle = Light_Build.fFalloffAngle;
         aLight.push_back (Light_Data);
      }

      std::vector<BOX_DATA> aBox_Data;
      aBox_Data.reserve (aBoxBuild.size ());
      for (const auto& bb : aBoxBuild)
      {
         BOX_DATA Box_Data;
         for (int j = 0; j < 4; j++)
         {
            Box_Data.mWorld.f[j * 4 + 0] = static_cast<float> (bb.mWorld.d[j * 4 + 0] * dRenderScale);
            Box_Data.mWorld.f[j * 4 + 1] = static_cast<float> (bb.mWorld.d[j * 4 + 1] * dRenderScale);
            Box_Data.mWorld.f[j * 4 + 2] = static_cast<float> (bb.mWorld.d[j * 4 + 2] * dRenderScale);
            Box_Data.mWorld.f[j * 4 + 3] = static_cast<float> (bb.mWorld.d[j * 4 + 3]);
         }
         Box_Data.rgbColor = bb.rgbColor;
         aBox_Data.push_back (Box_Data);
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

         RMAP::MAP::MAP_OBJECT::VEC3 vAnchor = pb.vWorld * dRenderScale;

         // Billboard basis: +Z (panel local normal) points at the eye; world-up = (0,0,1).
         RMAP::MAP::MAP_OBJECT::VEC3   vNormal = { vEye.dX - vAnchor.dX, vEye.dY - vAnchor.dY, vEye.dZ - vAnchor.dZ };
         double dNLen   = vNormal.Length ();
         if (dNLen < 1e-9) { vNormal = { 1.0, 0.0, 0.0 }; dNLen = 1.0; }
         vNormal.dX /= dNLen; vNormal.dY /= dNLen; vNormal.dZ /= dNLen;

         // right = normalize(worldUp x normal); worldUp = (0,0,1)
         RMAP::MAP::MAP_OBJECT::VEC3   vRight = { -vNormal.dY, vNormal.dX, 0.0 };
         double dRLen  = vRight.Length ();
         if (dRLen < 1e-9) { vRight = { 1.0, 0.0, 0.0 }; dRLen = 1.0; }
         vRight.dX /= dRLen; vRight.dY /= dRLen;

         // up = normal x right
         RMAP::MAP::MAP_OBJECT::VEC3 vUp;

         vUp.dX = vNormal.dY * vRight.dZ - vNormal.dZ * vRight.dY;
         vUp.dY = vNormal.dZ * vRight.dX - vNormal.dX * vRight.dZ;
         vUp.dZ = vNormal.dX * vRight.dY - vNormal.dY * vRight.dX;

         PANEL_DATA Panel_Data;
         Panel_Data.mWorld.f[0]  = static_cast<float> (vRight.dX * dWidth);
         Panel_Data.mWorld.f[1]  = static_cast<float> (vRight.dY * dWidth);
         Panel_Data.mWorld.f[2]  = static_cast<float> (vRight.dZ * dWidth);
         Panel_Data.mWorld.f[3]  = 0.0f;
         Panel_Data.mWorld.f[4]  = static_cast<float> (vUp.dX * dHeight);
         Panel_Data.mWorld.f[5]  = static_cast<float> (vUp.dY * dHeight);
         Panel_Data.mWorld.f[6]  = static_cast<float> (vUp.dZ * dHeight);
         Panel_Data.mWorld.f[7]  = 0.0f;
         Panel_Data.mWorld.f[8]  = static_cast<float> (vNormal.dX);
         Panel_Data.mWorld.f[9]  = static_cast<float> (vNormal.dY);
         Panel_Data.mWorld.f[10] = static_cast<float> (vNormal.dZ);
         Panel_Data.mWorld.f[11] = 0.0f;
         Panel_Data.mWorld.f[12] = static_cast<float> (vAnchor.dX);
         Panel_Data.mWorld.f[13] = static_cast<float> (vAnchor.dY);
         Panel_Data.mWorld.f[14] = static_cast<float> (vAnchor.dZ);
         Panel_Data.mWorld.f[15] = 1.0f;

         Panel_Data.pbPixels = pb.pbPixels;
         Panel_Data.dim.nW = pb.dim.nW;
         Panel_Data.dim.nH = pb.dim.nH;
         aPanel_Data.push_back (Panel_Data);
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
         mesh.pInstanceOwner = mb.pInstanceOwner;
         mesh.nDrawIx        = mb.nDrawIx;
         for (int j = 0; j < 4; j++)
         {
            mesh.mWorld.f[j * 4 + 0] = static_cast<float> (mb.mWorld.d[j * 4 + 0] * dRenderScale);
            mesh.mWorld.f[j * 4 + 1] = static_cast<float> (mb.mWorld.d[j * 4 + 1] * dRenderScale);
            mesh.mWorld.f[j * 4 + 2] = static_cast<float> (mb.mWorld.d[j * 4 + 2] * dRenderScale);
            mesh.mWorld.f[j * 4 + 3] = static_cast<float> (mb.mWorld.d[j * 4 + 3]);
         }
         aMesh_Data.push_back (mesh);
      }

      pRenderer->SetLights (aLight);

      // Scene-global ambient + directional ("sun") are properties of the scene,
      // set by the primary fabric -- passed on their own channel, never as
      // placed LIGHT_DATA. Absent scene => both default off.
      if (pScene)
         pRenderer->SetSceneLighting (pScene->Ambient (), pScene->Directional ());
      else
         pRenderer->SetSceneLighting (SCENE_LIGHT {}, SCENE_LIGHT {});

      pViewport->Accumulate (VIEWPORT::kACCUMULATE_SCENE, tpSceneStart);

      RGBA rgbaBackground;
      if (pScene  &&  pScene->Background_Consume (rgbaBackground))
         pRenderer->SetBackground (rgbaBackground.fR, rgbaBackground.fG, rgbaBackground.fB, rgbaBackground.fA);

      pRenderer->BeginFrame ();
      pRenderer->BoundingBoxOverlay (bBoundingBox);
      pRenderer->SubmitSpheres (aSphere_Data);
      pRenderer->SubmitCurves (aCurve_Data);
      pRenderer->SubmitBoxes (aBox_Data);
      pRenderer->SubmitPanels (aPanel_Data);
      pRenderer->SubmitMeshes (aMesh_Data);
      pRenderer->EndFrame ();

      for (auto& Collapse : aCollapse)
      {
         if (Collapse.first)
            Collapse.first->Node_Collapse (Collapse.second);
      }

      for (auto& Expand : aExpand)
      {
         if (Expand.first)
            Expand.first->Node_Expand (Expand.second);
      }

      pViewport->Accumulate (VIEWPORT::kACCUMULATE_SUBMIT, pRenderer->GetLastSubmitSeconds ());
      pViewport->Accumulate (VIEWPORT::kACCUMULATE_RENDER, pRenderer->GetLastRenderSeconds ());

      // Native-swapchain Halogen no longer flushAndWait (that hung the
      // compositor after a large glTF upload / GPU TDR). Filament's frame
      // skipper then makes beginFrame return immediately while the GPU is
      // busy, and this job would spin at tens of thousands of FPS - the FPS
      // log shows 0.0 ms and a nonsense frame count. Cap to 60 Hz so camera
      // dt and the trace line stay meaningful. When beginFrame does present,
      // FIFO vsync still applies on top of this floor.
      auto   tpLoopEnd  = std::chrono::steady_clock::now ();
      double dElapsed   = std::chrono::duration<double> (tpLoopEnd - tpLoopStart).count ();
      double dFrameMin  = 1.0 / 60.0;

      if (dElapsed < dFrameMin)
      {
         int64_t nSleepNs = static_cast<int64_t> ((dFrameMin - dElapsed) * 1000000000.0);

         if (nSleepNs > 0)
            std::this_thread::sleep_for (std::chrono::nanoseconds (nSleepNs));
      }

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
   pViewport->Diagnostics (pRenderer  &&  pRenderer->LastPresented ());

   pJob_Compositor->m_nLastFrame = ++s_nGlobalFrameSeq;

   pJob_Compositor->Return (JOB_COMPOSITOR::kSTATE_RENDER);
}
