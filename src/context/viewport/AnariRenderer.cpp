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

// ------------------------------------------------------------------------------------------------------------------------------------------------------
//  FILAMENT THREADING AND VSYNC CONSTRAINTS
// ------------------------------------------------------------------------------------------------------------------------------------------------------
//
//  Filament's API is not thread-safe. All Filament API calls for a given engine instance must be made from a single dedicated thread. Filament
//  offloads GPU work to an internal render thread, but the caller-facing API (beginFrame, render, endFrame) is a serial command queue. This
//  means all ANARI calls across all viewports must be serialized onto one compositor agent (agent 0). Multiple agents calling anariRenderFrame
//  concurrently against the same Filament engine will crash.
//
//  Filament's Vulkan backend hardcodes VK_PRESENT_MODE_FIFO_KHR (vsync ON) in VulkanPlatformSwapChainImpl.cpp. FIFO blocks beginFrame() until
//  the display's vsync releases a swapchain image — approximately 16.67ms at 60 Hz. This wait is baked into anariRenderFrame (not
//  anariFrameReady, which returns instantly). With one viewport, the compositor achieves 60 FPS with 16.5ms of idle vsync wait per frame.
//
//  THE PROBLEM: With N viewports rendered sequentially on one thread, each anariRenderFrame incurs its own vsync wait, so total frame time
//  is N * 16ms. Ten viewports = 6 FPS each. The vsync wait is a per-viewport multiplier, not a shared constant.
//
//  PROPOSED SOLUTIONS:
//
//  1. MAILBOX PRESENT MODE (preferred). Modify MetaversalCorp/filament to use VK_PRESENT_MODE_MAILBOX_KHR instead of FIFO_KHR. Mailbox
//     doesn't tear and doesn't block — the GPU renders as fast as it can, only the latest frame is shown at vsync. anariRenderFrame would
//     return in under 1ms. All viewports could render within a single vsync interval. Trade-off: the compositor would need its own frame
//     pacing (the metronome already provides infrastructure for this).
//
//  2. OFFSCREEN READBACK PATH. Render to ANARI framebuffers instead of native swapchains. No Filament swapchain = no per-viewport vsync.
//     The compositor reads pixels back and the host application presents them. This path already exists as the non-native-surface fallback.
//
//  3. HYBRID. Foreground viewport gets native surface rendering (direct GPU-to-screen). Background viewports render offscreen at reduced
//     priority. Only one viewport ever pays the vsync cost.
//
// ------------------------------------------------------------------------------------------------------------------------------------------------------

#include <Sneeze.h>
#include "AnariRenderer.h"
#include "ui/Ui_Context.h"
#include <anari/anari.h>

#define ANARI_RENDERER_TYPE ANARI_DATA_TYPE_DEFINE(514)
#undef ANARI_RENDERER

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <cmath>

using namespace SNEEZE;

using RENDERER = VIEWPORT::RENDERER;

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define SNEEZE_ANARI_OVERRIDE_LIBDIR 1
#include <dlfcn.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#define ANARI_LOGE(fmt, ...) __android_log_print (ANDROID_LOG_ERROR, "Sneeze.Anari", fmt, ##__VA_ARGS__)
#define ANARI_LOGI(fmt, ...) __android_log_print (ANDROID_LOG_INFO,  "Sneeze.Anari", fmt, ##__VA_ARGS__)
#endif

#if defined(SNEEZE_ANARI_OVERRIDE_LIBDIR)
// ANARI's anchor-based lib-path detection uses dlsym(RTLD_DEFAULT, "_anari_anchor")
// + dladdr to find the dir it should dlopen devices from. That is unreliable when
// anari_static is linked into the main binary (Android linker-namespace isolation;
// iOS main-binary dir vs bundle Frameworks/ dir). Resolve the dir ourselves and
// pass it to anariLoadLibrary via the "name,path/" comma syntax.
static std::string GetLocalLibDir ()
{
   std::string sDir;
   Dl_info info;
   if (dladdr ((const void*) &GetLocalLibDir, &info) && info.dli_fname)
   {
      std::string sPath = info.dli_fname;
      auto nSlash = sPath.rfind ('/');
      if (nSlash != std::string::npos)
      {
         sDir = sPath.substr (0, nSlash + 1);
#if defined(__APPLE__) && TARGET_OS_IPHONE
         // iOS: dylibs are bundled in <App>.app/Frameworks/
         sDir += "Frameworks/";
#endif
      }
   }
   return sDir;
}
#endif

// ---------------------------------------------------------------------------
//  Retained scene state — ANARI objects that persist across frames
// ---------------------------------------------------------------------------

struct RENDERER::ANARI::SCENE_STATE
{
   bool bBuilt = false;

   ANARIArray1D  pSharedPosArr = nullptr;
   ANARIArray1D  pSharedNrmArr = nullptr;
   ANARIArray1D  pSharedIdxArr = nullptr;

   ANARIArray1D  pBoxPosArr = nullptr;
   ANARIArray1D  pBoxNrmArr = nullptr;
   ANARIArray1D  pBoxIdxArr = nullptr;

   ANARIArray1D  pQuadPosArr = nullptr;
   ANARIArray1D  pQuadNrmArr = nullptr;
   ANARIArray1D  pQuadUvArr  = nullptr;
   ANARIArray1D  pQuadIdxArr = nullptr;

   std::vector<ANARILight> aLight;
   ANARIArray1D            pLightArr     = nullptr;

   ANARIGroup    pSurfaceGroup = nullptr;
   ANARIInstance pSurfaceInst  = nullptr;

   ANARIArray1D  pWorldInstArr = nullptr;

   struct SPHERE_ENTRY
   {
      bool           bTextured   = false;
      const uint8_t* pTextureKey = nullptr;
      ANARIGeometry  pGeom       = nullptr;
      ANARIArray1D   pColorArr   = nullptr;
      ANARIMaterial  pMat        = nullptr;
      ANARISurface   pSurf       = nullptr;
      ANARIGroup     pGroup      = nullptr;
      ANARIInstance  pInst       = nullptr;
   };

   struct CURVE_ENTRY
   {
      ANARIGeometry pGeom = nullptr;
      ANARIMaterial pMat  = nullptr;
      ANARISurface  pSurf = nullptr;
      size_t        nPointCount = 0;
   };

   struct BOX_ENTRY
   {
      ANARIGeometry pGeom  = nullptr;
      ANARIMaterial pMat   = nullptr;
      ANARISurface  pSurf  = nullptr;
      ANARIGroup    pGroup = nullptr;
      ANARIInstance pInst  = nullptr;
   };

   // One in-scene UI panel: an unlit, alpha-blended textured quad. Geometry is
   // the shared unit quad (pQuad* arrays); each panel owns its image/sampler/
   // material/instance. pPixelKey detects when a panel's canvas pointer changes.
   struct PANEL_ENTRY
   {
      const uint8_t* pPixelKey = nullptr;
      ANARIArray2D   pImageArr = nullptr;
      ANARISampler   pSampler  = nullptr;
      ANARIGeometry  pGeom     = nullptr;
      ANARIMaterial  pMat      = nullptr;
      ANARISurface   pSurf     = nullptr;
      ANARIGroup     pGroup    = nullptr;
      ANARIInstance  pInst     = nullptr;
   };

   // One drawable from a loaded glTF: indexed triangle geometry, a metallic-
   // roughness material (base color either a factor or an image2D sampler), and
   // a per-mesh instance carrying the baked world transform. pVertexKey detects
   // when the source vertex buffer changes (model reload); pTextureKey detects a
   // base-color texture swap.
   struct MESH_ENTRY
   {
      const float*   pVertexKey  = nullptr;
      const uint8_t* pTextureKey = nullptr;
      ANARIArray1D   pPosArr   = nullptr;
      ANARIArray1D   pNrmArr   = nullptr;
      ANARIArray1D   pUvArr    = nullptr;
      ANARIArray1D   pIdxArr   = nullptr;
      ANARIArray2D   pImageArr = nullptr;
      ANARISampler   pSampler  = nullptr;
      ANARIGeometry  pGeom     = nullptr;
      ANARIMaterial  pMat      = nullptr;
      ANARISurface   pSurf     = nullptr;
      ANARIGroup     pGroup    = nullptr;
      ANARIInstance  pInst     = nullptr;
   };

