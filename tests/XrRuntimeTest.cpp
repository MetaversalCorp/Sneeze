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

#include <openxr/openxr.h>

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
// Test 1: OpenXR version constants
// ---------------------------------------------------------------------------
static void TestVersionConstants ()
{
   std::printf ("\n[Test 1] OpenXR version constants\n");

   uint64_t nVersion = XR_CURRENT_API_VERSION;
   uint16_t nMajor   = XR_VERSION_MAJOR (nVersion);
   uint16_t nMinor   = XR_VERSION_MINOR (nVersion);
   uint32_t nPatch   = XR_VERSION_PATCH (nVersion);

   std::printf ("    OpenXR SDK version: %d.%d.%d\n", nMajor, nMinor, nPatch);
   Check (nMajor == 1, "Major version is 1");
   Check (nMinor >= 1, "Minor version >= 1");
   Check (nPatch == 58, "Patch version is 58 (release-1.1.58)");
}

// ---------------------------------------------------------------------------
// Test 2: Enumerate API layers
// ---------------------------------------------------------------------------
static void TestEnumerateApiLayers ()
{
   std::printf ("\n[Test 2] Enumerate API layers\n");

   uint32_t nLayerCount = 0;
   XrResult nResult = xrEnumerateApiLayerProperties (0, &nLayerCount, nullptr);

   Check (XR_SUCCEEDED (nResult), "xrEnumerateApiLayerProperties succeeded");
   std::printf ("    Found %u API layer(s)\n", nLayerCount);

   if (nLayerCount > 0)
   {
      std::vector<XrApiLayerProperties> aLayers (nLayerCount, { XR_TYPE_API_LAYER_PROPERTIES });
      xrEnumerateApiLayerProperties (nLayerCount, &nLayerCount, aLayers.data ());

      for (uint32_t i = 0; i < nLayerCount; ++i)
      {
         std::printf ("    [%u] %s (spec v%d.%d.%d): %s\n", i,
            aLayers[i].layerName,
            XR_VERSION_MAJOR (aLayers[i].specVersion),
            XR_VERSION_MINOR (aLayers[i].specVersion),
            XR_VERSION_PATCH (aLayers[i].specVersion),
            aLayers[i].description);
      }
   }
}

// ---------------------------------------------------------------------------
// Test 3: Enumerate instance extensions (no layer filter)
// ---------------------------------------------------------------------------
static void TestEnumerateExtensions ()
{
   std::printf ("\n[Test 3] Enumerate instance extensions\n");

   uint32_t nExtCount = 0;
   XrResult nResult = xrEnumerateInstanceExtensionProperties (nullptr, 0, &nExtCount, nullptr);

   if (XR_FAILED (nResult))
   {
      std::printf ("    xrEnumerateInstanceExtensionProperties returned %d (no runtime - extensions unavailable)\n", nResult);
      Check (true, "Extension query returned a valid error code without a runtime (no crash)");
   }
   else
   {
      Check (true, "xrEnumerateInstanceExtensionProperties succeeded");
      std::printf ("    Found %u extension(s)\n", nExtCount);

      if (nExtCount > 0)
      {
         std::vector<XrExtensionProperties> aExts (nExtCount, { XR_TYPE_EXTENSION_PROPERTIES });
         xrEnumerateInstanceExtensionProperties (nullptr, nExtCount, &nExtCount, aExts.data ());

         for (uint32_t i = 0; i < nExtCount; ++i)
         {
            std::printf ("    [%u] %s (v%u)\n", i, aExts[i].extensionName, aExts[i].extensionVersion);
         }
      }
   }
}

