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

#include "camera/Capture_Platform.h"
#include "camera/Capture_Convert.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <TargetConditionals.h>

#include <mutex>
#include <unordered_map>
#include <vector>

using namespace SNEEZE::DEP;

@interface SneezeCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
   std::mutex            m_mxFrame;
   CAPTURE_PLATFORM::FRAME m_Frame;
}
- (BOOL)copyLatest:(CAPTURE_PLATFORM::FRAME&)frame;
@end

@implementation SneezeCaptureDelegate

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
   (void)output;
   (void)connection;
   CVImageBufferRef image = CMSampleBufferGetImageBuffer (sampleBuffer);
   if (image)
   {
      CVPixelBufferLockBaseAddress (image, kCVPixelBufferLock_ReadOnly);
      const int nWidth  = static_cast<int> (CVPixelBufferGetWidth (image));
      const int nHeight = static_cast<int> (CVPixelBufferGetHeight (image));
      const int nStride = static_cast<int> (CVPixelBufferGetBytesPerRow (image));
      const uint8_t* pSrc = static_cast<const uint8_t*> (CVPixelBufferGetBaseAddress (image));
      std::vector<uint8_t> aRgba;
      CAPTURE_CONVERT::Bgra (pSrc, nWidth, nHeight, nStride, aRgba);
      CVPixelBufferUnlockBaseAddress (image, kCVPixelBufferLock_ReadOnly);

      if (!aRgba.empty ())
      {
         std::lock_guard<std::mutex> lock (m_mxFrame);
         m_Frame.nWidth  = nWidth;
         m_Frame.nHeight = nHeight;
         m_Frame.nFrameIx++;
         m_Frame.aRgba.swap (aRgba);
      }
   }
}

- (BOOL)copyLatest:(CAPTURE_PLATFORM::FRAME&)frame
{
   BOOL bResult = NO;
   std::lock_guard<std::mutex> lock (m_mxFrame);
   if (!m_Frame.aRgba.empty ())
   {
      frame = m_Frame;
      bResult = YES;
   }
   return bResult;
}

@end

namespace
{
   struct DEVICE
   {
      AVCaptureSession*       pSession  = nil;
      AVCaptureDeviceInput*   pInput    = nil;
      AVCaptureVideoDataOutput* pOutput = nil;
      SneezeCaptureDelegate*  pDelegate = nil;
      dispatch_queue_t        pQueue    = nil;
   };

   std::mutex                          s_mxMap;
   uint32_t                            s_nNext = 1;
   std::unordered_map<uint32_t, DEVICE*> s_umpDevice;

   NSArray<AVCaptureDevice*>* DeviceList ()
   {
      NSMutableArray<AVCaptureDeviceType>* aType = [NSMutableArray array];
      [aType addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#if TARGET_OS_IPHONE
      if (@available (iOS 13.0, *))
         [aType addObject:AVCaptureDeviceTypeBuiltInUltraWideCamera];
      [aType addObject:AVCaptureDeviceTypeBuiltInTelephotoCamera];
#else
      if (@available (macOS 14.0, *))
         [aType addObject:AVCaptureDeviceTypeExternal];
      else
         [aType addObject:AVCaptureDeviceTypeExternalUnknown];
#endif
      AVCaptureDeviceDiscoverySession* pDiscover =
         [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:aType
                                                                mediaType:AVMediaTypeVideo
                                                                 position:AVCaptureDevicePositionUnspecified];
      return pDiscover.devices;
   }

   void DestroyDevice (DEVICE* pDevice)
   {
      if (pDevice)
      {
         if (pDevice->pSession)
            [pDevice->pSession stopRunning];
         pDevice->pSession  = nil;
         pDevice->pInput    = nil;
         pDevice->pOutput   = nil;
         pDevice->pDelegate = nil;
         pDevice->pQueue    = nil;
         delete pDevice;
      }
   }
}

bool CAPTURE_PLATFORM::Startup ()
{
   return true;
}

void CAPTURE_PLATFORM::Shutdown ()
{
   std::lock_guard<std::mutex> lock (s_mxMap);
   for (auto& pair : s_umpDevice)
      DestroyDevice (pair.second);
   s_umpDevice.clear ();
}

void CAPTURE_PLATFORM::Enumerate (std::vector<INFO>& aInfo)
{
   aInfo.clear ();
   NSArray<AVCaptureDevice*>* aDevice = DeviceList ();
   uint32_t nIndex = 0;
   for (AVCaptureDevice* pDevice in aDevice)
   {
      INFO info;
      const char* pName = pDevice.localizedName.UTF8String;
      if (pName)
         info.sName = pName;
      else
         info.sName = "Camera " + std::to_string (nIndex);
      aInfo.push_back (info);
      nIndex++;
   }
}

uint32_t CAPTURE_PLATFORM::Open (int nIndex)
{
   uint32_t nHandle = 0;
   NSArray<AVCaptureDevice*>* aDevice = DeviceList ();
   if (nIndex >= 0  &&  nIndex < static_cast<int> (aDevice.count))
   {
      AVCaptureDevice* pCam = aDevice[static_cast<NSUInteger> (nIndex)];
      NSError* pError = nil;
      AVCaptureDeviceInput* pInput = [AVCaptureDeviceInput deviceInputWithDevice:pCam error:&pError];
      if (pInput)
      {
         DEVICE* pDevice = new DEVICE ();
         pDevice->pSession  = [[AVCaptureSession alloc] init];
         pDevice->pInput    = pInput;
         pDevice->pOutput   = [[AVCaptureVideoDataOutput alloc] init];
         pDevice->pDelegate = [[SneezeCaptureDelegate alloc] init];
         pDevice->pQueue    = dispatch_queue_create ("sneeze.capture", DISPATCH_QUEUE_SERIAL);

         NSDictionary* pSettings = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
         };
         pDevice->pOutput.videoSettings = pSettings;
         [pDevice->pOutput setSampleBufferDelegate:pDevice->pDelegate queue:pDevice->pQueue];

         if ([pDevice->pSession canAddInput:pDevice->pInput])
            [pDevice->pSession addInput:pDevice->pInput];
         if ([pDevice->pSession canAddOutput:pDevice->pOutput])
            [pDevice->pSession addOutput:pDevice->pOutput];

         [pDevice->pSession startRunning];

         std::lock_guard<std::mutex> lock (s_mxMap);
         nHandle = s_nNext++;
         s_umpDevice.emplace (nHandle, pDevice);
      }
   }
   return nHandle;
}

void CAPTURE_PLATFORM::Close (uint32_t nHandle)
{
   DEVICE* pDevice = nullptr;
   {
      std::lock_guard<std::mutex> lock (s_mxMap);
      auto it = s_umpDevice.find (nHandle);
      if (it != s_umpDevice.end ())
      {
         pDevice = it->second;
         s_umpDevice.erase (it);
      }
   }
   DestroyDevice (pDevice);
}

bool CAPTURE_PLATFORM::Latest (uint32_t nHandle, FRAME& Frame)
{
   bool bResult = false;
   SneezeCaptureDelegate* pDelegate = nil;
   {
      std::lock_guard<std::mutex> lock (s_mxMap);
      auto it = s_umpDevice.find (nHandle);
      if (it != s_umpDevice.end ()  &&  it->second)
         pDelegate = it->second->pDelegate;
   }
   if (pDelegate)
      bResult = [pDelegate copyLatest:Frame] ? true : false;
   return bResult;
}
