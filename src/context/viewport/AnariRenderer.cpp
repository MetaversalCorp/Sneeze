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
//  the display's vsync releases a swapchain image - approximately 16.67ms at 60 Hz. This wait is baked into anariRenderFrame (not
//  anariFrameReady, which returns instantly). With one viewport, the compositor achieves 60 FPS with 16.5ms of idle vsync wait per frame.
//
//  THE PROBLEM: With N viewports rendered sequentially on one thread, each anariRenderFrame incurs its own vsync wait, so total frame time
//  is N * 16ms. Ten viewports = 6 FPS each. The vsync wait is a per-viewport multiplier, not a shared constant.
//
//  PROPOSED SOLUTIONS:
//
//  1. MAILBOX PRESENT MODE (preferred). Modify MetaversalCorp/filament to use VK_PRESENT_MODE_MAILBOX_KHR instead of FIFO_KHR. Mailbox
//     doesn't tear and doesn't block - the GPU renders as fast as it can, only the latest frame is shown at vsync. anariRenderFrame would
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

#include "AnariRenderer.h"
#include "ui/Ui_Context.h"
#include <anari/anari.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#define ANARI_RENDERER_TYPE ANARI_DATA_TYPE_DEFINE(514)
#undef ANARI_RENDERER

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
//  Retained scene state - ANARI objects that persist across frames
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
   std::vector<ANARIInstance> aWorldInstanceHandle; // host storage for pWorldInstanceArray
   size_t nBox_Bound = 0;                           // box pool slots included in the last bind

   // Handles awaiting release, outer-to-inner. Drained after the frame that
   // first renders without them -- see Retire().
   std::vector<ANARIObject> aRetire;

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

      // Last centre+radius pushed to ANARI. SPHERE_DATA is rebuilt from scratch
      // every frame, so it has no stable address -- unlike the glTF mesh path,
      // which keys off its persistent vertex-buffer pointer. Compare by value.
      float          dCommX = 0.0f, dCommY = 0.0f, dCommZ = 0.0f, dCommR = 0.0f;

   };

   struct CURVE_ENTRY
   {
      ANARIGeometry pGeometry   = nullptr;
      ANARIMaterial pMaterial   = nullptr;
      ANARISurface  pSurface    = nullptr;
      size_t        nPointCount = 0;
      uint64_t      nPointHash  = 0;   // fingerprint of last-committed control points
   };

   struct BOX_ENTRY
   {
      ANARIGeometry pGeometry = nullptr;
      ANARIMaterial pMaterial = nullptr;
      ANARISurface  pSurface  = nullptr;
      ANARIGroup    pGroup    = nullptr;
      ANARIInstance pInstance = nullptr;
      float         m16Comm[16] = {};   // last-committed world transform
      RGB           rgbComm     = {};   // last-committed colour (debug boxes change type as the camera moves)
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
      float          m16Comm[16] = {};   // last-committed world transform

   };

   // One placed glTF draw: an ANARI instance with its own transform. Geometry
   // arrays, the triangle object, and the material/surface/group are borrowed
   // from mapGeometry / mapGroup (refcounted) so the same primitive instanced
   // N times uploads once. pInstanceOwner+nDrawIx identify the placed draw
   // across frames; GroupKey finds the shared GPU group on retire.
   struct MESH_GEOMETRY_KEY
   {
      const float*    pfPosition    = nullptr;
      const float*    pfNormal      = nullptr;
      const float*    pfTexCoord    = nullptr;
      const uint32_t* puIndex       = nullptr;
      uint32_t        uCount_Vertex = 0;
      uint32_t        uCount_Index  = 0;

      bool operator== (const MESH_GEOMETRY_KEY& other) const
      {
         return pfPosition    == other.pfPosition
             && pfNormal      == other.pfNormal
             && pfTexCoord    == other.pfTexCoord
             && puIndex       == other.puIndex
             && uCount_Vertex == other.uCount_Vertex
             && uCount_Index  == other.uCount_Index;
      }
   };

   struct MESH_GEOMETRY_KEY_HASH
   {
      size_t operator() (const MESH_GEOMETRY_KEY& Key) const
      {
         size_t n = reinterpret_cast<size_t> (Key.pfPosition);
         n ^= reinterpret_cast<size_t> (Key.pfNormal)   + 0x9e3779b9u + (n << 6) + (n >> 2);
         n ^= reinterpret_cast<size_t> (Key.pfTexCoord) + 0x9e3779b9u + (n << 6) + (n >> 2);
         n ^= reinterpret_cast<size_t> (Key.puIndex)    + 0x9e3779b9u + (n << 6) + (n >> 2);
         n ^= static_cast<size_t> (Key.uCount_Vertex)   + 0x9e3779b9u + (n << 6) + (n >> 2);
         n ^= static_cast<size_t> (Key.uCount_Index)    + 0x9e3779b9u + (n << 6) + (n >> 2);
         return n;
      }
   };

   struct MESH_GROUP_KEY
   {
      MESH_GEOMETRY_KEY Geometry;
      const uint8_t*    pbTexture  = nullptr;
      float             fBaseR     = 1.0f;
      float             fBaseG     = 1.0f;
      float             fBaseB     = 1.0f;
      float             fBaseA     = 1.0f;
      float             fMetallic  = 1.0f;
      float             fRoughness = 1.0f;
      float             fEmissiveR = 0.0f;
      float             fEmissiveG = 0.0f;
      float             fEmissiveB = 0.0f;

      bool operator== (const MESH_GROUP_KEY& other) const
      {
         return Geometry    == other.Geometry
             && pbTexture   == other.pbTexture
             && fBaseR      == other.fBaseR
             && fBaseG      == other.fBaseG
             && fBaseB      == other.fBaseB
             && fBaseA      == other.fBaseA
             && fMetallic   == other.fMetallic
             && fRoughness  == other.fRoughness
             && fEmissiveR  == other.fEmissiveR
             && fEmissiveG  == other.fEmissiveG
             && fEmissiveB  == other.fEmissiveB;
      }
   };

   struct MESH_GROUP_KEY_HASH
   {
      MESH_GEOMETRY_KEY_HASH GeometryHash;

      size_t operator() (const MESH_GROUP_KEY& Key) const
      {
         size_t n = GeometryHash (Key.Geometry);
         n ^= reinterpret_cast<size_t> (Key.pbTexture) + 0x9e3779b9u + (n << 6) + (n >> 2);
         uint32_t nBits = 0;
         std::memcpy (&nBits, &Key.fBaseR, sizeof (nBits)); n ^= static_cast<size_t> (nBits) + 0x9e3779b9u + (n << 6) + (n >> 2);
         std::memcpy (&nBits, &Key.fMetallic, sizeof (nBits)); n ^= static_cast<size_t> (nBits) + 0x9e3779b9u + (n << 6) + (n >> 2);
         return n;
      }
   };

   struct MESH_GEOMETRY_GPU
   {
      ANARIArray1D  pPositionArray = nullptr;
      ANARIArray1D  pNormalArray   = nullptr;
      ANARIArray1D  pUvArray       = nullptr;
      ANARIArray1D  pIndexArray    = nullptr;
      ANARIGeometry pGeometry      = nullptr;
      int           nRef           = 0;
   };

   struct MESH_GROUP_GPU
   {
      MESH_GEOMETRY_KEY GeometryKey;
      const uint8_t*    pTextureKey = nullptr;
      ANARIMaterial     pMaterial   = nullptr;
      ANARISurface      pSurface    = nullptr;
      ANARIGroup        pGroup      = nullptr;
      int               nRef        = 0;
   };

   struct MESH_ENTRY
   {
      const void*     pInstanceOwner = nullptr;
      uint32_t        nDrawIx        = 0;
      MESH_GROUP_KEY  GroupKey       = {};
      ANARIInstance   pInstance      = nullptr;
      float           m16Comm[16]    = {};
   };

   // Deduped GPU upload of a decoded base-color image, keyed by the CPU pixel
   // pointer the compositor submits. Many glTF primitives share one albedo;
   // nRef is the number of MESH_GROUP_GPUs holding this sampler.
   struct TEXTURE_ENTRY
   {
      ANARIArray2D pImageArray = nullptr;
      ANARISampler pSampler    = nullptr;
      int          nRef        = 0;
   };

   std::vector<SPHERE_ENTRY> aSphere_Entry;
   std::vector<CURVE_ENTRY>  aCurve_Entry;
   std::vector<BOX_ENTRY>    aBox_Entry;
   std::vector<PANEL_ENTRY>  aPanel_Entry;
   std::vector<MESH_ENTRY>   aMesh_Entry;
   std::unordered_map<const uint8_t*, TEXTURE_ENTRY> mapTexture;
   std::unordered_map<MESH_GEOMETRY_KEY, MESH_GEOMETRY_GPU, MESH_GEOMETRY_KEY_HASH> mapGeometry;
   std::unordered_map<MESH_GROUP_KEY, MESH_GROUP_GPU, MESH_GROUP_KEY_HASH> mapGroup;
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
   m_bBoundingBoxOverlay (false),
   m_dLoadElapsed        (0.0),
   m_dLastDisplaySeconds (0.0),
   m_nAdmitGeometry      (4),
   m_nAdmitCreatesLast   (0),
   m_dLastSubmitSeconds (0.0),
   m_dLastRenderSeconds (0.0),
   m_bLastPresented     (true)
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

         // ReleaseScene only queues an empty world. Filament unregisters
         // Renderables on finalize, which is anariRenderFrame -- not commit.
         // A heavy fabric (hundreds of 100k-tri meshes) must get that empty
         // frame before the native swapchain / device die, or the next
         // context's nativeSurface on the same HWND comes up blank.
         if (m_pFrame)
         {
            anariCommitParameters (m_pDevice, m_pFrame);
            anariRenderFrame (m_pDevice, m_pFrame);
            anariFrameReady (m_pDevice, m_pFrame, ANARI_WAIT);
         }

         DrainRetired ();

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
   // ANativeWindow* (supplied via HALOGEN_NATIVE_SURFACE below) - no JNI.
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
               // 5th arg to anariSetParameter - NOT a pointer to it. The
               // C++ wrapper at anari_cpp_impl.hpp:530 dereferences one level
               // for this type; passing &m_pNativeWindow stores the wrong
               // value and crashes inside vkCreateAndroidSurfaceKHR on Vulkan.
               anariSetParameter (m_pDevice, ns, "nativeWindow", ANARI_VOID_POINTER, m_pNativeWindow);
