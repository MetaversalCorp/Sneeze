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
#include <RmlUi/Core/EventListener.h>

#include <algorithm>
#include <atomic>

using namespace SNEEZE::DEP;

namespace
{
   std::atomic<int> s_nPanelSeq { 0 };

   // Raster generation, engine-wide rather than per panel. The renderer decides
   // whether a panel's pixels are the ones it already uploaded by comparing this
   // value, and it finds a panel by the address of its pixel buffer -- an address
   // the allocator hands out again once a panel is destroyed. A per-panel counter
   // made every freshly built panel claim the same generation, so a reused address
   // carrying a different document looked like an upload already done and the
   // stale image stayed on screen. One counter for all panels means no two rasters
   // anywhere ever share a generation.
   std::atomic<uint32_t> s_nSerialSeq { 0 };

   // Default panel document: a translucent, bordered panel with Inter text and
   // a glow on the heading -- proves real RML+CSS (background alpha, borders,
   // font effects) rasterized and composited over the 3D scene. RmlUi has no
   // HTML user-agent stylesheet, so block-level display must be set explicitly
   // (elements default to inline). Separators use the literal UTF-8 middot
   // (\xC2\xB7); RML does not decode named entities like &middot;.
   const char* const szDEFAULT_DOC =
      "<rml>"
      "<head><style>"
      "body { display: block; width: 100%; height: 100%; font-family: Inter; font-weight: normal; color: #e9eef6; }"
      "#panel {"
      "   display: block; position: absolute; left: 7%; top: 7%; width: 86%; height: 86%;"
      "   box-sizing: border-box;"
      "   padding: 26px 26px;"
      "   overflow: hidden;"
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

class SNEEZE::DEP::UI_PANEL::CLICK : public Rml::EventListener
{
public:
   explicit CLICK (std::string* pId) : m_pId (pId) {}

   void ProcessEvent (Rml::Event& Event) override
   {
      if (!m_pId)
         return;

      Rml::Element* pElement = Event.GetTargetElement ();

      while (pElement)
      {
         const Rml::String& sId = pElement->GetId ();

         if (!sId.empty ())
         {
            *m_pId = sId;
            return;
         }

         pElement = pElement->GetParentNode ();
      }
   }

private:
   std::string* m_pId;
};

UI_PANEL::UI_PANEL ()
   : m_pEngine (nullptr)
   , m_pRmlContext (nullptr)
   , m_pDocument (nullptr)
   , m_pClick (nullptr)
   , m_sSource (szDEFAULT_DOC)
   , m_sName ("panel" + std::to_string (s_nPanelSeq.fetch_add (1)))
   , m_nWidth (0)
   , m_nHeight (0)
   , m_dDensity (0.0f)
   , m_nSerial (0)
   , m_nWaitLive (0)
   , m_bDirty (true)
   , m_bOver (false)
   , m_bListening (false)
{
   m_pClick = new CLICK (&m_sClick);
}

UI_PANEL::~UI_PANEL ()
{
   SNEEZE::ENGINE::UI_GUARD uiGuard;   // serialize process-global RmlUi access

   DetachClick ();

   if (m_pDocument)
   {
      m_pDocument->Close ();
      m_pDocument = nullptr;
   }

   delete m_pClick;
   m_pClick = nullptr;
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
   // Close() mutates the process-global RmlUi tree. The compositor renders
   // that tree under the same lock; doing it here without the lock aborts
   // the host (the failure a live RML rewrite used to hit).
   SNEEZE::ENGINE::UI_GUARD uiGuard;

   m_sSource = sSource;
   DetachClick ();
   if (m_pDocument)
   {
      m_pDocument->Close ();
      m_pDocument = nullptr;
   }
   m_bOver = false;
   m_bDirty = true;
}

void UI_PANEL::DetachClick ()
{
   if (m_pDocument  &&  m_pClick  &&  m_bListening)
   {
      m_pDocument->RemoveEventListener (Rml::EventId::Click, m_pClick, false);
      m_bListening = false;
   }
}

std::string UI_PANEL::Pointer (int nX, int nY, bool bDown, bool bUp)
{
   SNEEZE::ENGINE::UI_GUARD uiGuard;

   std::string sClick;
   m_sClick.clear ();

   if (m_pRmlContext  &&  m_pDocument)
   {
      if (nX < 0  ||  nY < 0)
      {
         if (m_bOver)
         {
            m_pRmlContext->ProcessMouseLeave ();
            m_bOver = false;
            m_bDirty = true;
         }

         if (bUp)
         {
            m_pRmlContext->ProcessMouseButtonUp (0, 0);
            m_bDirty = true;
         }
      }
      else
      {
         bool bMiss = m_pRmlContext->ProcessMouseMove (nX, nY, 0);
         bool bOver = !bMiss;

         if (bOver != m_bOver)
         {
            m_bOver = bOver;
            m_bDirty = true;
         }

         if (bDown)
         {
            m_pRmlContext->ProcessMouseButtonDown (0, 0);
            m_bDirty = true;
         }

         if (bUp)
         {
            m_pRmlContext->ProcessMouseButtonUp (0, 0);
            m_bDirty = true;
            sClick = m_sClick;
         }
      }
   }

   return sClick;
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

         if (m_pClick  &&  !m_bListening)
         {
            m_pDocument->AddEventListener (Rml::EventId::Click, m_pClick, false);
            m_bListening = true;
         }

         bResult = true;
      }
      else if (m_pEngine)
      {
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "UI_PANEL", "LoadDocumentFromMemory failed");
      }
   }

   return bResult;
}

