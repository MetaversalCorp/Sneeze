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

#include "ui/Ui_Render.h"
#include "camera/Capture.h"

#include "stb/stb_image.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace SNEEZE::DEP;

namespace
{
   // Signed area of the triangle (a, b, c) -- the 2D cross product of the
   // edge vectors. Positive or negative depending on winding.
   inline float Edge (float ax, float ay, float bx, float by, float cx, float cy)
   {
      return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
   }

   inline uint8_t ClampByte (float dValue)
   {
      const float dClamped = (dValue < 0.0f) ? 0.0f : ((dValue > 255.0f) ? 255.0f : dValue);
      return static_cast<uint8_t> (dClamped + 0.5f);
   }

   // Largest axis-aligned rect of nSrcW x nSrcH that fits in nDstW x nDstH,
   // centered. Live camera imgs are CSS-sized to the feed box (1x1 intrinsic),
   // so sampling must letterbox instead of stretching.
   void ContainRect (int nSrcW, int nSrcH, int nDstW, int nDstH, int& nFitW, int& nFitH, int& nOffX, int& nOffY)
   {
      nFitW = 0;
      nFitH = 0;
      nOffX = 0;
      nOffY = 0;
      if (nSrcW > 0  &&  nSrcH > 0  &&  nDstW > 0  &&  nDstH > 0)
      {
         if (static_cast<long long> (nDstW) * nSrcH <= static_cast<long long> (nDstH) * nSrcW)
         {
            nFitW = nDstW;
            nFitH = static_cast<int> ((static_cast<long long> (nDstW) * nSrcH) / nSrcW);
            if (nFitH < 1)
               nFitH = 1;
            if (nFitH > nDstH)
               nFitH = nDstH;
            nOffY = (nDstH - nFitH) / 2;
         }
         else
         {
            nFitH = nDstH;
            nFitW = static_cast<int> ((static_cast<long long> (nDstH) * nSrcW) / nSrcH);
            if (nFitW < 1)
               nFitW = 1;
            if (nFitW > nDstW)
               nFitW = nDstW;
            nOffX = (nDstW - nFitW) / 2;
         }
      }
   }

   void BlitPremult (const uint8_t* pSrc, int nSrcW, int nSrcH, uint8_t* pDst, int nDstW, int nDstH)
   {
      if (pSrc  &&  pDst  &&  nSrcW > 0  &&  nSrcH > 0  &&  nDstW > 0  &&  nDstH > 0)
      {
         for (int nY = 0; nY < nDstH; nY++)
         {
            const int nSrcY = nY * nSrcH / nDstH;
            for (int nX = 0; nX < nDstW; nX++)
            {
               const int nSrcX = nX * nSrcW / nDstW;
               const uint8_t* pS = pSrc + (static_cast<size_t> (nSrcY) * static_cast<size_t> (nSrcW) + static_cast<size_t> (nSrcX)) * 4;
               uint8_t*       pD = pDst + (static_cast<size_t> (nY)    * static_cast<size_t> (nDstW) + static_cast<size_t> (nX))    * 4;
               const uint32_t nA = pS[3];
               pD[0] = static_cast<uint8_t> (pS[0] * nA / 255);
               pD[1] = static_cast<uint8_t> (pS[1] * nA / 255);
               pD[2] = static_cast<uint8_t> (pS[2] * nA / 255);
               pD[3] = static_cast<uint8_t> (nA);
            }
         }
      }
   }

   bool ParseCameraUrl (const Rml::String& sSource, int& nIndex)
   {
      bool bResult = false;
      nIndex = -1;
      const char*  szPrefix = "camera://";
      const size_t nPrefix  = 9;
      if (sSource.size () > nPrefix  &&  sSource.compare (0, nPrefix, szPrefix) == 0)
      {
         int  nValue = 0;
         bool bDigit = true;
         for (size_t i = nPrefix; i < sSource.size ()  &&  bDigit; i++)
         {
            const char c = sSource[i];
            if (c >= '0'  &&  c <= '9')
               nValue = nValue * 10 + (c - '0');
            else
               bDigit = false;
         }
         if (bDigit)
         {
            nIndex  = nValue;
            bResult = true;
         }
      }
      return bResult;
   }
}