   std::vector<SPHERE_ENTRY> aSphere_Entry;
   std::vector<CURVE_ENTRY>  aCurve_Entry;
   std::vector<BOX_ENTRY>    aBox_Entry;
   std::vector<PANEL_ENTRY>  aPanel_Entry;
   std::vector<MESH_ENTRY>   aMesh_Entry;
};

// ---------------------------------------------------------------------------

RENDERER::ANARI::ANARI (ENGINE* pEngine, const std::string& sLibrary) :
   m_pEngine            (pEngine),
   m_sLibrary           (sLibrary),
   m_pLibrary           (nullptr),
   m_pDevice            (nullptr),
   m_pWorld             (nullptr),
   m_pCamera            (nullptr),
   m_pRenderer          (nullptr),
   m_pFrame             (nullptr),
   m_pNativeSurface     (nullptr),
   m_pNativeWindow      (nullptr),
   m_bNativeSurface     (false),
   m_nWidth             (0),
   m_nHeight            (0),
   m_bUnitSphereReady   (false),
   m_bUnitBoxReady      (false),
   m_pSceneState        (new SCENE_STATE ()),
   m_bSceneDirty        (false),
   m_dLastSubmitSeconds (0.0),
   m_dLastRenderSeconds (0.0)
{
}

RENDERER::ANARI::~ANARI ()
{
   // Filament (halogen's backend) can throw utils::PostconditionPanic during
   // Vulkan teardown on some drivers (e.g. llvmpipe / WSL software Vulkan):
   // "enumerate size error". The throw happens on the compositor thread inside
   // a destructor, so letting it escape std::terminates the process on exit.
   // Swallow teardown panics so shutdown stays graceful (the process is going
   // away regardless, so any leaked GPU resources are reclaimed by the OS).
   try
   {
      if (m_pDevice)
      {
         ReleaseScene ();

         if (m_pFrame)
         {
            anariRelease (m_pDevice, m_pFrame);
            m_pFrame = nullptr;
         }
         if (m_pNativeSurface)
         {
            anariRelease (m_pDevice, reinterpret_cast<ANARIObject> (m_pNativeSurface));
            m_pNativeSurface = nullptr;
         }
         if (m_pRenderer)
         {
            anariRelease (m_pDevice, m_pRenderer);
            m_pRenderer = nullptr;
         }
         if (m_pCamera)
         {
            anariRelease (m_pDevice, m_pCamera);
            m_pCamera = nullptr;
         }
         if (m_pWorld)
         {
            anariRelease (m_pDevice, m_pWorld);
            m_pWorld = nullptr;
         }
         anariRelease (m_pDevice, m_pDevice);
         m_pDevice = nullptr;
      }
      if (m_pLibrary)
      {
         anariUnloadLibrary (m_pLibrary);
         m_pLibrary = nullptr;
      }
   }
   catch (...)
   {
      m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "ANARI",
         "exception during renderer teardown (ignored)");
   }

   delete m_pSceneState;
   m_pSceneState = nullptr;
   m_bNativeSurface = false;
}

void RENDERER::ANARI::SetNativeWindow (void* pHandle)
{
   m_pNativeWindow = pHandle;
}

bool RENDERER::ANARI::IsRenderingToNativeSurface () const
{
   return m_bNativeSurface;
}

// ---------------------------------------------------------------------------

namespace {

// Routes ANARI/Halogen status messages (including rejected parameters and
// unsupported subtypes) to the engine log. Registered via anariLoadLibrary so
// helium-based devices inherit it. May fire on the compositor thread.
void AnariStatusCallback (const void* userPtr, ANARIDevice /*device*/, ANARIObject /*source*/, ANARIDataType /*sourceType*/, ANARIStatusSeverity severity, ANARIStatusCode /*code*/, const char* message)
{
   SNEEZE::ENGINE* pEngine = const_cast<SNEEZE::ENGINE*> (reinterpret_cast<const SNEEZE::ENGINE*> (userPtr));
   if (!pEngine  ||  !message)
      return;

   IENGINE::eLOGLEVEL nLevel = IENGINE::kLOGLEVEL_Info;
   if (severity == ANARI_SEVERITY_FATAL_ERROR  ||  severity == ANARI_SEVERITY_ERROR)
      nLevel = IENGINE::kLOGLEVEL_Error;
   else if (severity == ANARI_SEVERITY_WARNING  ||  severity == ANARI_SEVERITY_PERFORMANCE_WARNING)
      nLevel = IENGINE::kLOGLEVEL_Warning;
   else if (severity == ANARI_SEVERITY_DEBUG)
      nLevel = IENGINE::kLOGLEVEL_Trace;

   pEngine->Log (nLevel, "ANARI", message);
}

// True if 'sName' appears in the null-terminated extension list returned by
// anariGetDeviceExtensions().
bool HasExtension (const char* const* pList, const char* sName)
{
   bool bFound = false;
   if (pList && sName)
   {
      for (const char* const* p = pList; *p; ++p)
      {
         if (std::strcmp (*p, sName) == 0)
            bFound = true;
      }
   }
   return bFound;
}

} // namespace

bool RENDERER::ANARI::Initialize (int nWidth, int nHeight)
{
   m_nWidth  = nWidth;
   m_nHeight = nHeight;
   m_aPixels.resize (nWidth * nHeight, 0);

#if defined(__ANDROID__)
   // Filament's DEFAULT backend on Android is OpenGL, whose engine init panics
   // unless a JavaVM* was captured via JNI_OnLoad. Halogen's .so is dlopen'd by
   // the ANARI runtime (not Java's System.loadLibrary), so JNI_OnLoad never
   // fires. Force Vulkan: it uses VK_KHR_android_surface on a raw
   // ANativeWindow* (supplied via HALOGEN_NATIVE_SURFACE below) — no JNI.
   // Halogen reads FILAMENT_BACKEND in its initDevice(); the equivalent
   // anariSetParameter("backend","vulkan") path is bypassed because Halogen
   // doesn't promote staged params before reading them.
   setenv ("FILAMENT_BACKEND", "vulkan", 0);
#endif

   bool bOk = false;

   std::string sLibraryArg = m_sLibrary;
#if defined(SNEEZE_ANARI_OVERRIDE_LIBDIR)
   // Explicitly steer ANARI to the bundled-native-lib dir so it does not rely
   // on the anchor-symbol fallback, which is unreliable on Android/iOS.
   std::string sLibDir = GetLocalLibDir ();
   if (!sLibDir.empty ())
   {
      sLibraryArg = m_sLibrary + "," + sLibDir;
#if defined(__ANDROID__)
      ANARI_LOGI ("lib dir: '%s'", sLibDir.c_str ());
#else
      m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "ANARI",
         "lib dir: '" + sLibDir + "'");
#endif
   }
   else
   {
#if defined(__ANDROID__)
      ANARI_LOGE ("could not resolve local lib dir via dladdr");
#else
      m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "ANARI",
         "could not resolve local lib dir via dladdr");
#endif
   }
#endif

#if defined(__ANDROID__)
   ANARI_LOGI ("loading library '%s'", sLibraryArg.c_str ());
#else
   m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "ANARI",
      "loading library '" + sLibraryArg + "'");
#endif
   m_pLibrary = anariLoadLibrary (sLibraryArg.c_str (), AnariStatusCallback, m_pEngine);
   if (!m_pLibrary)
   {
#if defined(__ANDROID__)
      ANARI_LOGE ("failed to load library '%s'", sLibraryArg.c_str ());
#else
      m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "ANARI",
         "failed to load library '" + sLibraryArg + "'");
#endif
   }
   else
   {
#if defined(__ANDROID__)
      ANARI_LOGI ("creating device 'default'");
#else
      m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "ANARI",
         "creating device 'default'");
#endif
      m_pDevice = anariNewDevice (m_pLibrary, "default");
      if (!m_pDevice)
      {
#if defined(__ANDROID__)
         ANARI_LOGE ("failed to create device from library '%s'", m_sLibrary.c_str ());
#else
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "ANARI",
            "failed to create device from library '" + m_sLibrary + "'");
