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
//
// Real OpenXR implementation. Compiled when SNEEZE_ENABLE_XR is ON.
// Galaxy XR face/body session lives in XrRuntime_Android.cpp (__ANDROID__).

#include "xr/XrRuntimeImpl.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef XR_ANDROID_FACE_TRACKING_EXTENSION_NAME
#define XR_ANDROID_FACE_TRACKING_EXTENSION_NAME "XR_ANDROID_face_tracking"
#endif
#ifndef XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME
#define XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME "XR_ANDROIDSYS_body_tracking"
#endif
#ifndef XR_EXT_HAND_TRACKING_EXTENSION_NAME
#define XR_EXT_HAND_TRACKING_EXTENSION_NAME "XR_EXT_hand_tracking"
#endif
// Android XR Unity / docs also advertise this experimental name.
#ifndef XR_ANDROIDX_BODY_TRACKING_EXTENSION_NAME
#define XR_ANDROIDX_BODY_TRACKING_EXTENSION_NAME "XR_ANDROIDX_body_tracking"
#endif
#ifndef XR_ANDROID_BODY_TRACKING_EXTENSION_NAME
#define XR_ANDROID_BODY_TRACKING_EXTENSION_NAME "XR_ANDROID_body_tracking"
#endif

namespace SNEEZE { namespace DEP {

XR_RUNTIME::XR_RUNTIME (ENGINE* pEngine) : m_pImpl (new Impl ())
{
   m_pImpl->m_pEngine = pEngine;
}

XR_RUNTIME::~XR_RUNTIME ()
{
   XrAndroid_EndSession (m_pImpl);
   if (m_pImpl->hInstance != XR_NULL_HANDLE)
   {
      xrDestroyInstance (m_pImpl->hInstance);
      m_pImpl->hInstance = XR_NULL_HANDLE;
   }
   delete m_pImpl;
}

static bool ExtNameEq (const char* a, const char* b)
{
   return a && b && std::strcmp (a, b) == 0;
}

void XR_RUNTIME::RefreshExtensionProbe ()
{
   m_pImpl->caps.aEnumeratedExtensions.clear ();
   m_pImpl->caps.bExtensionFaceAndroid = false;
   m_pImpl->caps.bExtensionBodyAndroidSys = false;
   m_pImpl->caps.bExtensionHandExt = false;

   uint32_t nExtCount = 0;
   XrResult nResult = xrEnumerateInstanceExtensionProperties (nullptr, 0, &nExtCount, nullptr);
   if (XR_FAILED (nResult) || nExtCount == 0)
   {
      if (m_pImpl->m_pEngine)
         m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
            "Extension probe: no instance extensions available (no runtime or empty list).");
      return;
   }

   std::vector<XrExtensionProperties> aExt (nExtCount, { XR_TYPE_EXTENSION_PROPERTIES });
   nResult = xrEnumerateInstanceExtensionProperties (nullptr, nExtCount, &nExtCount, aExt.data ());
   if (XR_FAILED (nResult))
      return;

   bool bBodyAndroid = false;
   bool bBodyAndroidX = false;
   bool bBodyAndroidSys = false;

   for (uint32_t i = 0; i < nExtCount; ++i)
   {
      const char* name = aExt[i].extensionName;
      m_pImpl->caps.aEnumeratedExtensions.emplace_back (name);

      if (ExtNameEq (name, XR_ANDROID_FACE_TRACKING_EXTENSION_NAME))
         m_pImpl->caps.bExtensionFaceAndroid = true;
      if (ExtNameEq (name, XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME))
         bBodyAndroidSys = true;
      if (ExtNameEq (name, XR_ANDROIDX_BODY_TRACKING_EXTENSION_NAME))
         bBodyAndroidX = true;
      if (ExtNameEq (name, XR_ANDROID_BODY_TRACKING_EXTENSION_NAME))
         bBodyAndroid = true;
      if (ExtNameEq (name, XR_EXT_HAND_TRACKING_EXTENSION_NAME))
         m_pImpl->caps.bExtensionHandExt = true;
   }

   m_pImpl->caps.bExtensionBodyAndroidSys = bBodyAndroidSys || bBodyAndroidX || bBodyAndroid;