UI_RENDER::UI_RENDER ()
   : m_nWidth (0)
   , m_nHeight (0)
   , m_hGeometryNext (1)
   , m_hTextureNext (1)
   , m_bScissor (false)
   , m_nScissorX (0)
   , m_nScissorY (0)
   , m_nScissorW (0)
   , m_nScissorH (0)
   , m_bClipMask (false)
   , m_eClipWrite (kCLIP_WRITE_COLOR)
   , m_nDrawCount (0)
   , m_nDrawTextured (0)
   , m_pCapture (nullptr)
{
}

UI_RENDER::~UI_RENDER ()
{
}

void UI_RENDER::Resize (int nWidth, int nHeight)
{
   m_nWidth  = (nWidth  > 0) ? nWidth  : 0;
   m_nHeight = (nHeight > 0) ? nHeight : 0;
   const size_t nPixel = static_cast<size_t> (m_nWidth) * static_cast<size_t> (m_nHeight);
   m_aPixel.assign (nPixel * 4, 0);
   m_aClip.assign (nPixel, 0);
   m_aClipTmp.assign (nPixel, 0);
}

void UI_RENDER::Clear ()
{
   std::fill (m_aPixel.begin (), m_aPixel.end (), static_cast<uint8_t> (0));
   std::fill (m_aClip.begin (), m_aClip.end (), static_cast<uint8_t> (0));
   m_nDrawCount    = 0;
   m_nDrawTextured = 0;
   m_bClipMask     = false;
   m_eClipWrite    = kCLIP_WRITE_COLOR;
   for (auto& pair : m_umpTexture)
   {
      if (pair.second.bLive)
      {
         pair.second.nDestX       = 0;
         pair.second.nDestY       = 0;
         pair.second.nDestW       = 0;
         pair.second.nDestH       = 0;
         pair.second.nCoverStride = 0;
         pair.second.aCover.clear ();
      }
   }
}

Rml::CompiledGeometryHandle UI_RENDER::CompileGeometry (Rml::Span<const Rml::Vertex> aVertex, Rml::Span<const int> aIndex)
{
   GEOMETRY geometry;
   geometry.aVertex.assign (aVertex.data (), aVertex.data () + aVertex.size ());
   geometry.aIndex.assign (aIndex.data (), aIndex.data () + aIndex.size ());

   const Rml::CompiledGeometryHandle hGeometry = m_hGeometryNext++;
   m_umpGeometry.emplace (hGeometry, std::move (geometry));
   return hGeometry;
}

void UI_RENDER::RenderGeometry (Rml::CompiledGeometryHandle hGeometry, Rml::Vector2f vTranslation, Rml::TextureHandle hTexture)
{
   auto itGeometry = m_umpGeometry.find (hGeometry);
   if (itGeometry == m_umpGeometry.end ()  ||  m_aPixel.empty ())
      return;

   TEXTURE* pTexture = nullptr;
   if (hTexture != 0)
   {
      auto itTexture = m_umpTexture.find (hTexture);
      if (itTexture != m_umpTexture.end ())
         pTexture = &itTexture->second;
   }

   m_nDrawCount++;
   if (pTexture)
      m_nDrawTextured++;

   const GEOMETRY& geometry = itGeometry->second;
   const size_t nTriangle = geometry.aIndex.size () / 3;
   for (size_t i = 0; i < nTriangle; i++)
   {
      Rml::Vertex v0 = geometry.aVertex[geometry.aIndex[i * 3 + 0]];
      Rml::Vertex v1 = geometry.aVertex[geometry.aIndex[i * 3 + 1]];
      Rml::Vertex v2 = geometry.aVertex[geometry.aIndex[i * 3 + 2]];
      v0.position += vTranslation;
      v1.position += vTranslation;
      v2.position += vTranslation;
      RasterTriangle (v0, v1, v2, pTexture);
   }
}

void UI_RENDER::ReleaseGeometry (Rml::CompiledGeometryHandle hGeometry)
{
   m_umpGeometry.erase (hGeometry);
}