#endif
         anariUnloadLibrary (m_pLibrary);
         m_pLibrary = nullptr;
      }
   }

   if (m_pDevice)
   {
      anariCommitParameters (m_pDevice, m_pDevice);

      // Opt into direct-to-window rendering when the implementation advertises
      // Halogen's native-surface extension AND the app has provided a window.
      if (m_pNativeWindow)
      {
         const char* const* pExtensions = anariGetDeviceExtensions (reinterpret_cast<ANARILibrary> (m_pLibrary), "default");
         if (HasExtension (pExtensions, "HALOGEN_NATIVE_SURFACE"))
         {
            ANARIObject ns = anariNewObject (m_pDevice, "nativeSurface", "default");
            if (ns)
            {
               // ANARI_VOID_POINTER takes the pointer value directly as the
               // 5th arg to anariSetParameter — NOT a pointer to it. The
               // C++ wrapper at anari_cpp_impl.hpp:530 dereferences one level
               // for this type; passing &m_pNativeWindow stores the wrong
               // value and crashes inside vkCreateAndroidSurfaceKHR on Vulkan.
               anariSetParameter (m_pDevice, ns, "nativeWindow", ANARI_VOID_POINTER, m_pNativeWindow);
               anariCommitParameters (m_pDevice, ns);
               m_pNativeSurface = reinterpret_cast<anari::api::Object*> (ns);
               m_bNativeSurface = true;
            }
         }
      }

      m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "ANARI",
         m_bNativeSurface ? "rendering path: native surface (direct-to-window)"
                          : "rendering path: offscreen readback");

      m_pWorld = anariNewWorld (m_pDevice);
      m_pCamera = anariNewCamera (m_pDevice, "perspective");
      m_pRenderer = anariNewRenderer (m_pDevice, "default");

      float bgColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
      anariSetParameter (m_pDevice, m_pRenderer, "background", ANARI_FLOAT32_VEC4, bgColor);
      float ambientColor[3] = { 1.0f, 1.0f, 1.0f };
      float ambientRadiance = 0.05f;
      anariSetParameter (m_pDevice, m_pRenderer, "ambientColor", ANARI_FLOAT32_VEC3, ambientColor);
      anariSetParameter (m_pDevice, m_pRenderer, "ambientRadiance", ANARI_FLOAT32, &ambientRadiance);
      anariCommitParameters (m_pDevice, m_pRenderer);

      m_pFrame = anariNewFrame (m_pDevice);
      uint32_t aSize[2] = { static_cast<uint32_t> (nWidth), static_cast<uint32_t> (nHeight) };
      anariSetParameter (m_pDevice, m_pFrame, "size", ANARI_UINT32_VEC2, aSize);
      ANARIDataType nColorType = ANARI_UFIXED8_RGBA_SRGB;
      anariSetParameter (m_pDevice, m_pFrame, "channel.color", ANARI_DATA_TYPE, &nColorType);
      anariSetParameter (m_pDevice, m_pFrame, "renderer", ANARI_RENDERER_TYPE, &m_pRenderer);
      anariSetParameter (m_pDevice, m_pFrame, "camera", ANARI_CAMERA, &m_pCamera);
      anariSetParameter (m_pDevice, m_pFrame, "world", ANARI_WORLD, &m_pWorld);
      if (m_pNativeSurface)
      {
         ANARIObject ns = reinterpret_cast<ANARIObject> (m_pNativeSurface);
         anariSetParameter (m_pDevice, m_pFrame, "nativeSurface", ANARI_OBJECT, &ns);
      }
      anariCommitParameters (m_pDevice, m_pFrame);

      bOk = true;
   }

   return bOk;
}

void RENDERER::ANARI::Resize (int nWidth, int nHeight)
{
   if (m_pDevice && m_pFrame)
   {
      m_nWidth  = nWidth;
      m_nHeight = nHeight;
      m_aPixels.resize (nWidth * nHeight, 0);

      uint32_t aSize[2] = { static_cast<uint32_t> (nWidth), static_cast<uint32_t> (nHeight) };
      anariSetParameter (m_pDevice, m_pFrame, "size", ANARI_UINT32_VEC2, aSize);
      anariCommitParameters (m_pDevice, m_pFrame);

//    if (m_pNativeSurface)
//       anariCommitParameters (m_pDevice, reinterpret_cast<ANARIObject> (m_pNativeSurface));
   }
}

// ---------------------------------------------------------------------------

void RENDERER::ANARI::SetCamera (const CAMERA_DATA& pCamera)
{
   float pos[3] = { pCamera.dPosX, pCamera.dPosY, pCamera.dPosZ };
   float dir[3] = { pCamera.dDirX, pCamera.dDirY, pCamera.dDirZ };
   float up[3]  = { pCamera.dUpX,  pCamera.dUpY,  pCamera.dUpZ };

   anariSetParameter (m_pDevice, m_pCamera, "position",  ANARI_FLOAT32_VEC3, pos);
   anariSetParameter (m_pDevice, m_pCamera, "direction", ANARI_FLOAT32_VEC3, dir);
   anariSetParameter (m_pDevice, m_pCamera, "up",        ANARI_FLOAT32_VEC3, up);
   anariSetParameter (m_pDevice, m_pCamera, "fovy",      ANARI_FLOAT32, &pCamera.dFovY);
   anariSetParameter (m_pDevice, m_pCamera, "aspect",    ANARI_FLOAT32, &pCamera.dAspect);
   anariSetParameter (m_pDevice, m_pCamera, "near",      ANARI_FLOAT32, &pCamera.dNear);
   anariSetParameter (m_pDevice, m_pCamera, "far",       ANARI_FLOAT32, &pCamera.dFar);
   anariCommitParameters (m_pDevice, m_pCamera);
}

void RENDERER::ANARI::SetBackground (float dRed, float dGreen, float dBlue, float dAlpha)
{
   if (m_pDevice  &&  m_pRenderer)
   {
      float bgColor[4] = { dRed, dGreen, dBlue, dAlpha };
      anariSetParameter (m_pDevice, m_pRenderer, "background", ANARI_FLOAT32_VEC4, bgColor);
      anariCommitParameters (m_pDevice, m_pRenderer);
   }
}

void RENDERER::ANARI::SetLights (const std::vector<LIGHT_DATA>& aLight)
{
   if (aLight.size () != m_aLight.size ())
      m_bSceneDirty = true;

   m_aLight = aLight;
}

void RENDERER::ANARI::BeginFrame ()
{
   m_aSphere_Data.clear ();
   m_aCurve_Data.clear ();
   m_aBox_Data.clear ();
   m_aPanel_Data.clear ();
   m_aMesh_Data.clear ();
}

void RENDERER::ANARI::SubmitSpheres (const std::vector<SPHERE_DATA>& aSphere_Data)
{
   m_aSphere_Data.insert (m_aSphere_Data.end (), aSphere_Data.begin (), aSphere_Data.end ());
}

void RENDERER::ANARI::SubmitCurves (const std::vector<CURVE_DATA>& aCurve_Data)
{
   m_aCurve_Data.insert (m_aCurve_Data.end (), aCurve_Data.begin (), aCurve_Data.end ());
}

void RENDERER::ANARI::SubmitBoxes (const std::vector<BOX_DATA>& aBox_Data)
{
   m_aBox_Data.insert (m_aBox_Data.end (), aBox_Data.begin (), aBox_Data.end ());
}

void RENDERER::ANARI::SubmitPanels (const std::vector<PANEL_DATA>& aPanel_Data)
{
   m_aPanel_Data.insert (m_aPanel_Data.end (), aPanel_Data.begin (), aPanel_Data.end ());
}

void RENDERER::ANARI::SubmitMeshes (const std::vector<MESH_DATA>& aMesh_Data)
{
   m_aMesh_Data.insert (m_aMesh_Data.end (), aMesh_Data.begin (), aMesh_Data.end ());
}

