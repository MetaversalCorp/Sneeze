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
#include "camera/Capture_Pinhole.h"

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
   std::mutex              m_mxFrame;
   CAPTURE_PLATFORM::FRAME m_Frame;
   CAPTURE::INTRINSICS        m_Intrinsics;
}
- (BOOL)copyLatest:(CAPTURE_PLATFORM::FRAME&)frame;
- (BOOL)copyPinhole:(CAPTURE::INTRINSICS&)pinhole;
- (void)setFallbackPinhole:(const CAPTURE::INTRINSICS&)pinhole;
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
         CAPTURE::INTRINSICS pin;
         bool bPin = false;
         CFTypeRef pAtt = CMGetAttachment (sampleBuffer, kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix, nullptr);
         if (pAtt  &&  CFGetTypeID (pAtt) == CFDataGetTypeID ())
         {
            CFDataRef pData = (CFDataRef)pAtt;
            const CFIndex nBytes = CFDataGetLength (pData);
            const float* pMat = reinterpret_cast<const float*> (CFDataGetBytePtr (pData));
            if (pMat  &&  nBytes >= 36)
            {
               CAPTURE_PINHOLE::Clear (pin);
               pin.nWidth       = nWidth;
               pin.nHeight      = nHeight;
               pin.eOrientation = CAPTURE::kORIENTATION_TOP_LEFT;
               if (nBytes >= 48)
               {
                  pin.dFx = pMat[0];
                  pin.dFy = pMat[5];
                  pin.dCx = pMat[8];
                  pin.dCy = pMat[9];
               }
               else
               {
                  pin.dFx = pMat[0];
                  pin.dFy = pMat[4];
                  pin.dCx = pMat[6];
                  pin.dCy = pMat[7];
               }
               bPin = CAPTURE_PINHOLE::Valid (pin);
            }
         }

         std::lock_guard<std::mutex> lock (m_mxFrame);
         m_Frame.nWidth  = nWidth;
         m_Frame.nHeight = nHeight;
         m_Frame.nFrameIx++;
         m_Frame.aRgba.swap (aRgba);
         if (bPin)
            m_Intrinsics = pin;
         else if (CAPTURE_PINHOLE::Valid (m_Intrinsics)  &&  (m_Intrinsics.nWidth != nWidth  ||  m_Intrinsics.nHeight != nHeight))
            CAPTURE_PINHOLE::Scale (m_Intrinsics, m_Intrinsics.nWidth, m_Intrinsics.nHeight, nWidth, nHeight);
         else if (!CAPTURE_PINHOLE::Valid (m_Intrinsics))
            CAPTURE_PINHOLE::From_Size (m_Intrinsics, nWidth, nHeight);
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

- (BOOL)copyPinhole:(CAPTURE::INTRINSICS&)pinhole
{
   BOOL bResult = NO;
   std::lock_guard<std::mutex> lock (m_mxFrame);
   if (CAPTURE_PINHOLE::Valid (m_Intrinsics))
   {
      pinhole = m_Intrinsics;
      bResult = YES;
   }
   else if (m_Frame.nWidth > 0  &&  m_Frame.nHeight > 0  &&  CAPTURE_PINHOLE::From_Size (pinhole, m_Frame.nWidth, m_Frame.nHeight))
   {
      m_Intrinsics = pinhole;
      bResult = YES;
   }
   return bResult;
}

- (void)setFallbackPinhole:(const CAPTURE::INTRINSICS&)pinhole
{
   std::lock_guard<std::mutex> lock (m_mxFrame);
   if (CAPTURE_PINHOLE::Valid (pinhole)  &&  !CAPTURE_PINHOLE::Valid (m_Intrinsics))
      m_Intrinsics = pinhole;
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

         AVCaptureConnection* pConn = [pDevice->pOutput connectionWithMediaType:AVMediaTypeVideo];
#if TARGET_OS_IPHONE
         if (@available (iOS 11.0, *))
         {
            if (pConn  &&  pConn.isCameraIntrinsicMatrixDeliverySupported)
               pConn.cameraIntrinsicMatrixDeliveryEnabled = YES;
         }
#endif

         AVCaptureDeviceFormat* pFmt = pCam.activeFormat;
         if (pFmt)
         {
            const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions (pFmt.formatDescription);
            CAPTURE::INTRINSICS pin;
            if (!CAPTURE_PINHOLE::From_Hfov (pin, dim.width, dim.height, pFmt.videoFieldOfView))
               CAPTURE_PINHOLE::From_Size (pin, dim.width, dim.height);
            [pDevice->pDelegate setFallbackPinhole:pin];
         }

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

bool CAPTURE_PLATFORM::Intrinsics (uint32_t nHandle, CAPTURE::INTRINSICS& Pinhole)
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
      bResult = [pDelegate copyPinhole:Pinhole] ? true : false;
   return bResult;
}