Rml::TextureHandle UI_RENDER::LoadTexture (Rml::Vector2i& vDimensions, const Rml::String& sSource)
{
   Rml::TextureHandle hTexture = Rml::TextureHandle (0);

   int nDevice = -1;
   if (ParseCameraUrl (sSource, nDevice)  &&  m_pCapture)
   {
      // Open may fail until the headset camera permission is granted. The
      // live texture stays registered so a later frame can retry the open.
      m_pCapture->Device_Open (nDevice);
      TEXTURE texture;
      // Report 1x1 so the img does not size the layout to a capture
      // resolution (640x480 overflowed a 512 panel). The staging buffer
      // grows to the real frame on the first sample.
      texture.nWidth  = 1;
      texture.nHeight = 1;
      texture.aPixel.assign (4, 0);
      texture.aPixel[0] = 11;
      texture.aPixel[1] = 13;
      texture.aPixel[2] = 18;
      texture.aPixel[3] = 255;
      texture.bLive   = true;
      texture.nDevice = nDevice;
      vDimensions = Rml::Vector2i (1, 1);
      hTexture = m_hTextureNext++;
      m_umpTexture.emplace (hTexture, std::move (texture));
   }
   else
   {
      int nWidth   = 0;
      int nHeight  = 0;
      int nChannel = 0;
      stbi_uc* pData = stbi_load (sSource.c_str (), &nWidth, &nHeight, &nChannel, 4);
      if (pData)
      {
         TEXTURE texture;
         texture.nWidth  = nWidth;
         texture.nHeight = nHeight;
         texture.aPixel.resize (static_cast<size_t> (nWidth) * nHeight * 4);

         const size_t nPixel = static_cast<size_t> (nWidth) * nHeight;
         for (size_t i = 0; i < nPixel; i++)
         {
            const uint32_t nAlpha = pData[i * 4 + 3];
            texture.aPixel[i * 4 + 0] = static_cast<uint8_t> (pData[i * 4 + 0] * nAlpha / 255);
            texture.aPixel[i * 4 + 1] = static_cast<uint8_t> (pData[i * 4 + 1] * nAlpha / 255);
            texture.aPixel[i * 4 + 2] = static_cast<uint8_t> (pData[i * 4 + 2] * nAlpha / 255);
            texture.aPixel[i * 4 + 3] = static_cast<uint8_t> (nAlpha);
         }
         stbi_image_free (pData);

         vDimensions = Rml::Vector2i (nWidth, nHeight);
         hTexture = m_hTextureNext++;
         m_umpTexture.emplace (hTexture, std::move (texture));
      }
   }

   return hTexture;
}

Rml::TextureHandle UI_RENDER::GenerateTexture (Rml::Span<const Rml::byte> aSource, Rml::Vector2i vDimensions)
{
   TEXTURE texture;
   texture.nWidth  = vDimensions.x;
   texture.nHeight = vDimensions.y;
   texture.aPixel.assign (aSource.data (), aSource.data () + aSource.size ());

   const Rml::TextureHandle hTexture = m_hTextureNext++;
   m_umpTexture.emplace (hTexture, std::move (texture));
   return hTexture;
}

void UI_RENDER::ReleaseTexture (Rml::TextureHandle hTexture)
{
   auto it = m_umpTexture.find (hTexture);
   if (it != m_umpTexture.end ())
   {
      if (it->second.bLive  &&  m_pCapture)
         m_pCapture->Device_Close (it->second.nDevice);
      m_umpTexture.erase (it);
   }
}

bool UI_RENDER::UpdateTexture (Rml::TextureHandle hTexture, Rml::Span<const Rml::byte> aSource, Rml::Vector2i vDimensions)
{
   bool bResult = false;
   auto it = m_umpTexture.find (hTexture);
   if (it != m_umpTexture.end ()  &&  vDimensions.x > 0  &&  vDimensions.y > 0)
   {
      TEXTURE& texture = it->second;
      texture.nWidth  = vDimensions.x;
      texture.nHeight = vDimensions.y;
      texture.aPixel.assign (aSource.data (), aSource.data () + aSource.size ());
      bResult = true;
   }
   return bResult;
}