   if (m_pImpl->m_pEngine)
   {
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
         "Extension probe: count=" + std::to_string (nExtCount)
         + " face_android=" + (m_pImpl->caps.bExtensionFaceAndroid ? "yes" : "no")
         + " body_android=" + (m_pImpl->caps.bExtensionBodyAndroidSys ? "yes" : "no")
         + " hand_ext=" + (m_pImpl->caps.bExtensionHandExt ? "yes" : "no"));
   }
}

bool XR_RUNTIME::Initialize ()
{
#ifdef _WIN32
   _putenv_s ("XR_LOADER_DEBUG", "none");
#else
   setenv ("XR_LOADER_DEBUG", "none", 1);
#endif

   XrApplicationInfo pAppInfo = {};
   std::strncpy (pAppInfo.applicationName, "Sneeze", XR_MAX_APPLICATION_NAME_SIZE);
   pAppInfo.applicationVersion = 1;
   std::strncpy (pAppInfo.engineName, "MBE", XR_MAX_ENGINE_NAME_SIZE);
   pAppInfo.engineVersion = 1;
   pAppInfo.apiVersion    = XR_API_VERSION_1_0;

   // Probe extensions before creating instance (works without a runtime on some loaders;
   // on failure we still try a zero-extension instance for HasRuntime detection).
   RefreshExtensionProbe ();

   std::vector<const char*> enabled;
   // On Android, enable Galaxy XR face/body when present. Desktop creates a
   // minimal instance for runtime detection only (extensions often need a session).
#ifdef __ANDROID__
   if (m_pImpl->caps.bExtensionFaceAndroid)
      enabled.push_back (XR_ANDROID_FACE_TRACKING_EXTENSION_NAME);
   if (m_pImpl->caps.bExtensionBodyAndroidSys)
   {
      // Prefer ANDROIDSYS name from jetpack-xr-natives; fall back to documented aliases.
      bool pushed = false;
      for (const std::string& e : m_pImpl->caps.aEnumeratedExtensions)
      {
         if (e == XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME)
         {
            enabled.push_back (XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME);
            pushed = true;
            break;
         }
      }
      if (!pushed)
      {
         for (const std::string& e : m_pImpl->caps.aEnumeratedExtensions)
         {
            if (e == XR_ANDROIDX_BODY_TRACKING_EXTENSION_NAME)
            {
               enabled.push_back (XR_ANDROIDX_BODY_TRACKING_EXTENSION_NAME);
               pushed = true;
               break;
            }
            if (e == XR_ANDROID_BODY_TRACKING_EXTENSION_NAME)
            {
               enabled.push_back (XR_ANDROID_BODY_TRACKING_EXTENSION_NAME);
               pushed = true;
               break;
            }
         }
      }
   }
   if (m_pImpl->caps.bExtensionHandExt)
      enabled.push_back (XR_EXT_HAND_TRACKING_EXTENSION_NAME);
#endif

   XrInstanceCreateInfo pCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
   pCreateInfo.applicationInfo        = pAppInfo;
   pCreateInfo.enabledApiLayerCount   = 0;
   pCreateInfo.enabledExtensionCount  = static_cast<uint32_t> (enabled.size ());
   pCreateInfo.enabledExtensionNames  = enabled.empty () ? nullptr : enabled.data ();

   XrResult nResult = xrCreateInstance (&pCreateInfo, &m_pImpl->hInstance);
   if (XR_FAILED (nResult))
   {
      m_pImpl->bHasRuntime = false;
      m_pImpl->caps.bHasRuntime = false;
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "XR_RUNTIME",
         "OpenXR loader initialized - no XR runtime detected (code " + std::to_string (nResult) + ")");
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "XR_RUNTIME",
         "This is normal on machines without a VR/AR headset or runtime installed.");
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
         "Fixture inject (InjectFaceFixture / InjectBodyFixture) remains available for DGX QA.");
      return true;
   }

   m_pImpl->bHasRuntime = true;
   m_pImpl->caps.bHasRuntime = true;

   XrInstanceProperties pProps = { XR_TYPE_INSTANCE_PROPERTIES };
   if (XR_SUCCEEDED (xrGetInstanceProperties (m_pImpl->hInstance, &pProps)))
   {
      m_pImpl->sRuntimeName = pProps.runtimeName;
      m_pImpl->caps.sRuntimeName = pProps.runtimeName;
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
         "OpenXR " + std::to_string (XR_VERSION_MAJOR (XR_CURRENT_API_VERSION)) + "."
         + std::to_string (XR_VERSION_MINOR (XR_CURRENT_API_VERSION)) + "."
         + std::to_string (XR_VERSION_PATCH (XR_CURRENT_API_VERSION))
         + " initialized - runtime: " + pProps.runtimeName
         + " (v" + std::to_string (XR_VERSION_MAJOR (pProps.runtimeVersion)) + "."
         + std::to_string (XR_VERSION_MINOR (pProps.runtimeVersion)) + "."
         + std::to_string (XR_VERSION_PATCH (pProps.runtimeVersion)) + ")");
   }

   // Re-probe after instance exists (some loaders only list fully after create).
   RefreshExtensionProbe ();
   m_pImpl->caps.bHasRuntime = true;
   m_pImpl->caps.sRuntimeName = m_pImpl->sRuntimeName;

   return true;
}

