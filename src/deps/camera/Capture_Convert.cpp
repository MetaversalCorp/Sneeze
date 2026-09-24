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

#include "camera/Capture_Convert.h"

using namespace SNEEZE::DEP;

namespace
{
   inline uint8_t ClampByte (int nValue)
   {
      uint8_t nResult = 255;
      if (nValue < 0)
         nResult = 0;
      else if (nValue < 255)
         nResult = static_cast<uint8_t> (nValue);
      return nResult;
   }

   inline void YuvPixel (int nY, int nU, int nV, uint8_t* pRgba)
   {
      const int nC = nY - 16;
      const int nD = nU - 128;
      const int nE = nV - 128;
      pRgba[0] = ClampByte ((298 * nC + 409 * nE + 128) >> 8);
      pRgba[1] = ClampByte ((298 * nC - 100 * nD - 208 * nE + 128) >> 8);
      pRgba[2] = ClampByte ((298 * nC + 516 * nD + 128) >> 8);
      pRgba[3] = 255;
   }
}

void CAPTURE_CONVERT::Bgra (const uint8_t* pSrc, int nWidth, int nHeight, int nStride, std::vector<uint8_t>& aRgba)
{
   if (!pSrc  ||  nWidth <= 0  ||  nHeight <= 0)
      aRgba.clear ();
   else
   {
      aRgba.resize (static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4);
      const int nAbsStride = (nStride >= 0) ? nStride : -nStride;
      for (int nY = 0; nY < nHeight; nY++)
      {
         const int nSrcY = (nStride >= 0) ? nY : (nHeight - 1 - nY);
         const uint8_t* pRow = pSrc + static_cast<size_t> (nSrcY) * static_cast<size_t> (nAbsStride);
         uint8_t* pDst = aRgba.data () + static_cast<size_t> (nY) * static_cast<size_t> (nWidth) * 4;
         for (int nX = 0; nX < nWidth; nX++)
         {
            pDst[0] = pRow[2];
            pDst[1] = pRow[1];
            pDst[2] = pRow[0];
            pDst[3] = 255;
            pRow += 4;
            pDst += 4;
         }
      }
   }
}

void CAPTURE_CONVERT::Rgb (const uint8_t* pSrc, int nWidth, int nHeight, int nStride, std::vector<uint8_t>& aRgba)
{
   if (!pSrc  ||  nWidth <= 0  ||  nHeight <= 0)
      aRgba.clear ();
   else
   {
      aRgba.resize (static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4);
      const int nAbsStride = (nStride >= 0) ? nStride : nWidth * 3;
      for (int nY = 0; nY < nHeight; nY++)
      {
         const uint8_t* pRow = pSrc + static_cast<size_t> (nY) * static_cast<size_t> (nAbsStride);
         uint8_t* pDst = aRgba.data () + static_cast<size_t> (nY) * static_cast<size_t> (nWidth) * 4;
         for (int nX = 0; nX < nWidth; nX++)
         {
            pDst[0] = pRow[0];
            pDst[1] = pRow[1];
            pDst[2] = pRow[2];
            pDst[3] = 255;
            pRow += 3;
            pDst += 4;
         }
      }
   }
}

void CAPTURE_CONVERT::Yuy2 (const uint8_t* pSrc, int nWidth, int nHeight, int nStride, std::vector<uint8_t>& aRgba)
{
   if (!pSrc  ||  nWidth <= 0  ||  nHeight <= 0)
      aRgba.clear ();
   else
   {
      aRgba.resize (static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4);
      const int nAbsStride = (nStride >= 0) ? nStride : nWidth * 2;
      for (int nY = 0; nY < nHeight; nY++)
      {
         const uint8_t* pRow = pSrc + static_cast<size_t> (nY) * static_cast<size_t> (nAbsStride);
         uint8_t* pDst = aRgba.data () + static_cast<size_t> (nY) * static_cast<size_t> (nWidth) * 4;
         for (int nX = 0; nX < nWidth; nX += 2)
         {
            const int nY0 = pRow[0];
            const int nU  = pRow[1];
            const int nY1 = pRow[2];
            const int nV  = pRow[3];
            YuvPixel (nY0, nU, nV, pDst);
            if (nX + 1 < nWidth)
               YuvPixel (nY1, nU, nV, pDst + 4);
            pRow += 4;
            pDst += 8;
         }
      }
   }
}

void CAPTURE_CONVERT::Nv12 (const uint8_t* pY, int nStrideY, const uint8_t* pUv, int nStrideUv, int nWidth, int nHeight, std::vector<uint8_t>& aRgba)
{
   if (!pY  ||  !pUv  ||  nWidth <= 0  ||  nHeight <= 0)
      aRgba.clear ();
   else
   {
      aRgba.resize (static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4);
      for (int nY = 0; nY < nHeight; nY++)
      {
         const uint8_t* pRowY  = pY  + static_cast<size_t> (nY)       * static_cast<size_t> (nStrideY);
         const uint8_t* pRowUv = pUv + static_cast<size_t> (nY / 2)   * static_cast<size_t> (nStrideUv);
         uint8_t* pDst = aRgba.data () + static_cast<size_t> (nY) * static_cast<size_t> (nWidth) * 4;
         for (int nX = 0; nX < nWidth; nX++)
         {
            const int nU = pRowUv[(nX / 2) * 2 + 0];
            const int nV = pRowUv[(nX / 2) * 2 + 1];
            YuvPixel (pRowY[nX], nU, nV, pDst);
            pDst += 4;
         }
      }
   }
}

void CAPTURE_CONVERT::Yuv420 (const uint8_t* pY, int nStrideY, const uint8_t* pU, int nStrideU, int nPixelU,
                             const uint8_t* pV, int nStrideV, int nPixelV, int nWidth, int nHeight, std::vector<uint8_t>& aRgba)
{
   if (!pY  ||  !pU  ||  !pV  ||  nWidth <= 0  ||  nHeight <= 0)
      aRgba.clear ();
   else
   {
      aRgba.resize (static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4);
      const int nStepU = (nPixelU > 0) ? nPixelU : 1;
      const int nStepV = (nPixelV > 0) ? nPixelV : 1;
      for (int nY = 0; nY < nHeight; nY++)
      {
         const uint8_t* pRowY = pY + static_cast<size_t> (nY)     * static_cast<size_t> (nStrideY);
         const uint8_t* pRowU = pU + static_cast<size_t> (nY / 2) * static_cast<size_t> (nStrideU);
         const uint8_t* pRowV = pV + static_cast<size_t> (nY / 2) * static_cast<size_t> (nStrideV);
         uint8_t* pDst = aRgba.data () + static_cast<size_t> (nY) * static_cast<size_t> (nWidth) * 4;
         for (int nX = 0; nX < nWidth; nX++)
         {
            YuvPixel (pRowY[nX], pRowU[(nX / 2) * nStepU], pRowV[(nX / 2) * nStepV], pDst);
            pDst += 4;
         }
      }
   }
}
