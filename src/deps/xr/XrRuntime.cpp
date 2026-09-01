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

#if defined(__ANDROID__)
#include <jni.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#endif

#include <openxr/openxr.h>

#if defined(__ANDROID__)
#include <openxr/openxr_platform.h>
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace SNEEZE { namespace DEP {

namespace {

XrPosef IdentityPose ()
{
   XrPosef Pose = {};
   Pose.orientation.w = 1.0f;
   return Pose;
}

void RotateQuat (const XrQuaternionf& q, float x, float y, float z, float& ox, float& oy, float& oz)
{
   const float qx = q.x, qy = q.y, qz = q.z, qw = q.w;
   const float ix =  qw * x + qy * z - qz * y;
   const float iy =  qw * y + qz * x - qx * z;
   const float iz =  qw * z + qx * y - qy * x;
   const float iw = -qx * x - qy * y - qz * z;
   ox = ix * qw + iw * -qx + iy * -qz - iz * -qy;
   oy = iy * qw + iw * -qy + iz * -qx - ix * -qz;
   oz = iz * qw + iw * -qz + ix * -qy - iy * -qx;
}

void ConvertPoseToSneeze (const XrPosef& Pose, float aPosition[3], float aDirection[3], float aUp[3])
{
   aPosition[0] =  Pose.position.x;
   aPosition[1] = -Pose.position.z;
   aPosition[2] =  Pose.position.y;

   float fx, fy, fz, ux, uy, uz;
   RotateQuat (Pose.orientation, 0.0f, 0.0f, -1.0f, fx, fy, fz);
   RotateQuat (Pose.orientation, 0.0f, 1.0f,  0.0f, ux, uy, uz);

   aDirection[0] =  fx;
   aDirection[1] = -fz;
   aDirection[2] =  fy;

   aUp[0] =  ux;
   aUp[1] = -uz;
   aUp[2] =  uy;
}

} // namespace

class XR_RUNTIME::Impl
{
public:
   ENGINE*     m_pEngine    = nullptr;
   XrInstance  hInstance    = XR_NULL_HANDLE;
   bool        bHasRuntime  = false;
   std::string sRuntimeName;

#if defined(__ANDROID__)
   XrSystemId              hSystem           = XR_NULL_SYSTEM_ID;
   XrSession               hSession          = XR_NULL_HANDLE;
   XrSpace                 hSpace            = XR_NULL_HANDLE;
   XrSpace                 hViewSpace        = XR_NULL_HANDLE;
   XrSwapchain             hSwapchain[2]     = { XR_NULL_HANDLE, XR_NULL_HANDLE };
   XrSwapchain             hChromeSwapchain  = XR_NULL_HANDLE;
   int32_t                 nWidth            = 0;
   int32_t                 nHeight           = 0;
   int32_t                 nChromeWidth      = 512;
   int32_t                 nChromeHeight     = 64;
   int64_t                 nVkFormat         = 0;
   uint32_t                nViewCount        = 0;
   XrFrameState            FrameState        = { XR_TYPE_FRAME_STATE };
   bool                    bSessionReady     = false;
   bool                    bSessionRunning   = false;
   bool                    bFrameWaited      = false;
   bool                    bShouldRender     = false;
   XrView                  aView[2]          = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
   XrViewStateFlags        nViewStateFlags   = 0;
   std::vector<XrSwapchainImageVulkan2KHR> aImage[2];
   std::vector<XrSwapchainImageVulkan2KHR> aChromeImage;
   uint32_t                nAcquired[2]      = { 0, 0 };
   bool                    bAcquired[2]      = { false, false };
   XrCompositionLayerProjectionView aProjView[2] = {};
   VkDevice                hVkDevice         = VK_NULL_HANDLE;
   VkPhysicalDevice        hVkPhysical       = VK_NULL_HANDLE;
   VkQueue                 hVkQueue          = VK_NULL_HANDLE;
   uint32_t                nQueueFamily      = 0;
   XrActionSet             hActionSet        = XR_NULL_HANDLE;
   XrAction                hUrlFocus         = XR_NULL_HANDLE;
   std::atomic<bool>       bUrlFocus         { false };

   std::mutex              mxChrome;
   std::vector<uint8_t>    aChromePixels;
   int                     nChromePixelW     = 0;
   int                     nChromePixelH     = 0;
   bool                    bChromeDirty      = false;
   VkCommandPool           hCmdPool          = VK_NULL_HANDLE;
   VkCommandBuffer         hCmd              = VK_NULL_HANDLE;
   VkBuffer                hStaging          = VK_NULL_HANDLE;
   VkDeviceMemory          hStagingMem       = VK_NULL_HANDLE;
   VkDeviceSize            nStagingSize      = 0;

   PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVkReqs         = nullptr;
   PFN_xrCreateVulkanInstanceKHR           pfnCreateVkInstance  = nullptr;
   PFN_xrCreateVulkanDeviceKHR             pfnCreateVkDevice    = nullptr;
   PFN_xrGetVulkanGraphicsDevice2KHR       pfnGetVkDevice       = nullptr;
   XR_VULKAN_CREATE                        VulkanCreate         = {};
   bool                                    bGraphicsPrepared    = false;

   static PFN_vkGetInstanceProcAddr VkGetInstanceProcAddr ()
   {
      static PFN_vkGetInstanceProcAddr s_pfn = nullptr;
      if (!s_pfn)
      {
         void* pLib = dlopen ("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
         if (pLib)
            s_pfn = reinterpret_cast<PFN_vkGetInstanceProcAddr> (dlsym (pLib, "vkGetInstanceProcAddr"));
      }
      return s_pfn;
   }

   static uint64_t HookCreateInstance (void* pUser, const void* pInfo)
   {
      uint64_t nInstance = 0;
      Impl* pImpl = static_cast<Impl*> (pUser);
      if (pImpl  &&  pImpl->pfnCreateVkInstance  &&  pInfo)
      {
         XrVulkanInstanceCreateInfoKHR XrInfo = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
         XrInfo.systemId              = pImpl->hSystem;
         XrInfo.pfnGetInstanceProcAddr = VkGetInstanceProcAddr ();
         XrInfo.vulkanCreateInfo      = static_cast<const VkInstanceCreateInfo*> (pInfo);

         VkInstance hInst = VK_NULL_HANDLE;
         VkResult nVk = VK_ERROR_INITIALIZATION_FAILED;
         XrResult nXr = pImpl->pfnCreateVkInstance (pImpl->hInstance, &XrInfo, &hInst, &nVk);
         if (XR_FAILED (nXr)  ||  nVk != VK_SUCCESS)
         {
            pImpl->Log (IENGINE::kLOGLEVEL_Error,
               "xrCreateVulkanInstanceKHR failed (xr=" + std::to_string (nXr)
               + " vk=" + std::to_string (nVk) + ")");
         }
         else
            nInstance = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (hInst));
      }
      return nInstance;
   }

   static uint64_t HookSelectPhysicalDevice (void* pUser, uint64_t nInstance)
   {
      uint64_t nPhysical = 0;
      Impl* pImpl = static_cast<Impl*> (pUser);
      if (pImpl  &&  pImpl->pfnGetVkDevice  &&  nInstance != 0)
      {
         XrVulkanGraphicsDeviceGetInfoKHR Info = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
         Info.systemId      = pImpl->hSystem;
         Info.vulkanInstance = reinterpret_cast<VkInstance> (static_cast<uintptr_t> (nInstance));
         VkPhysicalDevice hPhys = VK_NULL_HANDLE;
         if (XR_FAILED (pImpl->pfnGetVkDevice (pImpl->hInstance, &Info, &hPhys)))
            pImpl->Log (IENGINE::kLOGLEVEL_Error, "xrGetVulkanGraphicsDevice2KHR failed");
         else
            nPhysical = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (hPhys));
      }
      return nPhysical;
   }

   static uint64_t HookCreateDevice (void* pUser, const void* pInfo, uint64_t nPhysicalDevice)
   {
      uint64_t nDevice = 0;
      Impl* pImpl = static_cast<Impl*> (pUser);
      if (pImpl  &&  pImpl->pfnCreateVkDevice  &&  pInfo  &&  nPhysicalDevice != 0)
      {
         XrVulkanDeviceCreateInfoKHR XrInfo = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
         XrInfo.systemId               = pImpl->hSystem;
         XrInfo.pfnGetInstanceProcAddr = VkGetInstanceProcAddr ();
         XrInfo.vulkanPhysicalDevice   = reinterpret_cast<VkPhysicalDevice> (static_cast<uintptr_t> (nPhysicalDevice));
         XrInfo.vulkanCreateInfo       = static_cast<const VkDeviceCreateInfo*> (pInfo);

         VkDevice hDev = VK_NULL_HANDLE;
         VkResult nVk = VK_ERROR_INITIALIZATION_FAILED;
         XrResult nXr = pImpl->pfnCreateVkDevice (pImpl->hInstance, &XrInfo, &hDev, &nVk);
         if (XR_FAILED (nXr)  ||  nVk != VK_SUCCESS)
         {
            pImpl->Log (IENGINE::kLOGLEVEL_Error,
               "xrCreateVulkanDeviceKHR failed (xr=" + std::to_string (nXr)
               + " vk=" + std::to_string (nVk) + ")");
         }
         else
            nDevice = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (hDev));
      }
      return nDevice;
   }

   bool PrepareGraphics ()
   {
      bool bOk = bGraphicsPrepared;

      if (!bOk  &&  hInstance != XR_NULL_HANDLE)
      {
         XrSystemGetInfo SysInfo = { XR_TYPE_SYSTEM_GET_INFO };
         SysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
         if (XR_FAILED (xrGetSystem (hInstance, &SysInfo, &hSystem)))
            Log (IENGINE::kLOGLEVEL_Error, "xrGetSystem failed (no HMD)");
         else if (!pfnGetVkReqs)
            Log (IENGINE::kLOGLEVEL_Error, "Vulkan2 graphics requirements entry missing");
         else
         {
            XrGraphicsRequirementsVulkan2KHR Reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
            pfnGetVkReqs (hInstance, hSystem, &Reqs);

            xrGetInstanceProcAddr (hInstance, "xrCreateVulkanInstanceKHR",
               reinterpret_cast<PFN_xrVoidFunction*> (&pfnCreateVkInstance));
            xrGetInstanceProcAddr (hInstance, "xrCreateVulkanDeviceKHR",
               reinterpret_cast<PFN_xrVoidFunction*> (&pfnCreateVkDevice));
            xrGetInstanceProcAddr (hInstance, "xrGetVulkanGraphicsDevice2KHR",
               reinterpret_cast<PFN_xrVoidFunction*> (&pfnGetVkDevice));

            if (!pfnCreateVkInstance  ||  !pfnCreateVkDevice  ||  !pfnGetVkDevice)
               Log (IENGINE::kLOGLEVEL_Error, "Vulkan2 create-instance/device entry points missing");
            else
            {
               VulkanCreate.pUser                  = this;
               VulkanCreate.fnCreateInstance       = &HookCreateInstance;
               VulkanCreate.fnSelectPhysicalDevice = &HookSelectPhysicalDevice;
               VulkanCreate.fnCreateDevice         = &HookCreateDevice;
               bGraphicsPrepared = true;
               bOk = true;
               Log (IENGINE::kLOGLEVEL_Info, "Vulkan2 graphics prepared for Filament");
            }
         }
      }

      return bOk;
   }

   void Log (IENGINE::eLOGLEVEL Level, const std::string& sMessage)
   {
      m_pEngine->Log (Level, "XR_RUNTIME", sMessage);
   }

   void DestroySession ()
   {
      for (int nEye = 0; nEye < 2; nEye++)
      {
         if (hSwapchain[nEye] != XR_NULL_HANDLE)
         {
            xrDestroySwapchain (hSwapchain[nEye]);
            hSwapchain[nEye] = XR_NULL_HANDLE;
         }
         aImage[nEye].clear ();
      }
      if (hChromeSwapchain != XR_NULL_HANDLE)
      {
         xrDestroySwapchain (hChromeSwapchain);
         hChromeSwapchain = XR_NULL_HANDLE;
      }
      aChromeImage.clear ();
      if (hCmdPool != VK_NULL_HANDLE)
      {
         vkDestroyCommandPool (hVkDevice, hCmdPool, nullptr);
         hCmdPool = VK_NULL_HANDLE;
         hCmd = VK_NULL_HANDLE;
      }
      if (hStaging != VK_NULL_HANDLE)
      {
         vkDestroyBuffer (hVkDevice, hStaging, nullptr);
         hStaging = VK_NULL_HANDLE;
      }
      if (hStagingMem != VK_NULL_HANDLE)
      {
         vkFreeMemory (hVkDevice, hStagingMem, nullptr);
         hStagingMem = VK_NULL_HANDLE;
      }
      nStagingSize = 0;
      if (hViewSpace != XR_NULL_HANDLE)
      {
         xrDestroySpace (hViewSpace);
         hViewSpace = XR_NULL_HANDLE;
      }
      if (hSpace != XR_NULL_HANDLE)
      {
         xrDestroySpace (hSpace);
         hSpace = XR_NULL_HANDLE;
      }
      if (hUrlFocus != XR_NULL_HANDLE)
      {
         xrDestroyAction (hUrlFocus);
         hUrlFocus = XR_NULL_HANDLE;
      }
      if (hActionSet != XR_NULL_HANDLE)
      {
         xrDestroyActionSet (hActionSet);
         hActionSet = XR_NULL_HANDLE;
      }
      if (hSession != XR_NULL_HANDLE)
      {
         xrDestroySession (hSession);
         hSession = XR_NULL_HANDLE;
      }
      bSessionReady = false;
      bSessionRunning = false;
      hSystem = XR_NULL_SYSTEM_ID;
   }

   bool CreateInstance ()
   {
      bool bOk = false;

#ifdef _WIN32
      _putenv_s ("XR_LOADER_DEBUG", "none");
#else
      setenv ("XR_LOADER_DEBUG", "none", 1);
#endif

      IENGINE* pHost = m_pEngine->Host ();
      void* pVm       = pHost ? pHost->XrAndroidVm ()       : nullptr;
      void* pActivity = pHost ? pHost->XrAndroidActivity () : nullptr;

      if (!pVm  ||  !pActivity)
         Log (IENGINE::kLOGLEVEL_Warning, "Android VM/activity not available; OpenXR instance deferred");
      else
      {
      PFN_xrInitializeLoaderKHR pfnInitLoader = nullptr;
      xrGetInstanceProcAddr (XR_NULL_HANDLE, "xrInitializeLoaderKHR",
         reinterpret_cast<PFN_xrVoidFunction*> (&pfnInitLoader));
      if (!pfnInitLoader)
         Log (IENGINE::kLOGLEVEL_Warning, "xrInitializeLoaderKHR not found");
      else
      {
      XrLoaderInitInfoAndroidKHR LoaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
      LoaderInfo.applicationVM      = pVm;
      LoaderInfo.applicationContext = pActivity;
      XrResult nLoader = pfnInitLoader (
         reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*> (&LoaderInfo));
      if (XR_FAILED (nLoader))
         Log (IENGINE::kLOGLEVEL_Warning, "xrInitializeLoaderKHR failed (code " + std::to_string (nLoader) + ")");
      else
      {
      const char* asWanted[2] = {
         XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
         XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
      };
      const char* asExt[2] = {};
      uint32_t nExt = 0;

      uint32_t nAvail = 0;
      xrEnumerateInstanceExtensionProperties (nullptr, 0, &nAvail, nullptr);
      std::vector<XrExtensionProperties> aAvail (nAvail, { XR_TYPE_EXTENSION_PROPERTIES });
      if (nAvail > 0)
         xrEnumerateInstanceExtensionProperties (nullptr, nAvail, &nAvail, aAvail.data ());

      for (uint32_t nWant = 0; nWant < 2; nWant++)
      {
         for (uint32_t nHave = 0; nHave < nAvail; nHave++)
         {
            if (std::strcmp (aAvail[nHave].extensionName, asWanted[nWant]) == 0)
            {
               asExt[nExt++] = asWanted[nWant];
               break;
            }
         }
      }

      XrInstanceCreateInfoAndroidKHR AndroidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
      AndroidInfo.applicationVM       = pVm;
      AndroidInfo.applicationActivity = pActivity;

      if (nExt < 2)
      {
         Log (IENGINE::kLOGLEVEL_Warning, "Required OpenXR Android/Vulkan2 extensions not advertised");
      }
      else
      {
         XrApplicationInfo AppInfo = {};
         std::strncpy (AppInfo.applicationName, "Rubidium", XR_MAX_APPLICATION_NAME_SIZE);
         AppInfo.applicationName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';
         AppInfo.applicationVersion = 1;
         std::strncpy (AppInfo.engineName, "MBE", XR_MAX_ENGINE_NAME_SIZE);
         AppInfo.engineName[XR_MAX_ENGINE_NAME_SIZE - 1] = '\0';
         AppInfo.engineVersion = 1;
         AppInfo.apiVersion    = XR_API_VERSION_1_0;

         XrInstanceCreateInfo CreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
         CreateInfo.next                  = &AndroidInfo;
         CreateInfo.applicationInfo       = AppInfo;
         CreateInfo.enabledExtensionCount = nExt;
         CreateInfo.enabledExtensionNames = asExt;

         XrResult nResult = xrCreateInstance (&CreateInfo, &hInstance);
         if (XR_FAILED (nResult))
         {
            Log (IENGINE::kLOGLEVEL_Warning, "xrCreateInstance failed (code " + std::to_string (nResult) + ")");
         }
         else
         {
            bHasRuntime = true;
            XrInstanceProperties Props = { XR_TYPE_INSTANCE_PROPERTIES };
            if (XR_SUCCEEDED (xrGetInstanceProperties (hInstance, &Props)))
            {
               sRuntimeName = Props.runtimeName;
               Log (IENGINE::kLOGLEVEL_Info, std::string ("OpenXR runtime: ") + Props.runtimeName);
            }
            xrGetInstanceProcAddr (hInstance, "xrGetVulkanGraphicsRequirements2KHR",
               reinterpret_cast<PFN_xrVoidFunction*> (&pfnGetVkReqs));
            bOk = true;
         }
      }
      }
      }
      }

      return bOk;
   }

   bool CreateSwapchains ()
   {
      bool bOk = false;

      uint32_t nViewCap = 0;
      xrEnumerateViewConfigurationViews (hInstance, hSystem, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nViewCap, nullptr);
      std::vector<XrViewConfigurationView> aViewCfg (nViewCap, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
      xrEnumerateViewConfigurationViews (hInstance, hSystem, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nViewCap, &nViewCap, aViewCfg.data ());

      if (nViewCap >= 2)
      {
         nWidth  = static_cast<int32_t> (aViewCfg[0].recommendedImageRectWidth);
         nHeight = static_cast<int32_t> (aViewCfg[0].recommendedImageRectHeight);
         nViewCount = 2;

         uint32_t nFmtCount = 0;
         xrEnumerateSwapchainFormats (hSession, 0, &nFmtCount, nullptr);
         std::vector<int64_t> aFmt (nFmtCount);
         xrEnumerateSwapchainFormats (hSession, nFmtCount, &nFmtCount, aFmt.data ());

         nVkFormat = VK_FORMAT_R8G8B8A8_SRGB;
         bool bFound = false;
         for (int64_t nFmt : aFmt)
         {
            if (nFmt == VK_FORMAT_R8G8B8A8_SRGB)
            {
               nVkFormat = nFmt;
               bFound = true;
            }
         }
         if (!bFound  &&  nFmtCount > 0)
            nVkFormat = aFmt[0];

         bOk = true;
         for (uint32_t nEye = 0; nEye < 2 && bOk; nEye++)
         {
            XrSwapchainCreateInfo SwapInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            SwapInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
               | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
               | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT
               | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            SwapInfo.format     = nVkFormat;
            SwapInfo.sampleCount = 1;
            SwapInfo.width      = static_cast<uint32_t> (nWidth);
            SwapInfo.height     = static_cast<uint32_t> (nHeight);
            SwapInfo.faceCount  = 1;
            SwapInfo.arraySize  = 1;
            SwapInfo.mipCount   = 1;

            if (XR_FAILED (xrCreateSwapchain (hSession, &SwapInfo, &hSwapchain[nEye])))
               bOk = false;
            else
            {
               uint32_t nImg = 0;
               xrEnumerateSwapchainImages (hSwapchain[nEye], 0, &nImg, nullptr);
               aImage[nEye].resize (nImg, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
               xrEnumerateSwapchainImages (hSwapchain[nEye], nImg, &nImg,
                  reinterpret_cast<XrSwapchainImageBaseHeader*> (aImage[nEye].data ()));
            }
         }

         if (bOk)
         {
            XrSwapchainCreateInfo ChromeInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ChromeInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
               | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
               | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            ChromeInfo.format     = nVkFormat;
            ChromeInfo.sampleCount = 1;
            ChromeInfo.width      = static_cast<uint32_t> (nChromeWidth);
            ChromeInfo.height     = static_cast<uint32_t> (nChromeHeight);
            ChromeInfo.faceCount  = 1;
            ChromeInfo.arraySize  = 1;
            ChromeInfo.mipCount   = 1;
            if (XR_SUCCEEDED (xrCreateSwapchain (hSession, &ChromeInfo, &hChromeSwapchain)))
            {
               uint32_t nImg = 0;
               xrEnumerateSwapchainImages (hChromeSwapchain, 0, &nImg, nullptr);
               aChromeImage.resize (nImg, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
               xrEnumerateSwapchainImages (hChromeSwapchain, nImg, &nImg,
                  reinterpret_cast<XrSwapchainImageBaseHeader*> (aChromeImage.data ()));
            }
         }
      }

      return bOk;
   }

   bool CreateActions ()
   {
      XrActionSetCreateInfo SetInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
      std::strncpy (SetInfo.actionSetName, "sneeze", XR_MAX_ACTION_SET_NAME_SIZE);
      std::strncpy (SetInfo.localizedActionSetName, "Sneeze", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
      if (XR_FAILED (xrCreateActionSet (hInstance, &SetInfo, &hActionSet)))
         return false;

      XrActionCreateInfo ActInfo = { XR_TYPE_ACTION_CREATE_INFO };
      ActInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
      std::strncpy (ActInfo.actionName, "url_focus", XR_MAX_ACTION_NAME_SIZE);
      std::strncpy (ActInfo.localizedActionName, "URL Focus", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
      if (XR_FAILED (xrCreateAction (hActionSet, &ActInfo, &hUrlFocus)))
         return false;

      XrPath pTouch = XR_NULL_PATH;
      XrPath pA = XR_NULL_PATH, pMenuL = XR_NULL_PATH, pX = XR_NULL_PATH;
      xrStringToPath (hInstance, "/interaction_profiles/oculus/touch_controller", &pTouch);
      xrStringToPath (hInstance, "/user/hand/right/input/a/click", &pA);
      xrStringToPath (hInstance, "/user/hand/left/input/menu/click", &pMenuL);
      xrStringToPath (hInstance, "/user/hand/left/input/x/click", &pX);

      XrActionSuggestedBinding aBind[3] = {
         { hUrlFocus, pA }, { hUrlFocus, pMenuL }, { hUrlFocus, pX }
      };
      XrInteractionProfileSuggestedBinding Suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
      Suggest.interactionProfile = pTouch;
      Suggest.suggestedBindings = aBind;
      Suggest.countSuggestedBindings = 3;
      xrSuggestInteractionProfileBindings (hInstance, &Suggest);

      XrSessionActionSetsAttachInfo Attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
      Attach.countActionSets = 1;
      Attach.actionSets = &hActionSet;
      xrAttachSessionActionSets (hSession, &Attach);
      return true;
   }

   void PollActions ()
   {
      if (hActionSet == XR_NULL_HANDLE  ||  !bSessionRunning)
         return;

      XrActiveActionSet Active = { hActionSet, XR_NULL_PATH };
      XrActionsSyncInfo Sync = { XR_TYPE_ACTIONS_SYNC_INFO };
      Sync.countActiveActionSets = 1;
      Sync.activeActionSets = &Active;
      if (XR_FAILED (xrSyncActions (hSession, &Sync)))
         return;

      XrActionStateGetInfo GetInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
      GetInfo.action = hUrlFocus;
      XrActionStateBoolean State = { XR_TYPE_ACTION_STATE_BOOLEAN };
      if (XR_SUCCEEDED (xrGetActionStateBoolean (hSession, &GetInfo, &State)))
      {
         if (State.isActive  &&  State.changedSinceLastSync  &&  State.currentState)
            bUrlFocus.store (true);
      }
   }

   void PollEvents ()
   {
      XrEventDataBuffer Event = { XR_TYPE_EVENT_DATA_BUFFER };
      while (xrPollEvent (hInstance, &Event) == XR_SUCCESS)
      {
         if (Event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
         {
            auto* pChanged = reinterpret_cast<XrEventDataSessionStateChanged*> (&Event);
            const char* szState = "OTHER";
            if (pChanged->state == XR_SESSION_STATE_IDLE)
               szState = "IDLE";
            else if (pChanged->state == XR_SESSION_STATE_READY)
               szState = "READY";
            else if (pChanged->state == XR_SESSION_STATE_SYNCHRONIZED)
               szState = "SYNCHRONIZED";
            else if (pChanged->state == XR_SESSION_STATE_VISIBLE)
               szState = "VISIBLE";
            else if (pChanged->state == XR_SESSION_STATE_FOCUSED)
               szState = "FOCUSED";
            else if (pChanged->state == XR_SESSION_STATE_STOPPING)
               szState = "STOPPING";
            else if (pChanged->state == XR_SESSION_STATE_EXITING)
               szState = "EXITING";
            else if (pChanged->state == XR_SESSION_STATE_LOSS_PENDING)
               szState = "LOSS_PENDING";
            Log (IENGINE::kLOGLEVEL_Info, std::string ("Session state: ") + szState);

            if (pChanged->state == XR_SESSION_STATE_READY)
            {
               XrSessionBeginInfo Begin = { XR_TYPE_SESSION_BEGIN_INFO };
               Begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
               if (XR_SUCCEEDED (xrBeginSession (hSession, &Begin)))
                  bSessionRunning = true;
               else
                  Log (IENGINE::kLOGLEVEL_Error, "xrBeginSession failed");
            }
            else if (pChanged->state == XR_SESSION_STATE_STOPPING)
            {
               xrEndSession (hSession);
               bSessionRunning = false;
            }
            else if (pChanged->state == XR_SESSION_STATE_EXITING
                  ||  pChanged->state == XR_SESSION_STATE_LOSS_PENDING)
            {
               bSessionRunning = false;
               bSessionReady = false;
            }
         }
         Event = { XR_TYPE_EVENT_DATA_BUFFER };
      }
   }

   bool EnsureChromeUpload (VkDeviceSize nBytes)
   {
      bool bOk = (hCmd != VK_NULL_HANDLE  &&  nStagingSize >= nBytes);

      if (!bOk  &&  hVkDevice != VK_NULL_HANDLE  &&  hVkPhysical != VK_NULL_HANDLE)
      {
         if (hCmdPool == VK_NULL_HANDLE)
         {
            VkCommandPoolCreateInfo PoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            PoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            PoolInfo.queueFamilyIndex = nQueueFamily;
            if (vkCreateCommandPool (hVkDevice, &PoolInfo, nullptr, &hCmdPool) == VK_SUCCESS)
            {
               VkCommandBufferAllocateInfo Alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
               Alloc.commandPool = hCmdPool;
               Alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
               Alloc.commandBufferCount = 1;
               vkAllocateCommandBuffers (hVkDevice, &Alloc, &hCmd);
            }
         }

         if (hStaging == VK_NULL_HANDLE  ||  nStagingSize < nBytes)
         {
            if (hStaging != VK_NULL_HANDLE)
            {
               vkDestroyBuffer (hVkDevice, hStaging, nullptr);
               hStaging = VK_NULL_HANDLE;
            }
            if (hStagingMem != VK_NULL_HANDLE)
            {
               vkFreeMemory (hVkDevice, hStagingMem, nullptr);
               hStagingMem = VK_NULL_HANDLE;
            }

            VkBufferCreateInfo Buf = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            Buf.size = nBytes;
            Buf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            Buf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer (hVkDevice, &Buf, nullptr, &hStaging) == VK_SUCCESS)
            {
               VkMemoryRequirements Req = {};
               vkGetBufferMemoryRequirements (hVkDevice, hStaging, &Req);
               VkPhysicalDeviceMemoryProperties Mem = {};
               vkGetPhysicalDeviceMemoryProperties (hVkPhysical, &Mem);
               uint32_t nType = 0xFFFFFFFFu;
               for (uint32_t i = 0; i < Mem.memoryTypeCount; i++)
               {
                  if ((Req.memoryTypeBits & (1u << i))
                   && (Mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                   && (Mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                     nType = i;
               }
               if (nType != 0xFFFFFFFFu)
               {
                  VkMemoryAllocateInfo Ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                  Ai.allocationSize = Req.size;
                  Ai.memoryTypeIndex = nType;
                  if (vkAllocateMemory (hVkDevice, &Ai, nullptr, &hStagingMem) == VK_SUCCESS)
                  {
                     vkBindBufferMemory (hVkDevice, hStaging, hStagingMem, 0);
                     nStagingSize = nBytes;
                  }
               }
            }
         }

         bOk = (hCmd != VK_NULL_HANDLE  &&  hStaging != VK_NULL_HANDLE  &&  nStagingSize >= nBytes);
      }

      return bOk;
   }

   void UploadChrome (VkImage hImage)
   {
      std::vector<uint8_t> aCopy;
      int nW = 0, nH = 0;
      {
         std::lock_guard<std::mutex> lock (mxChrome);
         if (bChromeDirty  &&  !aChromePixels.empty ())
         {
            aCopy = aChromePixels;
            nW = nChromePixelW;
            nH = nChromePixelH;
            bChromeDirty = false;
         }
      }

      if (hImage != VK_NULL_HANDLE  &&  !aCopy.empty ()  &&  nW > 0  &&  nH > 0
       &&  EnsureChromeUpload (static_cast<VkDeviceSize> (aCopy.size ()))
       &&  hVkQueue != VK_NULL_HANDLE)
      {
         void* pMap = nullptr;
         if (vkMapMemory (hVkDevice, hStagingMem, 0, aCopy.size (), 0, &pMap) == VK_SUCCESS)
         {
            std::memcpy (pMap, aCopy.data (), aCopy.size ());
            vkUnmapMemory (hVkDevice, hStagingMem);

            vkQueueWaitIdle (hVkQueue);
            vkResetCommandBuffer (hCmd, 0);

            VkCommandBufferBeginInfo Begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer (hCmd, &Begin);

            VkImageMemoryBarrier ToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            ToDst.srcAccessMask = 0;
            ToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ToDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ToDst.image = hImage;
            ToDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ToDst.subresourceRange.levelCount = 1;
            ToDst.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier (hCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ToDst);

            VkBufferImageCopy Region = {};
            Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            Region.imageSubresource.layerCount = 1;
            Region.imageExtent = { static_cast<uint32_t> (nW), static_cast<uint32_t> (nH), 1 };
            vkCmdCopyBufferToImage (hCmd, hStaging, hImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);

            VkImageMemoryBarrier ToRead = ToDst;
            ToRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ToRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            ToRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier (hCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ToRead);

            vkEndCommandBuffer (hCmd);

            VkSubmitInfo Submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            Submit.commandBufferCount = 1;
            Submit.pCommandBuffers = &hCmd;
            vkQueueSubmit (hVkQueue, 1, &Submit, VK_NULL_HANDLE);
            vkQueueWaitIdle (hVkQueue);
         }
      }
   }
#endif
};

XR_RUNTIME::XR_RUNTIME (ENGINE* pEngine) : m_pImpl (new Impl ())
{
   m_pImpl->m_pEngine = pEngine;
}

XR_RUNTIME::~XR_RUNTIME ()
{
#if defined(__ANDROID__)
   m_pImpl->DestroySession ();
#endif
   if (m_pImpl->hInstance != XR_NULL_HANDLE)
   {
      xrDestroyInstance (m_pImpl->hInstance);
      m_pImpl->hInstance = XR_NULL_HANDLE;
   }
   delete m_pImpl;
}

bool XR_RUNTIME::Initialize ()
{
   bool bOk = true;

#if defined(__ANDROID__)
   // Instance first so Filament can wrap vkCreateInstance with
   // xrCreateVulkanInstanceKHR. No SDL window is created on Quest, so this
   // no longer races ANativeWindow.
   m_pImpl->CreateInstance ();
   if (m_pImpl->bHasRuntime)
      m_pImpl->PrepareGraphics ();
   bOk = true;
#else
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
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "XR_RUNTIME",
         "OpenXR loader initialized - no XR runtime detected (code " + std::to_string (nResult) + ")");
      m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "XR_RUNTIME",
         "This is normal on machines without a VR/AR headset or runtime installed.");
   }
   else
   {
      m_pImpl->bHasRuntime = true;

      XrInstanceProperties pProps = { XR_TYPE_INSTANCE_PROPERTIES };
      if (XR_SUCCEEDED (xrGetInstanceProperties (m_pImpl->hInstance, &pProps)))
      {
         m_pImpl->sRuntimeName = pProps.runtimeName;
         m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
            "OpenXR " + std::to_string (XR_VERSION_MAJOR (XR_CURRENT_API_VERSION)) + "."
            + std::to_string (XR_VERSION_MINOR (XR_CURRENT_API_VERSION)) + "."
            + std::to_string (XR_VERSION_PATCH (XR_CURRENT_API_VERSION))
            + " initialized - runtime: " + pProps.runtimeName
            + " (v" + std::to_string (XR_VERSION_MAJOR (pProps.runtimeVersion)) + "."
            + std::to_string (XR_VERSION_MINOR (pProps.runtimeVersion)) + "."
            + std::to_string (XR_VERSION_PATCH (pProps.runtimeVersion)) + ")");
      }
   }
#endif

   return bOk;
}

bool XR_RUNTIME::HasRuntime () const          { return m_pImpl->bHasRuntime;   }
std::string XR_RUNTIME::GetRuntimeName () const { return m_pImpl->sRuntimeName; }

bool XR_RUNTIME::WantsSession () const
{
   bool bWant = false;
#if defined(__ANDROID__)
   bWant = true;
#endif
   return bWant;
}

bool XR_RUNTIME::PrepareGraphics ()
{
   bool bOk = false;
#if defined(__ANDROID__)
   bOk = m_pImpl->PrepareGraphics ();
#endif
   return bOk;
}

const XR_RUNTIME::XR_VULKAN_CREATE* XR_RUNTIME::VulkanCreateHooks () const
{
   const XR_VULKAN_CREATE* pHooks = nullptr;
#if defined(__ANDROID__)
   if (m_pImpl->VulkanCreate.fnCreateInstance)
      pHooks = &m_pImpl->VulkanCreate;
#endif
   return pHooks;
}

bool XR_RUNTIME::BindGraphics (uint64_t nInstance, uint64_t nPhysicalDevice, uint64_t nDevice,
                               uint64_t nQueue, uint32_t nQueueFamily, uint32_t nQueueIndex)
{
   bool bOk = false;

#if defined(__ANDROID__)
   if (m_pImpl->hSession != XR_NULL_HANDLE)
      bOk = true;
   else
   {
      if (m_pImpl->hInstance == XR_NULL_HANDLE)
         m_pImpl->CreateInstance ();

      if (m_pImpl->hInstance != XR_NULL_HANDLE)
      {
         if (!m_pImpl->pfnGetVkReqs)
         {
            xrGetInstanceProcAddr (m_pImpl->hInstance, "xrGetVulkanGraphicsRequirements2KHR",
               reinterpret_cast<PFN_xrVoidFunction*> (&m_pImpl->pfnGetVkReqs));
         }

         if (m_pImpl->pfnGetVkReqs)
         {
            XrSystemGetInfo SysInfo = { XR_TYPE_SYSTEM_GET_INFO };
            SysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            if (XR_SUCCEEDED (xrGetSystem (m_pImpl->hInstance, &SysInfo, &m_pImpl->hSystem)))
            {
               XrGraphicsRequirementsVulkan2KHR Reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
               m_pImpl->pfnGetVkReqs (m_pImpl->hInstance, m_pImpl->hSystem, &Reqs);

               XrGraphicsBindingVulkan2KHR Binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
               Binding.instance         = reinterpret_cast<VkInstance> (static_cast<uintptr_t> (nInstance));
               Binding.physicalDevice   = reinterpret_cast<VkPhysicalDevice> (static_cast<uintptr_t> (nPhysicalDevice));
               Binding.device           = reinterpret_cast<VkDevice> (static_cast<uintptr_t> (nDevice));
               Binding.queueFamilyIndex = nQueueFamily;
               Binding.queueIndex       = nQueueIndex;

               m_pImpl->hVkDevice    = Binding.device;
               m_pImpl->hVkPhysical  = Binding.physicalDevice;
               m_pImpl->hVkQueue     = reinterpret_cast<VkQueue> (static_cast<uintptr_t> (nQueue));
               m_pImpl->nQueueFamily = nQueueFamily;

               XrSessionCreateInfo SessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
               SessionInfo.next     = &Binding;
               SessionInfo.systemId = m_pImpl->hSystem;
               XrResult nSession = xrCreateSession (m_pImpl->hInstance, &SessionInfo, &m_pImpl->hSession);
               if (XR_FAILED (nSession))
               {
                  char szResult[XR_MAX_RESULT_STRING_SIZE] = {};
                  xrResultToString (m_pImpl->hInstance, nSession, szResult);
                  m_pImpl->Log (IENGINE::kLOGLEVEL_Error,
                     std::string ("xrCreateSession failed: ") + szResult
                     + " (" + std::to_string (nSession) + ")");
               }
               else
               {
                  XrReferenceSpaceCreateInfo SpaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
                  SpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                  SpaceInfo.poseInReferenceSpace = IdentityPose ();
                  xrCreateReferenceSpace (m_pImpl->hSession, &SpaceInfo, &m_pImpl->hSpace);

                  SpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                  xrCreateReferenceSpace (m_pImpl->hSession, &SpaceInfo, &m_pImpl->hViewSpace);

                  m_pImpl->CreateActions ();
                  if (m_pImpl->CreateSwapchains ())
                  {
                     m_pImpl->bSessionReady = true;
                     bOk = true;
                     m_pImpl->Log (IENGINE::kLOGLEVEL_Info,
                        "Vulkan2 session ready (" + std::to_string (m_pImpl->nWidth) + "x"
                        + std::to_string (m_pImpl->nHeight) + " stereo)");
                  }
                  else
                     m_pImpl->Log (IENGINE::kLOGLEVEL_Error, "Failed to create stereo swapchains");
               }
            }
            else
               m_pImpl->Log (IENGINE::kLOGLEVEL_Error, "xrGetSystem failed (no HMD)");
         }
         else
            m_pImpl->Log (IENGINE::kLOGLEVEL_Error, "Vulkan2 graphics requirements entry missing");
      }
      else
         m_pImpl->Log (IENGINE::kLOGLEVEL_Error, "OpenXR instance or Vulkan2 extension missing");
   }
#else
   (void) nInstance; (void) nPhysicalDevice; (void) nDevice;
   (void) nQueue; (void) nQueueFamily; (void) nQueueIndex;
#endif

   return bOk;
}

void XR_RUNTIME::UnbindGraphics ()
{
#if defined(__ANDROID__)
   m_pImpl->DestroySession ();
   m_pImpl->hVkDevice   = VK_NULL_HANDLE;
   m_pImpl->hVkPhysical = VK_NULL_HANDLE;
   m_pImpl->hVkQueue    = VK_NULL_HANDLE;
#endif
}

bool XR_RUNTIME::HasSession () const
{
#if defined(__ANDROID__)
   return m_pImpl->bSessionReady;
#else
   return false;
#endif
}

int XR_RUNTIME::RecommendedWidth () const
{
#if defined(__ANDROID__)
   return m_pImpl->nWidth;
#else
   return 0;
#endif
}

int XR_RUNTIME::RecommendedHeight () const
{
#if defined(__ANDROID__)
   return m_pImpl->nHeight;
#else
   return 0;
#endif
}

bool XR_RUNTIME::WaitFrame ()
{
   bool bOk = false;

#if defined(__ANDROID__)
   if (m_pImpl->hSession != XR_NULL_HANDLE)
   {
      m_pImpl->PollEvents ();
      if (m_pImpl->bSessionRunning)
      {
         m_pImpl->PollActions ();
         XrFrameWaitInfo WaitInfo = { XR_TYPE_FRAME_WAIT_INFO };
         m_pImpl->FrameState = { XR_TYPE_FRAME_STATE };
         if (XR_SUCCEEDED (xrWaitFrame (m_pImpl->hSession, &WaitInfo, &m_pImpl->FrameState)))
         {
            m_pImpl->bFrameWaited  = true;
            m_pImpl->bShouldRender = m_pImpl->FrameState.shouldRender;
            bOk = true;
            static bool s_bLoggedWait = false;
            if (!s_bLoggedWait)
            {
               s_bLoggedWait = true;
               m_pImpl->Log (IENGINE::kLOGLEVEL_Info,
                  m_pImpl->bShouldRender ? "xrWaitFrame running (shouldRender)"
                                         : "xrWaitFrame running (skip GPU)");
            }
         }
      }
   }
#endif

   return bOk;
}

bool XR_RUNTIME::ShouldRender () const
{
#if defined(__ANDROID__)
   return m_pImpl->bShouldRender;
#else
   return false;
#endif
}

bool XR_RUNTIME::BeginFrame ()
{
   bool bOk = false;

#if defined(__ANDROID__)
   if (m_pImpl->bFrameWaited)
   {
      XrFrameBeginInfo BeginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
      bOk = XR_SUCCEEDED (xrBeginFrame (m_pImpl->hSession, &BeginInfo));
   }
#endif

   return bOk;
}

int XR_RUNTIME::ViewCount () const
{
#if defined(__ANDROID__)
   return static_cast<int> (m_pImpl->nViewCount);
#else
   return 0;
#endif
}

bool XR_RUNTIME::AcquireView (int nEye, XR_VIEW& View)
{
   bool bOk = false;

#if defined(__ANDROID__)
   if (nEye >= 0  &&  nEye < 2  &&  m_pImpl->hSwapchain[nEye] != XR_NULL_HANDLE)
   {
      XrViewLocateInfo LocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
      LocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      LocateInfo.displayTime           = m_pImpl->FrameState.predictedDisplayTime;
      LocateInfo.space                 = m_pImpl->hSpace;
      XrViewState ViewState = { XR_TYPE_VIEW_STATE };
      uint32_t nLocated = 2;
      if (nEye == 0)
      {
         if (XR_SUCCEEDED (xrLocateViews (m_pImpl->hSession, &LocateInfo, &ViewState, 2, &nLocated, m_pImpl->aView)))
            m_pImpl->nViewStateFlags = ViewState.viewStateFlags;
         else
            m_pImpl->nViewStateFlags = 0;
      }

      XrSwapchainImageAcquireInfo AcquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
      uint32_t nIx = 0;
      if (XR_SUCCEEDED (xrAcquireSwapchainImage (m_pImpl->hSwapchain[nEye], &AcquireInfo, &nIx))
       &&  nIx < m_pImpl->aImage[nEye].size ())
      {
         XrSwapchainImageWaitInfo WaitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
         WaitInfo.timeout = XR_INFINITE_DURATION;
         if (XR_SUCCEEDED (xrWaitSwapchainImage (m_pImpl->hSwapchain[nEye], &WaitInfo)))
         {
            m_pImpl->nAcquired[nEye] = nIx;
            m_pImpl->bAcquired[nEye] = true;

            ConvertPoseToSneeze (m_pImpl->aView[nEye].pose, View.aPosition, View.aDirection, View.aUp);

            View.bOrientationValid = (m_pImpl->nViewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            View.bPositionValid    = (m_pImpl->nViewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;

            const XrFovf& Fov = m_pImpl->aView[nEye].fov;
            View.fAngleLeft  = Fov.angleLeft;
            View.fAngleRight = Fov.angleRight;
            View.fAngleUp    = Fov.angleUp;
            View.fAngleDown  = Fov.angleDown;
            View.fFovY   = Fov.angleUp - Fov.angleDown;
            const float dTanH = std::tan (Fov.angleRight) - std::tan (Fov.angleLeft);
            const float dTanV = std::tan (Fov.angleUp) - std::tan (Fov.angleDown);
            View.fAspect = (dTanV > 1e-6f) ? (dTanH / dTanV) : 1.0f;
            View.nImage    = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (m_pImpl->aImage[nEye][nIx].image));
            View.nVkFormat = static_cast<uint32_t> (m_pImpl->nVkFormat);
            View.nWidth    = m_pImpl->nWidth;
            View.nHeight   = m_pImpl->nHeight;
            View.nSwapchainIx = nIx;

            m_pImpl->aProjView[nEye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            m_pImpl->aProjView[nEye].pose = m_pImpl->aView[nEye].pose;
            m_pImpl->aProjView[nEye].fov  = m_pImpl->aView[nEye].fov;
            m_pImpl->aProjView[nEye].subImage.swapchain = m_pImpl->hSwapchain[nEye];
            m_pImpl->aProjView[nEye].subImage.imageRect.offset = { 0, 0 };
            m_pImpl->aProjView[nEye].subImage.imageRect.extent = { m_pImpl->nWidth, m_pImpl->nHeight };

            bOk = true;
         }
      }
   }
#else
   (void) nEye; (void) View;
#endif

   return bOk;
}

void XR_RUNTIME::ReleaseView (int nEye)
{
#if defined(__ANDROID__)
   if (nEye >= 0  &&  nEye < 2  &&  m_pImpl->bAcquired[nEye])
   {
      XrSwapchainImageReleaseInfo ReleaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
      xrReleaseSwapchainImage (m_pImpl->hSwapchain[nEye], &ReleaseInfo);
      m_pImpl->bAcquired[nEye] = false;
   }
#else
   (void) nEye;
#endif
}

void XR_RUNTIME::EndFrame ()
{
#if defined(__ANDROID__)
   if (!m_pImpl->bFrameWaited)
      return;

   XrCompositionLayerProjection Projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
   Projection.space = m_pImpl->hSpace;
   Projection.viewCount = 2;
   Projection.views = m_pImpl->aProjView;

   XrCompositionLayerQuad Quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
   Quad.space = m_pImpl->hViewSpace;
   Quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
   Quad.subImage.swapchain = m_pImpl->hChromeSwapchain;
   Quad.subImage.imageRect.offset = { 0, 0 };
   Quad.subImage.imageRect.extent = { m_pImpl->nChromeWidth, m_pImpl->nChromeHeight };
   Quad.pose = IdentityPose ();
   Quad.pose.position.y = -0.12f;
   Quad.pose.position.z = -1.15f;
   Quad.size.width  = 0.72f;
   Quad.size.height = 0.10f;

   const XrCompositionLayerBaseHeader* aLayer[2] = {};
   uint32_t nLayer = 0;
   if (m_pImpl->bShouldRender)
   {
      aLayer[nLayer++] = reinterpret_cast<const XrCompositionLayerBaseHeader*> (&Projection);
      if (m_pImpl->hChromeSwapchain != XR_NULL_HANDLE  &&  m_pImpl->hViewSpace != XR_NULL_HANDLE)
      {
         uint32_t nIx = 0;
         XrSwapchainImageAcquireInfo AcquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
         if (XR_SUCCEEDED (xrAcquireSwapchainImage (m_pImpl->hChromeSwapchain, &AcquireInfo, &nIx)))
         {
            XrSwapchainImageWaitInfo WaitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            WaitInfo.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage (m_pImpl->hChromeSwapchain, &WaitInfo);
            if (nIx < m_pImpl->aChromeImage.size ())
               m_pImpl->UploadChrome (m_pImpl->aChromeImage[nIx].image);
            XrSwapchainImageReleaseInfo ReleaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage (m_pImpl->hChromeSwapchain, &ReleaseInfo);
            aLayer[nLayer++] = reinterpret_cast<const XrCompositionLayerBaseHeader*> (&Quad);
         }
      }
   }

   XrFrameEndInfo EndInfo = { XR_TYPE_FRAME_END_INFO };
   EndInfo.displayTime          = m_pImpl->FrameState.predictedDisplayTime;
   EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
   EndInfo.layerCount           = nLayer;
   EndInfo.layers               = aLayer;
   xrEndFrame (m_pImpl->hSession, &EndInfo);
   m_pImpl->bFrameWaited = false;
#endif
}

bool XR_RUNTIME::ConsumeUrlFocus ()
{
#if defined(__ANDROID__)
   return m_pImpl->bUrlFocus.exchange (false);
#else
   return false;
#endif
}

void XR_RUNTIME::SetChromePixels (const uint8_t* pRgba, int nWidth, int nHeight)
{
#if defined(__ANDROID__)
   if (pRgba  &&  nWidth > 0  &&  nHeight > 0)
   {
      std::lock_guard<std::mutex> lock (m_pImpl->mxChrome);
      m_pImpl->nChromePixelW = nWidth;
      m_pImpl->nChromePixelH = nHeight;
      m_pImpl->aChromePixels.assign (pRgba, pRgba + static_cast<size_t> (nWidth) * static_cast<size_t> (nHeight) * 4u);
      m_pImpl->bChromeDirty = true;
   }
#else
   (void) pRgba; (void) nWidth; (void) nHeight;
#endif
}

}} // namespace SNEEZE::DEP