#if defined(__ANDROID__)
               // filament::SwapChain::CONFIG_TRANSPARENT (0x1). Halogen wraps
               // bluevk vkCreateSwapchainKHR so Android gets PRE_MULTIPLIED
               // composite alpha (Filament 1.71's Vulkan backend ignores this
               // flag). A 0-alpha clear then composites over the camera view.
               uint64_t nFlags = 1ull;
               anariSetParameter (m_pDevice, ns, "flags", ANARI_UINT64, &nFlags);
#endif
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

void RENDERER::ANARI::BoundingBoxOverlay (bool bEnable)
{
   m_bBoundingBoxOverlay = bEnable;
}

void RENDERER::ANARI::SubmitPanels (const std::vector<PANEL_DATA>& aPanel_Data)
{
   m_aPanel_Data.insert (m_aPanel_Data.end (), aPanel_Data.begin (), aPanel_Data.end ());
}

void RENDERER::ANARI::SubmitMeshes (const std::vector<MESH_DATA>& aMesh_Data)
{
   m_aMesh_Data.insert (m_aMesh_Data.end (), aMesh_Data.begin (), aMesh_Data.end ());
}

// Incremental instance sync: adding or removing a glTF mesh (or a box/panel)
// must not ReleaseScene/BuildScene the whole world. Filament/ANARI stay on this
// thread; we only create or release the objects that appeared or vanished, then
// rebuild the world's instance list. helium copies Array2D host pixels on
// anariRelease, so a removed mesh's GPU objects are dropped here in EndFrame
// before the compositor Node_Close frees the CPU buffers.
namespace
{
   // Bytes per element for the array types this renderer builds.
   size_t Array_ElementBytes (ANARIDataType eType)
   {
      size_t nBytes = 0;

      switch (eType)
      {
         case ANARI_FLOAT32_VEC2: nBytes = 2 * sizeof (float);    break;
         case ANARI_FLOAT32_VEC3: nBytes = 3 * sizeof (float);    break;
         case ANARI_FLOAT32_VEC4: nBytes = 4 * sizeof (float);    break;
         case ANARI_UINT32_VEC3:  nBytes = 3 * sizeof (uint32_t); break;
         case ANARI_UFIXED8_VEC4: nBytes = 4 * sizeof (uint8_t);  break;
         default:                                                 break;
      }

      return nBytes;
   }

   // Hand ANARI its own storage instead of lending it ours.
   //
   // anariNewArray1D/2D with app memory and no deleter leaves helium holding a
   // bare POINTER to the caller's buffer (ArrayDataOwnership::SHARED) which it
   // reads on every device-side refresh and memcpys on release. That is only
   // safe while the buffer outlives the array -- and a glTF model's vertex
   // streams do not: NODE's destructor deletes the render model as soon as the
   // node closes, which on a scene switch happens outside the compositor's
   // release ordering. Passing null app memory makes the array MANAGED, so
   // helium allocates, owns and frees the copy and nothing points back at
   // Sneeze-side memory.
   ANARIArray1D NewArray1D_Copy (ANARIDevice pDevice, const void* pSource, ANARIDataType eType, uint64_t nCount)
   {
      ANARIArray1D pArray      = nullptr;
      size_t       nElementSize = Array_ElementBytes (eType);

      if (pSource  &&  nCount > 0  &&  nElementSize > 0)
      {
         pArray = anariNewArray1D (pDevice, nullptr, nullptr, nullptr, eType, nCount);

         void* pDest = anariMapArray (pDevice, pArray);
         if (pDest)
            std::memcpy (pDest, pSource, static_cast<size_t> (nCount) * nElementSize);
         anariUnmapArray (pDevice, pArray);
      }

      return pArray;
   }

   ANARIArray2D NewArray2D_Copy (ANARIDevice pDevice, const void* pSource, ANARIDataType eType, uint64_t nWidth, uint64_t nHeight)
   {
      ANARIArray2D pArray       = nullptr;
      size_t       nElementSize = Array_ElementBytes (eType);

      if (pSource  &&  nWidth > 0  &&  nHeight > 0  &&  nElementSize > 0)
      {
         pArray = anariNewArray2D (pDevice, nullptr, nullptr, nullptr, eType, nWidth, nHeight);

         void* pDest = anariMapArray (pDevice, pArray);
         if (pDest)
            std::memcpy (pDest, pSource, static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * nElementSize);
         anariUnmapArray (pDevice, pArray);
      }

      return pArray;
   }

   // Never anariRelease a scene object the moment we stop wanting it.
   //
   // Halogen maps an ANARI surface to a Filament Renderable and an ANARI
   // material to a Filament MaterialInstance, and Filament asserts hard
   // ("destroying MaterialInstance ... which is still in use by Renderable",
   // Material.cpp:1412 -> abort) if the MaterialInstance dies while the
   // Renderable is still registered in its scene. Filament only unregisters the
   // Renderable when the ANARI world is FINALIZED -- and anariCommitParameters
   // merely queues that work in helium's DeferredCommitBuffer, which is not
   // flushed until anariRenderFrame. So dropping our references at sync time,
   // or in ReleaseScene, always destroys the material one step too early.
   //
   // Instead, doomed handles go on the retirement queue in outer-to-inner
   // order. EndFrame drains it after anariFrameReady, by which point a full
   // frame has rendered with the object absent from the world's instance list
   // and Filament has let go of the Renderable.
   void Retire (RENDERER::ANARI::SCENE_STATE& S, ANARIObject pObject)
   {
      if (pObject)
         S.aRetire.push_back (pObject);
   }

   void TextureGpu_Release (RENDERER::ANARI::SCENE_STATE& S, const uint8_t* pbPixels)
   {
      if (pbPixels)
      {
         auto it = S.mapTexture.find (pbPixels);

         if (it != S.mapTexture.end ())
         {
            it->second.nRef--;

            if (it->second.nRef <= 0)
            {
               Retire (S, it->second.pSampler);
               Retire (S, it->second.pImageArray);
               S.mapTexture.erase (it);
            }
         }
      }
   }

   bool TextureGpu_Acquire (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, const uint8_t* pbPixels, int nWidth, int nHeight, ANARIArray2D& pImageArray, ANARISampler& pSampler)
   {
      bool bResult = false;

      if (pbPixels  &&  nWidth > 0  &&  nHeight > 0)
      {
         auto it = S.mapTexture.find (pbPixels);

         if (it != S.mapTexture.end ())
         {
            it->second.nRef++;
            pImageArray = it->second.pImageArray;
            pSampler    = it->second.pSampler;
            bResult     = true;
         }
         else
         {
            RENDERER::ANARI::SCENE_STATE::TEXTURE_ENTRY Texture;

            Texture.pImageArray = NewArray2D_Copy (pDevice, pbPixels, ANARI_UFIXED8_VEC4, static_cast<uint64_t> (nWidth), static_cast<uint64_t> (nHeight));
            Texture.pSampler    = anariNewSampler (pDevice, "image2D");
            anariSetParameter (pDevice, Texture.pSampler, "image",       ANARI_ARRAY2D, &Texture.pImageArray);
            anariSetParameter (pDevice, Texture.pSampler, "inAttribute", ANARI_STRING,  "attribute0");
            anariSetParameter (pDevice, Texture.pSampler, "filter",      ANARI_STRING,  "linear");
            anariCommitParameters (pDevice, Texture.pSampler);
            Texture.nRef = 1;

            S.mapTexture[pbPixels] = Texture;
            pImageArray = Texture.pImageArray;
            pSampler    = Texture.pSampler;
            bResult     = (Texture.pImageArray != nullptr  &&  Texture.pSampler != nullptr);
         }
      }

      return bResult;
   }

   using SCENE_STATE = RENDERER::ANARI::SCENE_STATE;

   SCENE_STATE::MESH_GEOMETRY_KEY Mesh_GeometryKey (const MESH_DATA& Mesh_Data)
   {
      SCENE_STATE::MESH_GEOMETRY_KEY Key;
      Key.pfPosition    = Mesh_Data.pfPosition;
      Key.pfNormal      = Mesh_Data.pfNormal;
      Key.pfTexCoord    = Mesh_Data.pfTexCoord;
      Key.puIndex       = Mesh_Data.puIndex;
      Key.uCount_Vertex = Mesh_Data.uCount_Vertex;
      Key.uCount_Index  = Mesh_Data.uCount_Index;
      return Key;
   }

   SCENE_STATE::MESH_GROUP_KEY Mesh_GroupKey (const MESH_DATA& Mesh_Data)
   {
      SCENE_STATE::MESH_GROUP_KEY Key;
      Key.Geometry    = Mesh_GeometryKey (Mesh_Data);
      Key.pbTexture   = Mesh_Data.pbTexturePixels;
      Key.fBaseR      = Mesh_Data.rgbaBaseColor.fR;
      Key.fBaseG      = Mesh_Data.rgbaBaseColor.fG;
      Key.fBaseB      = Mesh_Data.rgbaBaseColor.fB;
      Key.fBaseA      = Mesh_Data.rgbaBaseColor.fA;
      Key.fMetallic   = Mesh_Data.fMetallic;
      Key.fRoughness  = Mesh_Data.fRoughness;
      Key.fEmissiveR  = Mesh_Data.rgbEmissive.fR;
      Key.fEmissiveG  = Mesh_Data.rgbEmissive.fG;
      Key.fEmissiveB  = Mesh_Data.rgbEmissive.fB;
      return Key;
   }

   bool Mesh_InstanceMatch (const SCENE_STATE::MESH_ENTRY& Mesh_Entry, const MESH_DATA& Mesh_Data)
   {
      return Mesh_Entry.pInstanceOwner == Mesh_Data.pInstanceOwner
          && Mesh_Entry.nDrawIx        == Mesh_Data.nDrawIx;
   }

