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

#include "Viewport.h"
#include <cmath>

void GenerateUVSphere (UV_SPHERE& sphere, float dRadius,
                       int nStacks, int nSlices,
                       float dCenterX, float dCenterY, float dCenterZ)
{
   sphere.aPositions.clear ();
   sphere.aNormals.clear ();
   sphere.aTexCoords.clear ();
   sphere.aIndices.clear ();

   for (int nStack = 0; nStack <= nStacks; nStack++)
   {
      float dPhi = PI * static_cast<float> (nStack) / static_cast<float> (nStacks);
      float dSinPhi = std::sin (dPhi);
      float dCosPhi = std::cos (dPhi);

      for (int nSlice = 0; nSlice <= nSlices; nSlice++)
      {
         float dTheta = 2.0f * PI * static_cast<float> (nSlice) / static_cast<float> (nSlices);
         float dSinTheta = std::sin (dTheta);
         float dCosTheta = std::cos (dTheta);

         // Rx(+90) of the classic Y-up sphere (x, cosPhi, ...) -> Z-up poles on
         // +/-Z. Note the negation on Y: (x, y, z) -> (x, -z, y) is a proper
         // rotation (det +1). A bare Y/Z swap would be a reflection and invert
         // every triangle's winding (inside-out sphere / inward normals).
         float nx =  dSinPhi * dCosTheta;
         float ny = -dSinPhi * dSinTheta;
         float nz =  dCosPhi;

         sphere.aPositions.push_back (dCenterX + dRadius * nx);
         sphere.aPositions.push_back (dCenterY + dRadius * ny);
         sphere.aPositions.push_back (dCenterZ + dRadius * nz);

         sphere.aNormals.push_back (nx);
         sphere.aNormals.push_back (ny);
         sphere.aNormals.push_back (nz);

         float u = static_cast<float> (nSlice) / static_cast<float> (nSlices);
         float v = static_cast<float> (nStack) / static_cast<float> (nStacks);
         sphere.aTexCoords.push_back (u);
         sphere.aTexCoords.push_back (v);
      }
   }

   for (int nStack = 0; nStack < nStacks; nStack++)
   {
      for (int nSlice = 0; nSlice < nSlices; nSlice++)
      {
         uint32_t nCur  = static_cast<uint32_t> (nStack * (nSlices + 1) + nSlice);
         uint32_t nNext = nCur + static_cast<uint32_t> (nSlices + 1);

         sphere.aIndices.push_back (nCur);
         sphere.aIndices.push_back (nCur + 1);
         sphere.aIndices.push_back (nNext);

         sphere.aIndices.push_back (nCur + 1);
         sphere.aIndices.push_back (nNext + 1);
         sphere.aIndices.push_back (nNext);
      }
   }
}

void GenerateUnitBox (UV_SPHERE& box)
{
   box.aPositions.clear ();
   box.aNormals.clear ();
   box.aTexCoords.clear ();
   box.aIndices.clear ();

   // Six faces, each with four corners and one outward normal. Separate
   // vertices per face so normals are flat (no shared-vertex averaging).
   static const float aFaceNormal[6][3] =
   {
      {  0.0f,  0.0f,  1.0f },        // +Z
      {  0.0f,  0.0f, -1.0f },        // -Z
      {  1.0f,  0.0f,  0.0f },        // +X
      { -1.0f,  0.0f,  0.0f },        // -X
      {  0.0f,  1.0f,  0.0f },        // +Y
      {  0.0f, -1.0f,  0.0f },        // -Y
   };

   // Corner offsets per face, ordered CCW when viewed from outside.
   static const float aFaceCorner[6][4][3] =
   {
      { { -0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f }, {  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f } },
      { {  0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f }, {  0.5f,  0.5f, -0.5f } },
      { {  0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f, -0.5f }, {  0.5f,  0.5f, -0.5f }, {  0.5f,  0.5f,  0.5f } },
      { { -0.5f, -0.5f, -0.5f }, { -0.5f, -0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f, -0.5f } },
      { { -0.5f,  0.5f,  0.5f }, {  0.5f,  0.5f,  0.5f }, {  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f } },
      { { -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f,  0.5f }, { -0.5f, -0.5f,  0.5f } },
   };

   static const float aCornerUV[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };

   for (int nFace = 0; nFace < 6; nFace++)
   {
      uint32_t nBase = static_cast<uint32_t> (box.aPositions.size () / 3);

      for (int nCorner = 0; nCorner < 4; nCorner++)
      {
         box.aPositions.push_back (aFaceCorner[nFace][nCorner][0]);
         box.aPositions.push_back (aFaceCorner[nFace][nCorner][1]);
         box.aPositions.push_back (aFaceCorner[nFace][nCorner][2]);

         box.aNormals.push_back (aFaceNormal[nFace][0]);
         box.aNormals.push_back (aFaceNormal[nFace][1]);
         box.aNormals.push_back (aFaceNormal[nFace][2]);

         box.aTexCoords.push_back (aCornerUV[nCorner][0]);
         box.aTexCoords.push_back (aCornerUV[nCorner][1]);
      }

      box.aIndices.push_back (nBase);
      box.aIndices.push_back (nBase + 1);
      box.aIndices.push_back (nBase + 2);

      box.aIndices.push_back (nBase);
      box.aIndices.push_back (nBase + 2);
      box.aIndices.push_back (nBase + 3);
   }
}

