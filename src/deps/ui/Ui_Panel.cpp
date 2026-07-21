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

#include "ui/Ui_Panel.h"
#include "ui/Ui_Context.h"
#include "ui/Ui_Render.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <atomic>

using namespace SNEEZE::DEP;

namespace
{
   std::atomic<int> s_nPanelSeq { 0 };

   // Default panel document: a translucent, bordered panel with Inter text and
   // a glow on the heading -- proves real RML+CSS (background alpha, borders,
   // font effects) rasterized and composited over the 3D scene. RmlUi has no
   // HTML user-agent stylesheet, so block-level display must be set explicitly
   // (elements default to inline). Separators use the literal UTF-8 middot
   // (\xC2\xB7); RML does not decode named entities like &middot;.
   const char* const szDEFAULT_DOC =
      "<rml>"
      "<head><style>"
      "body { width: 100%; height: 100%; font-family: Inter; font-weight: normal; color: #e9eef6; }"
      "#panel {"
      "   position: absolute; left: 7%; top: 7%; width: 86%; height: 86%;"
      "   padding: 26px 26px;"
      "   background-color: rgba(22, 25, 30, 224);"
      "   border-width: 1px; border-color: rgba(255, 255, 255, 30);"
      "   border-radius: 18px;"
      "}"
      ".hd { display: flex; align-items: center; justify-content: space-between; margin: 0 0 20px 0; }"
      ".title { display: inline-block; font-size: 19px; font-weight: 600; color: #f2f5f9; }"
      ".x { display: inline-block; font-size: 17px; color: #8b94a3; }"
      ".row { display: flex; align-items: center; justify-content: space-between; padding: 9px 0; }"
      ".label { display: inline-block; font-size: 15px; color: #c4ccd8; }"
      ".dd { display: inline-block; font-size: 13px; color: #d2d9e4;"
      "      background-color: rgba(255, 255, 255, 16); border-radius: 7px; padding: 5px 11px; }"
      ".toggle { display: inline-block; position: relative; width: 38px; height: 20px;"
      "          border-radius: 10px; background-color: #ff8a3d; }"
      ".knob { position: absolute; top: 3px; left: 21px; width: 14px; height: 14px;"
      "        border-radius: 7px; background-color: #ffffff; }"
      ".btn { display: block; margin: 20px 0 0 0; padding: 12px 0; text-align: center;"
      "       color: #dbe2ec; font-size: 14px; font-weight: 500;"
      "       border-width: 1px; border-color: rgba(255, 255, 255, 40); border-radius: 11px;"
      "       background-color: rgba(255, 255, 255, 8); }"
      "</style></head>"
      "<body>"
      "<div id='panel'>"
      "<div class='hd'><span class='title'>Environment</span><span class='x'>\xC3\x97</span></div>"
      "<div class='row'><span class='label'>Scene</span><span class='dd'>Terminal A \xE2\x96\xBE</span></div>"
      "<div class='row'><span class='label'>Lighting</span><span class='dd'>Evening \xE2\x96\xBE</span></div>"
      "<div class='row'><span class='label'>Crowd Level</span><span class='dd'>Moderate \xE2\x96\xBE</span></div>"
      "<div class='row'><span class='label'>Ambient Audio</span><span class='toggle'><span class='knob'></span></span></div>"
      "<div class='row'><span class='label'>Live Data</span><span class='toggle'><span class='knob'></span></span></div>"
      "<div class='btn'>Reset to Default</div>"
      "</div>"
      "</body>"
      "</rml>";
} // anonymous namespace

UI_PANEL::UI_PANEL ()
   : m_pEngine (nullptr)
   , m_pRmlContext (nullptr)
   , m_pDocument (nullptr)
   , m_sSource (szDEFAULT_DOC)
   , m_sName ("panel" + std::to_string (s_nPanelSeq.fetch_add (1)))
   , m_nWidth (0)
   , m_nHeight (0)
   , m_bDirty (true)
{
}

UI_PANEL::~UI_PANEL ()
{
   if (m_pDocument)
   {
      m_pDocument->Close ();
      m_pDocument = nullptr;
   }
   // Remove only this panel's context. The render interface is shared and owned
   // by UI_CONTEXT, so it is not touched here -- that single interface must stay
   // alive for the whole engine so RmlUi never releases a render manager for a
   // freed interface (which is what the host's ReleaseRenderManagers would hit).
   if (m_pRmlContext)
   {
      Rml::RemoveContext (m_sName);
      m_pRmlContext = nullptr;
   }
}

