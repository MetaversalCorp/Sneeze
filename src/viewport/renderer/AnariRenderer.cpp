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

#include <Sneeze.h>
#include "AnariRenderer.h"
#include "UVSphere.h"
#include <anari/anari.h>

#define ANARI_RENDERER_TYPE ANARI_DATA_TYPE_DEFINE(514)
#undef ANARI_RENDERER

#include <cstring>
#include <chrono>

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

RENDERER::ANARI::ANARI (ENGINE* pEngine, const std::string& sLibrary)
   : m_pEngine (pEngine)
   , m_sLibrary (sLibrary)
   , m_pLibrary (nullptr)
   , m_pDevice (nullptr)
   , m_pWorld (nullptr)
   , m_pCamera (nullptr)
   , m_pRenderer (nullptr)
   , m_pFrame (nullptr)
   , m_pNativeSurface (nullptr)
   , m_pNativeWindow (nullptr)
   , m_bNativeSurface (false)
   , m_nWidth (0)
   , m_nHeight (0)
   , m_bUnitSphereReady (false)
   , m_dLastSubmitSeconds (0.0)
   , m_dLastRenderSeconds (0.0)
{
}

RENDERER::ANARI::~ANARI ()
{
   Shutdown ();
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
   m_pLibrary = anariLoadLibrary (sLibraryArg.c_str (), nullptr, nullptr);
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
         const char* const* pExtensions =
            anariGetDeviceExtensions (reinterpret_cast<ANARILibrary> (m_pLibrary), "default");
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
   }
}