void RENDERER::ANARI::EndFrame ()
{
   auto tpSubmitStart = std::chrono::steady_clock::now ();

   if (!m_pSceneState->bBuilt  ||  m_bSceneDirty  ||  SceneNeedsRebuild (m_aSphere_Data, m_aCurve_Data, m_aBox_Data, m_aPanel_Data, m_aMesh_Data))
   {
      ReleaseScene ();
      BuildScene (m_aSphere_Data, m_aCurve_Data, m_aBox_Data, m_aPanel_Data, m_aMesh_Data);

      m_bSceneDirty = false;
   }
   else
   {
      UpdateScene (m_aSphere_Data, m_aCurve_Data, m_aBox_Data, m_aPanel_Data, m_aMesh_Data);
   }

   anariCommitParameters (m_pDevice, m_pWorld);
   anariCommitParameters (m_pDevice, m_pFrame);

   auto tpRenderStart = std::chrono::steady_clock::now ();
   m_dLastSubmitSeconds = std::chrono::duration<double> (tpRenderStart - tpSubmitStart).count ();

   anariRenderFrame (m_pDevice, m_pFrame);
   anariFrameReady (m_pDevice, m_pFrame, ANARI_WAIT);

   auto tpRenderEnd = std::chrono::steady_clock::now ();
   m_dLastRenderSeconds = std::chrono::duration<double> (tpRenderEnd - tpRenderStart).count ();

   if (!m_bNativeSurface)
   {
      uint32_t nW = 0, nH = 0;
      ANARIDataType nType = ANARI_UNKNOWN;
      const void* pData = anariMapFrame (m_pDevice, m_pFrame, "channel.color", &nW, &nH, &nType);

      if (pData)
      {
         std::memcpy (m_aPixels.data (), pData, nW * nH * sizeof (uint32_t));
         anariUnmapFrame (m_pDevice, m_pFrame, "channel.color");
      }
   }
}

void RENDERER::ANARI::InvalidateScene ()
{
   m_bSceneDirty = true;
}

const uint32_t* RENDERER::ANARI::GetFrameBuffer () const
{
   const uint32_t* pPixels = nullptr;
   if (!m_bNativeSurface)
      pPixels = m_aPixels.data ();
   return pPixels;
}

int RENDERER::ANARI::GetWidth () const
{
   return m_nWidth;
}

int RENDERER::ANARI::GetHeight () const
{
   return m_nHeight;
}

// ---------------------------------------------------------------------------
//  SceneNeedsRebuild — detect structural changes (count, texture transitions)
// ---------------------------------------------------------------------------

bool RENDERER::ANARI::SceneNeedsRebuild (const std::vector<SPHERE_DATA>& aSphere_Data, const std::vector<CURVE_DATA>& aCurve_Data, const std::vector<BOX_DATA>& aBox_Data, const std::vector<PANEL_DATA>& aPanel_Data, const std::vector<MESH_DATA>& aMesh_Data) const
{
   const SCENE_STATE& S = *m_pSceneState;
   bool bRebuild = false;

   if (aSphere_Data.size () != S.aSphere_Entry.size ())
      bRebuild = true;

   if (!bRebuild  &&  aBox_Data.size () != S.aBox_Entry.size ())
      bRebuild = true;

   if (!bRebuild  &&  aPanel_Data.size () != S.aPanel_Entry.size ())
      bRebuild = true;

   if (!bRebuild  &&  aMesh_Data.size () != S.aMesh_Entry.size ())
      bRebuild = true;

   if (!bRebuild)
   {
      for (size_t i = 0; i < aMesh_Data.size (); i++)
      {
         if (aMesh_Data[i].pPosition != S.aMesh_Entry[i].pVertexKey  ||  aMesh_Data[i].pTexturePixels != S.aMesh_Entry[i].pTextureKey)
         {
            bRebuild = true;
            break;
         }
      }
   }

   if (!bRebuild)
   {
      for (size_t i = 0; i < aPanel_Data.size (); i++)
      {
         if (aPanel_Data[i].pPixels != S.aPanel_Entry[i].pPixelKey)
         {
            bRebuild = true;
            break;
         }
      }
   }

   if (!bRebuild)
   {
      size_t nCurveCount = 0;
      for (const auto& c : aCurve_Data)
      {
         if (!c.aPoints.empty ())
            nCurveCount++;
      }
      if (nCurveCount != S.aCurve_Entry.size ())
         bRebuild = true;
   }

   if (!bRebuild)
   {
      for (size_t i = 0; i < aSphere_Data.size (); i++)
      {
         bool bNowTextured = (aSphere_Data[i].pTexturePixels  &&  aSphere_Data[i].nTextureWidth > 0  &&  aSphere_Data[i].nTextureHeight > 0);
         if (bNowTextured != S.aSphere_Entry[i].bTextured)
         {
            bRebuild = true;
            break;
         }
         if (bNowTextured  &&  aSphere_Data[i].pTexturePixels != S.aSphere_Entry[i].pTextureKey)
         {
            bRebuild = true;
            break;
         }
      }
   }

   return bRebuild;
}

// ---------------------------------------------------------------------------
//  ReleaseScene — free all retained ANARI handles
// ---------------------------------------------------------------------------

void RENDERER::ANARI::ReleaseScene ()
{
   if (!m_pSceneState  ||  !m_pDevice)
      return;

   SCENE_STATE& S = *m_pSceneState;

   for (auto& entry : S.aSphere_Entry)
   {
      if (entry.pInst)     anariRelease (m_pDevice, entry.pInst);
      if (entry.pGroup)    anariRelease (m_pDevice, entry.pGroup);
      if (entry.pSurf)     anariRelease (m_pDevice, entry.pSurf);
      if (entry.pMat)      anariRelease (m_pDevice, entry.pMat);
      if (entry.pColorArr) anariRelease (m_pDevice, entry.pColorArr);
      if (entry.pGeom)     anariRelease (m_pDevice, entry.pGeom);
   }
   S.aSphere_Entry.clear ();

   for (auto& entry : S.aCurve_Entry)
   {
      if (entry.pSurf) anariRelease (m_pDevice, entry.pSurf);
      if (entry.pMat)  anariRelease (m_pDevice, entry.pMat);
      if (entry.pGeom) anariRelease (m_pDevice, entry.pGeom);
   }
   S.aCurve_Entry.clear ();

   for (auto& entry : S.aBox_Entry)
   {
      if (entry.pInst)  anariRelease (m_pDevice, entry.pInst);
      if (entry.pGroup) anariRelease (m_pDevice, entry.pGroup);
      if (entry.pSurf)  anariRelease (m_pDevice, entry.pSurf);
      if (entry.pMat)   anariRelease (m_pDevice, entry.pMat);
      if (entry.pGeom)  anariRelease (m_pDevice, entry.pGeom);
   }
   S.aBox_Entry.clear ();

   for (auto& entry : S.aPanel_Entry)
   {
      if (entry.pInst)     anariRelease (m_pDevice, entry.pInst);
      if (entry.pGroup)    anariRelease (m_pDevice, entry.pGroup);
      if (entry.pSurf)     anariRelease (m_pDevice, entry.pSurf);
      if (entry.pMat)      anariRelease (m_pDevice, entry.pMat);
      if (entry.pSampler)  anariRelease (m_pDevice, entry.pSampler);
      if (entry.pImageArr) anariRelease (m_pDevice, entry.pImageArr);
      if (entry.pGeom)     anariRelease (m_pDevice, entry.pGeom);
   }
   S.aPanel_Entry.clear ();

   for (auto& entry : S.aMesh_Entry)
   {
      if (entry.pInst)     anariRelease (m_pDevice, entry.pInst);
      if (entry.pGroup)    anariRelease (m_pDevice, entry.pGroup);
      if (entry.pSurf)     anariRelease (m_pDevice, entry.pSurf);
      if (entry.pMat)      anariRelease (m_pDevice, entry.pMat);
      if (entry.pSampler)  anariRelease (m_pDevice, entry.pSampler);
      if (entry.pImageArr) anariRelease (m_pDevice, entry.pImageArr);
      if (entry.pIdxArr)   anariRelease (m_pDevice, entry.pIdxArr);
      if (entry.pUvArr)    anariRelease (m_pDevice, entry.pUvArr);
      if (entry.pNrmArr)   anariRelease (m_pDevice, entry.pNrmArr);
      if (entry.pPosArr)   anariRelease (m_pDevice, entry.pPosArr);
      if (entry.pGeom)     anariRelease (m_pDevice, entry.pGeom);
   }
   S.aMesh_Entry.clear ();

   if (S.pQuadIdxArr) { anariRelease (m_pDevice, S.pQuadIdxArr); S.pQuadIdxArr = nullptr; }
   if (S.pQuadUvArr)  { anariRelease (m_pDevice, S.pQuadUvArr);  S.pQuadUvArr  = nullptr; }
   if (S.pQuadNrmArr) { anariRelease (m_pDevice, S.pQuadNrmArr); S.pQuadNrmArr = nullptr; }
   if (S.pQuadPosArr) { anariRelease (m_pDevice, S.pQuadPosArr); S.pQuadPosArr = nullptr; }

   if (S.pWorldInstArr) { anariRelease (m_pDevice, S.pWorldInstArr); S.pWorldInstArr = nullptr; }
   if (S.pSurfaceInst)  { anariRelease (m_pDevice, S.pSurfaceInst);  S.pSurfaceInst  = nullptr; }
   if (S.pSurfaceGroup) { anariRelease (m_pDevice, S.pSurfaceGroup); S.pSurfaceGroup = nullptr; }
   if (S.pLightArr)     { anariRelease (m_pDevice, S.pLightArr);     S.pLightArr     = nullptr; }
   for (auto pLight : S.aLight)  anariRelease (m_pDevice, pLight);
   S.aLight.clear ();
   if (S.pSharedIdxArr) { anariRelease (m_pDevice, S.pSharedIdxArr); S.pSharedIdxArr = nullptr; }
   if (S.pSharedNrmArr) { anariRelease (m_pDevice, S.pSharedNrmArr); S.pSharedNrmArr = nullptr; }
   if (S.pSharedPosArr) { anariRelease (m_pDevice, S.pSharedPosArr); S.pSharedPosArr = nullptr; }

   if (S.pBoxIdxArr) { anariRelease (m_pDevice, S.pBoxIdxArr); S.pBoxIdxArr = nullptr; }
   if (S.pBoxNrmArr) { anariRelease (m_pDevice, S.pBoxNrmArr); S.pBoxNrmArr = nullptr; }
   if (S.pBoxPosArr) { anariRelease (m_pDevice, S.pBoxPosArr); S.pBoxPosArr = nullptr; }

   S.bBuilt = false;
}