bool XR_RUNTIME::HasRuntime () const            { return m_pImpl->bHasRuntime;   }
std::string XR_RUNTIME::GetRuntimeName () const { return m_pImpl->sRuntimeName; }

XR_CAPABILITIES XR_RUNTIME::Capabilities () const
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   XR_CAPABILITIES c = m_pImpl->caps;
   c.bFixtureMode = m_pImpl->bFaceFixture || m_pImpl->bBodyFixture;
   c.bHasRuntime = m_pImpl->bHasRuntime;
   c.sRuntimeName = m_pImpl->sRuntimeName;
   return c;
}

void XR_RUNTIME::InjectFaceFixture (const XR_FACE_STATE& face)
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   m_pImpl->faceCached = face;
   m_pImpl->bFaceFixture = true;
   m_pImpl->caps.bFixtureMode = true;
}

void XR_RUNTIME::InjectBodyFixture (const XR_BODY_STATE& body)
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   m_pImpl->bodyCached = body;
   m_pImpl->bBodyFixture = true;
   m_pImpl->caps.bFixtureMode = true;
}

void XR_RUNTIME::ClearFixtures ()
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   m_pImpl->bFaceFixture = false;
   m_pImpl->bBodyFixture = false;
   m_pImpl->faceCached = {};
   m_pImpl->bodyCached = {};
   m_pImpl->caps.bFixtureMode = false;
}

bool XR_RUNTIME::PollFace (XR_FACE_STATE& outFace) const
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   outFace = m_pImpl->faceCached;
   return outFace.bValid || outFace.bTracking || m_pImpl->bFaceFixture;
}

bool XR_RUNTIME::PollBody (XR_BODY_STATE& outBody) const
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   outBody = m_pImpl->bodyCached;
   return outBody.bValid || m_pImpl->bBodyFixture;
}

bool XR_RUNTIME::BeginAndroidSession (void* pJavaVM, void* pActivity, void* pNativeWindow)
{
   return XrAndroid_BeginSession (m_pImpl, pJavaVM, pActivity, pNativeWindow);
}

void XR_RUNTIME::EndAndroidSession ()
{
   XrAndroid_EndSession (m_pImpl);
}

bool XR_RUNTIME::PumpAndroidTracking ()
{
   return XrAndroid_PumpTracking (m_pImpl);
}

void XR_RUNTIME::SetAvatarBindId (const std::string& sId)
{
   m_pImpl->sAvatarBindId = sId;
}

std::string XR_RUNTIME::AvatarBindId () const
{
   return m_pImpl->sAvatarBindId;
}

void XR_RUNTIME::SetBoundMorphWeightsJson (const std::string& json)
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   m_pImpl->sBoundMorphWeightsJson = json;
}

std::string XR_RUNTIME::BoundMorphWeightsJson () const
{
   std::lock_guard<std::mutex> lock (m_pImpl->mutex);
   return m_pImpl->sBoundMorphWeightsJson;
}

}} // namespace SNEEZE::DEP