bool UI_RENDER::LiveTexture_Update ()
{
   bool bDirty = false;

   if (m_pCapture)
   {
      for (auto& pair : m_umpTexture)
      {
         TEXTURE& texture = pair.second;
         if (texture.bLive)
         {
            // Permission may land after the page opens. Retry about once a
            // second; every frame would stall the compositor on the camera thread.
            static int nRetry = 0;
            nRetry++;
            if (texture.nFrameIx == 0  &&  !m_pCapture->Device_IsOpen (texture.nDevice)  &&  (nRetry % 60) == 1)
               m_pCapture->Device_Open (texture.nDevice);

            int      nWidth   = 0;
            int      nHeight  = 0;
            uint64_t nFrameIx = 0;
            std::vector<uint8_t> aRgba;
            if (m_pCapture->Frame_Latest (texture.nDevice, nWidth, nHeight, aRgba, nFrameIx)  &&  nFrameIx != texture.nFrameIx)
            {
               const size_t nNeed = static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4;
               if (nWidth > 0  &&  nHeight > 0  &&  aRgba.size () >= nNeed)
               {
                  if (texture.nWidth != nWidth  ||  texture.nHeight != nHeight  ||  texture.aPixel.size () < nNeed)
                  {
                     texture.nWidth  = nWidth;
                     texture.nHeight = nHeight;
                     texture.aPixel.resize (nNeed);
                  }
                  texture.nFrameIx = nFrameIx;
                  BlitPremult (aRgba.data (), nWidth, nHeight, texture.aPixel.data (), texture.nWidth, texture.nHeight);
                  bDirty = true;
               }
            }
         }
      }
   }

   return bDirty;
}

bool UI_RENDER::LiveTexture_Waiting () const
{
   bool bWaiting = false;

   for (const auto& pair : m_umpTexture)
   {
      if (pair.second.bLive  &&  pair.second.nFrameIx == 0)
         bWaiting = true;
   }

   return bWaiting;
}

int UI_RENDER::LiveDevice () const
{
   int nDevice = -1;

   for (const auto& pair : m_umpTexture)
   {
      if (pair.second.bLive  &&  nDevice < 0)
         nDevice = pair.second.nDevice;
   }

   return nDevice;
}

bool UI_RENDER::LiveTexture_Stamp (uint8_t* pDst, int nDstW, int nDstH) const
{
   bool bStamped = false;

   if (pDst  &&  nDstW > 0  &&  nDstH > 0)
   {
      for (const auto& pair : m_umpTexture)
      {
         const TEXTURE& texture = pair.second;
         if (texture.bLive
          &&  texture.nDestW > 0
          &&  texture.nDestH > 0
          &&  !texture.aPixel.empty ()
          &&  texture.nCoverStride == nDstW
          &&  texture.aCover.size () == static_cast<size_t> (nDstW) * static_cast<size_t> (nDstH)
          &&  texture.nDestX >= 0
          &&  texture.nDestY >= 0
          &&  texture.nDestX + texture.nDestW <= nDstW
          &&  texture.nDestY + texture.nDestH <= nDstH)
         {
            int nFitW = 0, nFitH = 0, nOffX = 0, nOffY = 0;
            ContainRect (texture.nWidth, texture.nHeight, texture.nDestW, texture.nDestH, nFitW, nFitH, nOffX, nOffY);
            for (int nY0 = 0; nY0 < texture.nDestH; nY0++)
            {
               const int nY = texture.nDestY + nY0;
               for (int nX0 = 0; nX0 < texture.nDestW; nX0++)
               {
                  const int nX = texture.nDestX + nX0;
                  if (texture.aCover[static_cast<size_t> (nY) * static_cast<size_t> (texture.nCoverStride) + static_cast<size_t> (nX)] == 0)
                     continue;

                  uint8_t* pD = pDst + (static_cast<size_t> (nY) * static_cast<size_t> (nDstW) + static_cast<size_t> (nX)) * 4;
                  if (pD[3] == 0)
                     continue;

                  const int nFitX = nX0 - nOffX;
                  const int nFitY = nY0 - nOffY;
                  if (nFitW < 1  ||  nFitH < 1  ||  nFitX < 0  ||  nFitY < 0  ||  nFitX >= nFitW  ||  nFitY >= nFitH)
                     continue;

                  const int nSrcX = nFitX * texture.nWidth  / nFitW;
                  const int nSrcY = nFitY * texture.nHeight / nFitH;
                  const uint8_t* pS = texture.aPixel.data () + (static_cast<size_t> (nSrcY) * static_cast<size_t> (texture.nWidth) + static_cast<size_t> (nSrcX)) * 4;
                  pD[0] = pS[0];
                  pD[1] = pS[1];
                  pD[2] = pS[2];
               }
            }
            bStamped = true;
         }
      }
   }

   return bStamped;
}

