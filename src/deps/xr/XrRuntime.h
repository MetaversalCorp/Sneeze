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

#ifndef SNEEZE_XR_RUNTIME_H
#define SNEEZE_XR_RUNTIME_H

#include <cstdint>
#include <string>

namespace SNEEZE
{
   namespace DEP
   {
      // One HMD eye after xrLocateViews + swapchain acquire. Coordinates are
      // Sneeze Z-up (x east, y north, z up). nImage is a VkImage handle.
      struct XR_VIEW
      {
         float    aPosition[3]  = { 0.0f, 0.0f, 0.0f };
         float    aDirection[3] = { 0.0f, 1.0f, 0.0f };
         float    aUp[3]        = { 0.0f, 0.0f, 1.0f };
         float    fFovY         = 1.0f;
         float    fAspect       = 1.0f;
         float    fAngleLeft    = 0.0f;
         float    fAngleRight   = 0.0f;
         float    fAngleUp      = 0.0f;
         float    fAngleDown    = 0.0f;
         uint64_t nImage        = 0;
         uint32_t nVkFormat     = 0;
         int      nWidth        = 0;
         int      nHeight       = 0;
         uint32_t nSwapchainIx  = 0;
         bool     bOrientationValid = false;
         bool     bPositionValid    = false;
      };

      // XR_RUNTIME exposes the OpenXR runtime to the engine. The implementation
      // is selected at CMake-configure time: when SNEEZE_ENABLE_XR is ON the
      // real OpenXR loader lives in XrRuntime.cpp; otherwise XrRuntime_Stub.cpp
      // provides a no-op stub (Initialize succeeds with HasRuntime () == false).
      // Either way the header is openxr-free so consumers don't need the SDK.
      class XR_RUNTIME
      {
      public:
         XR_RUNTIME (ENGINE* pEngine);
         ~XR_RUNTIME ();

         bool Initialize ();

         bool        HasRuntime () const;
         std::string GetRuntimeName () const;

         // True when this build will create an OpenXR Vulkan session (Quest).
         bool WantsSession () const;

         // Layout must match Halogen HALOGEN_VULKAN_CREATE (halogenSetVulkanCreate).
         struct XR_VULKAN_CREATE
         {
            void*    pUser;
            uint64_t (*fnCreateInstance) (void* pUser, const void* pVkInstanceCreateInfo);
            uint64_t (*fnSelectPhysicalDevice) (void* pUser, uint64_t nInstance);
            uint64_t (*fnCreateDevice) (void* pUser, const void* pVkDeviceCreateInfo, uint64_t nPhysicalDevice);
         };

         // xrGetSystem + xrGetVulkanGraphicsRequirements2KHR. Must run before
         // Filament creates a VkInstance. Returns the create hooks, or null.
         bool                        PrepareGraphics ();
         const XR_VULKAN_CREATE*     VulkanCreateHooks () const;

         // After Halogen/Filament exists. n* Vulkan handles are UINT64 values
         // from halogen.vk.* ANARI properties (pointer-sized on 64-bit).
         bool BindGraphics (uint64_t nInstance, uint64_t nPhysicalDevice, uint64_t nDevice,
                            uint64_t nQueue, uint32_t nQueueFamily, uint32_t nQueueIndex);

         // Tear down the session (and Halogen-owned Vulkan helpers) while the
         // Filament device is still alive. Called from renderer shutdown.
         void UnbindGraphics ();

         bool HasSession () const;
         int  RecommendedWidth () const;
         int  RecommendedHeight () const;

         // Compositor-thread frame loop. If WaitFrame returns false the session
         // is gone; still call EndFrame after a successful WaitFrame.
         bool WaitFrame ();
         bool ShouldRender () const;
         bool BeginFrame ();
         int  ViewCount () const;
         bool AcquireView (int nEye, XR_VIEW& View);
         void ReleaseView (int nEye);
         void EndFrame ();

         // RGBA8 pixels for the head-locked URL-bar quad (app thread). Copied
         // onto the chrome swapchain on the compositor thread in EndFrame.
         void SetChromePixels (const uint8_t* pRgba, int nWidth, int nHeight);

         // Quest menu / A. Polled on the compositor thread; consumed on the app thread.
         bool ConsumeUrlFocus ();

      private:
         class Impl;
         Impl* m_pImpl;
      };
   } // namespace DEP
}

#endif // SNEEZE_XR_RUNTIME_H