// ---------------------------------------------------------------------------
//  BuildScene — create all ANARI objects and retain handles
// ---------------------------------------------------------------------------

void RENDERER::ANARI::BuildScene (const std::vector<SPHERE_DATA>& aSphere_Data, const std::vector<CURVE_DATA>& aCurve_Data, const std::vector<BOX_DATA>& aBox_Data, const std::vector<PANEL_DATA>& aPanel_Data, const std::vector<MESH_DATA>& aMesh_Data)
{
   SCENE_STATE& S = *m_pSceneState;

   if (!m_bUnitSphereReady)
   {
      GenerateUVSphere (m_pUnitSphere, 1.0f, 64, 128, 0.0f, 0.0f, 0.0f);
      m_bUnitSphereReady = true;
   }

   if (!m_bUnitBoxReady)
   {
      GenerateUnitBox (m_pUnitBox);
      m_bUnitBoxReady = true;
   }

   uint64_t nVerts = m_pUnitSphere.aPositions.size () / 3;
   uint64_t nTris  = m_pUnitSphere.aIndices.size () / 3;

   S.pSharedPosArr = anariNewArray1D (m_pDevice, m_pUnitSphere.aPositions.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nVerts);
   S.pSharedNrmArr = anariNewArray1D (m_pDevice, m_pUnitSphere.aNormals.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nVerts);
   S.pSharedIdxArr = anariNewArray1D (m_pDevice, m_pUnitSphere.aIndices.data (), nullptr, nullptr, ANARI_UINT32_VEC3, nTris);

   std::vector<ANARISurface>  aSurfaceHandles;
   std::vector<ANARIInstance> aInstanceHandles;

   // --- Spheres ---

   for (const auto& s : aSphere_Data)
   {
      SCENE_STATE::SPHERE_ENTRY entry;

      if (s.pTexturePixels  &&  s.nTextureWidth > 0  &&  s.nTextureHeight > 0)
      {
         entry.bTextured   = true;
         entry.pTextureKey = s.pTexturePixels;

         auto it = m_pColorCache.find (s.pTexturePixels);
         if (it == m_pColorCache.end ())
         {
            float dBrightness = s.bEmissive ? 8.0f : 1.0f;
            std::vector<float> aColors;
            aColors.reserve (nVerts * 4);
            for (uint64_t i = 0; i < nVerts; i++)
            {
               float u = m_pUnitSphere.aTexCoords[i * 2];
               float v = m_pUnitSphere.aTexCoords[i * 2 + 1];
               int nPixX = static_cast<int> (u * (s.nTextureWidth - 1) + 0.5f);
               int nPixY = static_cast<int> (v * (s.nTextureHeight - 1) + 0.5f);
               if (nPixX < 0) nPixX = 0;
               if (nPixX >= s.nTextureWidth)  nPixX = s.nTextureWidth - 1;
               if (nPixY < 0) nPixY = 0;
               if (nPixY >= s.nTextureHeight) nPixY = s.nTextureHeight - 1;
               int nOff = (nPixY * s.nTextureWidth + nPixX) * 4;
               aColors.push_back (static_cast<float> (s.pTexturePixels[nOff])     / 255.0f * dBrightness);
               aColors.push_back (static_cast<float> (s.pTexturePixels[nOff + 1]) / 255.0f * dBrightness);
               aColors.push_back (static_cast<float> (s.pTexturePixels[nOff + 2]) / 255.0f * dBrightness);
               aColors.push_back (static_cast<float> (s.pTexturePixels[nOff + 3]) / 255.0f);
            }
            it = m_pColorCache.emplace (s.pTexturePixels, std::move (aColors)).first;
         }

         const std::vector<float>& aColors = it->second;
         entry.pColorArr = anariNewArray1D (m_pDevice, aColors.data (), nullptr, nullptr, ANARI_FLOAT32_VEC4, nVerts);

         entry.pGeom = anariNewGeometry (m_pDevice, "triangle");
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &S.pSharedPosArr);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.normal",   ANARI_ARRAY1D, &S.pSharedNrmArr);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.color",    ANARI_ARRAY1D, &entry.pColorArr);
         anariSetParameter (m_pDevice, entry.pGeom, "primitive.index",  ANARI_ARRAY1D, &S.pSharedIdxArr);
         anariCommitParameters (m_pDevice, entry.pGeom);

         entry.pMat = anariNewMaterial (m_pDevice, "matte");
         float matColor[3] = { 1.0f, 1.0f, 1.0f };
         anariSetParameter (m_pDevice, entry.pMat, "color", ANARI_FLOAT32_VEC3, matColor);
         anariCommitParameters (m_pDevice, entry.pMat);

         entry.pSurf = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, entry.pSurf, "geometry", ANARI_GEOMETRY, &entry.pGeom);
         anariSetParameter (m_pDevice, entry.pSurf, "material", ANARI_MATERIAL, &entry.pMat);
         anariCommitParameters (m_pDevice, entry.pSurf);

         ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, &entry.pSurf, nullptr, nullptr, ANARI_SURFACE, 1);
         entry.pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
         anariCommitParameters (m_pDevice, entry.pGroup);
         anariRelease (m_pDevice, pSurfArr);

         float xfm[16] =
         {
            s.dRadius, 0.0f,      0.0f,      0.0f,
            0.0f,      s.dRadius, 0.0f,      0.0f,
            0.0f,      0.0f,      s.dRadius, 0.0f,
            s.x,       s.y,       s.z,       1.0f,
         };

         entry.pInst = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, entry.pInst, "group", ANARI_GROUP, &entry.pGroup);
         anariSetParameter (m_pDevice, entry.pInst, "transform", ANARI_FLOAT32_MAT4, xfm);
         anariCommitParameters (m_pDevice, entry.pInst);

         aInstanceHandles.push_back (entry.pInst);
      }
      else
      {
         entry.bTextured   = false;
         entry.pTextureKey = nullptr;

         entry.pGeom = anariNewGeometry (m_pDevice, "sphere");
         float pos[3] = { s.x, s.y, s.z };
         ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, &pos, nullptr, nullptr, ANARI_FLOAT32_VEC3, 1);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
         anariSetParameter (m_pDevice, entry.pGeom, "radius", ANARI_FLOAT32, &s.dRadius);
         anariCommitParameters (m_pDevice, entry.pGeom);
         anariRelease (m_pDevice, pPosArr);

         entry.pMat = anariNewMaterial (m_pDevice, "matte");
         float color[3] = { s.r, s.g, s.b };
         anariSetParameter (m_pDevice, entry.pMat, "color", ANARI_FLOAT32_VEC3, color);
         anariCommitParameters (m_pDevice, entry.pMat);

         entry.pSurf = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, entry.pSurf, "geometry", ANARI_GEOMETRY, &entry.pGeom);
         anariSetParameter (m_pDevice, entry.pSurf, "material", ANARI_MATERIAL, &entry.pMat);
         anariCommitParameters (m_pDevice, entry.pSurf);

         aSurfaceHandles.push_back (entry.pSurf);
      }

      S.aSphere_Entry.push_back (entry);
   }

   // --- Curves ---

   for (const auto& c : aCurve_Data)
   {
      if (c.aPoints.empty ()) continue;

      SCENE_STATE::CURVE_ENTRY entry;
      entry.nPointCount = c.aPoints.size ();

      std::vector<float> aPos;
      std::vector<float> aRadii;
      aPos.reserve (c.aPoints.size () * 3);
      aRadii.reserve (c.aPoints.size ());

      for (const auto& p : c.aPoints)
      {
         aPos.push_back (p.x);
         aPos.push_back (p.y);
         aPos.push_back (p.z);
         aRadii.push_back (p.dRadius);
      }

      entry.pGeom = anariNewGeometry (m_pDevice, "curve");

      ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, aPos.data (),   nullptr, nullptr, ANARI_FLOAT32_VEC3, c.aPoints.size ());
      ANARIArray1D pRadArr = anariNewArray1D (m_pDevice, aRadii.data (), nullptr, nullptr, ANARI_FLOAT32,      c.aPoints.size ());

      anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
      anariSetParameter (m_pDevice, entry.pGeom, "vertex.radius", ANARI_ARRAY1D, &pRadArr);
      anariCommitParameters (m_pDevice, entry.pGeom);

      anariRelease (m_pDevice, pPosArr);
      anariRelease (m_pDevice, pRadArr);

      entry.pMat = anariNewMaterial (m_pDevice, "physicallyBased");
      float black[4]    = { 0.0f, 0.0f, 0.0f, 1.0f };
      float emissive[3] = { c.r, c.g, c.b };
      float dMetallic   = 0.0f;
      float dRoughness  = 1.0f;
      anariSetParameter (m_pDevice, entry.pMat, "baseColor", ANARI_FLOAT32_VEC4, black);
      anariSetParameter (m_pDevice, entry.pMat, "metallic",  ANARI_FLOAT32,      &dMetallic);
      anariSetParameter (m_pDevice, entry.pMat, "roughness", ANARI_FLOAT32,      &dRoughness);
      anariSetParameter (m_pDevice, entry.pMat, "emissive",  ANARI_FLOAT32_VEC3, emissive);
      anariCommitParameters (m_pDevice, entry.pMat);

      entry.pSurf = anariNewSurface (m_pDevice);
      anariSetParameter (m_pDevice, entry.pSurf, "geometry", ANARI_GEOMETRY, &entry.pGeom);
      anariSetParameter (m_pDevice, entry.pSurf, "material", ANARI_MATERIAL, &entry.pMat);
      anariCommitParameters (m_pDevice, entry.pSurf);

      aSurfaceHandles.push_back (entry.pSurf);

      S.aCurve_Entry.push_back (entry);
   }

   // --- Boxes (one instance per box, unit cube + per-box transform) ---

   if (!aBox_Data.empty ())
   {
      uint64_t nBoxVerts = m_pUnitBox.aPositions.size () / 3;
      uint64_t nBoxTris  = m_pUnitBox.aIndices.size () / 3;

      S.pBoxPosArr = anariNewArray1D (m_pDevice, m_pUnitBox.aPositions.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nBoxVerts);
      S.pBoxNrmArr = anariNewArray1D (m_pDevice, m_pUnitBox.aNormals.data (),   nullptr, nullptr, ANARI_FLOAT32_VEC3, nBoxVerts);
      S.pBoxIdxArr = anariNewArray1D (m_pDevice, m_pUnitBox.aIndices.data (),   nullptr, nullptr, ANARI_UINT32_VEC3,  nBoxTris);

      for (const auto& box : aBox_Data)
      {
         SCENE_STATE::BOX_ENTRY entry;

         entry.pGeom = anariNewGeometry (m_pDevice, "triangle");
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &S.pBoxPosArr);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.normal",   ANARI_ARRAY1D, &S.pBoxNrmArr);
         anariSetParameter (m_pDevice, entry.pGeom, "primitive.index", ANARI_ARRAY1D, &S.pBoxIdxArr);
         anariCommitParameters (m_pDevice, entry.pGeom);

         entry.pMat = anariNewMaterial (m_pDevice, "physicallyBased");
         float baseColor[4] = { box.r, box.g, box.b, 1.0f };
         float dMetallic    = 0.0f;
         float dRoughness   = 0.85f;
         anariSetParameter (m_pDevice, entry.pMat, "baseColor", ANARI_FLOAT32_VEC4, baseColor);
         anariSetParameter (m_pDevice, entry.pMat, "metallic",  ANARI_FLOAT32,      &dMetallic);
         anariSetParameter (m_pDevice, entry.pMat, "roughness", ANARI_FLOAT32,      &dRoughness);
         anariCommitParameters (m_pDevice, entry.pMat);

         entry.pSurf = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, entry.pSurf, "geometry", ANARI_GEOMETRY, &entry.pGeom);
         anariSetParameter (m_pDevice, entry.pSurf, "material", ANARI_MATERIAL, &entry.pMat);
         anariCommitParameters (m_pDevice, entry.pSurf);

         ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, &entry.pSurf, nullptr, nullptr, ANARI_SURFACE, 1);
         entry.pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
         anariCommitParameters (m_pDevice, entry.pGroup);
         anariRelease (m_pDevice, pSurfArr);

         entry.pInst = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, entry.pInst, "group", ANARI_GROUP, &entry.pGroup);
         anariSetParameter (m_pDevice, entry.pInst, "transform", ANARI_FLOAT32_MAT4, box.m16);
         anariCommitParameters (m_pDevice, entry.pInst);

         aInstanceHandles.push_back (entry.pInst);

         S.aBox_Entry.push_back (entry);
      }
   }

   // --- Panels (unlit, alpha-blended textured quads; one instance per panel) ---

   if (!aPanel_Data.empty ())
   {
      // Shared unit quad in the local XY plane, +Z normal, attribute0 = UVs.
      // V is flipped vs. position: ANARI/Filament sample v=0 at the bottom while
      // the UI canvas is top-down, so the quad's top edge maps to v=1 to keep
      // the document upright. Double-sided (front + reversed winding) so
      // back-face culling can't hide a panel turned away from the camera.
      static const float aQPos[12] = { -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.5f, 0.5f, 0.0f,  -0.5f, 0.5f, 0.0f };
      static const float aQNrm[12] = {  0.0f,  0.0f, 1.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f };
      static const float aQUv[8]   = {  0.0f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 1.0f };
      static const uint32_t aQIdx[12] = { 0, 1, 2,  0, 2, 3,   0, 2, 1,  0, 3, 2 };

      S.pQuadPosArr = anariNewArray1D (m_pDevice, aQPos, nullptr, nullptr, ANARI_FLOAT32_VEC3, 4);
      S.pQuadNrmArr = anariNewArray1D (m_pDevice, aQNrm, nullptr, nullptr, ANARI_FLOAT32_VEC3, 4);
      S.pQuadUvArr  = anariNewArray1D (m_pDevice, aQUv,  nullptr, nullptr, ANARI_FLOAT32_VEC2, 4);
      S.pQuadIdxArr = anariNewArray1D (m_pDevice, aQIdx, nullptr, nullptr, ANARI_UINT32_VEC3, 4);

      for (const auto& panel : aPanel_Data)
      {
         SCENE_STATE::PANEL_ENTRY entry;
         entry.pPixelKey = panel.pPixels;

         entry.pGeom = anariNewGeometry (m_pDevice, "triangle");
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.position",   ANARI_ARRAY1D, &S.pQuadPosArr);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.normal",     ANARI_ARRAY1D, &S.pQuadNrmArr);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.attribute0", ANARI_ARRAY1D, &S.pQuadUvArr);
         anariSetParameter (m_pDevice, entry.pGeom, "primitive.index",   ANARI_ARRAY1D, &S.pQuadIdxArr);
         anariCommitParameters (m_pDevice, entry.pGeom);

         // image2D wants CPU RGBA8; Halogen's convertToRGBA8 decodes plain
         // UFIXED8 variants (the _SRGB forms fall through to black), so use
         // UFIXED8_VEC4. Pixels arrive straight-alpha from the panel.
         entry.pImageArr = anariNewArray2D (m_pDevice, panel.pPixels, nullptr, nullptr,
                                            ANARI_UFIXED8_VEC4, panel.nWidth, panel.nHeight);

         entry.pSampler = anariNewSampler (m_pDevice, "image2D");
         anariSetParameter (m_pDevice, entry.pSampler, "image",  ANARI_ARRAY2D, &entry.pImageArr);
         anariSetParameter (m_pDevice, entry.pSampler, "filter", ANARI_STRING,  "linear");
         anariCommitParameters (m_pDevice, entry.pSampler);

         // HALOGEN_MATERIAL_UNLIT: emits the sampled texel directly, lighting-
         // independent -- the correct model for UI. Per-texel alpha rides the
         // texture under alphaMode "blend".
         entry.pMat = anariNewMaterial (m_pDevice, "unlit");
         anariSetParameter (m_pDevice, entry.pMat, "alphaMode", ANARI_STRING, "blend");
         anariSetParameter (m_pDevice, entry.pMat, "color", ANARI_SAMPLER, &entry.pSampler);
         anariCommitParameters (m_pDevice, entry.pMat);

         entry.pSurf = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, entry.pSurf, "geometry", ANARI_GEOMETRY, &entry.pGeom);
         anariSetParameter (m_pDevice, entry.pSurf, "material", ANARI_MATERIAL, &entry.pMat);
         anariCommitParameters (m_pDevice, entry.pSurf);

         ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, &entry.pSurf, nullptr, nullptr, ANARI_SURFACE, 1);
         entry.pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
         anariCommitParameters (m_pDevice, entry.pGroup);
         anariRelease (m_pDevice, pSurfArr);

         entry.pInst = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, entry.pInst, "group", ANARI_GROUP, &entry.pGroup);
         anariSetParameter (m_pDevice, entry.pInst, "transform", ANARI_FLOAT32_MAT4, panel.m16);
         anariCommitParameters (m_pDevice, entry.pInst);

         aInstanceHandles.push_back (entry.pInst);

         S.aPanel_Entry.push_back (entry);
      }
   }

   // --- Meshes (loaded glTF; one instance per primitive, baked world transform) ---

   for (const auto& mesh : aMesh_Data)
   {
      if (!mesh.pPosition  ||  mesh.nVertexCount == 0)
         continue;

      SCENE_STATE::MESH_ENTRY entry;
      entry.pVertexKey  = mesh.pPosition;
      entry.pTextureKey = mesh.pTexturePixels;

      uint64_t nVerts = mesh.nVertexCount;

      bool bTextured = mesh.pTexturePixels  &&  mesh.nTextureWidth > 0  &&  mesh.nTextureHeight > 0  &&  mesh.pTexCoord;

      entry.pPosArr = anariNewArray1D (m_pDevice, mesh.pPosition, nullptr, nullptr, ANARI_FLOAT32_VEC3, nVerts);

      entry.pGeom = anariNewGeometry (m_pDevice, "triangle");
      anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &entry.pPosArr);

      if (mesh.pNormal)
      {
         entry.pNrmArr = anariNewArray1D (m_pDevice, mesh.pNormal, nullptr, nullptr, ANARI_FLOAT32_VEC3, nVerts);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.normal", ANARI_ARRAY1D, &entry.pNrmArr);
      }

      if (mesh.pTexCoord)
      {
         entry.pUvArr = anariNewArray1D (m_pDevice, mesh.pTexCoord, nullptr, nullptr, ANARI_FLOAT32_VEC2, nVerts);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.attribute0", ANARI_ARRAY1D, &entry.pUvArr);
      }

      if (mesh.pIndex  &&  mesh.nIndexCount >= 3)
      {
         entry.pIdxArr = anariNewArray1D (m_pDevice, mesh.pIndex, nullptr, nullptr, ANARI_UINT32_VEC3, mesh.nIndexCount / 3);
         anariSetParameter (m_pDevice, entry.pGeom, "primitive.index", ANARI_ARRAY1D, &entry.pIdxArr);
      }
      anariCommitParameters (m_pDevice, entry.pGeom);

      entry.pMat = anariNewMaterial (m_pDevice, "physicallyBased");
      if (bTextured)
      {
         entry.pImageArr = anariNewArray2D (m_pDevice, mesh.pTexturePixels, nullptr, nullptr, ANARI_UFIXED8_VEC4, mesh.nTextureWidth, mesh.nTextureHeight);

         entry.pSampler = anariNewSampler (m_pDevice, "image2D");
         anariSetParameter (m_pDevice, entry.pSampler, "image",       ANARI_ARRAY2D, &entry.pImageArr);
         anariSetParameter (m_pDevice, entry.pSampler, "inAttribute", ANARI_STRING,  "attribute0");
         anariSetParameter (m_pDevice, entry.pSampler, "filter",      ANARI_STRING,  "linear");
         anariCommitParameters (m_pDevice, entry.pSampler);

         anariSetParameter (m_pDevice, entry.pMat, "baseColor", ANARI_SAMPLER, &entry.pSampler);
      }
      else
      {
         anariSetParameter (m_pDevice, entry.pMat, "baseColor", ANARI_FLOAT32_VEC4, mesh.baseColor);
      }
      anariSetParameter (m_pDevice, entry.pMat, "metallic",  ANARI_FLOAT32,      &mesh.dMetallic);
      anariSetParameter (m_pDevice, entry.pMat, "roughness", ANARI_FLOAT32,      &mesh.dRoughness);
      anariSetParameter (m_pDevice, entry.pMat, "emissive",  ANARI_FLOAT32_VEC3, mesh.emissive);
      anariCommitParameters (m_pDevice, entry.pMat);

      entry.pSurf = anariNewSurface (m_pDevice);
      anariSetParameter (m_pDevice, entry.pSurf, "geometry", ANARI_GEOMETRY, &entry.pGeom);
      anariSetParameter (m_pDevice, entry.pSurf, "material", ANARI_MATERIAL, &entry.pMat);
      anariCommitParameters (m_pDevice, entry.pSurf);

      ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, &entry.pSurf, nullptr, nullptr, ANARI_SURFACE, 1);
      entry.pGroup = anariNewGroup (m_pDevice);
      anariSetParameter (m_pDevice, entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
      anariCommitParameters (m_pDevice, entry.pGroup);
      anariRelease (m_pDevice, pSurfArr);

      entry.pInst = anariNewInstance (m_pDevice, "transform");
      anariSetParameter (m_pDevice, entry.pInst, "group", ANARI_GROUP, &entry.pGroup);
      anariSetParameter (m_pDevice, entry.pInst, "transform", ANARI_FLOAT32_MAT4, mesh.m16);
      anariCommitParameters (m_pDevice, entry.pInst);

      aInstanceHandles.push_back (entry.pInst);

      S.aMesh_Entry.push_back (entry);
   }

   // --- Surface group for analytical spheres + curves ---

   if (!aSurfaceHandles.empty ())
   {
      ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, aSurfaceHandles.data (), nullptr, nullptr,
                                               ANARI_SURFACE, aSurfaceHandles.size ());
      S.pSurfaceGroup = anariNewGroup (m_pDevice);
      anariSetParameter (m_pDevice, S.pSurfaceGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
      anariCommitParameters (m_pDevice, S.pSurfaceGroup);
      anariRelease (m_pDevice, pSurfArr);

      S.pSurfaceInst = anariNewInstance (m_pDevice, "transform");
      anariSetParameter (m_pDevice, S.pSurfaceInst, "group", ANARI_GROUP, &S.pSurfaceGroup);
      anariCommitParameters (m_pDevice, S.pSurfaceInst);

      aInstanceHandles.push_back (S.pSurfaceInst);
   }

   // --- World instance array ---

   if (!aInstanceHandles.empty ())
   {
      S.pWorldInstArr = anariNewArray1D (m_pDevice, aInstanceHandles.data (), nullptr, nullptr,
                                          ANARI_INSTANCE, aInstanceHandles.size ());
      anariSetParameter (m_pDevice, m_pWorld, "instance", ANARI_ARRAY1D, &S.pWorldInstArr);
   }
   else
   {
      anariUnsetParameter (m_pDevice, m_pWorld, "instance");
   }

   // --- Scene lights ---
   //
   // Each star contributes a point light at its world position. When the scene
   // has no star (e.g. a planetary system loaded as the primary fabric, with
   // its sun in a parent fabric), fall back to a single ambient light so
   // nothing is lit from the scene origin.

   if (!m_aLight.empty ())
   {
      for (const auto& Light : m_aLight)
      {
         float lightColor[3] = { Light.r, Light.g, Light.b };

         if (Light.eType == LIGHT_DATA::kAMBIENT)
         {
            ANARILight pLight = anariNewLight (m_pDevice, "ambient");
            float ambRadiance = Light.dIntensity;
            anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, lightColor);
            anariSetParameter (m_pDevice, pLight, "radiance", ANARI_FLOAT32, &ambRadiance);
            anariCommitParameters (m_pDevice, pLight);
            S.aLight.push_back (pLight);
         }
         else if (Light.eType == LIGHT_DATA::kDIRECTIONAL)
         {
            ANARILight pLight = anariNewLight (m_pDevice, "directional");
            float dirDir[3] = { Light.x, Light.y, Light.z };
            float dirIrr    = Light.dIntensity;
            anariSetParameter (m_pDevice, pLight, "direction", ANARI_FLOAT32_VEC3, dirDir);
            anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, lightColor);
            anariSetParameter (m_pDevice, pLight, "irradiance", ANARI_FLOAT32, &dirIrr);
            anariCommitParameters (m_pDevice, pLight);
            S.aLight.push_back (pLight);
         }
         else
         {
            ANARILight pLight = anariNewLight (m_pDevice, "point");
            float lightPos[3]    = { Light.x, Light.y, Light.z };
            float lightIntensity = Light.dIntensity;
            anariSetParameter (m_pDevice, pLight, "position", ANARI_FLOAT32_VEC3, lightPos);
            anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, lightColor);
            anariSetParameter (m_pDevice, pLight, "intensity", ANARI_FLOAT32, &lightIntensity);
            anariCommitParameters (m_pDevice, pLight);
            S.aLight.push_back (pLight);
         }
      }
   }
   else
   {
      // No star in the scene: fill with ambient and add a strong directional key
      // light from above so geometry reads with shape (Filament's ambient term is
      // weak fill on its own without an environment map).
      ANARILight pAmbient = anariNewLight (m_pDevice, "ambient");
      float ambColor[3]   = { 1.0f, 1.0f, 1.0f };
      float ambRadiance   = 3.0f;
      anariSetParameter (m_pDevice, pAmbient, "color", ANARI_FLOAT32_VEC3, ambColor);
      anariSetParameter (m_pDevice, pAmbient, "radiance", ANARI_FLOAT32, &ambRadiance);
      anariCommitParameters (m_pDevice, pAmbient);
      S.aLight.push_back (pAmbient);

      ANARILight pDir   = anariNewLight (m_pDevice, "directional");
      float dirDir[3]   = { -0.4f, -1.0f, -0.3f };
      float dirColor[3] = { 1.0f, 1.0f, 1.0f };
      float dirIrr      = 1.0f;
      anariSetParameter (m_pDevice, pDir, "direction", ANARI_FLOAT32_VEC3, dirDir);
      anariSetParameter (m_pDevice, pDir, "color", ANARI_FLOAT32_VEC3, dirColor);
      anariSetParameter (m_pDevice, pDir, "irradiance", ANARI_FLOAT32, &dirIrr);
      anariCommitParameters (m_pDevice, pDir);
      S.aLight.push_back (pDir);
   }

   S.pLightArr = anariNewArray1D (m_pDevice, S.aLight.data (), nullptr, nullptr, ANARI_LIGHT, S.aLight.size ());
   anariSetParameter (m_pDevice, m_pWorld, "light", ANARI_ARRAY1D, &S.pLightArr);

   S.bBuilt = true;
}

