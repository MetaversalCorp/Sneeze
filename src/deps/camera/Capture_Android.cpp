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

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SNEEZE::DEP;

namespace
{
   struct DEVICE
   {
      ACameraManager*             pManager   = nullptr;
      ACameraDevice*              pDevice    = nullptr;
      AImageReader*               pReader    = nullptr;
      ANativeWindow*              pWindow    = nullptr;
      ACaptureSessionOutput*      pOutput    = nullptr;
      ACaptureSessionOutputContainer* pOutputs = nullptr;
      ACameraOutputTarget*        pTarget    = nullptr;
      ACaptureRequest*            pRequest   = nullptr;
      ACameraCaptureSession*      pSession   = nullptr;
      std::string                 sId;
      int                         nWidth     = 640;
      int                         nHeight    = 480;
      std::mutex                  mxFrame;
      std::vector<uint8_t>        aRgba;
      uint64_t                    nFrameIx   = 0;
   };

   std::mutex                            s_mxMap;
   uint32_t                              s_nNext = 1;
   std::unordered_map<uint32_t, DEVICE*> s_umpDevice;
   ACameraManager*                       s_pManager = nullptr;

   void OnImage (void* pContext, AImageReader* pReader)
   {
      DEVICE* pDevice = static_cast<DEVICE*> (pContext);
      AImage* pImage  = nullptr;
      if (pDevice  &&  AImageReader_acquireLatestImage (pReader, &pImage) == AMEDIA_OK  &&  pImage)
      {
         int32_t nWidth = 0, nHeight = 0;
         AImage_getWidth  (pImage, &nWidth);
         AImage_getHeight (pImage, &nHeight);

         uint8_t* pY = nullptr;
         uint8_t* pU = nullptr;
         uint8_t* pV = nullptr;
         int nLenY = 0, nLenU = 0, nLenV = 0;
         int nStrideY = 0, nStrideU = 0, nStrideV = 0;
         int nPixelU = 1, nPixelV = 1;
         AImage_getPlaneData           (pImage, 0, &pY, &nLenY);
         AImage_getPlaneData           (pImage, 1, &pU, &nLenU);
         AImage_getPlaneData           (pImage, 2, &pV, &nLenV);
         AImage_getPlaneRowStride      (pImage, 0, &nStrideY);
         AImage_getPlaneRowStride      (pImage, 1, &nStrideU);
         AImage_getPlaneRowStride      (pImage, 2, &nStrideV);
         AImage_getPlanePixelStride    (pImage, 1, &nPixelU);
         AImage_getPlanePixelStride    (pImage, 2, &nPixelV);

         std::vector<uint8_t> aRgba;
         CAPTURE_CONVERT::Yuv420 (pY, nStrideY, pU, nStrideU, nPixelU, pV, nStrideV, nPixelV, nWidth, nHeight, aRgba);
         AImage_delete (pImage);

         if (!aRgba.empty ())
         {
            std::lock_guard<std::mutex> lock (pDevice->mxFrame);
            pDevice->nWidth  = nWidth;
            pDevice->nHeight = nHeight;
            pDevice->aRgba.swap (aRgba);
            pDevice->nFrameIx++;
         }
      }
   }

   void OnDisconnected (void*, ACameraDevice* pCam)
   {
      (void)pCam;
   }

   void OnError (void*, ACameraDevice* pCam, int nError)
   {
      (void)pCam;
      (void)nError;
   }

   void OnSessionClosed (void*, ACameraCaptureSession*) {}
   void OnSessionReady  (void*, ACameraCaptureSession*) {}
   void OnSessionActive (void*, ACameraCaptureSession*) {}

   std::string DeviceName (ACameraManager* pManager, const char* pId)
   {
      std::string sName = pId ? pId : "Camera";
      ACameraMetadata* pMeta = nullptr;
      if (pManager  &&  pId  &&  ACameraManager_getCameraCharacteristics (pManager, pId, &pMeta) == ACAMERA_OK  &&  pMeta)
      {
         ACameraMetadata_const_entry entry = {};
         if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK  &&  entry.count > 0)
         {
            const uint8_t nFacing = entry.data.u8[0];
            if (nFacing == ACAMERA_LENS_FACING_FRONT)
               sName = "Front camera";
            else if (nFacing == ACAMERA_LENS_FACING_BACK)
               sName = "Back camera";
            else
               sName = "External camera";
         }
         ACameraMetadata_free (pMeta);
      }
      return sName;
   }

   void DestroyDevice (DEVICE* pDevice)
   {
      if (pDevice)
      {
         if (pDevice->pSession)
         {
            ACameraCaptureSession_stopRepeating (pDevice->pSession);
            ACameraCaptureSession_close (pDevice->pSession);
         }
         if (pDevice->pRequest)
            ACaptureRequest_free (pDevice->pRequest);
         if (pDevice->pTarget)
            ACameraOutputTarget_free (pDevice->pTarget);
         if (pDevice->pOutput)
            ACaptureSessionOutput_free (pDevice->pOutput);
         if (pDevice->pOutputs)
            ACaptureSessionOutputContainer_free (pDevice->pOutputs);
         if (pDevice->pWindow)
            ANativeWindow_release (pDevice->pWindow);
         if (pDevice->pReader)
            AImageReader_delete (pDevice->pReader);
         if (pDevice->pDevice)
            ACameraDevice_close (pDevice->pDevice);
         delete pDevice;
      }
   }
}