void UI_PANEL::Source (const std::string& sSource)
{
   m_sSource = sSource;
   if (m_pDocument)
   {
      m_pDocument->Close ();
      m_pDocument = nullptr;
   }
   m_bDirty = true;
}

bool UI_PANEL::EnsureDocument ()
{
   bool bResult = (m_pDocument != nullptr);

   if (!bResult  &&  m_pRmlContext)
   {
      m_pDocument = m_pRmlContext->LoadDocumentFromMemory (m_sSource);
      if (m_pDocument)
      {
         m_pDocument->Show ();
         bResult = true;
      }
      else if (m_pEngine)
      {
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "UI_PANEL", "LoadDocumentFromMemory failed");
      }
   }

   return bResult;
}

bool UI_PANEL::Render (ENGINE* pEngine, int nWidth, int nHeight)
{
   bool bResult = false;

   m_pEngine = pEngine;

   UI_CONTEXT* pUi_Context = pEngine ? pEngine->Ui_Context () : nullptr;
   UI_RENDER*  pUi_Render  = pUi_Context ? pUi_Context->Render () : nullptr;

   if (pUi_Context  &&  pUi_Render  &&  nWidth > 0  &&  nHeight > 0)
   {
      // Bind this panel's context to the one shared render interface. Many
      // contexts on one interface is the normal RmlUi arrangement; panels render
      // one at a time on the compositor thread, so the interface's single canvas
      // is reused as scratch and the result is copied out per panel.
      if (!m_pRmlContext)
      {
         m_pRmlContext = Rml::CreateContext (m_sName, Rml::Vector2i (nWidth, nHeight), pUi_Render);
         if (m_pRmlContext)
            m_bDirty = true;
      }
      else if (m_nWidth != nWidth  ||  m_nHeight != nHeight)
      {
         m_pRmlContext->SetDimensions (Rml::Vector2i (nWidth, nHeight));
         m_bDirty = true;
      }

      if (m_pRmlContext  &&  EnsureDocument ())
      {
         if (m_bDirty)
         {
            // The shared canvas may have been left at another panel's size, so
            // size it to this panel before rendering.
            pUi_Render->Resize (nWidth, nHeight);
            m_pRmlContext->Update ();
            pUi_Render->Clear ();
            m_pRmlContext->Render ();
            Straighten (pUi_Render);
            m_bDirty = false;
         }
         bResult = !m_aStraight.empty ();
      }
   }

   return bResult;
}

void UI_PANEL::Straighten (UI_RENDER* pUi_Render)
{
   // The UI canvas is premultiplied alpha; the renderer's unlit "blend" material
   // expects straight alpha, so convert here -- the panel owns the UI-format
   // knowledge so the renderer stays UI-agnostic.
   m_nWidth  = pUi_Render->Width ();
   m_nHeight = pUi_Render->Height ();

   const uint8_t* pSrc  = pUi_Render->Pixels ();
   const size_t   nPixel = static_cast<size_t> (m_nWidth) * m_nHeight;

   m_aStraight.resize (nPixel * 4);

   for (size_t i = 0; i < nPixel; i++)
   {
      const uint32_t nA = pSrc[i * 4 + 3];
      if (nA == 0)
      {
         m_aStraight[i * 4 + 0] = 0;
         m_aStraight[i * 4 + 1] = 0;
         m_aStraight[i * 4 + 2] = 0;
         m_aStraight[i * 4 + 3] = 0;
      }
      else
      {
         m_aStraight[i * 4 + 0] = static_cast<uint8_t> (std::min<uint32_t> (255, pSrc[i * 4 + 0] * 255 / nA));
         m_aStraight[i * 4 + 1] = static_cast<uint8_t> (std::min<uint32_t> (255, pSrc[i * 4 + 1] * 255 / nA));
         m_aStraight[i * 4 + 2] = static_cast<uint8_t> (std::min<uint32_t> (255, pSrc[i * 4 + 2] * 255 / nA));
         m_aStraight[i * 4 + 3] = static_cast<uint8_t> (nA);
      }
   }
}

const uint8_t* UI_PANEL::Pixels () const
{
   return m_aStraight.empty () ? nullptr : m_aStraight.data ();
}