void RENDERER::ANARI::Shutdown ()
{
   if (m_pDevice)
   {
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
   m_bNativeSurface = false;
}

// ---------------------------------------------------------------------------

void RENDERER::ANARI::SetCamera (const CAMERA_DATA& pCamera)
{
   float pos[3] = { pCamera.dPosX, pCamera.dPosY, pCamera.dPosZ };
   float dir[3] = { pCamera.dDirX, pCamera.dDirY, pCamera.dDirZ };
   float up[3]  = { pCamera.dUpX,  pCamera.dUpY,  pCamera.dUpZ };

   anariSetParameter (m_pDevice, m_pCamera, "position", ANARI_FLOAT32_VEC3, pos);
   anariSetParameter (m_pDevice, m_pCamera, "direction", ANARI_FLOAT32_VEC3, dir);
   anariSetParameter (m_pDevice, m_pCamera, "up", ANARI_FLOAT32_VEC3, up);
   anariSetParameter (m_pDevice, m_pCamera, "fovy", ANARI_FLOAT32, &pCamera.dFovY);
   anariSetParameter (m_pDevice, m_pCamera, "aspect", ANARI_FLOAT32, &pCamera.dAspect);
   anariSetParameter (m_pDevice, m_pCamera, "near", ANARI_FLOAT32, &pCamera.dNear);
   anariSetParameter (m_pDevice, m_pCamera, "far", ANARI_FLOAT32, &pCamera.dFar);
   anariCommitParameters (m_pDevice, m_pCamera);
}

void RENDERER::ANARI::BeginFrame ()
{
   m_aSpheres.clear ();
   m_aCurves.clear ();
}

void RENDERER::ANARI::SubmitSpheres (const std::vector<SPHERE_DATA>& aSpheres)
{
   m_aSpheres.insert (m_aSpheres.end (), aSpheres.begin (), aSpheres.end ());
}

void RENDERER::ANARI::SubmitCurves (const std::vector<CURVE_DATA>& aCurves)
{
   m_aCurves.insert (m_aCurves.end (), aCurves.begin (), aCurves.end ());
}

void RENDERER::ANARI::EndFrame ()
{
   auto tpSubmitStart = std::chrono::steady_clock::now ();

   RebuildWorld (m_aSpheres, m_aCurves);

   anariCommitParameters (m_pDevice, m_pWorld);
   anariCommitParameters (m_pDevice, m_pFrame);

   auto tpRenderStart = std::chrono::steady_clock::now ();
   m_dLastSubmitSeconds = std::chrono::duration<double> (tpRenderStart - tpSubmitStart).count ();

   anariRenderFrame (m_pDevice, m_pFrame);
   anariFrameReady (m_pDevice, m_pFrame, ANARI_WAIT);

   auto tpRenderEnd = std::chrono::steady_clock::now ();
   m_dLastRenderSeconds = std::chrono::duration<double> (tpRenderEnd - tpRenderStart).count ();

   // Native-surface mode: Halogen presented directly to the window; no readback.
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
//  RebuildWorld - recreate ANARI scene objects from submitted data
// ---------------------------------------------------------------------------

void RENDERER::ANARI::RebuildWorld (const std::vector<SPHERE_DATA>& aSpheres,
                                    const std::vector<CURVE_DATA>& aCurves)
{
   // --- Generate unit sphere mesh once ---

   if (!m_bUnitSphereReady)
   {
      GenerateUVSphere (m_pUnitSphere, 1.0f, 64, 128, 0.0f, 0.0f, 0.0f);
      m_bUnitSphereReady = true;
   }

   uint64_t nVerts = m_pUnitSphere.aPositions.size () / 3;
   uint64_t nTris  = m_pUnitSphere.aIndices.size () / 3;

   std::vector<ANARISurface>  aSurfaces;
   std::vector<ANARIInstance> aInstances;

   // --- Spheres: textured triangle mesh or analytical sphere per body ---

   for (const auto& s : aSpheres)
   {
      if (s.pTexturePixels  &&  s.nTextureWidth > 0  &&  s.nTextureHeight > 0)
      {
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

         ANARIGeometry pGeom = anariNewGeometry (m_pDevice, "triangle");

         ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, m_pUnitSphere.aPositions.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nVerts);
         ANARIArray1D pNrmArr = anariNewArray1D (m_pDevice, m_pUnitSphere.aNormals.data (), nullptr, nullptr, ANARI_FLOAT32_VEC3, nVerts);
         ANARIArray1D pClrArr = anariNewArray1D (m_pDevice, aColors.data (), nullptr, nullptr, ANARI_FLOAT32_VEC4, nVerts);
         ANARIArray1D pIdxArr = anariNewArray1D (m_pDevice, m_pUnitSphere.aIndices.data (), nullptr, nullptr, ANARI_UINT32_VEC3, nTris);

         anariSetParameter (m_pDevice, pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
         anariSetParameter (m_pDevice, pGeom, "vertex.normal",   ANARI_ARRAY1D, &pNrmArr);
         anariSetParameter (m_pDevice, pGeom, "vertex.color",    ANARI_ARRAY1D, &pClrArr);
         anariSetParameter (m_pDevice, pGeom, "primitive.index",  ANARI_ARRAY1D, &pIdxArr);
         anariCommitParameters (m_pDevice, pGeom);

         anariRelease (m_pDevice, pPosArr);
         anariRelease (m_pDevice, pNrmArr);
         anariRelease (m_pDevice, pClrArr);
         anariRelease (m_pDevice, pIdxArr);

         ANARIMaterial pMat = anariNewMaterial (m_pDevice, "matte");
         float matColor[3] = { 1.0f, 1.0f, 1.0f };
         anariSetParameter (m_pDevice, pMat, "color", ANARI_FLOAT32_VEC3, matColor);
         anariCommitParameters (m_pDevice, pMat);

         ANARISurface pSurf = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, pSurf, "geometry", ANARI_GEOMETRY, &pGeom);
         anariSetParameter (m_pDevice, pSurf, "material", ANARI_MATERIAL, &pMat);
         anariCommitParameters (m_pDevice, pSurf);

         ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, &pSurf, nullptr, nullptr, ANARI_SURFACE, 1);
         ANARIGroup pGroup = anariNewGroup (m_pDevice);
         anariSetParameter (m_pDevice, pGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
         anariCommitParameters (m_pDevice, pGroup);
         anariRelease (m_pDevice, pSurfArr);

         float xfm[16] =
         {
            s.dRadius, 0.0f,      0.0f,      0.0f,
            0.0f,      s.dRadius, 0.0f,      0.0f,
            0.0f,      0.0f,      s.dRadius, 0.0f,
            s.x,       s.y,       s.z,       1.0f,
         };

         ANARIInstance pInst = anariNewInstance (m_pDevice, "transform");
         anariSetParameter (m_pDevice, pInst, "group", ANARI_GROUP, &pGroup);
         anariSetParameter (m_pDevice, pInst, "transform", ANARI_FLOAT32_MAT4, xfm);
         anariCommitParameters (m_pDevice, pInst);

         aInstances.push_back (pInst);

         anariRelease (m_pDevice, pGeom);
         anariRelease (m_pDevice, pMat);
         anariRelease (m_pDevice, pSurf);
         anariRelease (m_pDevice, pGroup);
      }
      else
      {
         ANARIGeometry pGeom = anariNewGeometry (m_pDevice, "sphere");
         float pos[3] = { s.x, s.y, s.z };
         ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, &pos, nullptr, nullptr, ANARI_FLOAT32_VEC3, 1);
         anariSetParameter (m_pDevice, pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
         anariSetParameter (m_pDevice, pGeom, "radius", ANARI_FLOAT32, &s.dRadius);
         anariCommitParameters (m_pDevice, pGeom);
         anariRelease (m_pDevice, pPosArr);

         ANARIMaterial pMat = anariNewMaterial (m_pDevice, "matte");
         float color[3] = { s.r, s.g, s.b };
         anariSetParameter (m_pDevice, pMat, "color", ANARI_FLOAT32_VEC3, color);
         anariCommitParameters (m_pDevice, pMat);

         ANARISurface pSurf = anariNewSurface (m_pDevice);
         anariSetParameter (m_pDevice, pSurf, "geometry", ANARI_GEOMETRY, &pGeom);
         anariSetParameter (m_pDevice, pSurf, "material", ANARI_MATERIAL, &pMat);
         anariCommitParameters (m_pDevice, pSurf);

         aSurfaces.push_back (pSurf);

         anariRelease (m_pDevice, pGeom);
         anariRelease (m_pDevice, pMat);
      }
   }

   // --- Curves: one surface per orbit path ---

   for (const auto& c : aCurves)
   {
      if (c.aPoints.empty ()) continue;

      ANARIGeometry pGeom = anariNewGeometry (m_pDevice, "curve");

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

      ANARIArray1D pPosArr = anariNewArray1D (m_pDevice, aPos.data (), nullptr, nullptr,
                                               ANARI_FLOAT32_VEC3, c.aPoints.size ());
      ANARIArray1D pRadArr = anariNewArray1D (m_pDevice, aRadii.data (), nullptr, nullptr,
                                               ANARI_FLOAT32, c.aPoints.size ());

      anariSetParameter (m_pDevice, pGeom, "vertex.position", ANARI_ARRAY1D, &pPosArr);
      anariSetParameter (m_pDevice, pGeom, "vertex.radius", ANARI_ARRAY1D, &pRadArr);
      anariCommitParameters (m_pDevice, pGeom);

      anariRelease (m_pDevice, pPosArr);
      anariRelease (m_pDevice, pRadArr);

      ANARIMaterial pMat = anariNewMaterial (m_pDevice, "matte");
      float color[3] = { c.r, c.g, c.b };
      anariSetParameter (m_pDevice, pMat, "color", ANARI_FLOAT32_VEC3, color);
      anariCommitParameters (m_pDevice, pMat);

      ANARISurface pSurf = anariNewSurface (m_pDevice);
      anariSetParameter (m_pDevice, pSurf, "geometry", ANARI_GEOMETRY, &pGeom);
      anariSetParameter (m_pDevice, pSurf, "material", ANARI_MATERIAL, &pMat);
      anariCommitParameters (m_pDevice, pSurf);

      aSurfaces.push_back (pSurf);

      anariRelease (m_pDevice, pGeom);
      anariRelease (m_pDevice, pMat);
   }

   // --- Group + instances ---

   if (!aSurfaces.empty ())
   {
      ANARIArray1D pSurfArr = anariNewArray1D (m_pDevice, aSurfaces.data (), nullptr, nullptr,
                                                ANARI_SURFACE, aSurfaces.size ());
      ANARIGroup pGroup = anariNewGroup (m_pDevice);
      anariSetParameter (m_pDevice, pGroup, "surface", ANARI_ARRAY1D, &pSurfArr);
      anariCommitParameters (m_pDevice, pGroup);
      anariRelease (m_pDevice, pSurfArr);

      ANARIInstance pInst = anariNewInstance (m_pDevice, "transform");
      anariSetParameter (m_pDevice, pInst, "group", ANARI_GROUP, &pGroup);
      anariCommitParameters (m_pDevice, pInst);
      anariRelease (m_pDevice, pGroup);

      aInstances.push_back (pInst);
   }

   if (!aInstances.empty ())
   {
      ANARIArray1D pInstArr = anariNewArray1D (m_pDevice, aInstances.data (), nullptr, nullptr,
                                                ANARI_INSTANCE, aInstances.size ());
      anariSetParameter (m_pDevice, m_pWorld, "instance", ANARI_ARRAY1D, &pInstArr);
      anariRelease (m_pDevice, pInstArr);
   }

   // --- Point light at origin (the Sun) ---

   ANARILight pLight = anariNewLight (m_pDevice, "point");
   float lightPos[3] = { 0.0f, 0.0f, 0.0f };
   float lightColor[3] = { 1.0f, 1.0f, 0.95f };
   float lightIntensity = 25.0f;
   anariSetParameter (m_pDevice, pLight, "position", ANARI_FLOAT32_VEC3, lightPos);
   anariSetParameter (m_pDevice, pLight, "color", ANARI_FLOAT32_VEC3, lightColor);
   anariSetParameter (m_pDevice, pLight, "intensity", ANARI_FLOAT32, &lightIntensity);
   anariCommitParameters (m_pDevice, pLight);

   ANARIArray1D pLightArr = anariNewArray1D (m_pDevice, &pLight, nullptr, nullptr,
                                              ANARI_LIGHT, 1);
   anariSetParameter (m_pDevice, m_pWorld, "light", ANARI_ARRAY1D, &pLightArr);
   anariRelease (m_pDevice, pLightArr);
   anariRelease (m_pDevice, pLight);

   for (auto& s : aSurfaces)
      anariRelease (m_pDevice, s);
   for (auto& inst : aInstances)
      anariRelease (m_pDevice, inst);
}

