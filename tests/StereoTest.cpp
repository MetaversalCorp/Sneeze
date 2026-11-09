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
// Stereo suite: the two cameras of a stereo pair (Stereo_Eyes) checked against
// an independent projection, so no renderer or GPU is needed. A point is
// projected through a camera with the pinhole model and the camera's image
// region; the zero-parallax plane must land on the same frame column in both
// views, and nearer points must cross over (the left view's column is greater).
// ---------------------------------------------------------------------------

#include <Sneeze.h>
#include "context/viewport/Viewport.h"

#include <cmath>
#include <cstdio>

using namespace SNEEZE;

static int nPassed = 0;
static int nFailed = 0;

static void Check (bool bCondition, const char* szName)
{
   if (bCondition)
   {
      std::printf ("  PASS: %s\n", szName);
      nPassed++;
   }
   else
   {
      std::printf ("  FAIL: %s\n", szName);
      nFailed++;
   }
}

using VEC3 = RMAP::MAP::MAP_OBJECT::VEC3;

static double Dot (const VEC3& a, const VEC3& b) { return a.dX * b.dX + a.dY * b.dY + a.dZ * b.dZ; }
static VEC3   Sub (const VEC3& a, const VEC3& b) { return { a.dX - b.dX, a.dY - b.dY, a.dZ - b.dZ }; }

// The column, in [0, 1] across the frame, of a world point seen by a camera
// whose axes are the unit vectors vRight, vUp and vForward.
static double Column (const CAMERA_DATA& Camera, const VEC3& vRight, const VEC3& vForward, const VEC3& vPoint)
{
   VEC3   vRel   = Sub (vPoint, Camera.vPosition);
   double dDepth = Dot (vRel, vForward);
   double dWidth = 2.0 * dDepth * std::tan (0.5 * Camera.fFovY) * Camera.fAspect;
   double dImage = Dot (vRel, vRight) / dWidth + 0.5;

   return (dImage - Camera.afImageRegion[0]) / (Camera.afImageRegion[2] - Camera.afImageRegion[0]);
}