bool CAPTURE_PLATFORM::Startup ()
{
   bool bResult = false;
   s_pManager = ACameraManager_create ();
   if (s_pManager)
      bResult = true;
   return bResult;
}

void CAPTURE_PLATFORM::Shutdown ()
{
   {
      std::lock_guard<std::mutex> lock (s_mxMap);
      for (auto& pair : s_umpDevice)
         DestroyDevice (pair.second);
      s_umpDevice.clear ();
   }
   if (s_pManager)
   {
      ACameraManager_delete (s_pManager);
      s_pManager = nullptr;
   }
}

void CAPTURE_PLATFORM::Enumerate (std::vector<INFO>& aInfo)
{
   aInfo.clear ();
   if (s_pManager)
   {
      ACameraIdList* pList = nullptr;
      if (ACameraManager_getCameraIdList (s_pManager, &pList) == ACAMERA_OK  &&  pList)
      {
         for (int i = 0; i < pList->numCameras; i++)
         {
            INFO info;
            info.sName = DeviceName (s_pManager, pList->cameraIds[i]);
            aInfo.push_back (info);
         }
         ACameraManager_deleteCameraIdList (pList);
      }
   }
}

uint32_t CAPTURE_PLATFORM::Open (int nIndex)
{
   uint32_t nHandle = 0;
   if (s_pManager  &&  nIndex >= 0)
   {
      ACameraIdList* pList = nullptr;
      if (ACameraManager_getCameraIdList (s_pManager, &pList) == ACAMERA_OK  &&  pList)
      {
         if (nIndex < pList->numCameras)
         {
            DEVICE* pDevice = new DEVICE ();
            pDevice->sId      = pList->cameraIds[nIndex];
            pDevice->pManager = s_pManager;

            ACameraDevice_StateCallbacks deviceCb = {};
            deviceCb.context        = pDevice;
            deviceCb.onDisconnected = OnDisconnected;
            deviceCb.onError        = OnError;

            if (ACameraManager_openCamera (s_pManager, pDevice->sId.c_str (), &deviceCb, &pDevice->pDevice) == ACAMERA_OK)
            {
               if (AImageReader_new (pDevice->nWidth, pDevice->nHeight, AIMAGE_FORMAT_YUV_420_888, 4, &pDevice->pReader) == AMEDIA_OK)
               {
                  AImageReader_ImageListener listener = {};
                  listener.context = pDevice;
                  listener.onImageAvailable = OnImage;
                  AImageReader_setImageListener (pDevice->pReader, &listener);
                  AImageReader_getWindow (pDevice->pReader, &pDevice->pWindow);
                  if (pDevice->pWindow)
                     ANativeWindow_acquire (pDevice->pWindow);

                  ACaptureSessionOutputContainer_create (&pDevice->pOutputs);
                  ACaptureSessionOutput_create (pDevice->pWindow, &pDevice->pOutput);
                  ACaptureSessionOutputContainer_add (pDevice->pOutputs, pDevice->pOutput);
                  ACameraOutputTarget_create (pDevice->pWindow, &pDevice->pTarget);
                  ACameraDevice_createCaptureRequest (pDevice->pDevice, TEMPLATE_PREVIEW, &pDevice->pRequest);
                  ACaptureRequest_addTarget (pDevice->pRequest, pDevice->pTarget);

                  ACameraCaptureSession_stateCallbacks sessionCb = {};
                  sessionCb.context      = pDevice;
                  sessionCb.onClosed     = OnSessionClosed;
                  sessionCb.onReady      = OnSessionReady;
                  sessionCb.onActive     = OnSessionActive;

                  if (ACameraDevice_createCaptureSession (pDevice->pDevice, pDevice->pOutputs, &sessionCb, &pDevice->pSession) == ACAMERA_OK)
                  {
                     ACameraCaptureSession_setRepeatingRequest (pDevice->pSession, nullptr, 1, &pDevice->pRequest, nullptr);
                     std::lock_guard<std::mutex> lock (s_mxMap);
                     nHandle = s_nNext++;
                     s_umpDevice.emplace (nHandle, pDevice);
                  }
               }
            }

            if (nHandle == 0)
               DestroyDevice (pDevice);
         }
         ACameraManager_deleteCameraIdList (pList);
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
   DEVICE* pDevice = nullptr;
   {
      std::lock_guard<std::mutex> lock (s_mxMap);
      auto it = s_umpDevice.find (nHandle);
      if (it != s_umpDevice.end ())
         pDevice = it->second;
   }
   if (pDevice)
   {
      std::lock_guard<std::mutex> lock (pDevice->mxFrame);
      if (!pDevice->aRgba.empty ())
      {
         Frame.nWidth   = pDevice->nWidth;
         Frame.nHeight  = pDevice->nHeight;
         Frame.nFrameIx = pDevice->nFrameIx;
         Frame.aRgba    = pDevice->aRgba;
         bResult        = true;
      }
   }
   return bResult;
}
