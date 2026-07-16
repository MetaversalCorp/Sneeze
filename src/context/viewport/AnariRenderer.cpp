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

   ANARIArray1D  pSharedPositionArray = nullptr;
   ANARIArray1D  pSharedNormalArray   = nullptr;
   ANARIArray1D  pSharedIndexArray    = nullptr;

   ANARIArray1D  pBoxPositionArray = nullptr;
   ANARIArray1D  pBoxNormalArray   = nullptr;
   ANARIArray1D  pBoxIndexArray    = nullptr;

   ANARIArray1D  pQuadPositionArray = nullptr;
   ANARIArray1D  pQuadNormalArray   = nullptr;
   ANARIArray1D  pQuadUvArray       = nullptr;
   ANARIArray1D  pQuadIndexArray    = nullptr;

   std::vector<ANARILight> aLight;
   ANARIArray1D            pLightArray = nullptr;

   ANARIGroup    pSurfaceGroup    = nullptr;
   ANARIInstance pSurfaceInstance = nullptr;

   ANARIArray1D  pWorldInstanceArray = nullptr;

   struct SPHERE_ENTRY
   {
      bool           bTextured    = false;
      const uint8_t* pTextureKey  = nullptr;
      ANARIGeometry  pGeometry    = nullptr;
      ANARIArray1D   pColorArray  = nullptr;
      ANARIMaterial  pMaterial    = nullptr;
      ANARISurface   pSurface     = nullptr;
      ANARIGroup     pGroup       = nullptr;
      ANARIInstance  pInstance    = nullptr;
   };

   struct CURVE_ENTRY
   {
      ANARIGeometry pGeometry   = nullptr;
      ANARIMaterial pMaterial   = nullptr;
      ANARISurface  pSurface    = nullptr;
      size_t        nPointCount = 0;
   };

   struct BOX_ENTRY
   {
      ANARIGeometry pGeometry = nullptr;
      ANARIMaterial pMaterial = nullptr;
      ANARISurface  pSurface  = nullptr;
      ANARIGroup    pGroup    = nullptr;
      ANARIInstance pInstance = nullptr;
   };

   // One in-scene UI panel: an unlit, alpha-blended textured quad. Geometry is
   // the shared unit quad (pQuad* arrays); each panel owns its image/sampler/
   // material/instance. pPixelKey detects when a panel's canvas pointer changes.
   struct PANEL_ENTRY
   {
      const uint8_t* pPixelKey   = nullptr;
      ANARIArray2D   pImageArray = nullptr;
      ANARISampler   pSampler    = nullptr;
      ANARIGeometry  pGeometry   = nullptr;
      ANARIMaterial  pMaterial   = nullptr;
      ANARISurface   pSurface    = nullptr;
      ANARIGroup     pGroup      = nullptr;
      ANARIInstance  pInstance   = nullptr;
   };

   // One drawable from a loaded glTF: indexed triangle geometry, a metallic-
   // roughness material (base color either a factor or an image2D sampler), and
   // a per-mesh instance carrying the world transform (refreshed per frame by
   // UpdateScene). pVertexKey detects when the source vertex buffer changes
   // (model reload); pTextureKey detects a base-color texture swap.
   struct MESH_ENTRY
   {
      const float*   pVertexKey     = nullptr;
      const uint8_t* pTextureKey    = nullptr;
      ANARIArray1D   pPositionArray = nullptr;
      ANARIArray1D   pNormalArray   = nullptr;
      ANARIArray1D   pUvArray       = nullptr;
      ANARIArray1D   pIndexArray    = nullptr;
      ANARIArray2D   pImageArray    = nullptr;
      ANARISampler   pSampler       = nullptr;
      ANARIGeometry  pGeometry      = nullptr;
      ANARIMaterial  pMaterial      = nullptr;
      ANARISurface   pSurface       = nullptr;
      ANARIGroup     pGroup         = nullptr;
      ANARIInstance  pInstance      = nullptr;
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

      float afBackground[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
      anariSetParameter (m_pDevice, m_pRenderer, "background", ANARI_FLOAT32_VEC4, afBackground);
      RGB   rgbAmbient       = { 0.0f, 0.0f, 0.0f };
      float fAmbientRadiance = 0.0f;
      anariSetParameter (m_pDevice, m_pRenderer, "ambientColor", ANARI_FLOAT32_VEC3, &rgbAmbient);
      anariSetParameter (m_pDevice, m_pRenderer, "ambientRadiance", ANARI_FLOAT32, &fAmbientRadiance);
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

void RENDERER::ANARI::SetCamera (const CAMERA_DATA& Camera_Data)
{
   float afPosition[3]  = { static_cast<float> (Camera_Data.vPosition.dX),  static_cast<float> (Camera_Data.vPosition.dY),  static_cast<float> (Camera_Data.vPosition.dZ) };
   float afDirection[3] = { static_cast<float> (Camera_Data.vDirection.dX), static_cast<float> (Camera_Data.vDirection.dY), static_cast<float> (Camera_Data.vDirection.dZ) };
   float afUp[3]        = { static_cast<float> (Camera_Data.vUp.dX),        static_cast<float> (Camera_Data.vUp.dY),        static_cast<float> (Camera_Data.vUp.dZ) };

   anariSetParameter (m_pDevice, m_pCamera, "position",  ANARI_FLOAT32_VEC3, afPosition);
   anariSetParameter (m_pDevice, m_pCamera, "direction", ANARI_FLOAT32_VEC3, afDirection);
   anariSetParameter (m_pDevice, m_pCamera, "up",        ANARI_FLOAT32_VEC3, afUp);
   anariSetParameter (m_pDevice, m_pCamera, "fovy",      ANARI_FLOAT32, &Camera_Data.fFovY);
   anariSetParameter (m_pDevice, m_pCamera, "aspect",    ANARI_FLOAT32, &Camera_Data.fAspect);
   anariSetParameter (m_pDevice, m_pCamera, "near",      ANARI_FLOAT32, &Camera_Data.fNear);
   anariSetParameter (m_pDevice, m_pCamera, "far",       ANARI_FLOAT32, &Camera_Data.fFar);
   anariCommitParameters (m_pDevice, m_pCamera);
}

void RENDERER::ANARI::SetBackground (float fRed, float fGreen, float fBlue, float fAlpha)
{
   if (m_pDevice  &&  m_pRenderer)
   {
      float afBackground[4] = { fRed, fGreen, fBlue, fAlpha };
      anariSetParameter (m_pDevice, m_pRenderer, "background", ANARI_FLOAT32_VEC4, afBackground);
      anariCommitParameters (m_pDevice, m_pRenderer);
   }
}

void RENDERER::ANARI::SetLights (const std::vector<LIGHT_DATA>& aLight)
{
   if (aLight.size () != m_aLight.size ())
      m_bSceneDirty = true;

   m_aLight = aLight;
}

void RENDERER::ANARI::SetSceneLighting (const SCENE_LIGHT& Ambient, const SCENE_LIGHT& Directional)
{
   bool bChanged = false;
   bChanged |= (Ambient.rgbColor.fR != m_Ambient.rgbColor.fR  ||  Ambient.rgbColor.fG != m_Ambient.rgbColor.fG  ||  Ambient.rgbColor.fB != m_Ambient.rgbColor.fB  ||  Ambient.fIntensity != m_Ambient.fIntensity);
   bChanged |= (Directional.rgbColor.fR != m_Directional.rgbColor.fR  ||  Directional.rgbColor.fG != m_Directional.rgbColor.fG  ||  Directional.rgbColor.fB != m_Directional.rgbColor.fB  ||  Directional.fIntensity != m_Directional.fIntensity);
   bChanged |= (Directional.vDirection.dX != m_Directional.vDirection.dX  ||  Directional.vDirection.dY != m_Directional.vDirection.dY  ||  Directional.vDirection.dZ != m_Directional.vDirection.dZ);

   if (bChanged)
      m_bSceneDirty = true;

   m_Ambient     = Ambient;
   m_Directional = Directional;
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
         // Only a model reload (vertex buffer) or a base-color texture swap
         // needs a rebuild. A changed world transform does NOT -- it rides the
         // instance and is refreshed cheaply by UpdateScene each frame.
         if (aMesh_Data[i].pfPosition != S.aMesh_Entry[i].pVertexKey  ||  aMesh_Data[i].pbTexturePixels != S.aMesh_Entry[i].pTextureKey)
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
         if (aPanel_Data[i].pbPixels != S.aPanel_Entry[i].pPixelKey)
         {
            bRebuild = true;
            break;
         }
      }
   }

   if (!bRebuild)
   {
      size_t nCurveCount = 0;
      for (const CURVE_DATA& Curve_Data : aCurve_Data)
      {
         if (!Curve_Data.aPoints.empty ())
            nCurveCount++;
      }
      if (nCurveCount != S.aCurve_Entry.size ())
         bRebuild = true;
   }

   if (!bRebuild)
   {
      for (size_t i = 0; i < aSphere_Data.size (); i++)
      {
         bool bNowTextured = (aSphere_Data[i].pbTexturePixels  &&  aSphere_Data[i].dimTexture.nW > 0  &&  aSphere_Data[i].dimTexture.nH > 0);
         if (bNowTextured != S.aSphere_Entry[i].bTextured)
         {
            bRebuild = true;
            break;
         }
         if (bNowTextured  &&  aSphere_Data[i].pbTexturePixels != S.aSphere_Entry[i].pTextureKey)
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

   for (SCENE_STATE::SPHERE_ENTRY& Sphere_Entry : S.aSphere_Entry)
   {
      if (Sphere_Entry.pInstance)   anariRelease (m_pDevice, Sphere_Entry.pInstance);
      if (Sphere_Entry.pGroup)      anariRelease (m_pDevice, Sphere_Entry.pGroup);
      if (Sphere_Entry.pSurface)    anariRelease (m_pDevice, Sphere_Entry.pSurface);
      if (Sphere_Entry.pMaterial)   anariRelease (m_pDevice, Sphere_Entry.pMaterial);
      if (Sphere_Entry.pColorArray) anariRelease (m_pDevice, Sphere_Entry.pColorArray);
      if (Sphere_Entry.pGeometry)   anariRelease (m_pDevice, Sphere_Entry.pGeometry);
   }
   S.aSphere_Entry.clear ();

   for (SCENE_STATE::CURVE_ENTRY& Curve_Entry : S.aCurve_Entry)
   {
      if (Curve_Entry.pSurface)  anariRelease (m_pDevice, Curve_Entry.pSurface);
      if (Curve_Entry.pMaterial) anariRelease (m_pDevice, Curve_Entry.pMaterial);
      if (Curve_Entry.pGeometry) anariRelease (m_pDevice, Curve_Entry.pGeometry);
   }
   S.aCurve_Entry.clear ();

   for (SCENE_STATE::BOX_ENTRY& Box_Entry : S.aBox_Entry)
   {
      if (Box_Entry.pInstance) anariRelease (m_pDevice, Box_Entry.pInstance);
      if (Box_Entry.pGroup)    anariRelease (m_pDevice, Box_Entry.pGroup);
      if (Box_Entry.pSurface)  anariRelease (m_pDevice, Box_Entry.pSurface);
      if (Box_Entry.pMaterial) anariRelease (m_pDevice, Box_Entry.pMaterial);
      if (Box_Entry.pGeometry) anariRelease (m_pDevice, Box_Entry.pGeometry);
   }
   S.aBox_Entry.clear ();

   for (SCENE_STATE::PANEL_ENTRY& Panel_Entry : S.aPanel_Entry)
   {
      if (Panel_Entry.pInstance)   anariRelease (m_pDevice, Panel_Entry.pInstance);
      if (Panel_Entry.pGroup)      anariRelease (m_pDevice, Panel_Entry.pGroup);
      if (Panel_Entry.pSurface)    anariRelease (m_pDevice, Panel_Entry.pSurface);
      if (Panel_Entry.pMaterial)   anariRelease (m_pDevice, Panel_Entry.pMaterial);
      if (Panel_Entry.pSampler)    anariRelease (m_pDevice, Panel_Entry.pSampler);
      if (Panel_Entry.pImageArray) anariRelease (m_pDevice, Panel_Entry.pImageArray);
      if (Panel_Entry.pGeometry)   anariRelease (m_pDevice, Panel_Entry.pGeometry);
   }
   S.aPanel_Entry.clear ();

   for (SCENE_STATE::MESH_ENTRY& Mesh_Entry : S.aMesh_Entry)
   {
      if (Mesh_Entry.pInstance)      anariRelease (m_pDevice, Mesh_Entry.pInstance);
      if (Mesh_Entry.pGroup)         anariRelease (m_pDevice, Mesh_Entry.pGroup);
      if (Mesh_Entry.pSurface)       anariRelease (m_pDevice, Mesh_Entry.pSurface);
      if (Mesh_Entry.pMaterial)      anariRelease (m_pDevice, Mesh_Entry.pMaterial);
      if (Mesh_Entry.pSampler)       anariRelease (m_pDevice, Mesh_Entry.pSampler);
      if (Mesh_Entry.pImageArray)    anariRelease (m_pDevice, Mesh_Entry.pImageArray);
      if (Mesh_Entry.pIndexArray)    anariRelease (m_pDevice, Mesh_Entry.pIndexArray);
      if (Mesh_Entry.pUvArray)       anariRelease (m_pDevice, Mesh_Entry.pUvArray);
      if (Mesh_Entry.pNormalArray)   anariRelease (m_pDevice, Mesh_Entry.pNormalArray);
      if (Mesh_Entry.pPositionArray) anariRelease (m_pDevice, Mesh_Entry.pPositionArray);
      if (Mesh_Entry.pGeometry)      anariRelease (m_pDevice, Mesh_Entry.pGeometry);
   }
   S.aMesh_Entry.clear ();

   if (S.pQuadIndexArray)    { anariRelease (m_pDevice, S.pQuadIndexArray);    S.pQuadIndexArray    = nullptr; }
   if (S.pQuadUvArray)       { anariRelease (m_pDevice, S.pQuadUvArray);       S.pQuadUvArray       = nullptr; }
   if (S.pQuadNormalArray)   { anariRelease (m_pDevice, S.pQuadNormalArray);   S.pQuadNormalArray   = nullptr; }
   if (S.pQuadPositionArray) { anariRelease (m_pDevice, S.pQuadPositionArray); S.pQuadPositionArray = nullptr; }

   if (S.pWorldInstanceArray) { anariRelease (m_pDevice, S.pWorldInstanceArray); S.pWorldInstanceArray = nullptr; }
   if (S.pSurfaceInstance)    { anariRelease (m_pDevice, S.pSurfaceInstance);    S.pSurfaceInstance    = nullptr; }
   if (S.pSurfaceGroup)       { anariRelease (m_pDevice, S.pSurfaceGroup);       S.pSurfaceGroup       = nullptr; }
   if (S.pLightArray)         { anariRelease (m_pDevice, S.pLightArray);         S.pLightArray         = nullptr; }
   for (ANARILight pLight : S.aLight)  anariRelease (m_pDevice, pLight);
   S.aLight.clear ();
   if (S.pSharedIndexArray)    { anariRelease (m_pDevice, S.pSharedIndexArray);    S.pSharedIndexArray    = nullptr; }
   if (S.pSharedNormalArray)   { anariRelease (m_pDevice, S.pSharedNormalArray);   S.pSharedNormalArray   = nullptr; }
   if (S.pSharedPositionArray) { anariRelease (m_pDevice, S.pSharedPositionArray); S.pSharedPositionArray = nullptr; }

   if (S.pBoxIndexArray)    { anariRelease (m_pDevice, S.pBoxIndexArray);    S.pBoxIndexArray    = nullptr; }
   if (S.pBoxNormalArray)   { anariRelease (m_pDevice, S.pBoxNormalArray);   S.pBoxNormalArray   = nullptr; }
   if (S.pBoxPositionArray) { anariRelease (m_pDevice, S.pBoxPositionArray); S.pBoxPositionArray = nullptr; }

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

   uint64_t nVertexCount   = m_pUnitSphere.aPositions.size () / 3;
   uint64_t nTriangleCount = m_pUnitSphere.aIndices.size () / 3;

   S.pSharedPositionArray = anariNewArray1D (m_pDevice, m_pUnitSphere.aPositions.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nVertexCount);
   S.pSharedNormalArray   = anariNewArray1D (m_pDevice, m_pUnitSphere.aNormals.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nVertexCount);
   S.pSharedIndexArray    = anariNewArray1D (m_pDevice, m_pUnitSphere.aIndices.data (), nullptr, nullptr, ANARI_UINT32_VEC3, nTriangleCount);

   std::vector<ANARISurface>  aSurfaceHandle;
   std::vector<ANARIInstance> aInstanceHandle;

   // --- Spheres ---

   for (const SPHERE_DATA& Sphere_Data : aSphere_Data)
   {
      SCENE_STATE::SPHERE_ENTRY Sphere_Entry;

      if (Sphere_Data.pbTexturePixels  &&  Sphere_Data.dimTexture.nW > 0  &&  Sphere_Data.dimTexture.nH > 0)
      {
         Sphere_Entry.bTextured   = true;
         Sphere_Entry.pTextureKey = Sphere_Data.pbTexturePixels;

         auto itColor = m_pColorCache.find (Sphere_Data.pbTexturePixels);
         if (itColor == m_pColorCache.end ())
         {
            float fBrightness = Sphere_Data.bEmissive ? 8.0f : 1.0f;
            std::vector<float> aColor;
            aColor.reserve (nVertexCount * 4);
            for (uint64_t i = 0; i < nVertexCount; i++)
            {
               float fU = m_pUnitSphere.aTexCoords[i * 2];
               float fV = m_pUnitSphere.aTexCoords[i * 2 + 1];
               int nPixelX = static_cast<int> (fU * (Sphere_Data.dimTexture.nW - 1) + 0.5f);
               int nPixelY = static_cast<int> (fV * (Sphere_Data.dimTexture.nH - 1) + 0.5f);
               if (nPixelX < 0) nPixelX = 0;
               if (nPixelX >= Sphere_Data.dimTexture.nW)  nPixelX = Sphere_Data.dimTexture.nW - 1;
               if (nPixelY < 0) nPixelY = 0;
               if (nPixelY >= Sphere_Data.dimTexture.nH) nPixelY = Sphere_Data.dimTexture.nH - 1;
               int nOffset = (nPixelY * Sphere_Data.dimTexture.nW + nPixelX) * 4;
               aColor.push_back (static_cast<float> (Sphere_Data.pbTexturePixels[nOffset])     / 255.0f * fBrightness);
               aColor.push_back (static_cast<float> (Sphere_Data.pbTexturePixels[nOffset + 1]) / 255.0f * fBrightness);
               aColor.push_back (static_cast<float> (Sphere_Data.pbTexturePixels[nOffset + 2]) / 255.0f * fBrightness);
               aColor.push_back (static_cast<float> (Sphere_Data.pbTexturePixels[nOffset + 3]) / 255.0f);
            }
            itColor = m_pColorCache.emplace (Sphere_Data.pbTexturePixels, std::move (aColor)).first;
         }

         const std::vector<float>& aColor = itColor->second;
         Sphere_Entry.pColorArray = anariNewArray1D (m_pDevice, aColor.data (), nullptr, nullptr, ANARI_FLOAT32_VEC4, nVertexCount);

         Sphere_Entry.pGeometry = anariNewGeometry (m_pDevice, "triangle");
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &S.pSharedPositionArray);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "vertex.normal",   ANARI_ARRAY1D, &S.pSharedNormalArray);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "vertex.color",    ANARI_ARRAY1D, &Sphere_Entry.pColorArray);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "primitive.index",  ANARI_ARRAY1D, &S.pSharedIndexArray);
         anariCommitParameters (m_pDevice, Sphere_Entry.pGeometry);

         Sphere_Entry.pMaterial = anariNewMaterial (m_pDevice, "matte");
         float afMaterialColor[3] = { 1.0f, 1.0f, 1.0f };
         anariSetParameter (m_pDevice, Sphere_Entry.pMaterial, "color", ANARI_FLOAT32_VEC3, afMaterialColor);
         anariCommitParameters (m_pDevice, Sphere_Entry.pMaterial);

         Sphere_Entry.pSurface = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, Sphere_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Sphere_Entry.pGeometry);
         anariSetParameter (m_pDevice, Sphere_Entry.pSurface, "material", ANARI_MATERIAL, &Sphere_Entry.pMaterial);
         anariCommitParameters (m_pDevice, Sphere_Entry.pSurface);

         ANARIArray1D pSurfaceArray = anariNewArray1D (m_pDevice, &Sphere_Entry.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
         Sphere_Entry.pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, Sphere_Entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
         anariCommitParameters (m_pDevice, Sphere_Entry.pGroup);
         anariRelease (m_pDevice, pSurfaceArray);

         float afTransform[16] =
         {
            Sphere_Data.fRadius, 0.0f,                0.0f,                0.0f,
            0.0f,                Sphere_Data.fRadius, 0.0f,                0.0f,
            0.0f,                0.0f,                Sphere_Data.fRadius, 0.0f,
            static_cast<float> (Sphere_Data.vPosition.dX), static_cast<float> (Sphere_Data.vPosition.dY), static_cast<float> (Sphere_Data.vPosition.dZ), 1.0f,
         };

         Sphere_Entry.pInstance = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, Sphere_Entry.pInstance, "group", ANARI_GROUP, &Sphere_Entry.pGroup);
         anariSetParameter (m_pDevice, Sphere_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, afTransform);
         anariCommitParameters (m_pDevice, Sphere_Entry.pInstance);

         aInstanceHandle.push_back (Sphere_Entry.pInstance);
      }
      else
      {
         Sphere_Entry.bTextured   = false;
         Sphere_Entry.pTextureKey = nullptr;

         Sphere_Entry.pGeometry = anariNewGeometry (m_pDevice, "sphere");
         float afPosition[3] = { static_cast<float> (Sphere_Data.vPosition.dX), static_cast<float> (Sphere_Data.vPosition.dY), static_cast<float> (Sphere_Data.vPosition.dZ) };
         ANARIArray1D pPositionArray = anariNewArray1D (m_pDevice, &afPosition, nullptr, nullptr, ANARI_FLOAT32_VEC3, 1);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &pPositionArray);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "radius", ANARI_FLOAT32, &Sphere_Data.fRadius);
         anariCommitParameters (m_pDevice, Sphere_Entry.pGeometry);
         anariRelease (m_pDevice, pPositionArray);

         Sphere_Entry.pMaterial = anariNewMaterial (m_pDevice, "matte");
         float afColor[3] = { Sphere_Data.rgbColor.fR, Sphere_Data.rgbColor.fG, Sphere_Data.rgbColor.fB };
         anariSetParameter (m_pDevice, Sphere_Entry.pMaterial, "color", ANARI_FLOAT32_VEC3, afColor);
         anariCommitParameters (m_pDevice, Sphere_Entry.pMaterial);

         Sphere_Entry.pSurface = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, Sphere_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Sphere_Entry.pGeometry);
         anariSetParameter (m_pDevice, Sphere_Entry.pSurface, "material", ANARI_MATERIAL, &Sphere_Entry.pMaterial);
         anariCommitParameters (m_pDevice, Sphere_Entry.pSurface);

         aSurfaceHandle.push_back (Sphere_Entry.pSurface);
      }

      S.aSphere_Entry.push_back (Sphere_Entry);
   }

   // --- Curves ---

   for (const CURVE_DATA& Curve_Data : aCurve_Data)
   {
      if (Curve_Data.aPoints.empty ()) continue;

      SCENE_STATE::CURVE_ENTRY Curve_Entry;
      Curve_Entry.nPointCount = Curve_Data.aPoints.size ();

      std::vector<float> aPosition;
      std::vector<float> aRadius;
      aPosition.reserve (Curve_Data.aPoints.size () * 3);
      aRadius.reserve (Curve_Data.aPoints.size ());

      for (const CURVE_POINT& Point : Curve_Data.aPoints)
      {
         aPosition.push_back (static_cast<float> (Point.vPosition.dX));
         aPosition.push_back (static_cast<float> (Point.vPosition.dY));
         aPosition.push_back (static_cast<float> (Point.vPosition.dZ));
         aRadius.push_back (Point.fRadius);
      }

      Curve_Entry.pGeometry = anariNewGeometry (m_pDevice, "curve");

      ANARIArray1D pPositionArray = anariNewArray1D (m_pDevice, aPosition.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, Curve_Data.aPoints.size ());
      ANARIArray1D pRadiusArray   = anariNewArray1D (m_pDevice, aRadius.data (),   nullptr, nullptr, ANARI_FLOAT32,      Curve_Data.aPoints.size ());

      anariSetParameter (m_pDevice, Curve_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &pPositionArray);
      anariSetParameter (m_pDevice, Curve_Entry.pGeometry, "vertex.radius", ANARI_ARRAY1D, &pRadiusArray);
      anariCommitParameters (m_pDevice, Curve_Entry.pGeometry);

      anariRelease (m_pDevice, pPositionArray);
      anariRelease (m_pDevice, pRadiusArray);

      Curve_Entry.pMaterial = anariNewMaterial (m_pDevice, "physicallyBased");
      float afBaseColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
      float afEmissive[3]  = { Curve_Data.rgbColor.fR, Curve_Data.rgbColor.fG, Curve_Data.rgbColor.fB };
      float fMetallic      = 0.0f;
      float fRoughness     = 1.0f;
      anariSetParameter (m_pDevice, Curve_Entry.pMaterial, "baseColor", ANARI_FLOAT32_VEC4, afBaseColor);
      anariSetParameter (m_pDevice, Curve_Entry.pMaterial, "metallic",  ANARI_FLOAT32,      &fMetallic);
      anariSetParameter (m_pDevice, Curve_Entry.pMaterial, "roughness", ANARI_FLOAT32,      &fRoughness);
      anariSetParameter (m_pDevice, Curve_Entry.pMaterial, "emissive",  ANARI_FLOAT32_VEC3, afEmissive);
      anariCommitParameters (m_pDevice, Curve_Entry.pMaterial);

      Curve_Entry.pSurface = anariNewSurface (m_pDevice);
      anariSetParameter (m_pDevice, Curve_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Curve_Entry.pGeometry);
      anariSetParameter (m_pDevice, Curve_Entry.pSurface, "material", ANARI_MATERIAL, &Curve_Entry.pMaterial);
      anariCommitParameters (m_pDevice, Curve_Entry.pSurface);

      aSurfaceHandle.push_back (Curve_Entry.pSurface);

      S.aCurve_Entry.push_back (Curve_Entry);
   }

   // --- Boxes (one instance per box, unit cube + per-box transform) ---

   if (!aBox_Data.empty ())
   {
      uint64_t nBoxVertexCount   = m_pUnitBox.aPositions.size () / 3;
      uint64_t nBoxTriangleCount = m_pUnitBox.aIndices.size () / 3;

      S.pBoxPositionArray = anariNewArray1D (m_pDevice, m_pUnitBox.aPositions.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nBoxVertexCount);
      S.pBoxNormalArray   = anariNewArray1D (m_pDevice, m_pUnitBox.aNormals.data (),   nullptr, nullptr, ANARI_FLOAT32_VEC3, nBoxVertexCount);
      S.pBoxIndexArray    = anariNewArray1D (m_pDevice, m_pUnitBox.aIndices.data (),   nullptr, nullptr, ANARI_UINT32_VEC3,  nBoxTriangleCount);

      for (const BOX_DATA& Box_Data : aBox_Data)
      {
         SCENE_STATE::BOX_ENTRY Box_Entry;

         Box_Entry.pGeometry = anariNewGeometry (m_pDevice, "triangle");
         anariSetParameter (m_pDevice, Box_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &S.pBoxPositionArray);
         anariSetParameter (m_pDevice, Box_Entry.pGeometry, "vertex.normal",   ANARI_ARRAY1D, &S.pBoxNormalArray);
         anariSetParameter (m_pDevice, Box_Entry.pGeometry, "primitive.index", ANARI_ARRAY1D, &S.pBoxIndexArray);
         anariCommitParameters (m_pDevice, Box_Entry.pGeometry);

         Box_Entry.pMaterial = anariNewMaterial (m_pDevice, "physicallyBased");
         float afBaseColor[4] = { Box_Data.rgbColor.fR, Box_Data.rgbColor.fG, Box_Data.rgbColor.fB, 1.0f };
         float fMetallic      = 0.0f;
         float fRoughness     = 0.85f;
         anariSetParameter (m_pDevice, Box_Entry.pMaterial, "baseColor", ANARI_FLOAT32_VEC4, afBaseColor);
         anariSetParameter (m_pDevice, Box_Entry.pMaterial, "metallic",  ANARI_FLOAT32,      &fMetallic);
         anariSetParameter (m_pDevice, Box_Entry.pMaterial, "roughness", ANARI_FLOAT32,      &fRoughness);
         anariCommitParameters (m_pDevice, Box_Entry.pMaterial);

         Box_Entry.pSurface = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, Box_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Box_Entry.pGeometry);
         anariSetParameter (m_pDevice, Box_Entry.pSurface, "material", ANARI_MATERIAL, &Box_Entry.pMaterial);
         anariCommitParameters (m_pDevice, Box_Entry.pSurface);

         ANARIArray1D pSurfaceArray = anariNewArray1D (m_pDevice, &Box_Entry.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
         Box_Entry.pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, Box_Entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
         anariCommitParameters (m_pDevice, Box_Entry.pGroup);
         anariRelease (m_pDevice, pSurfaceArray);

         Box_Entry.pInstance = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, Box_Entry.pInstance, "group", ANARI_GROUP, &Box_Entry.pGroup);
         anariSetParameter (m_pDevice, Box_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, Box_Data.mWorld.f);
         anariCommitParameters (m_pDevice, Box_Entry.pInstance);

         aInstanceHandle.push_back (Box_Entry.pInstance);

         S.aBox_Entry.push_back (Box_Entry);
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
      static const float aQuadPosition[12] = { -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.5f, 0.5f, 0.0f,  -0.5f, 0.5f, 0.0f };
      static const float aQuadNormal[12]   = {  0.0f,  0.0f, 1.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f };
      static const float aQuadUv[8]        = {  0.0f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 1.0f };
      static const uint32_t aQuadIndex[12] = { 0, 1, 2,  0, 2, 3,   0, 2, 1,  0, 3, 2 };

      S.pQuadPositionArray = anariNewArray1D (m_pDevice, aQuadPosition, nullptr, nullptr, ANARI_FLOAT32_VEC3, 4);
      S.pQuadNormalArray   = anariNewArray1D (m_pDevice, aQuadNormal,   nullptr, nullptr, ANARI_FLOAT32_VEC3, 4);
      S.pQuadUvArray       = anariNewArray1D (m_pDevice, aQuadUv,       nullptr, nullptr, ANARI_FLOAT32_VEC2, 4);
      S.pQuadIndexArray    = anariNewArray1D (m_pDevice, aQuadIndex,    nullptr, nullptr, ANARI_UINT32_VEC3, 4);

      for (const PANEL_DATA& Panel_Data : aPanel_Data)
      {
         SCENE_STATE::PANEL_ENTRY Panel_Entry;
         Panel_Entry.pPixelKey = Panel_Data.pbPixels;

         Panel_Entry.pGeometry = anariNewGeometry (m_pDevice, "triangle");
         anariSetParameter (m_pDevice, Panel_Entry.pGeometry, "vertex.position",   ANARI_ARRAY1D, &S.pQuadPositionArray);
         anariSetParameter (m_pDevice, Panel_Entry.pGeometry, "vertex.normal",     ANARI_ARRAY1D, &S.pQuadNormalArray);
         anariSetParameter (m_pDevice, Panel_Entry.pGeometry, "vertex.attribute0", ANARI_ARRAY1D, &S.pQuadUvArray);
         anariSetParameter (m_pDevice, Panel_Entry.pGeometry, "primitive.index",   ANARI_ARRAY1D, &S.pQuadIndexArray);
         anariCommitParameters (m_pDevice, Panel_Entry.pGeometry);

         // image2D wants CPU RGBA8; Halogen's convertToRGBA8 decodes plain
         // UFIXED8 variants (the _SRGB forms fall through to black), so use
         // UFIXED8_VEC4. Pixels arrive straight-alpha from the panel.
         Panel_Entry.pImageArray = anariNewArray2D (m_pDevice, Panel_Data.pbPixels, nullptr, nullptr, ANARI_UFIXED8_VEC4, Panel_Data.dim.nW, Panel_Data.dim.nH);

         Panel_Entry.pSampler = anariNewSampler (m_pDevice, "image2D");
         anariSetParameter (m_pDevice, Panel_Entry.pSampler, "image",  ANARI_ARRAY2D, &Panel_Entry.pImageArray);
         anariSetParameter (m_pDevice, Panel_Entry.pSampler, "filter", ANARI_STRING,  "linear");
         anariCommitParameters (m_pDevice, Panel_Entry.pSampler);

         // HALOGEN_MATERIAL_UNLIT: emits the sampled texel directly, lighting-
         // independent -- the correct model for UI. Per-texel alpha rides the
         // texture under alphaMode "blend".
         Panel_Entry.pMaterial = anariNewMaterial (m_pDevice, "unlit");
         anariSetParameter (m_pDevice, Panel_Entry.pMaterial, "alphaMode", ANARI_STRING, "blend");
         anariSetParameter (m_pDevice, Panel_Entry.pMaterial, "color", ANARI_SAMPLER, &Panel_Entry.pSampler);
         anariCommitParameters (m_pDevice, Panel_Entry.pMaterial);

         Panel_Entry.pSurface = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, Panel_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Panel_Entry.pGeometry);
         anariSetParameter (m_pDevice, Panel_Entry.pSurface, "material", ANARI_MATERIAL, &Panel_Entry.pMaterial);
         anariCommitParameters (m_pDevice, Panel_Entry.pSurface);

         ANARIArray1D pSurfaceArray = anariNewArray1D (m_pDevice, &Panel_Entry.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
         Panel_Entry.pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, Panel_Entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
         anariCommitParameters (m_pDevice, Panel_Entry.pGroup);
         anariRelease (m_pDevice, pSurfaceArray);

         Panel_Entry.pInstance = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, Panel_Entry.pInstance, "group", ANARI_GROUP, &Panel_Entry.pGroup);
         anariSetParameter (m_pDevice, Panel_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, Panel_Data.mWorld.f);
         anariCommitParameters (m_pDevice, Panel_Entry.pInstance);

         aInstanceHandle.push_back (Panel_Entry.pInstance);

         S.aPanel_Entry.push_back (Panel_Entry);
      }
   }

   // --- Meshes (loaded glTF; one instance per primitive, per-instance world
   //     transform) ---
   //
   // The node's world transform rides the ANARI instance (set below and
   // refreshed each frame by UpdateScene) exactly like spheres, boxes and
   // panels. Vertex streams pass through model-local and untouched, so a moving
   // node or a shifting render scale is a cheap 16-float matrix swap rather than
   // a full CPU re-bake + re-upload of every vertex.

   for (const MESH_DATA& Mesh_Data : aMesh_Data)
   {
      if (!Mesh_Data.pfPosition  ||  Mesh_Data.uCount_Vertex == 0)
         continue;

      uint64_t nVertexCount = Mesh_Data.uCount_Vertex;

      bool bTextured = Mesh_Data.pbTexturePixels  &&  Mesh_Data.dimTexture.nW > 0  &&  Mesh_Data.dimTexture.nH > 0  &&  Mesh_Data.pfTexCoord;

      SCENE_STATE::MESH_ENTRY Mesh_Entry;
      Mesh_Entry.pVertexKey  = Mesh_Data.pfPosition;
      Mesh_Entry.pTextureKey = Mesh_Data.pbTexturePixels;

      Mesh_Entry.pPositionArray = anariNewArray1D (m_pDevice, Mesh_Data.pfPosition, nullptr, nullptr, ANARI_FLOAT32_VEC3, nVertexCount);

      Mesh_Entry.pGeometry = anariNewGeometry (m_pDevice, "triangle");
      anariSetParameter (m_pDevice, Mesh_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &Mesh_Entry.pPositionArray);

      if (Mesh_Data.pfNormal)
      {
         Mesh_Entry.pNormalArray = anariNewArray1D (m_pDevice, Mesh_Data.pfNormal, nullptr, nullptr, ANARI_FLOAT32_VEC3, nVertexCount);
         anariSetParameter (m_pDevice, Mesh_Entry.pGeometry, "vertex.normal", ANARI_ARRAY1D, &Mesh_Entry.pNormalArray);
      }

      if (Mesh_Data.pfTexCoord)
      {
         Mesh_Entry.pUvArray = anariNewArray1D (m_pDevice, Mesh_Data.pfTexCoord, nullptr, nullptr, ANARI_FLOAT32_VEC2, nVertexCount);
         anariSetParameter (m_pDevice, Mesh_Entry.pGeometry, "vertex.attribute0", ANARI_ARRAY1D, &Mesh_Entry.pUvArray);
      }

      if (Mesh_Data.puIndex  &&  Mesh_Data.uCount_Index >= 3)
      {
         Mesh_Entry.pIndexArray = anariNewArray1D (m_pDevice, Mesh_Data.puIndex, nullptr, nullptr, ANARI_UINT32_VEC3, Mesh_Data.uCount_Index / 3);
         anariSetParameter (m_pDevice, Mesh_Entry.pGeometry, "primitive.index", ANARI_ARRAY1D, &Mesh_Entry.pIndexArray);
      }
      anariCommitParameters (m_pDevice, Mesh_Entry.pGeometry);

      Mesh_Entry.pMaterial = anariNewMaterial (m_pDevice, "physicallyBased");
      if (bTextured)
      {
         Mesh_Entry.pImageArray = anariNewArray2D (m_pDevice, Mesh_Data.pbTexturePixels, nullptr, nullptr, ANARI_UFIXED8_VEC4, Mesh_Data.dimTexture.nW, Mesh_Data.dimTexture.nH);

         Mesh_Entry.pSampler = anariNewSampler (m_pDevice, "image2D");
         anariSetParameter (m_pDevice, Mesh_Entry.pSampler, "image",       ANARI_ARRAY2D, &Mesh_Entry.pImageArray);
         anariSetParameter (m_pDevice, Mesh_Entry.pSampler, "inAttribute", ANARI_STRING,  "attribute0");
         anariSetParameter (m_pDevice, Mesh_Entry.pSampler, "filter",      ANARI_STRING,  "linear");
         anariCommitParameters (m_pDevice, Mesh_Entry.pSampler);

         anariSetParameter (m_pDevice, Mesh_Entry.pMaterial, "baseColor", ANARI_SAMPLER, &Mesh_Entry.pSampler);
      }
      else
      {
         anariSetParameter (m_pDevice, Mesh_Entry.pMaterial, "baseColor", ANARI_FLOAT32_VEC4, &Mesh_Data.rgbaBaseColor);
      }
      anariSetParameter (m_pDevice, Mesh_Entry.pMaterial, "metallic",  ANARI_FLOAT32,      &Mesh_Data.fMetallic);
      anariSetParameter (m_pDevice, Mesh_Entry.pMaterial, "roughness", ANARI_FLOAT32,      &Mesh_Data.fRoughness);
      anariSetParameter (m_pDevice, Mesh_Entry.pMaterial, "emissive",  ANARI_FLOAT32_VEC3, &Mesh_Data.rgbEmissive);
      anariCommitParameters (m_pDevice, Mesh_Entry.pMaterial);

      Mesh_Entry.pSurface = anariNewSurface (m_pDevice);
      anariSetParameter (m_pDevice, Mesh_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Mesh_Entry.pGeometry);
      anariSetParameter (m_pDevice, Mesh_Entry.pSurface, "material", ANARI_MATERIAL, &Mesh_Entry.pMaterial);
      anariCommitParameters (m_pDevice, Mesh_Entry.pSurface);

      ANARIArray1D pSurfaceArray = anariNewArray1D (m_pDevice, &Mesh_Entry.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
      Mesh_Entry.pGroup = anariNewGroup (m_pDevice);
      anariSetParameter (m_pDevice, Mesh_Entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
      anariCommitParameters (m_pDevice, Mesh_Entry.pGroup);
      anariRelease (m_pDevice, pSurfaceArray);

      // Per-instance world transform, refreshed each frame by UpdateScene.
      Mesh_Entry.pInstance = anariNewInstance (m_pDevice, "transform");
      anariSetParameter (m_pDevice, Mesh_Entry.pInstance, "group", ANARI_GROUP, &Mesh_Entry.pGroup);
      anariSetParameter (m_pDevice, Mesh_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, Mesh_Data.mWorld.f);
      anariCommitParameters (m_pDevice, Mesh_Entry.pInstance);

      aInstanceHandle.push_back (Mesh_Entry.pInstance);

      S.aMesh_Entry.push_back (Mesh_Entry);
   }

   // --- Surface group for analytical spheres + curves ---

   if (!aSurfaceHandle.empty ())
   {
      ANARIArray1D pSurfaceArray = anariNewArray1D (m_pDevice, aSurfaceHandle.data (), nullptr, nullptr, ANARI_SURFACE, aSurfaceHandle.size ());
      S.pSurfaceGroup = anariNewGroup (m_pDevice);
      anariSetParameter (m_pDevice, S.pSurfaceGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
      anariCommitParameters (m_pDevice, S.pSurfaceGroup);
      anariRelease (m_pDevice, pSurfaceArray);

      S.pSurfaceInstance = anariNewInstance (m_pDevice, "transform");
      anariSetParameter (m_pDevice, S.pSurfaceInstance, "group", ANARI_GROUP, &S.pSurfaceGroup);
      anariCommitParameters (m_pDevice, S.pSurfaceInstance);

      aInstanceHandle.push_back (S.pSurfaceInstance);
   }

   // --- World instance array ---

   if (!aInstanceHandle.empty ())
   {
      S.pWorldInstanceArray = anariNewArray1D (m_pDevice, aInstanceHandle.data (), nullptr, nullptr, ANARI_INSTANCE, aInstanceHandle.size ());
      anariSetParameter (m_pDevice, m_pWorld, "instance", ANARI_ARRAY1D, &S.pWorldInstanceArray);
   }
   else
   {
      anariUnsetParameter (m_pDevice, m_pWorld, "instance");
   }

   // --- Scene lights ---
   //
   // Each star contributes a point light at its world position; the primary
   // fabric additionally contributes the scene-global ambient and directional.

   // Scene-global ambient: routed to the renderer's own ambient term, fed from
   // the scene's ambient property (not an ANARI light object -- Halogen ignores
   // an ANARI "ambient" light). Default intensity zero => no ambient added.
   RGB   rgbAmbient       = m_Ambient.rgbColor;
   float fAmbientRadiance = m_Ambient.fIntensity;
   anariSetParameter (m_pDevice, m_pRenderer, "ambientColor", ANARI_FLOAT32_VEC3, &rgbAmbient);
   anariSetParameter (m_pDevice, m_pRenderer, "ambientRadiance", ANARI_FLOAT32, &fAmbientRadiance);
   anariCommitParameters (m_pDevice, m_pRenderer);

   // Scene-global directional ("sun"): one ANARI directional light built from the
   // scene's directional property, omitted when its intensity is zero.
   if (m_Directional.fIntensity > 0.0f)
   {
      ANARILight pLight = anariNewLight (m_pDevice, "directional");
      float afDirection[3] = { static_cast<float> (m_Directional.vDirection.dX), static_cast<float> (m_Directional.vDirection.dY), static_cast<float> (m_Directional.vDirection.dZ) };
      float fIrradiance    = m_Directional.fIntensity;
      anariSetParameter (m_pDevice, pLight, "direction", ANARI_FLOAT32_VEC3, afDirection);
      anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, &m_Directional.rgbColor);
      anariSetParameter (m_pDevice, pLight, "irradiance", ANARI_FLOAT32, &fIrradiance);
      anariCommitParameters (m_pDevice, pLight);
      S.aLight.push_back (pLight);
   }

   // Placed lights (point / spot) gathered during traversal. Ambient and
   // directional are scene-global and handled above, never as LIGHT_DATA.
   for (const LIGHT_DATA& Light : m_aLight)
   {
      if (Light.eType == LIGHT_DATA::kSPOT  ||  Light.eType == LIGHT_DATA::kSPOT__DEPRECATED)
      {
         ANARILight pLight = anariNewLight (m_pDevice, "spot");
         float afPosition[3]  = { static_cast<float> (Light.vPosition.dX), static_cast<float> (Light.vPosition.dY), static_cast<float> (Light.vPosition.dZ) };
         float afDirection[3] = { static_cast<float> (Light.vDirection.dX), static_cast<float> (Light.vDirection.dY), static_cast<float> (Light.vDirection.dZ) };
         float fIntensity     = Light.fIntensity;
         float fOpeningAngle  = Light.fOpeningAngle;
         float fFalloffAngle  = Light.fFalloffAngle;
         anariSetParameter (m_pDevice, pLight, "position", ANARI_FLOAT32_VEC3, afPosition);
         anariSetParameter (m_pDevice, pLight, "direction", ANARI_FLOAT32_VEC3, afDirection);
         anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, &Light.rgbColor);
         anariSetParameter (m_pDevice, pLight, "intensity", ANARI_FLOAT32, &fIntensity);
         anariSetParameter (m_pDevice, pLight, "openingAngle", ANARI_FLOAT32, &fOpeningAngle);
         anariSetParameter (m_pDevice, pLight, "falloffAngle", ANARI_FLOAT32, &fFalloffAngle);
         anariCommitParameters (m_pDevice, pLight);
         S.aLight.push_back (pLight);
      }
      else if (Light.eType == LIGHT_DATA::kPOINT  ||  Light.eType == LIGHT_DATA::kPOINT__DEPRECATED)
      {
         ANARILight pLight = anariNewLight (m_pDevice, "point");
         float afPosition[3] = { static_cast<float> (Light.vPosition.dX), static_cast<float> (Light.vPosition.dY), static_cast<float> (Light.vPosition.dZ) };
         float fIntensity    = Light.fIntensity;
         anariSetParameter (m_pDevice, pLight, "position", ANARI_FLOAT32_VEC3, afPosition);
         anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, &Light.rgbColor);
         anariSetParameter (m_pDevice, pLight, "intensity", ANARI_FLOAT32, &fIntensity);
         anariCommitParameters (m_pDevice, pLight);
         S.aLight.push_back (pLight);
      }
   }

   S.pLightArray = anariNewArray1D (m_pDevice, S.aLight.data (), nullptr, nullptr, ANARI_LIGHT, S.aLight.size ());
   anariSetParameter (m_pDevice, m_pWorld, "light", ANARI_ARRAY1D, &S.pLightArray);

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
      const SPHERE_DATA& Sphere_Data = aSphere_Data[i];
      SCENE_STATE::SPHERE_ENTRY& Sphere_Entry = S.aSphere_Entry[i];

      if (Sphere_Entry.bTextured)
      {
         float afTransform[16] =
         {
            Sphere_Data.fRadius, 0.0f,                0.0f,                0.0f,
            0.0f,                Sphere_Data.fRadius, 0.0f,                0.0f,
            0.0f,                0.0f,                Sphere_Data.fRadius, 0.0f,
            static_cast<float> (Sphere_Data.vPosition.dX), static_cast<float> (Sphere_Data.vPosition.dY), static_cast<float> (Sphere_Data.vPosition.dZ), 1.0f,
         };
         anariSetParameter (m_pDevice, Sphere_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, afTransform);
         anariCommitParameters (m_pDevice, Sphere_Entry.pInstance);
      }
      else
      {
         float afPosition[3] = { static_cast<float> (Sphere_Data.vPosition.dX), static_cast<float> (Sphere_Data.vPosition.dY), static_cast<float> (Sphere_Data.vPosition.dZ) };
         ANARIArray1D pPositionArray = anariNewArray1D (m_pDevice, &afPosition, nullptr, nullptr, ANARI_FLOAT32_VEC3, 1);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &pPositionArray);
         anariSetParameter (m_pDevice, Sphere_Entry.pGeometry, "radius", ANARI_FLOAT32, &Sphere_Data.fRadius);
         anariCommitParameters (m_pDevice, Sphere_Entry.pGeometry);
         anariRelease (m_pDevice, pPositionArray);
      }
   }

   size_t nCurveIndex = 0;
   for (const CURVE_DATA& Curve_Data : aCurve_Data)
   {
      if (Curve_Data.aPoints.empty ()) continue;
      if (nCurveIndex >= S.aCurve_Entry.size ()) break;

      SCENE_STATE::CURVE_ENTRY& Curve_Entry = S.aCurve_Entry[nCurveIndex];

      std::vector<float> aPosition;
      aPosition.reserve (Curve_Data.aPoints.size () * 3);
      for (const CURVE_POINT& Point : Curve_Data.aPoints)
      {
         aPosition.push_back (static_cast<float> (Point.vPosition.dX));
         aPosition.push_back (static_cast<float> (Point.vPosition.dY));
         aPosition.push_back (static_cast<float> (Point.vPosition.dZ));
      }

      ANARIArray1D pPositionArray = anariNewArray1D (m_pDevice, aPosition.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, Curve_Data.aPoints.size ());
      anariSetParameter (m_pDevice, Curve_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &pPositionArray);
      anariCommitParameters (m_pDevice, Curve_Entry.pGeometry);
      anariRelease (m_pDevice, pPositionArray);

      nCurveIndex++;
   }

   for (size_t i = 0; i < aBox_Data.size ()  &&  i < S.aBox_Entry.size (); i++)
   {
      anariSetParameter (m_pDevice, S.aBox_Entry[i].pInstance, "transform", ANARI_FLOAT32_MAT4, aBox_Data[i].mWorld.f);
      anariCommitParameters (m_pDevice, S.aBox_Entry[i].pInstance);
   }

   for (size_t i = 0; i < aPanel_Data.size ()  &&  i < S.aPanel_Entry.size (); i++)
   {
      anariSetParameter (m_pDevice, S.aPanel_Entry[i].pInstance, "transform", ANARI_FLOAT32_MAT4, aPanel_Data[i].mWorld.f);
      anariCommitParameters (m_pDevice, S.aPanel_Entry[i].pInstance);
   }

   for (size_t i = 0; i < aMesh_Data.size ()  &&  i < S.aMesh_Entry.size (); i++)
   {
      anariSetParameter (m_pDevice, S.aMesh_Entry[i].pInstance, "transform", ANARI_FLOAT32_MAT4, aMesh_Data[i].mWorld.f);
      anariCommitParameters (m_pDevice, S.aMesh_Entry[i].pInstance);
   }
}