void UI_RENDER::EnableScissorRegion (bool bEnable)
{
   m_bScissor = bEnable;
}

void UI_RENDER::SetScissorRegion (Rml::Rectanglei rRegion)
{
   m_nScissorX = rRegion.Left ();
   m_nScissorY = rRegion.Top ();
   m_nScissorW = rRegion.Width ();
   m_nScissorH = rRegion.Height ();
}

void UI_RENDER::EnableClipMask (bool bEnable)
{
   m_bClipMask = bEnable;
}

void UI_RENDER::RenderToClipMask (Rml::ClipMaskOperation eOperation, Rml::CompiledGeometryHandle hGeometry, Rml::Vector2f vTranslation)
{
   const bool bClipMask = m_bClipMask;
   m_bClipMask = false;

   if (eOperation == Rml::ClipMaskOperation::Set)
   {
      std::fill (m_aClip.begin (), m_aClip.end (), static_cast<uint8_t> (0));
      m_eClipWrite = kCLIP_WRITE_ONE;
   }
   else if (eOperation == Rml::ClipMaskOperation::SetInverse)
   {
      std::fill (m_aClip.begin (), m_aClip.end (), static_cast<uint8_t> (1));
      m_eClipWrite = kCLIP_WRITE_ZERO;
   }
   else
   {
      std::fill (m_aClipTmp.begin (), m_aClipTmp.end (), static_cast<uint8_t> (0));
      m_eClipWrite = kCLIP_WRITE_INTERSECT;
   }

   RenderGeometry (hGeometry, vTranslation, 0);

   if (m_eClipWrite == kCLIP_WRITE_INTERSECT)
   {
      const size_t nCount = m_aClip.size ();
      for (size_t i = 0; i < nCount; i++)
         m_aClip[i] = (m_aClip[i]  &&  m_aClipTmp[i]) ? 1 : 0;
   }

   m_eClipWrite = kCLIP_WRITE_COLOR;
   m_bClipMask  = bClipMask;
}