   bool GeometryGpu_Acquire (ANARIDevice pDevice, SCENE_STATE& S, const MESH_DATA& Mesh_Data, SCENE_STATE::MESH_GEOMETRY_GPU*& pOut)
   {
      bool bResult = false;
      pOut = nullptr;

      SCENE_STATE::MESH_GEOMETRY_KEY Key = Mesh_GeometryKey (Mesh_Data);
      auto it = S.mapGeometry.find (Key);
      if (it != S.mapGeometry.end ())
      {
         it->second.nRef++;
         pOut = &it->second;
         bResult = true;
      }
      else
      {
         uint64_t nCount_Vertex = Mesh_Data.uCount_Vertex;
         SCENE_STATE::MESH_GEOMETRY_GPU Gpu;

         Gpu.pPositionArray = NewArray1D_Copy (pDevice, Mesh_Data.pfPosition, ANARI_FLOAT32_VEC3, nCount_Vertex);
         Gpu.pGeometry      = anariNewGeometry (pDevice, "triangle");
         anariSetParameter (pDevice, Gpu.pGeometry, "vertex.position", ANARI_ARRAY1D, &Gpu.pPositionArray);

         if (Mesh_Data.pfNormal)
         {
            Gpu.pNormalArray = NewArray1D_Copy (pDevice, Mesh_Data.pfNormal, ANARI_FLOAT32_VEC3, nCount_Vertex);
            anariSetParameter (pDevice, Gpu.pGeometry, "vertex.normal", ANARI_ARRAY1D, &Gpu.pNormalArray);
         }

         if (Mesh_Data.pfTexCoord)
         {
            Gpu.pUvArray = NewArray1D_Copy (pDevice, Mesh_Data.pfTexCoord, ANARI_FLOAT32_VEC2, nCount_Vertex);
            anariSetParameter (pDevice, Gpu.pGeometry, "vertex.attribute0", ANARI_ARRAY1D, &Gpu.pUvArray);
         }

         if (Mesh_Data.puIndex  &&  Mesh_Data.uCount_Index >= 3)
         {
            Gpu.pIndexArray = NewArray1D_Copy (pDevice, Mesh_Data.puIndex, ANARI_UINT32_VEC3, Mesh_Data.uCount_Index / 3);
            anariSetParameter (pDevice, Gpu.pGeometry, "primitive.index", ANARI_ARRAY1D, &Gpu.pIndexArray);
         }

         anariCommitParameters (pDevice, Gpu.pGeometry);
         Gpu.nRef = 1;

         if (Gpu.pPositionArray  &&  Gpu.pGeometry)
         {
            auto Inserted = S.mapGeometry.emplace (Key, Gpu);
            pOut = &Inserted.first->second;
            bResult = true;
         }
         else
         {
            Retire (S, Gpu.pIndexArray);
            Retire (S, Gpu.pUvArray);
            Retire (S, Gpu.pNormalArray);
            Retire (S, Gpu.pPositionArray);
            Retire (S, Gpu.pGeometry);
         }
      }

      return bResult;
   }

   void GeometryGpu_Release (SCENE_STATE& S, const SCENE_STATE::MESH_GEOMETRY_KEY& Key)
   {
      auto it = S.mapGeometry.find (Key);
      if (it != S.mapGeometry.end ())
      {
         it->second.nRef--;
         if (it->second.nRef <= 0)
         {
            Retire (S, it->second.pIndexArray);
            Retire (S, it->second.pUvArray);
            Retire (S, it->second.pNormalArray);
            Retire (S, it->second.pPositionArray);
            Retire (S, it->second.pGeometry);
            S.mapGeometry.erase (it);
         }
      }
   }

