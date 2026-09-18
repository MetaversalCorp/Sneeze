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

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Span.h>
#include <RmlUi/Core/StreamMemory.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Vertex.h>

#include "ui/Ui_Render.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Stub interfaces (same as Ui_Context.cpp - self-contained for the test)
// ---------------------------------------------------------------------------

namespace
{

class STUB_SYSTEM : public Rml::SystemInterface
{
public:
   STUB_SYSTEM ()
   {
      tpStart = std::chrono::steady_clock::now ();
   }

   double GetElapsedTime () override
   {
      auto tpNow = std::chrono::steady_clock::now ();
      return std::chrono::duration<double> (tpNow - tpStart).count ();
   }

   bool LogMessage (Rml::Log::Type, const Rml::String& sMessage) override
   {
      std::printf ("    [RmlUi] %s\n", sMessage.c_str ());
      return true;
   }

private:
   std::chrono::steady_clock::time_point tpStart;
};

class STUB_RENDER : public Rml::RenderInterface
{
public:
   Rml::CompiledGeometryHandle CompileGeometry (Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override
   {
      return Rml::CompiledGeometryHandle (1);
   }

   void RenderGeometry (Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
   void ReleaseGeometry (Rml::CompiledGeometryHandle) override {}

   Rml::TextureHandle LoadTexture (Rml::Vector2i& pDimensions, const Rml::String&) override
   {
      pDimensions = Rml::Vector2i (1, 1);
      return Rml::TextureHandle (1);
   }

   Rml::TextureHandle GenerateTexture (Rml::Span<const Rml::byte>, Rml::Vector2i) override
   {
      return Rml::TextureHandle (1);
   }

   void ReleaseTexture (Rml::TextureHandle) override {}
   void EnableScissorRegion (bool) override {}
   void SetScissorRegion (Rml::Rectanglei) override {}
};

class STUB_FONT_ENGINE : public Rml::FontEngineInterface
{
public:
   Rml::FontFaceHandle GetFontFaceHandle (const Rml::String&, Rml::Style::FontStyle, Rml::Style::FontWeight, int) override
   {
      return Rml::FontFaceHandle (1);
   }

   const Rml::FontMetrics& GetFontMetrics (Rml::FontFaceHandle) override
   {
      static Rml::FontMetrics pMetrics = {};
      pMetrics.ascent       = 12.0f;
      pMetrics.descent      = 3.0f;
      pMetrics.line_spacing = 16.0f;
      return pMetrics;
   }

   int GetStringWidth (Rml::FontFaceHandle, Rml::StringView sString, const Rml::TextShapingContext&, Rml::Character) override
   {
      return static_cast<int> (sString.size ()) * 8;
   }
};

static STUB_SYSTEM      pStubSystem;
static STUB_RENDER      pStubRender;
static STUB_FONT_ENGINE pStubFontEngine;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1: Version and initialization
// ---------------------------------------------------------------------------
static void TestInitialization ()
{
   std::printf ("\n[Test 1] Version and initialization\n");

   Rml::String sVersion = Rml::GetVersion ();
   Check (!sVersion.empty (), "Version string is non-empty");
   std::printf ("    RmlUi version: %s\n", sVersion.c_str ());
}

// ---------------------------------------------------------------------------
// Test 2: Create and destroy a context
// ---------------------------------------------------------------------------
static void TestContext ()
{
   std::printf ("\n[Test 2] Create and destroy a context\n");

   Rml::Context* pContext = Rml::CreateContext ("test", Rml::Vector2i (1280, 720));
   Check (pContext != nullptr, "CreateContext returned a context");

   if (pContext)
   {
      Check (pContext->GetName () == "test", "Context name is 'test'");

      Rml::Vector2i pDimensions = pContext->GetDimensions ();
      Check (pDimensions.x == 1280  &&  pDimensions.y == 720, "Context dimensions are 1280x720");

      Rml::RemoveContext ("test");
      Check (true, "RemoveContext completed (no crash)");
   }
}

// ---------------------------------------------------------------------------
// Test 3: Load an RML document from memory
// ---------------------------------------------------------------------------
static void TestLoadDocument ()
{
   std::printf ("\n[Test 3] Load an RML document from memory\n");

   Rml::Context* pContext = Rml::CreateContext ("doc_test", Rml::Vector2i (800, 600));
   Check (pContext != nullptr, "CreateContext for document test");

   if (pContext)
   {
      const char* szRml =
         "<rml>"
         "<head><title>Test</title>"
         "<style>body { width: 100%; height: 100%; } "
         "#hello { color: white; font-size: 16px; }</style>"
         "</head>"
         "<body>"
         "<div id='hello'>Hello from RmlUi</div>"
         "</body>"
         "</rml>";

      Rml::ElementDocument* pDocument = pContext->LoadDocumentFromMemory (szRml);
      Check (pDocument != nullptr, "LoadDocumentFromMemory returned a document");

      if (pDocument)
      {
         pDocument->Show ();

         Rml::String sTitle = pDocument->GetTitle ();
         Check (sTitle == "Test", "Document title is 'Test'");

         Rml::Element* pElement = pDocument->GetElementById ("hello");
         Check (pElement != nullptr, "GetElementById found #hello");

         if (pElement)
         {
            Rml::String sTag = pElement->GetTagName ();
            Check (sTag == "div", "Element tag is 'div'");

            Rml::String sId = pElement->GetId ();
            Check (sId == "hello", "Element id is 'hello'");
         }

         pDocument->Close ();
         Check (true, "Document closed (no crash)");
      }

      Rml::RemoveContext ("doc_test");
   }
}

// ---------------------------------------------------------------------------
// Test 4: Update cycle (simulate a frame)
// ---------------------------------------------------------------------------
static void TestUpdateCycle ()
{
   std::printf ("\n[Test 4] Update cycle (simulate a frame)\n");

   Rml::Context* pContext = Rml::CreateContext ("update_test", Rml::Vector2i (640, 480));
   Check (pContext != nullptr, "CreateContext for update test");

   if (pContext)
   {
      const char* szRml =
         "<rml><head><title>Frame</title></head>"
         "<body><div>Frame test</div></body></rml>";

      Rml::ElementDocument* pDocument = pContext->LoadDocumentFromMemory (szRml);
      if (pDocument)
         pDocument->Show ();

      bool bUpdateOk = pContext->Update ();
      Check (bUpdateOk, "Context.Update() succeeded");

      pContext->Render ();
      Check (true, "Context.Render() completed (no crash)");

      if (pDocument)
         pDocument->Close ();

      Rml::RemoveContext ("update_test");
   }
}

// ---------------------------------------------------------------------------
// Test 5: Multiple contexts
// ---------------------------------------------------------------------------
static void TestMultipleContexts ()
{
   std::printf ("\n[Test 5] Multiple contexts\n");

   Rml::Context* pCtxA = Rml::CreateContext ("ctx_a", Rml::Vector2i (100, 100));
   Rml::Context* pCtxB = Rml::CreateContext ("ctx_b", Rml::Vector2i (200, 200));

   Check (pCtxA != nullptr, "Context A created");
   Check (pCtxB != nullptr, "Context B created");
   Check (Rml::GetNumContexts () >= 2, "At least 2 contexts active");

   if (pCtxA  &&  pCtxB)
   {
      Check (pCtxA->GetName () != pCtxB->GetName (), "Context names are distinct");
   }

   Rml::RemoveContext ("ctx_a");
   Rml::RemoveContext ("ctx_b");
   Check (true, "Both contexts removed (no crash)");
}

// ---------------------------------------------------------------------------
// Test 6: Software render interface (UI_RENDER) rasterization
// ---------------------------------------------------------------------------
static void TestSoftwareRasterizer ()
{
   std::printf ("\n[Test 6] Software rasterizer (UI_RENDER)\n");

   SNEEZE::DEP::UI_RENDER render;
   render.Resize (64, 64);
   render.Clear ();

   const uint8_t* aPx = render.Pixels ();
   auto At = [&] (int x, int y, int c) -> int { return aPx[(static_cast<size_t> (y) * 64 + x) * 4 + c]; };

   auto MakeVertex = [] (float x, float y, Rml::ColourbPremultiplied colour, float u, float v) -> Rml::Vertex
   {
      Rml::Vertex vertex;
      vertex.position  = Rml::Vector2f (x, y);
      vertex.colour    = colour;
      vertex.tex_coord = Rml::Vector2f (u, v);
      return vertex;
   };

   const int aIndex[6] = { 0, 1, 2,  0, 2, 3 };

   // --- Opaque red quad over the left half ---
   const Rml::ColourbPremultiplied red (255, 0, 0, 255);
   Rml::Vertex aRed[4] = {
      MakeVertex (0.0f,  0.0f,  red, 0.0f, 0.0f),
      MakeVertex (32.0f, 0.0f,  red, 1.0f, 0.0f),
      MakeVertex (32.0f, 64.0f, red, 1.0f, 1.0f),
      MakeVertex (0.0f,  64.0f, red, 0.0f, 1.0f),
   };
   Rml::CompiledGeometryHandle hRed = render.CompileGeometry (Rml::Span<const Rml::Vertex> (aRed, 4), Rml::Span<const int> (aIndex, 6));
   render.RenderGeometry (hRed, Rml::Vector2f (0.0f, 0.0f), 0);

   Check (At (10, 10, 0) == 255  &&  At (10, 10, 3) == 255, "opaque red fills inside the quad");
   Check (At (50, 10, 3) == 0, "area outside the quad stays transparent");

   // --- 50% blue over the whole canvas: premultiplied (0,0,128,128) ---
   const Rml::ColourbPremultiplied blue (0, 0, 128, 128);
   Rml::Vertex aBlue[4] = {
      MakeVertex (0.0f,  0.0f,  blue, 0.0f, 0.0f),
      MakeVertex (64.0f, 0.0f,  blue, 1.0f, 0.0f),
      MakeVertex (64.0f, 64.0f, blue, 1.0f, 1.0f),
      MakeVertex (0.0f,  64.0f, blue, 0.0f, 1.0f),
   };
   Rml::CompiledGeometryHandle hBlue = render.CompileGeometry (Rml::Span<const Rml::Vertex> (aBlue, 4), Rml::Span<const int> (aIndex, 6));
   render.RenderGeometry (hBlue, Rml::Vector2f (0.0f, 0.0f), 0);

   // Over red: R = 0 + 255*(127/255) = 127, B = 128, A clamps to 255.
   Check (std::abs (At (10, 10, 0) - 127) <= 2, "blue-over-red red channel ~127");
   Check (std::abs (At (10, 10, 2) - 128) <= 2, "blue-over-red blue channel ~128");
   // Over transparent: straight premultiplied blue.
   Check (std::abs (At (50, 10, 2) - 128) <= 1  &&  At (50, 10, 3) == 128, "blue over empty is premultiplied blue");

   // --- Generated texture sampled through a white quad ---
   render.Clear ();
   const uint8_t aTexel[16] = {
      255, 0,   0,   255,   0,   255, 0,   255,
      0,   0,   255, 255,   255, 255, 255, 255,
   };
   Rml::TextureHandle hTex = render.GenerateTexture (Rml::Span<const Rml::byte> (aTexel, 16), Rml::Vector2i (2, 2));

   const Rml::ColourbPremultiplied white (255, 255, 255, 255);
   Rml::Vertex aTexQuad[4] = {
      MakeVertex (0.0f,  0.0f,  white, 0.0f, 0.0f),
      MakeVertex (64.0f, 0.0f,  white, 1.0f, 0.0f),
      MakeVertex (64.0f, 64.0f, white, 1.0f, 1.0f),
      MakeVertex (0.0f,  64.0f, white, 0.0f, 1.0f),
   };
   Rml::CompiledGeometryHandle hTexGeom = render.CompileGeometry (Rml::Span<const Rml::Vertex> (aTexQuad, 4), Rml::Span<const int> (aIndex, 6));
   render.RenderGeometry (hTexGeom, Rml::Vector2f (0.0f, 0.0f), hTex);

   Check (At (16, 16, 0) == 255  &&  At (16, 16, 1) == 0, "texture top-left texel is red");
   Check (At (48, 16, 1) == 255  &&  At (48, 16, 0) == 0, "texture top-right texel is green");
   Check (At (16, 48, 2) == 255, "texture bottom-left texel is blue");

   const uint8_t aBlueTex[16] = {
      0, 0, 255, 255,  0, 0, 255, 255,
      0, 0, 255, 255,  0, 0, 255, 255,
   };
   Check (render.UpdateTexture (hTex, Rml::Span<const Rml::byte> (aBlueTex, 16), Rml::Vector2i (2, 2)), "UpdateTexture replaces texel data");
   render.Clear ();
   render.RenderGeometry (hTexGeom, Rml::Vector2f (0.0f, 0.0f), hTex);
   Check (At (16, 16, 2) == 255  &&  At (16, 16, 0) == 0, "updated texture samples as blue");

   // --- Scissor clipping ---
   render.Clear ();
   render.EnableScissorRegion (true);
   render.SetScissorRegion (Rml::Rectanglei::FromPositionSize (Rml::Vector2i (0, 0), Rml::Vector2i (16, 16)));
   Rml::CompiledGeometryHandle hClip = render.CompileGeometry (Rml::Span<const Rml::Vertex> (aBlue, 4), Rml::Span<const int> (aIndex, 6));
   render.RenderGeometry (hClip, Rml::Vector2f (0.0f, 0.0f), 0);
   render.EnableScissorRegion (false);

   Check (At (8, 8, 3) == 128, "pixel inside scissor is drawn");
   Check (At (32, 32, 3) == 0, "pixel outside scissor is clipped");

   render.ReleaseGeometry (hRed);
   render.ReleaseGeometry (hBlue);
   render.ReleaseGeometry (hTexGeom);
   render.ReleaseGeometry (hClip);
   render.ReleaseTexture (hTex);
}

// ---------------------------------------------------------------------------
// Test 7: Rounded-rect triangle fan (RmlUi GeometryBackgroundBorder winding)
// ---------------------------------------------------------------------------
static void TestRoundedRectFan ()
{
   std::printf ("\n[Test 7] Rounded-rect triangle fan\n");

   SNEEZE::DEP::UI_RENDER render;
   render.Resize (256, 256);
   render.Clear ();

   const uint8_t* aPx = render.Pixels ();
   auto At = [&] (int x, int y, int c) -> int { return aPx[(static_cast<size_t> (y) * 256 + x) * 4 + c]; };

   const Rml::ColourbPremultiplied grey (40, 44, 56, 232);
   const float dPi = 3.14159265f;
   const float dX0 = 24.0f;
   const float dY0 = 24.0f;
   const float dX1 = 232.0f;
   const float dY1 = 232.0f;
   const float dR  = 40.0f;
   const int   nArc = 10;

   auto PushArc = [&] (std::vector<Rml::Vertex>& aVertex, float dCx, float dCy, float dA0, float dA1)
   {
      for (int i = 0; i < nArc; i++)
      {
         const float t = float (i) / float (nArc - 1);
         const float a = dA0 + t * (dA1 - dA0);
         Rml::Vertex vertex;
         vertex.position  = Rml::Vector2f (dCx + std::cos (a) * dR, dCy + std::sin (a) * dR);
         vertex.colour    = grey;
         vertex.tex_coord = Rml::Vector2f (0.0f, 0.0f);
         aVertex.push_back (vertex);
      }
   };

   std::vector<Rml::Vertex> aVertex;
   PushArc (aVertex, dX0 + dR, dY0 + dR, dPi,          1.5f * dPi); // TL
   PushArc (aVertex, dX1 - dR, dY0 + dR, 1.5f * dPi,   2.0f * dPi); // TR
   PushArc (aVertex, dX1 - dR, dY1 - dR, 0.0f,         0.5f * dPi); // BR
   PushArc (aVertex, dX0 + dR, dY1 - dR, 0.5f * dPi,   dPi);        // BL

   std::vector<int> aIndex;
   const int nVertex = static_cast<int> (aVertex.size ());
   for (int i = 0; i < nVertex - 2; i++)
   {
      aIndex.push_back (0);
      aIndex.push_back (i + 2);
      aIndex.push_back (i + 1);
   }

   Rml::CompiledGeometryHandle hFan = render.CompileGeometry (
      Rml::Span<const Rml::Vertex> (aVertex.data (), aVertex.size ()),
      Rml::Span<const int> (aIndex.data (), aIndex.size ()));
   render.RenderGeometry (hFan, Rml::Vector2f (0.0f, 0.0f), 0);

   Check (At (128, 128, 3) > 200, "rounded-rect interior is opaque");
   Check (At (24,  24,  3) == 0, "top-left bounding corner is outside the arc");
   Check (At (231, 24,  3) == 0, "top-right bounding corner is outside the arc");
   Check (At (231, 231, 3) == 0, "bottom-right bounding corner is outside the arc");
   Check (At (24,  231, 3) == 0, "bottom-left bounding corner is outside the arc");
   Check (At (128, 30,  3) > 200, "top edge away from corners is filled");
   Check (At (30,  128, 3) > 200, "left edge away from corners is filled");
   Check (At (226, 128, 3) > 200, "right edge away from corners is filled");
   Check (At (128, 226, 3) > 200, "bottom edge away from corners is filled");

   render.ReleaseGeometry (hFan);
}

// ---------------------------------------------------------------------------
// Test 8: CPU clip mask stencils later geometry
// ---------------------------------------------------------------------------
static void TestClipMask ()
{
   std::printf ("\n[Test 8] Clip mask\n");

   SNEEZE::DEP::UI_RENDER render;
   render.Resize (64, 64);
   render.Clear ();

   const uint8_t* aPx = render.Pixels ();
   auto At = [&] (int x, int y, int c) -> int { return aPx[(static_cast<size_t> (y) * 64 + x) * 4 + c]; };

   auto MakeVertex = [] (float x, float y, Rml::ColourbPremultiplied colour) -> Rml::Vertex
   {
      Rml::Vertex vertex;
      vertex.position  = Rml::Vector2f (x, y);
      vertex.colour    = colour;
      vertex.tex_coord = Rml::Vector2f (0.0f, 0.0f);
      return vertex;
   };

   const int aIndex[6] = { 0, 1, 2,  0, 2, 3 };
   const Rml::ColourbPremultiplied white (255, 255, 255, 255);

   Rml::Vertex aMask[4] = {
      MakeVertex (16.0f, 16.0f, white),
      MakeVertex (48.0f, 16.0f, white),
      MakeVertex (48.0f, 48.0f, white),
      MakeVertex (16.0f, 48.0f, white),
   };
   Rml::Vertex aFull[4] = {
      MakeVertex (0.0f,  0.0f,  white),
      MakeVertex (64.0f, 0.0f,  white),
      MakeVertex (64.0f, 64.0f, white),
      MakeVertex (0.0f,  64.0f, white),
   };

   Rml::CompiledGeometryHandle hMask = render.CompileGeometry (Rml::Span<const Rml::Vertex> (aMask, 4), Rml::Span<const int> (aIndex, 6));
   Rml::CompiledGeometryHandle hFull = render.CompileGeometry (Rml::Span<const Rml::Vertex> (aFull, 4), Rml::Span<const int> (aIndex, 6));

   render.EnableClipMask (true);
   render.RenderToClipMask (Rml::ClipMaskOperation::Set, hMask, Rml::Vector2f (0.0f, 0.0f));
   render.RenderGeometry (hFull, Rml::Vector2f (0.0f, 0.0f), 0);
   render.EnableClipMask (false);

   Check (At (32, 32, 3) == 255, "pixel inside clip mask is drawn");
   Check (At (4,  4,  3) == 0,   "pixel outside clip mask is discarded");
   Check (At (60, 60, 3) == 0,   "bottom-right outside clip mask is discarded");

   render.ReleaseGeometry (hMask);
   render.ReleaseGeometry (hFull);
}

// ---------------------------------------------------------------------------
// Test 9: Error-page document -- all four #card corners must be rounded
// ---------------------------------------------------------------------------
static void TestErrorPageCorners ()
{
   std::printf ("\n[Test 9] Error-page rounded card\n");

   const int nW = 512;
   const int nH = 512;

   SNEEZE::DEP::UI_RENDER render;
   render.Resize (nW, nH);

   Rml::Context* pContext = Rml::CreateContext ("error_page", Rml::Vector2i (nW, nH), &render);
   Check (pContext != nullptr, "CreateContext for error page");
   if (!pContext)
      return;

   const char* szRml =
      "<rml>"
      "<head><style>"
      "body { display: block; width: 100%; height: 100%; }"
      "#card {"
      "   display: block; position: absolute; left: 8%; top: 8%; width: 84%; height: 84%;"
      "   box-sizing: border-box;"
      "   padding: 36px 36px;"
      "   overflow: hidden;"
      "   background-color: rgba(20, 22, 28, 232);"
      "   border-width: 1px; border-color: rgba(255, 120, 120, 60);"
      "   border-radius: 18px;"
      "}"
      "</style></head>"
      "<body>"
      "<div id='card'></div>"
      "</body>"
      "</rml>";

   Rml::ElementDocument* pDocument = pContext->LoadDocumentFromMemory (szRml);
   Check (pDocument != nullptr, "error-page document loaded");
   if (pDocument)
   {
      pDocument->Show ();
      pContext->Update ();
      render.Clear ();
      pContext->Render ();

      const uint8_t* aPx = render.Pixels ();
      auto At = [&] (int x, int y) -> int { return aPx[(static_cast<size_t> (y) * nW + x) * 4 + 3]; };

      int nX0 = nW, nY0 = nH, nX1 = 0, nY1 = 0;
      for (int nY = 0; nY < nH; nY++)
      {
         for (int nX = 0; nX < nW; nX++)
         {
            if (At (nX, nY) != 0)
            {
               if (nX < nX0) nX0 = nX;
               if (nY < nY0) nY0 = nY;
               if (nX + 1 > nX1) nX1 = nX + 1;
               if (nY + 1 > nY1) nY1 = nY + 1;
            }
         }
      }

      const int nMidX = (nX0 + nX1) / 2;
      const int nMidY = (nY0 + nY1) / 2;
      std::printf ("    draw batches=%d  opaque AABB [%d,%d)..[%d,%d)  TL=%d TR=%d BR=%d BL=%d  edges T=%d R=%d B=%d L=%d  center=%d\n",
         render.DrawCount (), nX0, nY0, nX1, nY1,
         At (nX0, nY0), At (nX1 - 1, nY0), At (nX1 - 1, nY1 - 1), At (nX0, nY1 - 1),
         At (nMidX, nY0), At (nX1 - 1, nMidY), At (nMidX, nY1 - 1), At (nX0, nMidY),
         At (nW / 2, nH / 2));

      Check (nX1 > nX0  &&  nY1 > nY0, "card produced an opaque AABB");
      Check (nX1 < nW  &&  nY1 < nH, "card stays inset from the canvas edge");
      Check (At (nW / 2, nH / 2) > 200, "card interior is opaque");
      Check (At (nX0, nY0) == 0, "top-left AABB corner is outside the arc");
      Check (At (nX1 - 1, nY0) == 0, "top-right AABB corner is outside the arc");
      Check (At (nX1 - 1, nY1 - 1) == 0, "bottom-right AABB corner is outside the arc");
      Check (At (nX0, nY1 - 1) == 0, "bottom-left AABB corner is outside the arc");
      Check (At (nMidX, nY0 + 8) > 200, "top edge away from corners is filled");
      Check (At (nX1 - 1 - 8, nMidY) > 200, "right edge away from corners is filled");
      Check (At (nMidX, nY1 - 1 - 8) > 200, "bottom edge away from corners is filled");
      Check (At (nX0 + 8, nMidY) > 200, "left edge away from corners is filled");

      pDocument->Close ();
   }

   Rml::RemoveContext ("error_page");
}

// ---------------------------------------------------------------------------

int RunUiTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== RmlUi Integration Test Suite ===\n");

   Rml::SetSystemInterface (&pStubSystem);
   Rml::SetRenderInterface (&pStubRender);
   Rml::SetFontEngineInterface (&pStubFontEngine);

   bool bInitOk = Rml::Initialise ();

   if (bInitOk)
   {
      TestInitialization ();
      TestContext ();
      TestLoadDocument ();
      TestUpdateCycle ();
      TestMultipleContexts ();
      TestSoftwareRasterizer ();
      TestRoundedRectFan ();
      TestClipMask ();
      TestErrorPageCorners ();

      Rml::Shutdown ();
   }
   else
   {
      std::fprintf (stderr, "Rml::Initialise failed\n");
      nFailed++;
   }

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