void UI_RENDER::RasterTriangle (const Rml::Vertex& vIn0, const Rml::Vertex& vIn1, const Rml::Vertex& vIn2, TEXTURE* pTexture)
{
   Rml::Vertex v0 = vIn0;
   Rml::Vertex v1 = vIn1;
   Rml::Vertex v2 = vIn2;

   float dArea = Edge (v0.position.x, v0.position.y, v1.position.x, v1.position.y, v2.position.x, v2.position.y);
   if (std::fabs (dArea) < 1e-6f)
      return;

   // Normalize to a single winding (positive area) so the top-left fill rule
   // below classifies edges consistently.
   if (dArea < 0.0f)
   {
      std::swap (v1, v2);
      dArea = -dArea;
   }
   const float dInvArea = 1.0f / dArea;

   const float x0 = v0.position.x, y0 = v0.position.y;
   const float x1 = v1.position.x, y1 = v1.position.y;
   const float x2 = v2.position.x, y2 = v2.position.y;

   // Top-left rule: a pixel exactly on a shared edge is rasterized by exactly
   // one of the adjacent triangles. An edge is "top-left" if it points down
   // (screen y grows downward) or is the upper horizontal edge. Top-left edges
   // include their boundary pixels; all others exclude them.
   auto IsTopLeft = [] (float ax, float ay, float bx, float by) -> bool
   {
      return (by > ay)  ||  (ay == by  &&  bx < ax);
   };
   const bool bTopLeft0 = IsTopLeft (x1, y1, x2, y2);
   const bool bTopLeft1 = IsTopLeft (x2, y2, x0, y0);
   const bool bTopLeft2 = IsTopLeft (x0, y0, x1, y1);

   // Clip rectangle: full canvas, optionally narrowed by the active scissor.
   int nClipX0 = 0;
   int nClipY0 = 0;
   int nClipX1 = m_nWidth;
   int nClipY1 = m_nHeight;
   if (m_bScissor)
   {
      nClipX0 = std::max (nClipX0, m_nScissorX);
      nClipY0 = std::max (nClipY0, m_nScissorY);
      nClipX1 = std::min (nClipX1, m_nScissorX + m_nScissorW);
      nClipY1 = std::min (nClipY1, m_nScissorY + m_nScissorH);
   }

   int nMinX = static_cast<int> (std::floor (std::min ({ x0, x1, x2 })));
   int nMaxX = static_cast<int> (std::ceil  (std::max ({ x0, x1, x2 })));
   int nMinY = static_cast<int> (std::floor (std::min ({ y0, y1, y2 })));
   int nMaxY = static_cast<int> (std::ceil  (std::max ({ y0, y1, y2 })));
   nMinX = std::max (nMinX, nClipX0);
   nMinY = std::max (nMinY, nClipY0);
   nMaxX = std::min (nMaxX, nClipX1);
   nMaxY = std::min (nMaxY, nClipY1);

   const float dQuadW = std::max ({ x0, x1, x2 }) - std::min ({ x0, x1, x2 });
   const float dQuadH = std::max ({ y0, y1, y2 }) - std::min ({ y0, y1, y2 });

   for (int py = nMinY; py < nMaxY; py++)
   {
      for (int px = nMinX; px < nMaxX; px++)
      {
         const float dPx = px + 0.5f;
         const float dPy = py + 0.5f;

         const float w0 = Edge (x1, y1, x2, y2, dPx, dPy) * dInvArea;
         const float w1 = Edge (x2, y2, x0, y0, dPx, dPy) * dInvArea;
         const float w2 = Edge (x0, y0, x1, y1, dPx, dPy) * dInvArea;
         const bool bIn0 = bTopLeft0 ? (w0 >= 0.0f) : (w0 > 0.0f);
         const bool bIn1 = bTopLeft1 ? (w1 >= 0.0f) : (w1 > 0.0f);
         const bool bIn2 = bTopLeft2 ? (w2 >= 0.0f) : (w2 > 0.0f);
         if (!(bIn0  &&  bIn1  &&  bIn2))
            continue;

         const size_t nCover = static_cast<size_t> (py) * static_cast<size_t> (m_nWidth) + static_cast<size_t> (px);

         if (m_eClipWrite != kCLIP_WRITE_COLOR)
         {
            if (m_eClipWrite == kCLIP_WRITE_ONE)
               m_aClip[nCover] = 1;
            else if (m_eClipWrite == kCLIP_WRITE_ZERO)
               m_aClip[nCover] = 0;
            else
               m_aClipTmp[nCover] = 1;
            continue;
         }

         if (m_bClipMask  &&  (nCover >= m_aClip.size ()  ||  m_aClip[nCover] == 0))
            continue;

         float dR = w0 * v0.colour.red   + w1 * v1.colour.red   + w2 * v2.colour.red;
         float dG = w0 * v0.colour.green + w1 * v1.colour.green + w2 * v2.colour.green;
         float dB = w0 * v0.colour.blue  + w1 * v1.colour.blue  + w2 * v2.colour.blue;
         float dA = w0 * v0.colour.alpha + w1 * v1.colour.alpha + w2 * v2.colour.alpha;

         if (pTexture  &&  pTexture->nWidth > 0  &&  pTexture->nHeight > 0)
         {
            const float u = w0 * v0.tex_coord.x + w1 * v1.tex_coord.x + w2 * v2.tex_coord.x;
            const float v = w0 * v0.tex_coord.y + w1 * v1.tex_coord.y + w2 * v2.tex_coord.y;
            float uSrc = u;
            float vSrc = v;

            if (pTexture->bLive  &&  pTexture->nWidth > 1  &&  pTexture->nHeight > 1  &&  dQuadW > 0.5f  &&  dQuadH > 0.5f)
            {
               const float dScale = std::min (dQuadW / static_cast<float> (pTexture->nWidth), dQuadH / static_cast<float> (pTexture->nHeight));
               const float dFitW  = static_cast<float> (pTexture->nWidth)  * dScale;
               const float dFitH  = static_cast<float> (pTexture->nHeight) * dScale;
               const float dU0    = 0.5f * (1.0f - dFitW / dQuadW);
               const float dV0    = 0.5f * (1.0f - dFitH / dQuadH);
               const float dUW    = dFitW / dQuadW;
               const float dVH    = dFitH / dQuadH;
               if (dUW <= 0.0f  ||  dVH <= 0.0f  ||  u < dU0  ||  v < dV0  ||  u >= dU0 + dUW  ||  v >= dV0 + dVH)
                  continue;
               uSrc = (u - dU0) / dUW;
               vSrc = (v - dV0) / dVH;
            }

            int tx = static_cast<int> (uSrc * pTexture->nWidth);
            int ty = static_cast<int> (vSrc * pTexture->nHeight);
            tx = std::max (0, std::min (pTexture->nWidth  - 1, tx));
            ty = std::max (0, std::min (pTexture->nHeight - 1, ty));

            const size_t nTexel = (static_cast<size_t> (ty) * pTexture->nWidth + tx) * 4;
            // Modulate premultiplied texel by the premultiplied vertex colour.
            dR = pTexture->aPixel[nTexel + 0] * (dR / 255.0f);
            dG = pTexture->aPixel[nTexel + 1] * (dG / 255.0f);
            dB = pTexture->aPixel[nTexel + 2] * (dB / 255.0f);
            dA = pTexture->aPixel[nTexel + 3] * (dA / 255.0f);
         }

         const float dSrcA = dA;
         if (dSrcA <= 0.0f)
            continue;

         const float dInvSrcA = (255.0f - dSrcA) / 255.0f;
         const size_t nDst = nCover * 4;
         m_aPixel[nDst + 0] = ClampByte (dR + m_aPixel[nDst + 0] * dInvSrcA);
         m_aPixel[nDst + 1] = ClampByte (dG + m_aPixel[nDst + 1] * dInvSrcA);
         m_aPixel[nDst + 2] = ClampByte (dB + m_aPixel[nDst + 2] * dInvSrcA);
         m_aPixel[nDst + 3] = ClampByte (dA + m_aPixel[nDst + 3] * dInvSrcA);

         if (pTexture  &&  pTexture->bLive)
         {
            const size_t nCoverNeed = static_cast<size_t> (m_nWidth) * static_cast<size_t> (m_nHeight);
            if (pTexture->aCover.size () != nCoverNeed  ||  pTexture->nCoverStride != m_nWidth)
            {
               pTexture->aCover.assign (nCoverNeed, 0);
               pTexture->nCoverStride = m_nWidth;
            }
            pTexture->aCover[nCover] = 1;
            if (pTexture->nDestW <= 0  ||  pTexture->nDestH <= 0)
            {
               pTexture->nDestX = px;
               pTexture->nDestY = py;
               pTexture->nDestW = 1;
               pTexture->nDestH = 1;
            }
            else
            {
               const int nRight  = std::max (pTexture->nDestX + pTexture->nDestW, px + 1);
               const int nBottom = std::max (pTexture->nDestY + pTexture->nDestH, py + 1);
               pTexture->nDestX  = std::min (pTexture->nDestX, px);
               pTexture->nDestY  = std::min (pTexture->nDestY, py);
               pTexture->nDestW  = nRight  - pTexture->nDestX;
               pTexture->nDestH  = nBottom - pTexture->nDestY;
            }
         }
      }
   }
}