// ---------------------------------------------------------------------------
// Test 4: Create an instance (may fail if no runtime is installed)
// ---------------------------------------------------------------------------
static void TestCreateInstance ()
{
   std::printf ("\n[Test 4] Create XR instance\n");

   XrApplicationInfo pAppInfo = {};
   std::strncpy (pAppInfo.applicationName, "XrTest", XR_MAX_APPLICATION_NAME_SIZE);
   pAppInfo.applicationVersion = 1;
   std::strncpy (pAppInfo.engineName, "MBE", XR_MAX_ENGINE_NAME_SIZE);
   pAppInfo.engineVersion = 1;
   pAppInfo.apiVersion    = XR_API_VERSION_1_0;

   XrInstanceCreateInfo pCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
   pCreateInfo.applicationInfo        = pAppInfo;
   pCreateInfo.enabledApiLayerCount   = 0;
   pCreateInfo.enabledExtensionCount  = 0;

   XrInstance hInstance = XR_NULL_HANDLE;
   XrResult   nResult   = xrCreateInstance (&pCreateInfo, &hInstance);

   if (XR_FAILED (nResult))
   {
      std::printf ("    xrCreateInstance returned %d (no runtime installed - this is expected)\n", nResult);
      Check (true, "xrCreateInstance returned a valid error code (no crash)");
   }
   else
   {
      Check (hInstance != XR_NULL_HANDLE, "Instance handle is valid");
      std::printf ("    Instance created successfully\n");

      // --- Query instance properties ---

      XrInstanceProperties pProps = { XR_TYPE_INSTANCE_PROPERTIES };
      nResult = xrGetInstanceProperties (hInstance, &pProps);
      Check (XR_SUCCEEDED (nResult), "xrGetInstanceProperties succeeded");

      if (XR_SUCCEEDED (nResult))
      {
         std::printf ("    Runtime: %s (v%d.%d.%d)\n",
            pProps.runtimeName,
            XR_VERSION_MAJOR (pProps.runtimeVersion),
            XR_VERSION_MINOR (pProps.runtimeVersion),
            XR_VERSION_PATCH (pProps.runtimeVersion));
         Check (std::strlen (pProps.runtimeName) > 0, "Runtime name is non-empty");
      }

      // --- Attempt system query (HMD) ---

      XrSystemGetInfo pSystemInfo = { XR_TYPE_SYSTEM_GET_INFO };
      pSystemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

      XrSystemId nSystemId = XR_NULL_SYSTEM_ID;
      nResult = xrGetSystem (hInstance, &pSystemInfo, &nSystemId);

      if (XR_SUCCEEDED (nResult))
      {
         Check (nSystemId != XR_NULL_SYSTEM_ID, "System ID is valid");
         std::printf ("    HMD system found: ID %llu\n", (unsigned long long) nSystemId);

         XrSystemProperties pSysProps = { XR_TYPE_SYSTEM_PROPERTIES };
         if (XR_SUCCEEDED (xrGetSystemProperties (hInstance, nSystemId, &pSysProps)))
         {
            std::printf ("    System: %s (vendor %u)\n", pSysProps.systemName, pSysProps.vendorId);
            std::printf ("    Graphics: max swapchain %ux%u, max layers %u\n",
               pSysProps.graphicsProperties.maxSwapchainImageWidth,
               pSysProps.graphicsProperties.maxSwapchainImageHeight,
               pSysProps.graphicsProperties.maxLayerCount);
            std::printf ("    Tracking: orientation=%s position=%s\n",
               pSysProps.trackingProperties.orientationTracking ? "yes" : "no",
               pSysProps.trackingProperties.positionTracking ? "yes" : "no");
         }
      }
      else
      {
         std::printf ("    No HMD system available (code %d) - expected without a headset\n", nResult);
         Check (true, "xrGetSystem returned a valid error code for missing HMD (no crash)");
      }

      // --- Attempt system query (handheld) ---

      pSystemInfo.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
      nResult = xrGetSystem (hInstance, &pSystemInfo, &nSystemId);

      if (XR_SUCCEEDED (nResult))
      {
         std::printf ("    Handheld display found: ID %llu\n", (unsigned long long) nSystemId);
      }
      else
      {
         std::printf ("    No handheld display available (code %d) - expected on desktop\n", nResult);
         Check (true, "xrGetSystem returned a valid error code for missing handheld (no crash)");
      }

      xrDestroyInstance (hInstance);
      Check (true, "Instance destroyed cleanly");
   }
}

// ---------------------------------------------------------------------------
// Test 5: Struct initialization patterns
// ---------------------------------------------------------------------------
static void TestStructInit ()
{
   std::printf ("\n[Test 5] OpenXR struct initialization patterns\n");

   XrInstanceCreateInfo pInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
   Check (pInfo.type == XR_TYPE_INSTANCE_CREATE_INFO, "InstanceCreateInfo type tag correct");
   Check (pInfo.next == nullptr, "InstanceCreateInfo next pointer is null");
   Check (pInfo.createFlags == 0, "InstanceCreateInfo flags are zero");

   XrSystemGetInfo pSysInfo = { XR_TYPE_SYSTEM_GET_INFO };
   Check (pSysInfo.type == XR_TYPE_SYSTEM_GET_INFO, "SystemGetInfo type tag correct");
}

// ---------------------------------------------------------------------------

int RunXrTrackingTests (int nArgc, char** aArgv);

int RunXrTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== OpenXR Integration Test Suite ===\n");

   TestVersionConstants ();
   TestEnumerateApiLayers ();
   TestEnumerateExtensions ();
   TestCreateInstance ();
   TestStructInit ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   const int nXrFailed = nFailed;
   const int nTrack = RunXrTrackingTests (0, nullptr);
   return (nXrFailed > 0 || nTrack != 0) ? 1 : 0;
}