// ---------------------------------------------------------------------------
//  UpdateScene — update transforms and curve positions (no object creation)
// ---------------------------------------------------------------------------

void RENDERER::ANARI::UpdateScene (const std::vector<SPHERE_DATA>& aSphere_Data, const std::vector<CURVE_DATA>& aCurve_Data, const std::vector<BOX_DATA>& aBox_Data, const std::vector<PANEL_DATA>& aPanel_Data, const std::vector<MESH_DATA>& aMesh_Data)
{
   SCENE_STATE& S = *m_pSceneState;

   for (size_t i = 0; i < aSphere_Data.size ()  &&  i < S.aSphere_Entry.size (); i++)
   {
      const SPHERE_DATA& s = aSphere_Data[i];
      SCENE_STATE::SPHERE_ENTRY& entry = S.aSphere_Entry[i];

      if (entry.bTextured)
      {
         float xfm[16] =
         {
            s.dRadius, 0.0f,      0.0f,      0.0f,
            0.0f,      s.dRadius, 0.0f,      0.0f,
            0.0f,      0.0f,      s.dRadius, 0.0f,
            s.x,       s.y,       s.z,       1.0f,
         };
         anariSetParameter (m_pDevice, entry.pInst, "transform", ANARI_FLOAT32_MAT4, xfm);
         anariCommitParameters (m_pDevice, entry.pInst);
      }
      else
      {
         float pos[3] = { s.x, s.y, s.z };
         ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, &pos, nullptr, nullptr, ANARI_FLOAT32_VEC3, 1);
         anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
         anariSetParameter (m_pDevice, entry.pGeom, "radius", ANARI_FLOAT32, &s.dRadius);
         anariCommitParameters (m_pDevice, entry.pGeom);
         anariRelease (m_pDevice, pPosArr);
      }
   }

   size_t nCurveIz = 0;
   for (const auto& c : aCurve_Data)
   {
      if (c.aPoints.empty ()) continue;
      if (nCurveIz >= S.aCurve_Entry.size ()) break;

      SCENE_STATE::CURVE_ENTRY& entry = S.aCurve_Entry[nCurveIz];

      std::vector<float> aPos;
      aPos.reserve (c.aPoints.size () * 3);
      for (const auto& p : c.aPoints)
      {
         aPos.push_back (p.x);
         aPos.push_back (p.y);
         aPos.push_back (p.z);
      }

      ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, aPos.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, c.aPoints.size ());
      anariSetParameter (m_pDevice, entry.pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
      anariCommitParameters (m_pDevice, entry.pGeom);
      anariRelease (m_pDevice, pPosArr);

      nCurveIz++;
   }

   for (size_t i = 0; i < aBox_Data.size ()  &&  i < S.aBox_Entry.size (); i++)
   {
      anariSetParameter (m_pDevice, S.aBox_Entry[i].pInst, "transform", ANARI_FLOAT32_MAT4, aBox_Data[i].m16);
      anariCommitParameters (m_pDevice, S.aBox_Entry[i].pInst);
   }

   for (size_t i = 0; i < aPanel_Data.size ()  &&  i < S.aPanel_Entry.size (); i++)
   {
      anariSetParameter (m_pDevice, S.aPanel_Entry[i].pInst, "transform", ANARI_FLOAT32_MAT4, aPanel_Data[i].m16);
      anariCommitParameters (m_pDevice, S.aPanel_Entry[i].pInst);
   }

   for (size_t i = 0; i < aMesh_Data.size ()  &&  i < S.aMesh_Entry.size (); i++)
   {
      anariSetParameter (m_pDevice, S.aMesh_Entry[i].pInst, "transform", ANARI_FLOAT32_MAT4, aMesh_Data[i].m16);
      anariCommitParameters (m_pDevice, S.aMesh_Entry[i].pInst);
   }
}


