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

#ifndef SNEEZE_UI_PANEL_H
#define SNEEZE_UI_PANEL_H

#include <cstdint>
#include <string>
#include <vector>

namespace Rml
{
   class Context;
   class ElementDocument;
}

namespace SNEEZE
{
   class ENGINE;

   namespace DEP
   {
      class UI_RENDER;

      // One in-scene UI panel: its own RmlUi context and document, rasterized
      // into a straight-alpha pixel buffer it owns. The owning MAP_OBJECT_PANEL
      // holds one of these; a scene may hold many. Global RmlUi lifecycle (init,
      // fonts) and the single shared render interface live in UI_CONTEXT; each
      // panel drives its own document through that shared interface (panels
      // render sequentially on the compositor thread) and copies the result out.
      class UI_PANEL
      {
      public:
         UI_PANEL ();
         ~UI_PANEL ();

         // Set the RML+CSS document source. Marks the canvas dirty so the next
         // Render rasterizes. If never called, a built-in default document is
         // used (the current test panel).
         void Source (const std::string& sSource);

         // Rasterize the document into the canvas at the given size. Lazily
         // creates the RmlUi context/document on the first call. Must be called
         // on the render (compositor) thread. Re-rasterizes only when dirty, so
         // calling every frame is cheap. Returns true if Pixels() is valid.
         bool Render (ENGINE* pEngine, int nWidth, int nHeight);

         int            Width  () const { return m_nWidth; }
         int            Height () const { return m_nHeight; }
         uint32_t       Serial () const { return m_nSerial; }
         const uint8_t* Pixels () const;   // straight-alpha RGBA8, row-major, top-down

      private:
         bool EnsureDocument ();
         void Straighten (UI_RENDER* pUi_Render);   // premultiplied canvas -> straight-alpha output

         ENGINE*               m_pEngine;
         Rml::Context*         m_pRmlContext;
         Rml::ElementDocument* m_pDocument;
         std::string           m_sSource;
         std::string           m_sName;
         std::vector<uint8_t>  m_aStraight;
         int                   m_nWidth;
         int                   m_nHeight;
         uint32_t              m_nSerial;
         int                   m_nWaitLive;
         bool                  m_bDirty;
      };
   } // namespace DEP
}
#endif // SNEEZE_UI_PANEL_H
