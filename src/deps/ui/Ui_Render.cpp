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

   void BlitOpaque (const uint8_t* pSrc, int nSrcW, int nSrcH, uint8_t* pDst, int nDstStrideW, int nX, int nY, int nW, int nH)
   {
      if (pSrc  &&  pDst  &&  nSrcW > 0  &&  nSrcH > 0  &&  nW > 0  &&  nH > 0)
      {
         for (int nY0 = 0; nY0 < nH; nY0++)
         {
            const int nSrcY = nY0 * nSrcH / nH;
            for (int nX0 = 0; nX0 < nW; nX0++)
            {
               const int nSrcX = nX0 * nSrcW / nW;
               const uint8_t* pS = pSrc + (static_cast<size_t> (nSrcY) * static_cast<size_t> (nSrcW) + static_cast<size_t> (nSrcX)) * 4;
               uint8_t*       pD = pDst + (static_cast<size_t> (nY + nY0) * static_cast<size_t> (nDstStrideW) + static_cast<size_t> (nX + nX0)) * 4;
               pD[0] = pS[0];
               pD[1] = pS[1];
               pD[2] = pS[2];
               pD[3] = 255;
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
   m_aPixel.assign (static_cast<size_t> (m_nWidth) * m_nHeight * 4, 0);
}

void UI_RENDER::Clear ()
{
   std::fill (m_aPixel.begin (), m_aPixel.end (), static_cast<uint8_t> (0));
   m_nDrawCount    = 0;
   m_nDrawTextured = 0;
   for (auto& pair : m_umpTexture)
   {
      if (pair.second.bLive)
      {
         pair.second.nDestX = 0;
         pair.second.nDestY = 0;
         pair.second.nDestW = 0;
         pair.second.nDestH = 0;
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

      if (pTexture  &&  pTexture->bLive)
      {
         const int nMinX = static_cast<int> (std::floor (std::min ({ v0.position.x, v1.position.x, v2.position.x })));
         const int nMinY = static_cast<int> (std::floor (std::min ({ v0.position.y, v1.position.y, v2.position.y })));
         const int nMaxX = static_cast<int> (std::ceil  (std::max ({ v0.position.x, v1.position.x, v2.position.x })));
         const int nMaxY = static_cast<int> (std::ceil  (std::max ({ v0.position.y, v1.position.y, v2.position.y })));
         const int nX0 = std::max (0, nMinX);
         const int nY0 = std::max (0, nMinY);
         const int nX1 = std::min (m_nWidth, nMaxX);
         const int nY1 = std::min (m_nHeight, nMaxY);
         if (nX1 > nX0  &&  nY1 > nY0)
         {
            if (pTexture->nDestW <= 0  ||  pTexture->nDestH <= 0)
            {
               pTexture->nDestX = nX0;
               pTexture->nDestY = nY0;
               pTexture->nDestW = nX1 - nX0;
               pTexture->nDestH = nY1 - nY0;
            }
            else
            {
               const int nRight  = std::max (pTexture->nDestX + pTexture->nDestW, nX1);
               const int nBottom = std::max (pTexture->nDestY + pTexture->nDestH, nY1);
               pTexture->nDestX  = std::min (pTexture->nDestX, nX0);
               pTexture->nDestY  = std::min (pTexture->nDestY, nY0);
               pTexture->nDestW  = nRight  - pTexture->nDestX;
               pTexture->nDestH  = nBottom - pTexture->nDestY;
            }
         }
      }
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
   if (ParseCameraUrl (sSource, nDevice)  &&  m_pCapture  &&  m_pCapture->Device_Open (nDevice))
   {
      TEXTURE texture;
      texture.nWidth  = 640;
      texture.nHeight = 480;
      texture.aPixel.resize (static_cast<size_t> (texture.nWidth) * texture.nHeight * 4);
      for (size_t i = 0; i < texture.aPixel.size (); i += 4)
      {
         texture.aPixel[i + 0] = 11;
         texture.aPixel[i + 1] = 13;
         texture.aPixel[i + 2] = 18;
         texture.aPixel[i + 3] = 255;
      }
      texture.bLive   = true;
      texture.nDevice = nDevice;
      vDimensions = Rml::Vector2i (texture.nWidth, texture.nHeight);
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
            int      nWidth   = 0;
            int      nHeight  = 0;
            uint64_t nFrameIx = 0;
            std::vector<uint8_t> aRgba;
            if (m_pCapture->Frame_Latest (texture.nDevice, nWidth, nHeight, aRgba, nFrameIx)  &&  nFrameIx != texture.nFrameIx)
            {
               const size_t nNeed = static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4;
               if (nWidth > 0  &&  nHeight > 0  &&  aRgba.size () >= nNeed  &&  !texture.aPixel.empty ())
               {
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
          &&  texture.nDestX >= 0
          &&  texture.nDestY >= 0
          &&  texture.nDestX + texture.nDestW <= nDstW
          &&  texture.nDestY + texture.nDestH <= nDstH)
         {
            BlitOpaque (texture.aPixel.data (), texture.nWidth, texture.nHeight, pDst, nDstW, texture.nDestX, texture.nDestY, texture.nDestW, texture.nDestH);
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

void UI_RENDER::RasterTriangle (const Rml::Vertex& vIn0, const Rml::Vertex& vIn1, const Rml::Vertex& vIn2, const TEXTURE* pTexture)
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

         float dR = w0 * v0.colour.red   + w1 * v1.colour.red   + w2 * v2.colour.red;
         float dG = w0 * v0.colour.green + w1 * v1.colour.green + w2 * v2.colour.green;
         float dB = w0 * v0.colour.blue  + w1 * v1.colour.blue  + w2 * v2.colour.blue;
         float dA = w0 * v0.colour.alpha + w1 * v1.colour.alpha + w2 * v2.colour.alpha;

         if (pTexture  &&  pTexture->nWidth > 0  &&  pTexture->nHeight > 0)
         {
            const float u = w0 * v0.tex_coord.x + w1 * v1.tex_coord.x + w2 * v2.tex_coord.x;
            const float v = w0 * v0.tex_coord.y + w1 * v1.tex_coord.y + w2 * v2.tex_coord.y;

            int tx = static_cast<int> (u * pTexture->nWidth);
            int ty = static_cast<int> (v * pTexture->nHeight);
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
         const size_t nDst = (static_cast<size_t> (py) * m_nWidth + px) * 4;
         m_aPixel[nDst + 0] = ClampByte (dR + m_aPixel[nDst + 0] * dInvSrcA);
         m_aPixel[nDst + 1] = ClampByte (dG + m_aPixel[nDst + 1] * dInvSrcA);
         m_aPixel[nDst + 2] = ClampByte (dB + m_aPixel[nDst + 2] * dInvSrcA);
         m_aPixel[nDst + 3] = ClampByte (dA + m_aPixel[nDst + 3] * dInvSrcA);
      }
   }
}