bool UI_PANEL::Render (ENGINE* pEngine, int nWidth, int nHeight, float dDensity)
{
   SNEEZE::ENGINE::UI_GUARD uiGuard;   // serialize process-global RmlUi access

   bool bResult = false;

   m_pEngine = pEngine;

   if (dDensity < 1.0f)
      dDensity = 1.0f;

   UI_CONTEXT* pUi_Context = pEngine ? pEngine->Ui_Context () : nullptr;
   UI_RENDER*  pUi_Render  = pUi_Context ? pUi_Context->Render () : nullptr;

   if (pUi_Context  &&  pUi_Render  &&  nWidth > 0  &&  nHeight > 0)
   {
      // Bind this panel's context to the one shared render interface. Many
      // contexts on one interface is the normal RmlUi arrangement; panels render
      // one at a time on the compositor thread, so the interface's single canvas
      // is reused as scratch and the result is copied out per panel.
      // dDensity scales the 'dp' unit. CSS 'px' stays one texture pixel, so a
      // document written in dp keeps its layout when the raster is supersampled.
      if (!m_pRmlContext)
      {
         m_pRmlContext = Rml::CreateContext (m_sName, Rml::Vector2i (nWidth, nHeight), pUi_Render);
         if (m_pRmlContext)
         {
            m_pRmlContext->SetDensityIndependentPixelRatio (dDensity);
            m_dDensity = dDensity;
            m_bDirty = true;
         }
      }
      else
      {
         if (m_nWidth != nWidth  ||  m_nHeight != nHeight)
         {
            m_pRmlContext->SetDimensions (Rml::Vector2i (nWidth, nHeight));
            m_bDirty = true;
         }
         if (m_dDensity != dDensity)
         {
            m_pRmlContext->SetDensityIndependentPixelRatio (dDensity);
            m_dDensity = dDensity;
            m_bDirty = true;
         }
      }

      if (m_pRmlContext  &&  EnsureDocument ())
      {
         const bool bLive = pUi_Render->LiveTexture_Update ();

         // A new camera sample only needs the img rect rewritten. A full RmlUi
         // software raster of the panel is what drops a 60 Hz compositor.
         // A size or density change sets m_bDirty and must take the full path,
         // because the stamp writes into the previous canvas.
         if (bLive  &&  !m_bDirty  &&  !m_aStraight.empty ()  &&  pUi_Render->LiveTexture_Stamp (m_aStraight.data (), m_nWidth, m_nHeight))
         {
            m_nSerial = ++s_nSerialSeq;
         }
         else
         {
            if (bLive)
               m_bDirty = true;

            if (m_bDirty)
            {
               // The shared canvas may have been left at another panel's size, so
               // size it to this panel before rendering. The first pass also runs
               // LoadTexture so camera:// devices open.
               pUi_Render->Resize (nWidth, nHeight);
               m_pRmlContext->Update ();
               pUi_Render->Clear ();
               m_pRmlContext->Render ();
               Straighten (pUi_Render);
               m_bDirty = false;
            }
         }

         // Hold the panel off the GPU until the first camera sample (or a
         // short timeout) so the first ANARI copy is a real frame, not the
         // opaque placeholder that LoadTexture installs.
         if (pUi_Render->LiveTexture_Waiting ()  &&  m_nWaitLive < 90)
         {
            m_nWaitLive++;
            m_bDirty = true;
         }
         else
         {
            bResult = !m_aStraight.empty ();
         }
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

   m_nSerial = ++s_nSerialSeq;
}

const uint8_t* UI_PANEL::Pixels () const
{
   return m_aStraight.empty () ? nullptr : m_aStraight.data ();
}
