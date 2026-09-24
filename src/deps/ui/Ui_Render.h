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

#ifndef SNEEZE_UI_RENDER_H
#define SNEEZE_UI_RENDER_H

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SNEEZE
{
   namespace DEP
   {
      class CAPTURE;

      // Software RmlUi render interface. Rasterizes RmlUi's 2D geometry into a
      // CPU RGBA8 canvas (premultiplied alpha, row-major, top-down origin) that
      // is then handed to ANARI as an image2D sampler for an unlit quad. Keeps
      // the UI path fully portable and free of any GPU coupling. Implements the
      // required entry points plus a CPU stencil for clip masks (overflow +
      // border-radius). Layer / filter / shader hooks keep their base no-op
      // defaults.
      class UI_RENDER : public Rml::RenderInterface
      {
      public:
         UI_RENDER ();
         ~UI_RENDER () override;

         void Resize (int nWidth, int nHeight);
         void Clear ();

         int            Width  () const { return m_nWidth; }
         int            Height () const { return m_nHeight; }
         const uint8_t* Pixels () const { return m_aPixel.data (); }

         // Diagnostics (reset on Clear): how many geometry batches the last
         // pass drew, and how many of those carried a texture (text/atlas).
         int DrawCount    () const { return m_nDrawCount; }
         int DrawTextured () const { return m_nDrawTextured; }

         // Live camera textures. Capture () is set by UI_CONTEXT once the
         // engine CAPTURE singleton exists. LoadTexture ("camera://N") opens
         // device N; LiveTexture_Update copies new frames into those textures.
         void Capture (CAPTURE* pCapture) { m_pCapture = pCapture; }
         bool UpdateTexture (Rml::TextureHandle hTexture, Rml::Span<const Rml::byte> aSource, Rml::Vector2i vDimensions);
         bool LiveTexture_Update ();
         bool LiveTexture_Waiting () const;
         bool LiveTexture_Stamp (uint8_t* pDst, int nDstW, int nDstH) const;

         Rml::CompiledGeometryHandle CompileGeometry (Rml::Span<const Rml::Vertex> aVertex, Rml::Span<const int> aIndex) override;
         void                        RenderGeometry (Rml::CompiledGeometryHandle hGeometry, Rml::Vector2f vTranslation, Rml::TextureHandle hTexture) override;
         void                        ReleaseGeometry (Rml::CompiledGeometryHandle hGeometry) override;

         Rml::TextureHandle LoadTexture (Rml::Vector2i& vDimensions, const Rml::String& sSource) override;
         Rml::TextureHandle GenerateTexture (Rml::Span<const Rml::byte> aSource, Rml::Vector2i vDimensions) override;
         void               ReleaseTexture (Rml::TextureHandle hTexture) override;

         void EnableScissorRegion (bool bEnable) override;
         void SetScissorRegion (Rml::Rectanglei rRegion) override;

         void EnableClipMask (bool bEnable) override;
         void RenderToClipMask (Rml::ClipMaskOperation eOperation, Rml::CompiledGeometryHandle hGeometry, Rml::Vector2f vTranslation) override;

      private:
         struct GEOMETRY
         {
            std::vector<Rml::Vertex> aVertex;
            std::vector<int>         aIndex;
         };

         struct TEXTURE
         {
            int                  nWidth   = 0;
            int                  nHeight  = 0;
            std::vector<uint8_t> aPixel;
            bool                 bLive    = false;
            int                  nDevice  = -1;
            uint64_t             nFrameIx = 0;
            int                  nDestX       = 0;
            int                  nDestY       = 0;
            int                  nDestW       = 0;
            int                  nDestH       = 0;
            int                  nCoverStride = 0;
            std::vector<uint8_t> aCover;
         };

         enum eCLIP_WRITE
         {
            kCLIP_WRITE_COLOR = 0,
            kCLIP_WRITE_ONE,
            kCLIP_WRITE_ZERO,
            kCLIP_WRITE_INTERSECT,
         };

         void RasterTriangle (const Rml::Vertex& v0, const Rml::Vertex& v1, const Rml::Vertex& v2, TEXTURE* pTexture);

         int                  m_nWidth;
         int                  m_nHeight;
         std::vector<uint8_t> m_aPixel;
         std::vector<uint8_t> m_aClip;
         std::vector<uint8_t> m_aClipTmp;

         std::unordered_map<Rml::CompiledGeometryHandle, GEOMETRY> m_umpGeometry;
         std::unordered_map<Rml::TextureHandle, TEXTURE>          m_umpTexture;
         Rml::CompiledGeometryHandle                              m_hGeometryNext;
         Rml::TextureHandle                                       m_hTextureNext;

         bool         m_bScissor;
         int          m_nScissorX;
         int          m_nScissorY;
         int          m_nScissorW;
         int          m_nScissorH;
         bool         m_bClipMask;
         eCLIP_WRITE  m_eClipWrite;

         int  m_nDrawCount;
         int  m_nDrawTextured;

         CAPTURE* m_pCapture;
      };
   } // namespace DEP
}
#endif // SNEEZE_UI_RENDER_H
