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
// Real OpenXR implementation. Compiled when SNEEZE_ENABLE_XR is ON. The
// SDL-only / no-XR build picks XrRuntime_Stub.cpp instead.

#include <Sneeze.h>
#include "xr/XrRuntime.h"

#include <openxr/openxr.h>

#include <cstdlib>
#include <cstring>

namespace SNEEZE { namespace DEP {

class XR_RUNTIME::Impl
{
public:
   ENGINE*     m_pEngine    = nullptr;
   XrInstance  hInstance    = XR_NULL_HANDLE;
   bool        bHasRuntime  = false;
   std::string sRuntimeName;
};

XR_RUNTIME::XR_RUNTIME () : m_pImpl (new Impl ())
{
}

XR_RUNTIME::~XR_RUNTIME ()
{
   if (m_pImpl->hInstance != XR_NULL_HANDLE)
   {
      xrDestroyInstance (m_pImpl->hInstance);
      m_pImpl->hInstance = XR_NULL_HANDLE;
   }
   delete m_pImpl;
}

bool XR_RUNTIME::Initialize (ENGINE* pEngine)
{
   m_pImpl->m_pEngine = pEngine;
   // Suppress the loader's own stderr diagnostics - we handle all error cases
   // ourselves with clearer messages. Without this, the loader prints alarming
   // "Error [GENERAL | xrCreateInstance | OpenXR-Loader]" lines on machines
   // that simply don't have a VR/AR runtime installed.
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

   XrInstanceCreateInfo pCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
   pCreateInfo.applicationInfo        = pAppInfo;
   pCreateInfo.enabledApiLayerCount   = 0;
   pCreateInfo.enabledExtensionCount  = 0;

   XrResult nResult = xrCreateInstance (&pCreateInfo, &m_pImpl->hInstance);
   if (XR_FAILED (nResult))
   {
      m_pImpl->bHasRuntime = false;
      pEngine->Log (IENGINE::kLOGLEVEL_Warning, "XR_RUNTIME",
         "OpenXR loader initialized - no XR runtime detected (code " + std::to_string (nResult) + ")");
      pEngine->Log (IENGINE::kLOGLEVEL_Warning, "XR_RUNTIME",
         "This is normal on machines without a VR/AR headset or runtime installed.");
      return true;
   }

   m_pImpl->bHasRuntime = true;

   XrInstanceProperties pProps = { XR_TYPE_INSTANCE_PROPERTIES };
   if (XR_SUCCEEDED (xrGetInstanceProperties (m_pImpl->hInstance, &pProps)))
   {
      m_pImpl->sRuntimeName = pProps.runtimeName;
      pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
         "OpenXR " + std::to_string (XR_VERSION_MAJOR (XR_CURRENT_API_VERSION)) + "."
         + std::to_string (XR_VERSION_MINOR (XR_CURRENT_API_VERSION)) + "."
         + std::to_string (XR_VERSION_PATCH (XR_CURRENT_API_VERSION))
         + " initialized - runtime: " + pProps.runtimeName
         + " (v" + std::to_string (XR_VERSION_MAJOR (pProps.runtimeVersion)) + "."
         + std::to_string (XR_VERSION_MINOR (pProps.runtimeVersion)) + "."
         + std::to_string (XR_VERSION_PATCH (pProps.runtimeVersion)) + ")");
   }

   return true;
}

bool XR_RUNTIME::HasRuntime () const          { return m_pImpl->bHasRuntime;   }
std::string XR_RUNTIME::GetRuntimeName () const { return m_pImpl->sRuntimeName; }

}} // namespace SNEEZE::DEP