static void TestEyes (const char* szName, const VEC3& vDirection, const VEC3& vUp, const VEC3& vRight, double dRenderScale)
{
   std::printf ("%s\n", szName);

   CAMERA_DATA Centre;
   Centre.vPosition  = { 1.5, -2.0, 0.75 };
   Centre.vDirection = vDirection;
   Centre.vUp        = vUp;
   Centre.fFovY      = 1.0f;
   Centre.fAspect    = 16.0f / 9.0f;
   Centre.fNear      = 0.1f;
   Centre.fFar       = 1000.0f;

   VIEWPORT::STEREO Stereo;
   Stereo.bEnable     = true;
   Stereo.dInterpupillaryDistance = 0.064;
   Stereo.dZeroParallaxDistance      = 4.0;

   CAMERA_DATA Left, Right;
   Stereo_Eyes (Centre, Stereo, dRenderScale, Left, Right);

   const double dB = Stereo.dInterpupillaryDistance * dRenderScale;
   VEC3 vBase = Sub (Right.vPosition, Left.vPosition);
   Check (std::fabs (Dot (vBase, vRight) - dB) < 1.0e-9  &&  std::fabs (Dot (vBase, vBase) - dB * dB) < 1.0e-9, "the eyes sit one interpupillary distance apart along the right vector");
   Check (std::fabs (Dot (vBase, vDirection)) < 1.0e-9, "the eyes are level: no offset along the view");
   VEC3 vMid = { 0.5 * (Left.vPosition.dX + Right.vPosition.dX), 0.5 * (Left.vPosition.dY + Right.vPosition.dY), 0.5 * (Left.vPosition.dZ + Right.vPosition.dZ) };
   Check (std::fabs (vMid.dX - Centre.vPosition.dX) + std::fabs (vMid.dY - Centre.vPosition.dY) + std::fabs (vMid.dZ - Centre.vPosition.dZ) < 1.0e-9, "the centre camera is midway");
   Check (Left.vDirection.dX == Centre.vDirection.dX  &&  Right.vDirection.dY == Centre.vDirection.dY  &&  Left.vUp.dZ == Centre.vUp.dZ  &&  Right.fFovY == Centre.fFovY  &&  Left.fAspect == Centre.fAspect, "axes, field of view and aspect are the centre camera's: parallel, never toed in");
   Check (Left.afImageRegion[1] == 0.0f  &&  Left.afImageRegion[3] == 1.0f  &&  Right.afImageRegion[1] == 0.0f  &&  Right.afImageRegion[3] == 1.0f, "the region keeps the whole height: no vertical offset");
   Check (std::fabs (Left.afImageRegion[2] - Left.afImageRegion[0] - 1.0f) < 1.0e-6f  &&  std::fabs (Right.afImageRegion[2] - Right.afImageRegion[0] - 1.0f) < 1.0e-6f, "each region is one image wide: no scaling");
   Check (std::fabs (Left.afImageRegion[0] + Right.afImageRegion[0]) < 1.0e-7f  &&  Left.afImageRegion[0] > 0.0f, "the regions shift by the same amount in opposite directions");

   const double dShift = (0.5 * dB) / (2.0 * Stereo.dZeroParallaxDistance * dRenderScale * std::tan (0.5) * Centre.fAspect);
   Check (std::fabs (Left.afImageRegion[0] - dShift) < 1.0e-6, "the shift is (b / 2) / (width of the image plane at the zero-parallax distance)");

   // Points on the zero-parallax plane: the same column in both views.
   VEC3 vOnPlane[3];
   for (int n = 0; n < 3; ++n)
   {
      double dAcross = -1.0 + n;     // render units either side of the middle
      vOnPlane[n] = { Centre.vPosition.dX + vDirection.dX * Stereo.dZeroParallaxDistance * dRenderScale + vRight.dX * dAcross * dRenderScale,
                      Centre.vPosition.dY + vDirection.dY * Stereo.dZeroParallaxDistance * dRenderScale + vRight.dY * dAcross * dRenderScale,
                      Centre.vPosition.dZ + vDirection.dZ * Stereo.dZeroParallaxDistance * dRenderScale + vRight.dZ * dAcross * dRenderScale };
   }
   double dWorst = 0.0;
   for (int n = 0; n < 3; ++n)
      dWorst = std::fmax (dWorst, std::fabs (Column (Left, vRight, vDirection, vOnPlane[n]) - Column (Right, vRight, vDirection, vOnPlane[n])));
   Check (dWorst < 1.0e-6, "a point on the zero-parallax plane has the same column in both views");
   double dMiddle = Column (Left, vRight, vDirection, vOnPlane[1]);
   Check (std::fabs (dMiddle - 0.5) < 1.0e-6, "the middle of the plane is the middle of each view");

   // Nearer and farther points: crossed and uncrossed disparity.
   for (double dMetres : { 2.0, 3.0, 8.0, 12.0 })
   {
      VEC3 vPoint = { Centre.vPosition.dX + vDirection.dX * dMetres * dRenderScale,
                      Centre.vPosition.dY + vDirection.dY * dMetres * dRenderScale,
                      Centre.vPosition.dZ + vDirection.dZ * dMetres * dRenderScale };
      double dL = Column (Left, vRight, vDirection, vPoint), dR = Column (Right, vRight, vDirection, vPoint);
      // On the axis, the disparity in image widths is (b / width at the point) - (b / width at the plane)
      double dTan  = 2.0 * std::tan (0.5) * Centre.fAspect;
      double dPred = dB / (dMetres * dRenderScale * dTan) - dB / (Stereo.dZeroParallaxDistance * dRenderScale * dTan);
      char szCase[96];
      std::snprintf (szCase, sizeof (szCase), "disparity at %.0f m matches b (1/z - 1/Zs) (%.5f against %.5f of the width)", dMetres, dL - dR, dPred);
      Check (std::fabs ((dL - dR) - dPred) < 1.0e-6, szCase);
      Check ((dMetres < Stereo.dZeroParallaxDistance) == (dL > dR), "nearer than the plane crosses, farther does not");
   }
}

// The row, in [0, 1] from the top of the frame, of a world point seen by a camera.
static double Row (const CAMERA_DATA& Camera, const VEC3& vUp, const VEC3& vForward, const VEC3& vPoint)
{
   VEC3   vRel   = Sub (vPoint, Camera.vPosition);
   double dDepth = Dot (vRel, vForward);

   return 0.5 - Dot (vRel, vUp) / (2.0 * dDepth * std::tan (0.5 * Camera.fFovY));
}