   bool GroupGpu_Acquire (ANARIDevice pDevice, SCENE_STATE& S, const MESH_DATA& Mesh_Data, SCENE_STATE::MESH_GROUP_GPU*& pOut)
   {
      bool bResult = false;
      pOut = nullptr;

      SCENE_STATE::MESH_GROUP_KEY Key = Mesh_GroupKey (Mesh_Data);
      auto it = S.mapGroup.find (Key);
      if (it != S.mapGroup.end ())
      {
         it->second.nRef++;
         pOut = &it->second;
         bResult = true;
      }
      else
      {
         SCENE_STATE::MESH_GEOMETRY_GPU* pGeometry = nullptr;
         if (GeometryGpu_Acquire (pDevice, S, Mesh_Data, pGeometry)  &&  pGeometry)
         {
            bool         bTextured = Mesh_Data.pbTexturePixels  &&  Mesh_Data.dimTexture.nW > 0  &&  Mesh_Data.dimTexture.nH > 0  &&  Mesh_Data.pfTexCoord;
            ANARIArray2D pImageArray = nullptr;
            ANARISampler pSampler    = nullptr;

            SCENE_STATE::MESH_GROUP_GPU Group;
            Group.GeometryKey = Key.Geometry;
            Group.pTextureKey = Mesh_Data.pbTexturePixels;
            Group.pMaterial   = anariNewMaterial (pDevice, "physicallyBased");

            if (bTextured  &&  TextureGpu_Acquire (pDevice, S, Mesh_Data.pbTexturePixels, Mesh_Data.dimTexture.nW, Mesh_Data.dimTexture.nH, pImageArray, pSampler))
               anariSetParameter (pDevice, Group.pMaterial, "baseColor", ANARI_SAMPLER, &pSampler);
            else
               anariSetParameter (pDevice, Group.pMaterial, "baseColor", ANARI_FLOAT32_VEC4, &Mesh_Data.rgbaBaseColor);
            anariSetParameter (pDevice, Group.pMaterial, "metallic",  ANARI_FLOAT32,      &Mesh_Data.fMetallic);
            anariSetParameter (pDevice, Group.pMaterial, "roughness", ANARI_FLOAT32,      &Mesh_Data.fRoughness);
            anariSetParameter (pDevice, Group.pMaterial, "emissive",  ANARI_FLOAT32_VEC3, &Mesh_Data.rgbEmissive);
            anariCommitParameters (pDevice, Group.pMaterial);

            Group.pSurface = anariNewSurface (pDevice);
            anariSetParameter (pDevice, Group.pSurface, "geometry", ANARI_GEOMETRY, &pGeometry->pGeometry);
            anariSetParameter (pDevice, Group.pSurface, "material", ANARI_MATERIAL, &Group.pMaterial);
            anariCommitParameters (pDevice, Group.pSurface);

            ANARIArray1D pSurfaceArray = anariNewArray1D (pDevice, &Group.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
            Group.pGroup = anariNewGroup (pDevice);
            anariSetParameter (pDevice, Group.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
            anariCommitParameters (pDevice, Group.pGroup);
            anariRelease (pDevice, pSurfaceArray);

            Group.nRef = 1;
            auto Inserted = S.mapGroup.emplace (Key, Group);
            pOut = &Inserted.first->second;
            bResult = true;
         }
      }

      return bResult;
   }

   void GroupGpu_Release (SCENE_STATE& S, const SCENE_STATE::MESH_GROUP_KEY& Key)
   {
      auto it = S.mapGroup.find (Key);
      if (it != S.mapGroup.end ())
      {
         it->second.nRef--;
         if (it->second.nRef <= 0)
         {
            TextureGpu_Release (S, it->second.pTextureKey);
            Retire (S, it->second.pGroup);
            Retire (S, it->second.pSurface);
            Retire (S, it->second.pMaterial);
            GeometryGpu_Release (S, it->second.GeometryKey);
            S.mapGroup.erase (it);
         }
      }
   }

   void MeshEntry_Retire (SCENE_STATE& S, SCENE_STATE::MESH_ENTRY& Mesh_Entry)
   {
      if (Mesh_Entry.pInstance)
         GroupGpu_Release (S, Mesh_Entry.GroupKey);
      Retire (S, Mesh_Entry.pInstance);
      Mesh_Entry = SCENE_STATE::MESH_ENTRY ();
   }

   void MeshEntry_Create (ANARIDevice pDevice, SCENE_STATE& S, SCENE_STATE::MESH_ENTRY& Mesh_Entry, const MESH_DATA& Mesh_Data)
   {
      SCENE_STATE::MESH_GROUP_GPU* pGroup = nullptr;

      Mesh_Entry.pInstanceOwner = Mesh_Data.pInstanceOwner;
      Mesh_Entry.nDrawIx        = Mesh_Data.nDrawIx;

      if (GroupGpu_Acquire (pDevice, S, Mesh_Data, pGroup)  &&  pGroup)
      {
         Mesh_Entry.GroupKey  = Mesh_GroupKey (Mesh_Data);
         Mesh_Entry.pInstance = anariNewInstance (pDevice, "transform");
         if (Mesh_Entry.pInstance)
         {
            anariSetParameter (pDevice, Mesh_Entry.pInstance, "group", ANARI_GROUP, &pGroup->pGroup);
            anariSetParameter (pDevice, Mesh_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, Mesh_Data.mWorld.f);
            anariCommitParameters (pDevice, Mesh_Entry.pInstance);
            std::memcpy (Mesh_Entry.m16Comm, Mesh_Data.mWorld.f, sizeof (Mesh_Entry.m16Comm));
         }
         else
         {
            GroupGpu_Release (S, Mesh_Entry.GroupKey);
            Mesh_Entry.GroupKey = SCENE_STATE::MESH_GROUP_KEY ();
         }
      }
   }

   void BoxEntry_Retire (RENDERER::ANARI::SCENE_STATE& S, RENDERER::ANARI::SCENE_STATE::BOX_ENTRY& Box_Entry)
   {
      Retire (S, Box_Entry.pInstance);
      Retire (S, Box_Entry.pGroup);
      Retire (S, Box_Entry.pSurface);
      Retire (S, Box_Entry.pMaterial);
      Retire (S, Box_Entry.pGeometry);
      Box_Entry = RENDERER::ANARI::SCENE_STATE::BOX_ENTRY ();
   }

   void PanelEntry_Retire (RENDERER::ANARI::SCENE_STATE& S, RENDERER::ANARI::SCENE_STATE::PANEL_ENTRY& Panel_Entry)
   {
      Retire (S, Panel_Entry.pInstance);
      Retire (S, Panel_Entry.pGroup);
      Retire (S, Panel_Entry.pSurface);
      Retire (S, Panel_Entry.pMaterial);
      Retire (S, Panel_Entry.pSampler);
      Retire (S, Panel_Entry.pImageArray);
      Retire (S, Panel_Entry.pGeometry);
      Panel_Entry = RENDERER::ANARI::SCENE_STATE::PANEL_ENTRY ();
   }

   // A material is never re-committed once its surface has been finalized.
   // Halogen's Material::commitParameters destroys its filament MaterialInstance
   // and builds a fresh one, and helium runs every commitParameters() in a flush
   // before the first finalize(). So re-committing a live material destroys an
   // instance the surface's Renderable is still bound to, which trips an assert
   // inside filament and aborts the process. Recolouring a box means a brand new
   // material object instead -- see the colour branch in SyncBoxes.
   ANARIMaterial BoxMaterial_Create (ANARIDevice pDevice, const RGB& rgbColor)
   {
      ANARIMaterial pMaterial  = anariNewMaterial (pDevice, "physicallyBased");
      float         fMetallic  = 0.0f;
      float         fRoughness = 0.85f;
      float         fOpacity   = 0.05f;   // TEMP debug: box translucency (0 = invisible, 1 = solid)

      // Halogen's blend material is Filament "transparent", which expects
      // PREMULTIPLIED alpha, but the shader leaves rgb un-premultiplied. So
      // premultiply here (rgb *= opacity) and pass the same opacity as alpha.
      // Without this the box color is added at full brightness regardless of
      // opacity -- overlaps blow out to white and changing opacity does
      // nothing visible.
      float afBaseColor[4] = { rgbColor.fR * fOpacity, rgbColor.fG * fOpacity, rgbColor.fB * fOpacity, 1.0f };
      anariSetParameter (pDevice, pMaterial, "baseColor", ANARI_FLOAT32_VEC4, afBaseColor);
      anariSetParameter (pDevice, pMaterial, "metallic",  ANARI_FLOAT32,      &fMetallic);
      anariSetParameter (pDevice, pMaterial, "roughness", ANARI_FLOAT32,      &fRoughness);
      anariSetParameter (pDevice, pMaterial, "opacity",   ANARI_FLOAT32,      &fOpacity);
      anariSetParameter (pDevice, pMaterial, "alphaMode", ANARI_STRING,       "blend");
      anariCommitParameters (pDevice, pMaterial);

      return pMaterial;
   }

   void BoxEntry_Create (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, RENDERER::ANARI::SCENE_STATE::BOX_ENTRY& Box_Entry, const BOX_DATA& Box_Data)
   {
      Box_Entry.pGeometry = anariNewGeometry (pDevice, "triangle");
      anariSetParameter (pDevice, Box_Entry.pGeometry, "vertex.position", ANARI_ARRAY1D, &S.pBoxPositionArray);
      anariSetParameter (pDevice, Box_Entry.pGeometry, "vertex.normal",   ANARI_ARRAY1D, &S.pBoxNormalArray);
      anariSetParameter (pDevice, Box_Entry.pGeometry, "primitive.index", ANARI_ARRAY1D, &S.pBoxIndexArray);
      anariCommitParameters (pDevice, Box_Entry.pGeometry);

      Box_Entry.pMaterial = BoxMaterial_Create (pDevice, Box_Data.rgbColor);

      Box_Entry.pSurface = anariNewSurface (pDevice);
      anariSetParameter (pDevice, Box_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Box_Entry.pGeometry);
      anariSetParameter (pDevice, Box_Entry.pSurface, "material", ANARI_MATERIAL, &Box_Entry.pMaterial);
      anariCommitParameters (pDevice, Box_Entry.pSurface);

      ANARIArray1D pSurfaceArray = anariNewArray1D (pDevice, &Box_Entry.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
      Box_Entry.pGroup = anariNewGroup (pDevice);
      anariSetParameter (pDevice, Box_Entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
      anariCommitParameters (pDevice, Box_Entry.pGroup);
      anariRelease (pDevice, pSurfaceArray);

      Box_Entry.pInstance = anariNewInstance (pDevice, "transform");
      anariSetParameter (pDevice, Box_Entry.pInstance, "group", ANARI_GROUP, &Box_Entry.pGroup);
      anariSetParameter (pDevice, Box_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, Box_Data.mWorld.f);
      anariCommitParameters (pDevice, Box_Entry.pInstance);
      std::memcpy (Box_Entry.m16Comm, Box_Data.mWorld.f, sizeof (Box_Entry.m16Comm));
      Box_Entry.rgbComm = Box_Data.rgbColor;
   }

   void PanelEntry_Create (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, RENDERER::ANARI::SCENE_STATE::PANEL_ENTRY& Panel_Entry, const PANEL_DATA& Panel_Data)
   {
      Panel_Entry.pPixelKey = Panel_Data.pbPixels;

      Panel_Entry.pGeometry = anariNewGeometry (pDevice, "triangle");
      anariSetParameter (pDevice, Panel_Entry.pGeometry, "vertex.position",   ANARI_ARRAY1D, &S.pQuadPositionArray);
      anariSetParameter (pDevice, Panel_Entry.pGeometry, "vertex.normal",     ANARI_ARRAY1D, &S.pQuadNormalArray);
      anariSetParameter (pDevice, Panel_Entry.pGeometry, "vertex.attribute0", ANARI_ARRAY1D, &S.pQuadUvArray);
      anariSetParameter (pDevice, Panel_Entry.pGeometry, "primitive.index",   ANARI_ARRAY1D, &S.pQuadIndexArray);
      anariCommitParameters (pDevice, Panel_Entry.pGeometry);

      // image2D wants CPU RGBA8; Halogen's convertToRGBA8 decodes plain
      // UFIXED8 variants (the _SRGB forms fall through to black), so use
      // UFIXED8_VEC4. Pixels arrive straight-alpha from the panel.
      Panel_Entry.pImageArray = NewArray2D_Copy (pDevice, Panel_Data.pbPixels, ANARI_UFIXED8_VEC4, Panel_Data.dim.nW, Panel_Data.dim.nH);

      Panel_Entry.pSampler = anariNewSampler (pDevice, "image2D");
      anariSetParameter (pDevice, Panel_Entry.pSampler, "image",  ANARI_ARRAY2D, &Panel_Entry.pImageArray);
      anariSetParameter (pDevice, Panel_Entry.pSampler, "filter", ANARI_STRING,  "linear");
      anariCommitParameters (pDevice, Panel_Entry.pSampler);

      // HALOGEN_MATERIAL_UNLIT: emits the sampled texel directly, lighting-
      // independent -- the correct model for UI. Per-texel alpha rides the
      // texture under alphaMode "blend".
      Panel_Entry.pMaterial = anariNewMaterial (pDevice, "unlit");
      anariSetParameter (pDevice, Panel_Entry.pMaterial, "alphaMode", ANARI_STRING, "blend");
      anariSetParameter (pDevice, Panel_Entry.pMaterial, "color", ANARI_SAMPLER, &Panel_Entry.pSampler);
      anariCommitParameters (pDevice, Panel_Entry.pMaterial);

      Panel_Entry.pSurface = anariNewSurface (pDevice);
      anariSetParameter (pDevice, Panel_Entry.pSurface, "geometry", ANARI_GEOMETRY, &Panel_Entry.pGeometry);
      anariSetParameter (pDevice, Panel_Entry.pSurface, "material", ANARI_MATERIAL, &Panel_Entry.pMaterial);
      anariCommitParameters (pDevice, Panel_Entry.pSurface);

      ANARIArray1D pSurfaceArray = anariNewArray1D (pDevice, &Panel_Entry.pSurface, nullptr, nullptr, ANARI_SURFACE, 1);
      Panel_Entry.pGroup = anariNewGroup (pDevice);
      anariSetParameter (pDevice, Panel_Entry.pGroup, "surface", ANARI_ARRAY1D, &pSurfaceArray);
      anariCommitParameters (pDevice, Panel_Entry.pGroup);
      anariRelease (pDevice, pSurfaceArray);

      Panel_Entry.pInstance = anariNewInstance (pDevice, "transform");
      anariSetParameter (pDevice, Panel_Entry.pInstance, "group", ANARI_GROUP, &Panel_Entry.pGroup);
      anariSetParameter (pDevice, Panel_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, Panel_Data.mWorld.f);
      anariCommitParameters (pDevice, Panel_Entry.pInstance);
      std::memcpy (Panel_Entry.m16Comm, Panel_Data.mWorld.f, sizeof (Panel_Entry.m16Comm));
   }

   void BoxShared_Ensure (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, const UV_SPHERE& UnitBox)
   {
      if (!S.pBoxPositionArray  &&  !UnitBox.aPositions.empty ())
      {
         uint64_t nBoxVertexCount   = UnitBox.aPositions.size () / 3;
         uint64_t nBoxTriangleCount = UnitBox.aIndices.size () / 3;
         S.pBoxPositionArray = anariNewArray1D (pDevice, UnitBox.aPositions.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nBoxVertexCount);
         S.pBoxNormalArray   = anariNewArray1D (pDevice, UnitBox.aNormals.data (),   nullptr, nullptr, ANARI_FLOAT32_VEC3, nBoxVertexCount);
         S.pBoxIndexArray    = anariNewArray1D (pDevice, UnitBox.aIndices.data (),   nullptr, nullptr, ANARI_UINT32_VEC3,  nBoxTriangleCount);
      }
   }

   void PanelShared_Ensure (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S)
   {
      if (!S.pQuadPositionArray)
      {
         // Shared unit quad in the local XY plane, +Z normal, attribute0 = UVs.
         // V is flipped vs. position: ANARI/Filament sample v=0 at the bottom
         // while the UI canvas is top-down, so the quad's top edge maps to v=1
         // to keep the document upright. Double-sided (front + reversed
         // winding) so back-face culling can't hide a panel turned away from
         // the camera.
         static const float aQuadPosition[12] = { -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.5f, 0.5f, 0.0f,  -0.5f, 0.5f, 0.0f };
         static const float aQuadNormal[12]   = {  0.0f,  0.0f, 1.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f };
         static const float aQuadUv[8]        = {  0.0f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 1.0f };
         static const uint32_t aQuadIndex[12] = { 0, 1, 2,  0, 2, 3,   0, 2, 1,  0, 3, 2 };

         S.pQuadPositionArray = anariNewArray1D (pDevice, aQuadPosition, nullptr, nullptr, ANARI_FLOAT32_VEC3, 4);
         S.pQuadNormalArray   = anariNewArray1D (pDevice, aQuadNormal,   nullptr, nullptr, ANARI_FLOAT32_VEC3, 4);
         S.pQuadUvArray       = anariNewArray1D (pDevice, aQuadUv,       nullptr, nullptr, ANARI_FLOAT32_VEC2, 4);
         S.pQuadIndexArray    = anariNewArray1D (pDevice, aQuadIndex,    nullptr, nullptr, ANARI_UINT32_VEC3, 4);
      }
   }

   bool Mesh_IsDrawable (const MESH_DATA& Mesh_Data)
   {
      return Mesh_Data.pfPosition  &&  Mesh_Data.uCount_Vertex > 0;
   }

   bool Mesh_IsTextured (const MESH_DATA& Mesh_Data)
   {
      return Mesh_Data.pbTexturePixels  &&  Mesh_Data.dimTexture.nW > 0  &&  Mesh_Data.dimTexture.nH > 0  &&  Mesh_Data.pfTexCoord;
   }

   bool Mesh_NeedsTextureUpload (const RENDERER::ANARI::SCENE_STATE& S, const MESH_DATA& Mesh_Data)
   {
      return Mesh_IsTextured (Mesh_Data)  &&  S.mapTexture.find (Mesh_Data.pbTexturePixels) == S.mapTexture.end ();
   }

   // New GPU geometry (and unique texture uploads) per EndFrame. Instance-only
   // creates reuse an already-uploaded primitive and are capped separately so
   // a Tester01-style repeat does not count as four mesh uploads. Remaining
   // draws stay on this frame's submit list and are admitted later; a collapse
   // that drops them from the list means they are never created.
   //
   // Triangle budget: doughnut1Mb is ~100k tris per primitive. Admitting four
   // of those in one flush used to stall the compositor inside Halogen's
   // flushAndWait (camera, FPS log, and URL Cancel all freeze). The first
   // new geometry of a frame is always allowed even when it exceeds the
   // budget, so a single huge primitive still uploads; further creates wait.
   //
   // Display-time governor: the compositor reports the previous presented
   // frame's wall time (scene + submit + present, before the 60 Hz sleep).
   // The mesh/texture cap starts at 4 and is raised by 2 while that time stays
   // under TARGET_DISPLAY_SECONDS (20 ms / 50 Hz) and unique meshes were still
   // uploading. A frame over 20 ms halves the cap. Skipped presents (Filament
   // frame skipper) do not raise the cap -- the GPU is still busy.
   static constexpr size_t   MAX_MESH_CREATES_PER_FRAME     = 4;
   static constexpr size_t   MAX_MESH_CREATES_CEILING       = 32;
   static constexpr size_t   MAX_MESH_INSTANCES_PER_FRAME   = 64;
   static constexpr size_t   MAX_TEXTURE_UPLOADS_PER_FRAME  = 4;
   static constexpr size_t   MAX_TEXTURE_UPLOADS_CEILING    = 32;
   static constexpr uint32_t MAX_NEW_TRIANGLES_PER_FRAME    = 65536;
   static constexpr uint32_t MAX_NEW_TRIANGLES_CEILING      = 262144;
   static constexpr double   TARGET_DISPLAY_SECONDS         = 0.020;

   struct MESH_ADMIT_BUDGET
   {
      size_t   nMaxGeometry;
      size_t   nMaxInstance;
      size_t   nMaxTexture;
      uint32_t nMaxTriangles;
   };

   void Mesh_AdmitAdjust (double dLastDisplaySeconds, bool bLastPresented, size_t nAdmitCreatesLast, size_t& nAdmitGeometry)
   {
      if (dLastDisplaySeconds > TARGET_DISPLAY_SECONDS)
      {
         nAdmitGeometry = nAdmitGeometry / 2;
         if (nAdmitGeometry < 1)
            nAdmitGeometry = 1;
      }
      else if (dLastDisplaySeconds > 0.0  &&  bLastPresented  &&  nAdmitCreatesLast > 0)
      {
         nAdmitGeometry += 2;
         if (nAdmitGeometry > MAX_MESH_CREATES_CEILING)
            nAdmitGeometry = MAX_MESH_CREATES_CEILING;
      }
   }

   MESH_ADMIT_BUDGET Mesh_AdmitBudget (size_t nAdmitGeometry)
   {
      MESH_ADMIT_BUDGET Budget;
      uint64_t          nTriangles = 0;

      Budget.nMaxGeometry  = nAdmitGeometry;
      Budget.nMaxInstance  = MAX_MESH_INSTANCES_PER_FRAME;
      Budget.nMaxTexture   = nAdmitGeometry;
      if (Budget.nMaxTexture > MAX_TEXTURE_UPLOADS_CEILING)
         Budget.nMaxTexture = MAX_TEXTURE_UPLOADS_CEILING;

      nTriangles = static_cast<uint64_t> (MAX_NEW_TRIANGLES_PER_FRAME) * nAdmitGeometry / MAX_MESH_CREATES_PER_FRAME;
      if (nTriangles < 1)
         nTriangles = 1;
      if (nTriangles > MAX_NEW_TRIANGLES_CEILING)
         nTriangles = MAX_NEW_TRIANGLES_CEILING;
      Budget.nMaxTriangles = static_cast<uint32_t> (nTriangles);

      return Budget;
   }

   uint32_t Mesh_TriangleCount (const MESH_DATA& Mesh_Data)
   {
      uint32_t nTriangles = 0;

      if (Mesh_Data.puIndex  &&  Mesh_Data.uCount_Index >= 3)
         nTriangles = Mesh_Data.uCount_Index / 3;
      else if (Mesh_Data.uCount_Vertex >= 3)
         nTriangles = Mesh_Data.uCount_Vertex / 3;

      return nTriangles;
   }

   bool Mesh_CanAdmit (const SCENE_STATE& S, const MESH_DATA& Mesh_Data, size_t nCreate_Geometry, size_t nCreate_Instance, size_t nCreate_Texture, uint32_t nCreate_Triangles, const MESH_ADMIT_BUDGET& Budget, bool& bNewGeometry, bool& bNeedsTexture)
   {
      bool     bAdmit         = false;
      bool     bNewGroup      = S.mapGroup.find (Mesh_GroupKey (Mesh_Data)) == S.mapGroup.end ();
      uint32_t nThisTriangles = Mesh_TriangleCount (Mesh_Data);

      bNewGeometry  = S.mapGeometry.find (Mesh_GeometryKey (Mesh_Data)) == S.mapGeometry.end ();
      bNeedsTexture = bNewGroup  &&  Mesh_NeedsTextureUpload (S, Mesh_Data);

      if (bNeedsTexture  &&  nCreate_Texture >= Budget.nMaxTexture)
         bAdmit = false;
      else if (bNewGeometry)
      {
         if (nCreate_Geometry >= Budget.nMaxGeometry)
            bAdmit = false;
         else if (nCreate_Geometry > 0  &&  nCreate_Triangles + nThisTriangles > Budget.nMaxTriangles)
            bAdmit = false;
         else
            bAdmit = true;
      }
      else
         bAdmit = nCreate_Instance < Budget.nMaxInstance;

      return bAdmit;
   }

   size_t Mesh_PendingUnique (const SCENE_STATE& S, const std::vector<MESH_DATA>& aMesh_Data)
   {
      std::unordered_set<SCENE_STATE::MESH_GEOMETRY_KEY, SCENE_STATE::MESH_GEOMETRY_KEY_HASH> setPending;
      size_t nPending = 0;

      for (const MESH_DATA& Mesh_Data : aMesh_Data)
      {
         if (Mesh_IsDrawable (Mesh_Data))
         {
            SCENE_STATE::MESH_GEOMETRY_KEY Key = Mesh_GeometryKey (Mesh_Data);

            if (S.mapGeometry.find (Key) == S.mapGeometry.end ())
               setPending.insert (Key);
         }
      }

      nPending = setPending.size ();
      return nPending;
   }

   void Mesh_LogGpuUpload (SNEEZE::ENGINE* pEngine, const MESH_DATA& Mesh_Data, uint32_t nTriangles, size_t nCreate_Geometry, const MESH_ADMIT_BUDGET& Budget, const SCENE_STATE& S, const std::vector<MESH_DATA>& aMesh_Data, double dLoadElapsed)
   {
      if (pEngine)
      {
         char szTime[32];

         std::snprintf (szTime, sizeof (szTime), "%.3f", dLoadElapsed);

         pEngine->Log (IENGINE::kLOGLEVEL_Info, "ANARI",
            std::string ("mesh GPU upload t=") + szTime + "s"
            + " vertices=" + std::to_string (Mesh_Data.uCount_Vertex)
            + " triangles=" + std::to_string (nTriangles)
            + " frame=" + std::to_string (nCreate_Geometry) + "/" + std::to_string (Budget.nMaxGeometry)
            + " loaded=" + std::to_string (S.mapGeometry.size ())
            + " pending=" + std::to_string (Mesh_PendingUnique (S, aMesh_Data)));
      }
   }

   void Mesh_AccountCreate (SNEEZE::ENGINE* pEngine, const MESH_DATA& Mesh_Data, bool bNewGeometry, bool bNeedsTexture, size_t& nCreate_Geometry, size_t& nCreate_Instance, size_t& nCreate_Texture, uint32_t& nCreate_Triangles, const MESH_ADMIT_BUDGET& Budget, const SCENE_STATE& S, const std::vector<MESH_DATA>& aMesh_Data, double dLoadElapsed)
   {
      if (bNewGeometry)
      {
         uint32_t nTriangles = Mesh_TriangleCount (Mesh_Data);

         nCreate_Geometry++;
         nCreate_Triangles += nTriangles;

         Mesh_LogGpuUpload (pEngine, Mesh_Data, nTriangles, nCreate_Geometry, Budget, S, aMesh_Data, dLoadElapsed);
      }
      else
         nCreate_Instance++;

      if (bNeedsTexture)
         nCreate_Texture++;
   }

   bool SceneNeedsInstanceSync (const RENDERER::ANARI::SCENE_STATE& S, const std::vector<BOX_DATA>& aBox_Data, const std::vector<PANEL_DATA>& aPanel_Data, const std::vector<MESH_DATA>& aMesh_Data, bool bBoundingBoxOverlay)
   {
      bool bSync = false;

      // The live box count is bound as a prefix of the pool, so any change to
      // it -- growth, a view-cull dip, or the overlay being toggled -- needs a
      // rebind. That is a handle-array swap, not object churn.
      size_t nWantBoxBind = bBoundingBoxOverlay ? aBox_Data.size () : 0;
      if (nWantBoxBind != S.nBox_Bound)
         bSync = true;

      if (!bSync  &&  aPanel_Data.size () != S.aPanel_Entry.size ())
         bSync = true;

      if (!bSync)
      {
         size_t nMesh = 0;
         for (const MESH_DATA& Mesh_Data : aMesh_Data)
         {
            if (Mesh_IsDrawable (Mesh_Data))
               nMesh++;
         }
         if (nMesh != S.aMesh_Entry.size ())
            bSync = true;
      }

      if (!bSync)
      {
         for (size_t i = 0; i < aPanel_Data.size (); i++)
         {
            if (aPanel_Data[i].pbPixels != S.aPanel_Entry[i].pPixelKey)
            {
               bSync = true;
               break;
            }
         }
      }

      if (!bSync)
      {
         size_t nEntry = 0;
         for (const MESH_DATA& Mesh_Data : aMesh_Data)
         {
            if (!Mesh_IsDrawable (Mesh_Data))
               continue;
            if (nEntry >= S.aMesh_Entry.size ()
             ||  !Mesh_InstanceMatch (S.aMesh_Entry[nEntry], Mesh_Data)
             ||  !(S.aMesh_Entry[nEntry].GroupKey == Mesh_GroupKey (Mesh_Data)))
            {
               bSync = true;
               break;
            }
            nEntry++;
         }
      }

      return bSync;
   }

   bool SyncMeshes (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, const std::vector<MESH_DATA>& aMesh_Data, SNEEZE::ENGINE* pEngine, double dLoadElapsed, double dLastDisplaySeconds, bool bLastPresented, size_t& nAdmitGeometry, size_t& nAdmitCreatesLast)
   {
      bool              bDirty            = false;
      size_t            nCreate_Geometry  = 0;
      size_t            nCreate_Instance  = 0;
      size_t            nCreate_Texture   = 0;
      uint32_t          nCreate_Triangles = 0;
      MESH_ADMIT_BUDGET Budget;

      Mesh_AdmitAdjust (dLastDisplaySeconds, bLastPresented, nAdmitCreatesLast, nAdmitGeometry);
      Budget = Mesh_AdmitBudget (nAdmitGeometry);
      std::vector<char> aUsed (S.aMesh_Entry.size (), 0);
      std::vector<RENDERER::ANARI::SCENE_STATE::MESH_ENTRY> aNext;

      for (const MESH_DATA& Mesh_Data : aMesh_Data)
      {
         if (!Mesh_IsDrawable (Mesh_Data))
            continue;

         int nFound = -1;
         for (size_t i = 0; i < S.aMesh_Entry.size (); i++)
         {
            if (!aUsed[i]  &&  Mesh_InstanceMatch (S.aMesh_Entry[i], Mesh_Data))
            {
               nFound = static_cast<int> (i);
               break;
            }
         }

         if (nFound >= 0)
         {
            aUsed[static_cast<size_t> (nFound)] = 1;
            RENDERER::ANARI::SCENE_STATE::MESH_ENTRY Mesh_Entry = S.aMesh_Entry[static_cast<size_t> (nFound)];

            if (!(Mesh_Entry.GroupKey == Mesh_GroupKey (Mesh_Data)))
            {
               bool bNewGeometry  = false;
               bool bNeedsTexture = false;

               if (Mesh_CanAdmit (S, Mesh_Data, nCreate_Geometry, nCreate_Instance, nCreate_Texture, nCreate_Triangles, Budget, bNewGeometry, bNeedsTexture))
               {
                  MeshEntry_Retire (S, Mesh_Entry);
                  MeshEntry_Create (pDevice, S, Mesh_Entry, Mesh_Data);
                  Mesh_AccountCreate (pEngine, Mesh_Data, bNewGeometry, bNeedsTexture, nCreate_Geometry, nCreate_Instance, nCreate_Texture, nCreate_Triangles, Budget, S, aMesh_Data, dLoadElapsed);
                  bDirty = true;
               }
            }

            aNext.push_back (Mesh_Entry);
         }
         else
         {
            bool bNewGeometry  = false;
            bool bNeedsTexture = false;

            if (Mesh_CanAdmit (S, Mesh_Data, nCreate_Geometry, nCreate_Instance, nCreate_Texture, nCreate_Triangles, Budget, bNewGeometry, bNeedsTexture))
            {
               RENDERER::ANARI::SCENE_STATE::MESH_ENTRY Mesh_Entry;
               MeshEntry_Create (pDevice, S, Mesh_Entry, Mesh_Data);
               Mesh_AccountCreate (pEngine, Mesh_Data, bNewGeometry, bNeedsTexture, nCreate_Geometry, nCreate_Instance, nCreate_Texture, nCreate_Triangles, Budget, S, aMesh_Data, dLoadElapsed);
               aNext.push_back (Mesh_Entry);
               bDirty = true;
            }
         }
      }

      for (size_t i = 0; i < S.aMesh_Entry.size (); i++)
      {
         if (!aUsed[i])
         {
            MeshEntry_Retire (S, S.aMesh_Entry[i]);
            bDirty = true;
         }
      }

      if (bDirty  ||  aNext.size () != S.aMesh_Entry.size ())
         S.aMesh_Entry = std::move (aNext);
      else
      {
         for (size_t i = 0; i < aNext.size (); i++)
         {
            if (S.aMesh_Entry[i].pInstanceOwner != aNext[i].pInstanceOwner  ||  S.aMesh_Entry[i].nDrawIx != aNext[i].nDrawIx)
            {
               S.aMesh_Entry = std::move (aNext);
               break;
            }
         }
      }

      nAdmitCreatesLast = nCreate_Geometry;

      return bDirty;
   }

   bool SyncBoxes (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, const UV_SPHERE& UnitBox, bool bUnitBoxReady, const std::vector<BOX_DATA>& aBox_Data)
   {
      bool bDirty = false;
      (void) bUnitBoxReady;

      // Grow-only. Developer bounding boxes are view-culled, so the submitted
      // count chatters as the camera moves. Releasing the extras on every dip
      // churns geometry/material/instance objects, so spare slots are kept and
      // only the live prefix [0, aBox_Data.size()) is bound into the world. That
      // is what hides the spares -- omission from the instance list, never a
      // zero-scale transform (Filament treats that as singular and culls the
      // instance for good) and never opacity 0.
      if (!aBox_Data.empty ())
         BoxShared_Ensure (pDevice, S, UnitBox);

      if (S.pBoxPositionArray)
      {
         while (S.aBox_Entry.size () < aBox_Data.size ())
         {
            RENDERER::ANARI::SCENE_STATE::BOX_ENTRY Box_Entry;
            BoxEntry_Create (pDevice, S, Box_Entry, aBox_Data[S.aBox_Entry.size ()]);
            S.aBox_Entry.push_back (Box_Entry);
            bDirty = true;
         }
      }

      return bDirty;
   }

   bool SyncPanels (ANARIDevice pDevice, RENDERER::ANARI::SCENE_STATE& S, const std::vector<PANEL_DATA>& aPanel_Data)
   {
      bool bDirty = false;
      std::vector<char> aUsed (S.aPanel_Entry.size (), 0);
      std::vector<RENDERER::ANARI::SCENE_STATE::PANEL_ENTRY> aNext;

      if (!aPanel_Data.empty ())
         PanelShared_Ensure (pDevice, S);

      for (const PANEL_DATA& Panel_Data : aPanel_Data)
      {
         int nFound = -1;
         for (size_t i = 0; i < S.aPanel_Entry.size (); i++)
         {
            if (!aUsed[i]  &&  S.aPanel_Entry[i].pPixelKey == Panel_Data.pbPixels)
            {
               nFound = static_cast<int> (i);
               break;
            }
         }

         if (nFound >= 0)
         {
            aUsed[static_cast<size_t> (nFound)] = 1;
            aNext.push_back (S.aPanel_Entry[static_cast<size_t> (nFound)]);
         }
         else
         {
            RENDERER::ANARI::SCENE_STATE::PANEL_ENTRY Panel_Entry;
            PanelEntry_Create (pDevice, S, Panel_Entry, Panel_Data);
            aNext.push_back (Panel_Entry);
            bDirty = true;
         }
      }

      for (size_t i = 0; i < S.aPanel_Entry.size (); i++)
      {
         if (!aUsed[i])
         {
            PanelEntry_Retire (S, S.aPanel_Entry[i]);
            bDirty = true;
         }
      }

      S.aPanel_Entry = std::move (aNext);
      return bDirty;
   }

   // nBox_Bind is how many box pool slots to include, counted from the front.
   // The pool is grow-only, so this is the live prefix -- spare slots past it
   // are simply left out of the world.
   void BindWorldInstances (ANARIDevice pDevice, ANARIWorld pWorld, RENDERER::ANARI::SCENE_STATE& S, size_t nBox_Bind)
   {
      S.aWorldInstanceHandle.clear ();

      for (const RENDERER::ANARI::SCENE_STATE::SPHERE_ENTRY& Sphere_Entry : S.aSphere_Entry)
      {
         if (Sphere_Entry.pInstance)
            S.aWorldInstanceHandle.push_back (Sphere_Entry.pInstance);
      }

      if (S.pSurfaceInstance)
         S.aWorldInstanceHandle.push_back (S.pSurfaceInstance);

      if (nBox_Bind > S.aBox_Entry.size ())
         nBox_Bind = S.aBox_Entry.size ();

      for (size_t i = 0; i < nBox_Bind; i++)
      {
         if (S.aBox_Entry[i].pInstance)
            S.aWorldInstanceHandle.push_back (S.aBox_Entry[i].pInstance);
      }

      for (const RENDERER::ANARI::SCENE_STATE::PANEL_ENTRY& Panel_Entry : S.aPanel_Entry)
      {
         if (Panel_Entry.pInstance)
            S.aWorldInstanceHandle.push_back (Panel_Entry.pInstance);
      }

      for (const RENDERER::ANARI::SCENE_STATE::MESH_ENTRY& Mesh_Entry : S.aMesh_Entry)
      {
         if (Mesh_Entry.pInstance)
            S.aWorldInstanceHandle.push_back (Mesh_Entry.pInstance);
      }

      if (!S.aWorldInstanceHandle.empty ())
      {
         ANARIArray1D pWorldInstanceArray = anariNewArray1D (pDevice, S.aWorldInstanceHandle.data (), nullptr, nullptr, ANARI_INSTANCE, S.aWorldInstanceHandle.size ());
         anariSetParameter (pDevice, pWorld, "instance", ANARI_ARRAY1D, &pWorldInstanceArray);
         if (S.pWorldInstanceArray)
            anariRelease (pDevice, S.pWorldInstanceArray);
         S.pWorldInstanceArray = pWorldInstanceArray;
      }
      else
      {
         anariUnsetParameter (pDevice, pWorld, "instance");
         if (S.pWorldInstanceArray)
         {
            anariRelease (pDevice, S.pWorldInstanceArray);
            S.pWorldInstanceArray = nullptr;
         }
      }

      S.nBox_Bound = nBox_Bind;
   }
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
      if (!m_bUnitBoxReady)
      {
         GenerateUnitBox (m_pUnitBox);
         m_bUnitBoxReady = true;
      }

      if (SceneNeedsInstanceSync (*m_pSceneState, m_aBox_Data, m_aPanel_Data, m_aMesh_Data, m_bBoundingBoxOverlay))
      {
         bool bBind = false;
         bBind = SyncBoxes (m_pDevice, *m_pSceneState, m_pUnitBox, m_bUnitBoxReady, m_aBox_Data)  ||  bBind;
         bBind = SyncPanels (m_pDevice, *m_pSceneState, m_aPanel_Data)  ||  bBind;
         bBind = SyncMeshes (m_pDevice, *m_pSceneState, m_aMesh_Data, m_pEngine, m_dLoadElapsed, m_dLastDisplaySeconds, m_bLastPresented, m_nAdmitGeometry, m_nAdmitCreatesLast)  ||  bBind;
         size_t nBoxBind = m_bBoundingBoxOverlay ? m_aBox_Data.size () : 0;
         if (nBoxBind > m_pSceneState->aBox_Entry.size ())
            nBoxBind = m_pSceneState->aBox_Entry.size ();

         if (bBind  ||  nBoxBind != m_pSceneState->nBox_Bound)
            BindWorldInstances (m_pDevice, m_pWorld, *m_pSceneState, nBoxBind);
      }

      UpdateScene (m_aSphere_Data, m_aCurve_Data, m_aBox_Data, m_aPanel_Data, m_aMesh_Data);
   }

   anariCommitParameters (m_pDevice, m_pWorld);
   anariCommitParameters (m_pDevice, m_pFrame);

   auto tpRenderStart = std::chrono::steady_clock::now ();
   m_dLastSubmitSeconds = std::chrono::duration<double> (tpRenderStart - tpSubmitStart).count ();

   anariRenderFrame (m_pDevice, m_pFrame);
   anariFrameReady (m_pDevice, m_pFrame, ANARI_WAIT);

   m_bLastPresented = true;
   if (m_pDevice  &&  m_pFrame)
   {
      uint32_t nPresented = 1;

      if (anariGetProperty (m_pDevice, m_pFrame, "presented", ANARI_UINT32, &nPresented, sizeof (nPresented), ANARI_NO_WAIT))
         m_bLastPresented = (nPresented != 0);
   }

   // The world has now been finalized and rendered without anything on the
   // retirement queue, so Filament has released the matching Renderables and
   // these handles are finally safe to drop.
   DrainRetired ();

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

// Releases everything the retirement queue is holding. Safe only once a frame
// has been finalized with these objects absent from the world.
void RENDERER::ANARI::DrainRetired ()
{
   if (m_pSceneState  &&  m_pDevice)
   {
      for (ANARIObject pObject : m_pSceneState->aRetire)
         anariRelease (m_pDevice, pObject);

      m_pSceneState->aRetire.clear ();
   }
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
//  SceneNeedsRebuild - detect structural changes (count, texture transitions)
// ---------------------------------------------------------------------------

bool RENDERER::ANARI::SceneNeedsRebuild (const std::vector<SPHERE_DATA>& aSphere_Data, const std::vector<CURVE_DATA>& aCurve_Data, const std::vector<BOX_DATA>& aBox_Data, const std::vector<PANEL_DATA>& aPanel_Data, const std::vector<MESH_DATA>& aMesh_Data) const
{
   (void) aBox_Data;
   (void) aPanel_Data;
   (void) aMesh_Data;

   const SCENE_STATE& S = *m_pSceneState;
   bool bRebuild = false;

   if (aSphere_Data.size () != S.aSphere_Entry.size ())
      bRebuild = true;

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
//  ReleaseScene - free all retained ANARI handles
// ---------------------------------------------------------------------------

void RENDERER::ANARI::ReleaseScene ()
{
   if (!m_pSceneState  ||  !m_pDevice)
      return;

   SCENE_STATE& S = *m_pSceneState;

   // Empty the world first so the frame that follows this rebuild finalizes it
   // without the old instances and lights, which is what lets Filament drop
   // their Renderables. Everything below is retired, not released -- see
   // Retire() for why releasing here aborts the process.
   anariUnsetParameter (m_pDevice, m_pWorld, "instance");
   anariUnsetParameter (m_pDevice, m_pWorld, "light");
   anariCommitParameters (m_pDevice, m_pWorld);

   for (SCENE_STATE::SPHERE_ENTRY& Sphere_Entry : S.aSphere_Entry)
   {
      Retire (S, Sphere_Entry.pInstance);
      Retire (S, Sphere_Entry.pGroup);
      Retire (S, Sphere_Entry.pSurface);
      Retire (S, Sphere_Entry.pMaterial);
      Retire (S, Sphere_Entry.pColorArray);
      Retire (S, Sphere_Entry.pGeometry);
   }
   S.aSphere_Entry.clear ();

   for (SCENE_STATE::CURVE_ENTRY& Curve_Entry : S.aCurve_Entry)
   {
      Retire (S, Curve_Entry.pSurface);
      Retire (S, Curve_Entry.pMaterial);
      Retire (S, Curve_Entry.pGeometry);
   }
   S.aCurve_Entry.clear ();

   for (SCENE_STATE::BOX_ENTRY& Box_Entry : S.aBox_Entry)
      BoxEntry_Retire (S, Box_Entry);
   S.aBox_Entry.clear ();

   for (SCENE_STATE::PANEL_ENTRY& Panel_Entry : S.aPanel_Entry)
      PanelEntry_Retire (S, Panel_Entry);
   S.aPanel_Entry.clear ();

   for (SCENE_STATE::MESH_ENTRY& Mesh_Entry : S.aMesh_Entry)
      MeshEntry_Retire (S, Mesh_Entry);
   S.aMesh_Entry.clear ();

   // MeshEntry_Retire drops group/geometry/texture refs. Force-clear anything
   // left behind if a refcount drifted, so teardown never leaves live GPU
   // objects after the empty-world flush.
   for (auto& Pair : S.mapGroup)
   {
      Retire (S, Pair.second.pGroup);
      Retire (S, Pair.second.pSurface);
      Retire (S, Pair.second.pMaterial);
   }
   S.mapGroup.clear ();

   for (auto& Pair : S.mapGeometry)
   {
      Retire (S, Pair.second.pIndexArray);
      Retire (S, Pair.second.pUvArray);
      Retire (S, Pair.second.pNormalArray);
      Retire (S, Pair.second.pPositionArray);
      Retire (S, Pair.second.pGeometry);
   }
   S.mapGeometry.clear ();

   for (auto& Pair : S.mapTexture)
   {
      Retire (S, Pair.second.pSampler);
      Retire (S, Pair.second.pImageArray);
   }
   S.mapTexture.clear ();

   Retire (S, S.pQuadIndexArray);    S.pQuadIndexArray    = nullptr;
   Retire (S, S.pQuadUvArray);       S.pQuadUvArray       = nullptr;
   Retire (S, S.pQuadNormalArray);   S.pQuadNormalArray   = nullptr;
   Retire (S, S.pQuadPositionArray); S.pQuadPositionArray = nullptr;

   Retire (S, S.pWorldInstanceArray); S.pWorldInstanceArray = nullptr;
   Retire (S, S.pSurfaceInstance);    S.pSurfaceInstance    = nullptr;
   Retire (S, S.pSurfaceGroup);       S.pSurfaceGroup       = nullptr;
   Retire (S, S.pLightArray);         S.pLightArray         = nullptr;
   for (ANARILight pLight : S.aLight)  Retire (S, pLight);
   S.aLight.clear ();
   Retire (S, S.pSharedIndexArray);    S.pSharedIndexArray    = nullptr;
   Retire (S, S.pSharedNormalArray);   S.pSharedNormalArray   = nullptr;
   Retire (S, S.pSharedPositionArray); S.pSharedPositionArray = nullptr;

   Retire (S, S.pBoxIndexArray);    S.pBoxIndexArray    = nullptr;
   Retire (S, S.pBoxNormalArray);   S.pBoxNormalArray   = nullptr;
   Retire (S, S.pBoxPositionArray); S.pBoxPositionArray = nullptr;

   S.aWorldInstanceHandle.clear ();
   S.nBox_Bound = 0;
   S.bBuilt = false;
}

// ---------------------------------------------------------------------------
//  BuildScene - create all ANARI objects and retain handles
// ---------------------------------------------------------------------------

namespace
{
   // FNV-1a over the raw control-point bytes. Curve data is re-submitted every
   // frame into fresh storage, so there is no stable pointer to key off (the way
   // the glTF path keys off its persistent vertex buffer); a content fingerprint
   // is the cheapest reliable "did this change" test. Paired with a point-count
   // check at the call site, collisions are a non-issue in practice.
   uint64_t Hash_Points (const std::vector<CURVE_POINT>& aPoint)
   {
      uint64_t nHash = 14695981039346656037ull;
      const uint8_t* pByte = reinterpret_cast<const uint8_t*> (aPoint.data ());
      const size_t   nByte = aPoint.size () * sizeof (CURVE_POINT);
      for (size_t i = 0; i < nByte; i++)
      {
         nHash ^= pByte[i];
         nHash *= 1099511628211ull;
      }
      return nHash;
   }
}

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

      Sphere_Entry.dCommX = static_cast<float> (Sphere_Data.vPosition.dX);
      Sphere_Entry.dCommY = static_cast<float> (Sphere_Data.vPosition.dY);
      Sphere_Entry.dCommZ = static_cast<float> (Sphere_Data.vPosition.dZ);
      Sphere_Entry.dCommR = Sphere_Data.fRadius;

      S.aSphere_Entry.push_back (Sphere_Entry);

   }

   // --- Curves ---

   for (const CURVE_DATA& Curve_Data : aCurve_Data)
   {
      if (Curve_Data.aPoints.empty ()) continue;

      SCENE_STATE::CURVE_ENTRY Curve_Entry;
      Curve_Entry.nPointCount = Curve_Data.aPoints.size ();
      Curve_Entry.nPointHash  = Hash_Points (Curve_Data.aPoints);


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

   if (m_bBoundingBoxOverlay  &&  !aBox_Data.empty ())
      BoxShared_Ensure (m_pDevice, S, m_pUnitBox);

   if (S.pBoxPositionArray)
   {
      for (const BOX_DATA& Box_Data : aBox_Data)
      {
         SCENE_STATE::BOX_ENTRY Box_Entry;
         BoxEntry_Create (m_pDevice, S, Box_Entry, Box_Data);

         aInstanceHandle.push_back (Box_Entry.pInstance);

         S.aBox_Entry.push_back (Box_Entry);
      }
   }

   // --- Panels (unlit, alpha-blended textured quads; one instance per panel) ---

   if (!aPanel_Data.empty ())
   {
      PanelShared_Ensure (m_pDevice, S);

      for (const PANEL_DATA& Panel_Data : aPanel_Data)
      {
         SCENE_STATE::PANEL_ENTRY Panel_Entry;
         PanelEntry_Create (m_pDevice, S, Panel_Entry, Panel_Data);

         aInstanceHandle.push_back (Panel_Entry.pInstance);

         S.aPanel_Entry.push_back (Panel_Entry);
      }
   }

   // --- Meshes (loaded glTF; one ANARI instance per placed draw, shared
   //     geometry/group for identical primitives) ---
   //
   // The node's world transform rides the ANARI instance (set below and
   // refreshed each frame by UpdateScene) exactly like spheres, boxes and
   // panels. Vertex streams pass through model-local and untouched, so a moving
   // node or a shifting render scale is a cheap 16-float matrix swap rather than
   // a full CPU re-bake + re-upload of every vertex. New unique geometry is
   // admitted a few uploads per frame; instance-only creates of an already-
   // resident primitive are capped separately. Later frames finish via SyncMeshes.

   SyncMeshes (m_pDevice, S, aMesh_Data, m_pEngine, m_dLoadElapsed, m_dLastDisplaySeconds, m_bLastPresented, m_nAdmitGeometry, m_nAdmitCreatesLast);

   for (const SCENE_STATE::MESH_ENTRY& Mesh_Entry : S.aMesh_Entry)
   {
      if (Mesh_Entry.pInstance)
         aInstanceHandle.push_back (Mesh_Entry.pInstance);
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

   S.aWorldInstanceHandle = aInstanceHandle;
   if (!S.aWorldInstanceHandle.empty ())
   {
      S.pWorldInstanceArray = anariNewArray1D (m_pDevice, S.aWorldInstanceHandle.data (), nullptr, nullptr, ANARI_INSTANCE, S.aWorldInstanceHandle.size ());
      anariSetParameter (m_pDevice, m_pWorld, "instance", ANARI_ARRAY1D, &S.pWorldInstanceArray);
   }
   else
   {
      anariUnsetParameter (m_pDevice, m_pWorld, "instance");
   }
   S.nBox_Bound = S.aBox_Entry.size ();

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
//  UpdateScene - update transforms and curve positions (no object creation)
// ---------------------------------------------------------------------------

void RENDERER::ANARI::UpdateScene (const std::vector<SPHERE_DATA>& aSphere_Data, const std::vector<CURVE_DATA>& aCurve_Data, const std::vector<BOX_DATA>& aBox_Data, const std::vector<PANEL_DATA>& aPanel_Data, const std::vector<MESH_DATA>& aMesh_Data)
{
   SCENE_STATE& S = *m_pSceneState;

   // Instances are not change-observed by the World, so committing an instance
   // transform on its own never re-runs World::finalize -- the sole place ANARI
   // transforms reach Filament. Track whether any transform actually moved and,
   // if so, nudge the World once at the end. Geometry edits (non-textured
   // spheres, curves) don't need the nudge: the World observes their geometry
   // and re-finalizes on its own when they re-commit.
   bool bTransformDirty = false;

   for (size_t i = 0; i < aSphere_Data.size ()  &&  i < S.aSphere_Entry.size (); i++)
   {
      const SPHERE_DATA& Sphere_Data = aSphere_Data[i];
      SCENE_STATE::SPHERE_ENTRY& Sphere_Entry = S.aSphere_Entry[i];

      // Both the textured sphere's transform and the non-textured sphere's baked
      // geometry are a pure function of centre + radius. Skip the whole update
      // when neither changed -- this is what stops the per-frame commitSphere /
      // buffer teardown in Halogen.
      if (static_cast<float> (Sphere_Data.vPosition.dX) == Sphere_Entry.dCommX  &&
          static_cast<float> (Sphere_Data.vPosition.dY) == Sphere_Entry.dCommY  &&
          static_cast<float> (Sphere_Data.vPosition.dZ) == Sphere_Entry.dCommZ  &&
          Sphere_Data.fRadius == Sphere_Entry.dCommR)
         continue;

      Sphere_Entry.dCommX = static_cast<float> (Sphere_Data.vPosition.dX);
      Sphere_Entry.dCommY = static_cast<float> (Sphere_Data.vPosition.dY);
      Sphere_Entry.dCommZ = static_cast<float> (Sphere_Data.vPosition.dZ);
      Sphere_Entry.dCommR = Sphere_Data.fRadius;

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
         bTransformDirty = true;

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
      nCurveIndex++;

      // Re-tessellate (commitCurve: Catmull-Rom + parallel-transport frames +
      // fresh GPU buffers) only when the control points actually change.
      const uint64_t nHash = Hash_Points (Curve_Data.aPoints);
      if (Curve_Data.aPoints.size () == Curve_Entry.nPointCount  &&  nHash == Curve_Entry.nPointHash)
         continue;

      Curve_Entry.nPointCount = Curve_Data.aPoints.size ();
      Curve_Entry.nPointHash  = nHash;


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

   }

   for (size_t i = 0; i < aBox_Data.size ()  &&  i < S.aBox_Entry.size (); i++)
   {
      SCENE_STATE::BOX_ENTRY& Box_Entry = S.aBox_Entry[i];
      if (!Box_Entry.pInstance  ||  !Box_Entry.pMaterial)
         continue;

      const RGB& rgb = aBox_Data[i].rgbColor;
      if (Box_Entry.rgbComm.fR != rgb.fR  ||  Box_Entry.rgbComm.fG != rgb.fG  ||  Box_Entry.rgbComm.fB != rgb.fB)
      {
         // Swap in a fresh material rather than re-committing this one, and keep
         // the old handle alive on the retirement queue until the frame that
         // rebuilds the Renderable has finished. See BoxMaterial_Create.
         Retire (S, Box_Entry.pMaterial);
         Box_Entry.pMaterial = BoxMaterial_Create (m_pDevice, rgb);
         anariSetParameter (m_pDevice, Box_Entry.pSurface, "material", ANARI_MATERIAL, &Box_Entry.pMaterial);
         anariCommitParameters (m_pDevice, Box_Entry.pSurface);
         Box_Entry.rgbComm = rgb;
      }

      const float* pfWorld = aBox_Data[i].mWorld.f;
      if (std::memcmp (Box_Entry.m16Comm, pfWorld, sizeof (Box_Entry.m16Comm)) != 0)
      {
         std::memcpy (Box_Entry.m16Comm, pfWorld, sizeof (Box_Entry.m16Comm));
         anariSetParameter (m_pDevice, Box_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, pfWorld);
         anariCommitParameters (m_pDevice, Box_Entry.pInstance);
         bTransformDirty = true;
      }
   }

   for (size_t i = 0; i < aPanel_Data.size ()  &&  i < S.aPanel_Entry.size (); i++)
   {
      SCENE_STATE::PANEL_ENTRY& Panel_Entry = S.aPanel_Entry[i];
      if (std::memcmp (Panel_Entry.m16Comm, aPanel_Data[i].mWorld.f, sizeof (Panel_Entry.m16Comm)) == 0)
         continue;
      std::memcpy (Panel_Entry.m16Comm, aPanel_Data[i].mWorld.f, sizeof (Panel_Entry.m16Comm));
      anariSetParameter (m_pDevice, Panel_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, aPanel_Data[i].mWorld.f);
      anariCommitParameters (m_pDevice, Panel_Entry.pInstance);
      bTransformDirty = true;

   }

   for (SCENE_STATE::MESH_ENTRY& Mesh_Entry : S.aMesh_Entry)
   {
      if (!Mesh_Entry.pInstance)
         continue;

      const MESH_DATA* pMesh_Data = nullptr;

      for (const MESH_DATA& Mesh_Data : aMesh_Data)
      {
         if (Mesh_IsDrawable (Mesh_Data)  &&  Mesh_InstanceMatch (Mesh_Entry, Mesh_Data))
         {
            pMesh_Data = &Mesh_Data;
            break;
         }
      }

      if (!pMesh_Data)
         continue;

      if (std::memcmp (Mesh_Entry.m16Comm, pMesh_Data->mWorld.f, sizeof (Mesh_Entry.m16Comm)) == 0)
         continue;
      std::memcpy (Mesh_Entry.m16Comm, pMesh_Data->mWorld.f, sizeof (Mesh_Entry.m16Comm));
      anariSetParameter (m_pDevice, Mesh_Entry.pInstance, "transform", ANARI_FLOAT32_MAT4, pMesh_Data->mWorld.f);
      anariCommitParameters (m_pDevice, Mesh_Entry.pInstance);
      bTransformDirty = true;
   }

   // Force one World::finalize so the moved transforms actually reach Filament.
   // helium's setParameter only bumps an object's parameter clock when the value
   // differs, so re-setting the same instance-array handle would be a no-op;
   // unset-then-set the identical handle to make the change register. The
   // anariCommitParameters(m_pWorld) already issued each frame in EndFrame then
   // runs exactly one finalize -- no geometry buffers are rebuilt.
   if (bTransformDirty  &&  S.pWorldInstanceArray)
   {
      anariUnsetParameter (m_pDevice, m_pWorld, "instance");
      anariSetParameter (m_pDevice, m_pWorld, "instance", ANARI_ARRAY1D, &S.pWorldInstanceArray);

   }
}