// The pointer ray (VIEWPORT::Ray_From_Pointer) from the basis the compositor
// publishes: for one view the camera's own, for two views the left camera's
// with its image region. Every point of the ray must be seen by that camera at
// the pixel the ray came from.
static void TestPointer (double dRenderScale)
{
   std::printf ("The pointer ray, render scale %g\n", dRenderScale);

   const int  nW = 640, nH = 360;
   const VEC3 vForward = { 1.0, 0.0, 0.0 }, vUp = { 0.0, 0.0, 1.0 }, vRight = { 0.0, -1.0, 0.0 };

   CAMERA_DATA Centre;
   Centre.vPosition  = { 1.5 * dRenderScale, -2.0 * dRenderScale, 0.75 * dRenderScale };
   Centre.vDirection = vForward;
   Centre.vUp        = vUp;
   Centre.fFovY      = 1.0f;
   Centre.fAspect    = static_cast<float> (nW) / static_cast<float> (nH);

   VIEWPORT::STEREO Stereo;
   Stereo.bEnable                 = true;
   Stereo.dInterpupillaryDistance = 0.064;
   Stereo.dZeroParallaxDistance   = 4.0;

   CAMERA_DATA Left, Right;
   Stereo_Eyes (Centre, Stereo, dRenderScale, Left, Right);

   for (int nViews = 1; nViews <= 2; ++nViews)
   {
      const CAMERA_DATA& Camera = (nViews == 2) ? Left : Centre;

      VIEWPORT Viewport (nullptr);
      Viewport.Pointer_Basis (Camera.vPosition.dX / dRenderScale, Camera.vPosition.dY / dRenderScale, Camera.vPosition.dZ / dRenderScale,
                              vForward.dX, vForward.dY, vForward.dZ, vRight.dX, vRight.dY, vRight.dZ, vUp.dX, vUp.dY, vUp.dZ,
                              std::tan (0.5 * Camera.fFovY), Camera.fAspect, nW, nH, Camera.afImageRegion[0]);

      double dWorst = 0.0;
      bool   bRays  = true;

      for (int nX : { 0, 101, 320, 517, 639 })
      {
         int    nY = (nX * 7) % nH;
         double dOx, dOy, dOz, dDx, dDy, dDz;

         bRays = bRays  &&  Viewport.Ray_From_Pointer (nX, nY, dOx, dOy, dOz, dDx, dDy, dDz);

         for (double dMetres : { 2.0, 4.0, 12.0 })
         {
            double dT = dMetres / dDx;    // the ray's point dMetres ahead (forward is +X)
            VEC3   vPoint = { (dOx + dDx * dT) * dRenderScale, (dOy + dDy * dT) * dRenderScale, (dOz + dDz * dT) * dRenderScale };

            dWorst = std::fmax (dWorst, std::fabs (Column (Camera, vRight, vForward, vPoint) * nW - (nX + 0.5)));
            dWorst = std::fmax (dWorst, std::fabs (Row (Camera, vUp, vForward, vPoint) * nH - (nY + 0.5)));
         }
      }

      // The region is float, so its width x1 - x0 is 1 only to float precision: about 4e-5 px at 640 px.
      char szCase[128];
      std::snprintf (szCase, sizeof (szCase), "%s: the ray through a pixel meets what %s shows at that pixel at 2, 4 and 12 m (worst %.2g px)",
                     (nViews == 2) ? "two views" : "one view", (nViews == 2) ? "the left view" : "the camera", dWorst);
      Check (bRays  &&  dWorst < 1.0e-3, szCase);
   }
}

static void TestDefaultRegion ()
{
   std::printf ("A camera with no stereo\n");

   CAMERA_DATA Camera;
   Check (Camera.afImageRegion[0] == 0.0f  &&  Camera.afImageRegion[1] == 0.0f  &&  Camera.afImageRegion[2] == 1.0f  &&  Camera.afImageRegion[3] == 1.0f, "the default region is the whole image, which is never sent to the renderer");
}

int RunStereoTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Stereo Test Suite ===\n");

   TestDefaultRegion ();
   TestEyes ("Looking along +X, Z up, render scale 1", { 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.0, -1.0, 0.0 }, 1.0);
   TestEyes ("Looking along +X, Z up, render scale 37.5 (units are metres times the scene scale)", { 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.0, -1.0, 0.0 }, 37.5);
   // A direction turned 30 degrees about Z: right = forward x up turns with it.
   TestEyes ("Turned 30 degrees", { 0.8660254037844386, 0.5, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.5, -0.8660254037844386, 0.0 }, 1.0);
   TestPointer (1.0);
   TestPointer (37.5);

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
