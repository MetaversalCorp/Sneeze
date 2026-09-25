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

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <android/looper.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace SNEEZE::DEP;

namespace
{
   // Meta vendor tags. camera_source 0 is a passthrough RGB camera.
   // position 0 is the left camera, 1 is the right, from the wearer.
   const uint32_t kTAG_CAMERA_SOURCE = 0x80004d00u;
   const uint32_t kTAG_POSITION      = 0x80004d01u;

   struct DEVICE
   {
      ACameraManager*                  pManager   = nullptr;
      ACameraDevice*                   pDevice    = nullptr;
      AImageReader*                    pReader    = nullptr;
      ANativeWindow*                   pWindow    = nullptr;
      ACaptureSessionOutput*           pOutput    = nullptr;
      ACaptureSessionOutputContainer*  pOutputs   = nullptr;
      ACameraOutputTarget*             pTarget    = nullptr;
      ACaptureRequest*                 pRequest   = nullptr;
      ACameraCaptureSession*           pSession   = nullptr;
      std::string                      sId;
      int                              nWidth     = 1280;
      int                              nHeight    = 960;
      std::mutex                       mxFrame;
      std::vector<uint8_t>             aRgba;
      uint64_t                         nFrameIx   = 0;
      CAPTURE::INTRINSICS                 Intrinsics;
   };

   struct CAM
   {
      std::string sId;
      std::string sName;
      int         nSource   = -1;
      int         nPosition = -1;
   };

   struct JOB
   {
      int      nOpenIndex   = -1;
      uint32_t nCloseHandle = 0;
      uint32_t nResult      = 0;
      bool     bDone        = false;
   };

   std::mutex                            s_mxMap;
   uint32_t                              s_nNext = 1;
   std::unordered_map<uint32_t, DEVICE*> s_umpDevice;
   ACameraManager*                       s_pManager = nullptr;

   std::mutex              s_mxThread;
   std::condition_variable s_cvThread;
   std::thread             s_thCamera;
   ALooper*                s_pLooper  = nullptr;
   bool                    s_bReady   = false;
   bool                    s_bStop    = false;
   JOB*                    s_pJob     = nullptr;

   void Log (const char* szMessage)
   {
      __android_log_print (ANDROID_LOG_INFO, "CAPTURE", "%s", szMessage);
   }

   int MetaInt (ACameraMetadata* pMeta, uint32_t nTag)
   {
      int nValue = -1;
      ACameraMetadata_const_entry entry = {};
      if (pMeta  &&  ACameraMetadata_getConstEntry (pMeta, nTag, &entry) == ACAMERA_OK  &&  entry.count > 0)
      {
         if (entry.type == ACAMERA_TYPE_INT32)
            nValue = entry.data.i32[0];
         else if (entry.type == ACAMERA_TYPE_BYTE)
            nValue = entry.data.u8[0];
      }
      return nValue;
   }

   void PickSize (ACameraManager* pManager, const char* pId, int& nWidth, int& nHeight)
   {
      nWidth  = 1280;
      nHeight = 960;

      ACameraMetadata* pMeta = nullptr;
      if (pManager  &&  pId  &&  ACameraManager_getCameraCharacteristics (pManager, pId, &pMeta) == ACAMERA_OK  &&  pMeta)
      {
         ACameraMetadata_const_entry entry = {};
         if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) == ACAMERA_OK)
         {
            int nBestW    = 0;
            int nBestH    = 0;
            int nBestDist = 0x7fffffff;
            int nSmallW   = 0;
            int nSmallH   = 0;
            int nSmall    = 0x7fffffff;
            const int nWant = 1280 * 960;

            for (uint32_t i = 0; i + 3 < entry.count; i += 4)
            {
               const int nFormat = entry.data.i32[i];
               const int nW      = entry.data.i32[i + 1];
               const int nH      = entry.data.i32[i + 2];
               const int nInput  = entry.data.i32[i + 3];
               if (nInput != 0  ||  nFormat != AIMAGE_FORMAT_YUV_420_888  ||  nW < 160  ||  nH < 120)
                  continue;

               const int nArea = nW * nH;
               if (nArea < nSmall)
               {
                  nSmall  = nArea;
                  nSmallW = nW;
                  nSmallH = nH;
               }
               if (nW <= 1920  &&  nH <= 1920)
               {
                  int nDist = nArea - nWant;
                  if (nDist < 0)
                     nDist = -nDist;
                  if (nDist < nBestDist)
                  {
                     nBestDist = nDist;
                     nBestW    = nW;
                     nBestH    = nH;
                  }
               }
            }

            if (nBestW > 0)
            {
               nWidth  = nBestW;
               nHeight = nBestH;
            }
            else if (nSmallW > 0)
            {
               nWidth  = nSmallW;
               nHeight = nSmallH;
            }
         }
         ACameraMetadata_free (pMeta);
      }
   }

   bool FillPinhole (ACameraManager* pManager, const char* pId, int nStreamW, int nStreamH, CAPTURE::INTRINSICS& Pinhole)
   {
      bool bResult = false;
      int  nOrient = 0;
      bool bOrient = false;
      CAPTURE_PINHOLE::Clear (Pinhole);

      ACameraMetadata* pMeta = nullptr;
      if (pManager  &&  pId  &&  ACameraManager_getCameraCharacteristics (pManager, pId, &pMeta) == ACAMERA_OK  &&  pMeta)
      {
         ACameraMetadata_const_entry entry = {};
         if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_SENSOR_ORIENTATION, &entry) == ACAMERA_OK  &&  entry.count > 0)
         {
            nOrient = entry.data.i32[0];
            bOrient = true;
         }

         int nArrW = 0;
         int nArrH = 0;
#ifdef ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE
         if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, &entry) == ACAMERA_OK  &&  entry.count >= 4)
         {
            nArrW = entry.data.i32[2] - entry.data.i32[0];
            nArrH = entry.data.i32[3] - entry.data.i32[1];
         }
#endif
         if ((nArrW <= 0  ||  nArrH <= 0)  &&
             ACameraMetadata_getConstEntry (pMeta, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &entry) == ACAMERA_OK  &&  entry.count >= 2)
         {
            nArrW = entry.data.i32[0];
            nArrH = entry.data.i32[1];
         }

#ifdef ACAMERA_LENS_INTRINSIC_CALIBRATION
         if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_LENS_INTRINSIC_CALIBRATION, &entry) == ACAMERA_OK  &&  entry.count >= 4)
         {
            Pinhole.nWidth       = nArrW > 0 ? nArrW : nStreamW;
            Pinhole.nHeight      = nArrH > 0 ? nArrH : nStreamH;
            Pinhole.dFx          = entry.data.f[0];
            Pinhole.dFy          = entry.data.f[1];
            Pinhole.dCx          = entry.data.f[2];
            Pinhole.dCy          = entry.data.f[3];
            if (nStreamW > 0  &&  nStreamH > 0)
               CAPTURE_PINHOLE::Scale (Pinhole, Pinhole.nWidth, Pinhole.nHeight, nStreamW, nStreamH);
            bResult = CAPTURE_PINHOLE::Valid (Pinhole);
         }
#endif

         if (!bResult)
         {
            double dFocal = 0.0;
            double dSensW = 0.0;
            double dSensH = 0.0;
            if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &entry) == ACAMERA_OK  &&  entry.count > 0)
               dFocal = entry.data.f[0];
            if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &entry) == ACAMERA_OK  &&  entry.count >= 2)
            {
               dSensW = entry.data.f[0];
               dSensH = entry.data.f[1];
            }
            if (CAPTURE_PINHOLE::From_Focal_Mm (Pinhole, nStreamW, nStreamH, dFocal, dSensW, dSensH))
               bResult = true;
         }

         ACameraMetadata_free (pMeta);
      }

      if (!bResult)
         bResult = CAPTURE_PINHOLE::From_Size (Pinhole, nStreamW, nStreamH);

      if (bResult  &&  bOrient)
         Pinhole.eOrientation = CAPTURE_PINHOLE::Orientation_From_Sensor (nOrient);

      return bResult;
   }

   void ListCameras (std::vector<CAM>& aCam)
   {
      aCam.clear ();
      if (s_pManager)
      {
         ACameraIdList* pList = nullptr;
         if (ACameraManager_getCameraIdList (s_pManager, &pList) == ACAMERA_OK  &&  pList)
         {
            for (int i = 0; i < pList->numCameras; i++)
            {
               CAM cam;
               cam.sId = pList->cameraIds[i] ? pList->cameraIds[i] : "";
               cam.sName = cam.sId.empty () ? "Camera" : cam.sId;

               ACameraMetadata* pMeta = nullptr;
               if (ACameraManager_getCameraCharacteristics (s_pManager, cam.sId.c_str (), &pMeta) == ACAMERA_OK  &&  pMeta)
               {
                  cam.nSource   = MetaInt (pMeta, kTAG_CAMERA_SOURCE);
                  cam.nPosition = MetaInt (pMeta, kTAG_POSITION);
                  ACameraMetadata_const_entry entry = {};
                  if (ACameraMetadata_getConstEntry (pMeta, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK  &&  entry.count > 0)
                  {
                     const uint8_t nFacing = entry.data.u8[0];
                     if (nFacing == ACAMERA_LENS_FACING_FRONT)
                        cam.sName = "Front camera";
                     else if (nFacing == ACAMERA_LENS_FACING_BACK)
                        cam.sName = "Back camera";
                     else
                        cam.sName = "External camera";
                  }
                  if (cam.nSource == 0  &&  cam.nPosition == 0)
                     cam.sName = "Left camera";
                  else if (cam.nSource == 0  &&  cam.nPosition == 1)
                     cam.sName = "Right camera";
                  ACameraMetadata_free (pMeta);
               }
               aCam.push_back (cam);
            }
            ACameraManager_deleteCameraIdList (pList);
         }
      }

      std::stable_sort (aCam.begin (), aCam.end (), [] (const CAM& a, const CAM& b)
      {
         const int nRankA = (a.nSource == 0) ? a.nPosition : 1000;
         const int nRankB = (b.nSource == 0) ? b.nPosition : 1000;
         return nRankA < nRankB;
      });
   }

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
            if (CAPTURE_PINHOLE::Valid (pDevice->Intrinsics)  &&  (nWidth != pDevice->Intrinsics.nWidth  ||  nHeight != pDevice->Intrinsics.nHeight))
               CAPTURE_PINHOLE::Scale (pDevice->Intrinsics, pDevice->Intrinsics.nWidth, pDevice->Intrinsics.nHeight, nWidth, nHeight);
            pDevice->nWidth  = nWidth;
            pDevice->nHeight = nHeight;
            pDevice->aRgba.swap (aRgba);
            pDevice->nFrameIx++;
         }
      }
   }

   void OnDisconnected (void*, ACameraDevice*) {}
   void OnError (void*, ACameraDevice*, int nError)
   {
      __android_log_print (ANDROID_LOG_WARN, "CAPTURE", "camera device error %d", nError);
   }
   void OnSessionClosed (void*, ACameraCaptureSession*) {}
   void OnSessionReady  (void*, ACameraCaptureSession*) {}
   void OnSessionActive (void*, ACameraCaptureSession*) {}

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

   uint32_t OpenOnThread (int nIndex)
   {
      uint32_t nHandle = 0;
      std::vector<CAM> aCam;
      ListCameras (aCam);
      if (nIndex >= 0  &&  nIndex < static_cast<int> (aCam.size ()))
      {
         DEVICE* pDevice = new DEVICE ();
         pDevice->sId      = aCam[nIndex].sId;
         pDevice->pManager = s_pManager;
         PickSize (s_pManager, pDevice->sId.c_str (), pDevice->nWidth, pDevice->nHeight);
         FillPinhole (s_pManager, pDevice->sId.c_str (), pDevice->nWidth, pDevice->nHeight, pDevice->Intrinsics);

         ACameraDevice_StateCallbacks deviceCb = {};
         deviceCb.context        = pDevice;
         deviceCb.onDisconnected = OnDisconnected;
         deviceCb.onError        = OnError;

         camera_status_t nStatus = ACameraManager_openCamera (s_pManager, pDevice->sId.c_str (), &deviceCb, &pDevice->pDevice);
         if (nStatus == ACAMERA_OK)
         {
            media_status_t nReader = AImageReader_new (pDevice->nWidth, pDevice->nHeight, AIMAGE_FORMAT_YUV_420_888, 4, &pDevice->pReader);
            if (nReader == AMEDIA_OK)
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
               sessionCb.context  = pDevice;
               sessionCb.onClosed = OnSessionClosed;
               sessionCb.onReady  = OnSessionReady;
               sessionCb.onActive = OnSessionActive;

               nStatus = ACameraDevice_createCaptureSession (pDevice->pDevice, pDevice->pOutputs, &sessionCb, &pDevice->pSession);
               if (nStatus == ACAMERA_OK)
               {
                  nStatus = ACameraCaptureSession_setRepeatingRequest (pDevice->pSession, nullptr, 1, &pDevice->pRequest, nullptr);
                  if (nStatus == ACAMERA_OK)
                  {
                     std::lock_guard<std::mutex> lock (s_mxMap);
                     nHandle = s_nNext++;
                     s_umpDevice.emplace (nHandle, pDevice);
                     __android_log_print (ANDROID_LOG_INFO, "CAPTURE", "session %s %dx%d (%s)", pDevice->sId.c_str (), pDevice->nWidth, pDevice->nHeight, aCam[nIndex].sName.c_str ());
                  }
               }
            }
            else
               __android_log_print (ANDROID_LOG_WARN, "CAPTURE", "AImageReader_new %dx%d failed %d", pDevice->nWidth, pDevice->nHeight, static_cast<int> (nReader));
         }
         else
            __android_log_print (ANDROID_LOG_WARN, "CAPTURE", "openCamera %s failed %d", pDevice->sId.c_str (), static_cast<int> (nStatus));

         if (nHandle == 0)
         {
            if (nStatus != ACAMERA_OK)
               __android_log_print (ANDROID_LOG_WARN, "CAPTURE", "capture session failed %d", static_cast<int> (nStatus));
            DestroyDevice (pDevice);
         }
      }
      else
         __android_log_print (ANDROID_LOG_WARN, "CAPTURE", "camera index %d out of range (%d devices)", nIndex, static_cast<int> (aCam.size ()));

      return nHandle;
   }

   void CloseOnThread (uint32_t nHandle)
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

   void CameraThread ()
   {
      ALooper_prepare (ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
      {
         std::lock_guard<std::mutex> lock (s_mxThread);
         s_pLooper = ALooper_forThread ();
         s_bReady  = true;
      }
      s_cvThread.notify_all ();

      while (!s_bStop)
      {
         JOB* pJob = nullptr;
         {
            std::lock_guard<std::mutex> lock (s_mxThread);
            pJob   = s_pJob;
            s_pJob = nullptr;
         }

         if (pJob)
         {
            if (pJob->nOpenIndex >= 0)
               pJob->nResult = OpenOnThread (pJob->nOpenIndex);
            else if (pJob->nCloseHandle != 0)
               CloseOnThread (pJob->nCloseHandle);

            {
               std::lock_guard<std::mutex> lock (s_mxThread);
               pJob->bDone = true;
            }
            s_cvThread.notify_all ();
         }

         // Image callbacks are posted to this looper. pollOnce runs them.
         ALooper_pollOnce (16, nullptr, nullptr, nullptr);
      }

      std::vector<DEVICE*> aDevice;
      {
         std::lock_guard<std::mutex> lock (s_mxMap);
         for (auto& pair : s_umpDevice)
            aDevice.push_back (pair.second);
         s_umpDevice.clear ();
      }
      for (DEVICE* pDevice : aDevice)
         DestroyDevice (pDevice);

      std::lock_guard<std::mutex> lock (s_mxThread);
      s_pLooper = nullptr;
      s_bReady  = false;
   }

   void EnsureThread ()
   {
      std::unique_lock<std::mutex> lock (s_mxThread);
      if (!s_thCamera.joinable ())
      {
         s_bStop  = false;
         s_bReady = false;
         s_thCamera = std::thread (CameraThread);
      }
      s_cvThread.wait (lock, [] { return s_bReady  ||  s_bStop; });
   }

   void Post (JOB& job)
   {
      EnsureThread ();
      {
         std::lock_guard<std::mutex> lock (s_mxThread);
         s_pJob = &job;
         if (s_pLooper)
            ALooper_wake (s_pLooper);
      }
      std::unique_lock<std::mutex> lock (s_mxThread);
      s_cvThread.wait_for (lock, std::chrono::seconds (5), [&job] { return job.bDone; });
   }
}

bool CAPTURE_PLATFORM::Startup ()
{
   bool bResult = false;
   s_pManager = ACameraManager_create ();
   if (s_pManager)
   {
      EnsureThread ();
      bResult = true;
      std::vector<CAM> aCam;
      ListCameras (aCam);
      __android_log_print (ANDROID_LOG_INFO, "CAPTURE", "cameras: %d", static_cast<int> (aCam.size ()));
      for (size_t i = 0; i < aCam.size (); i++)
         __android_log_print (ANDROID_LOG_INFO, "CAPTURE", "  [%d] %s source=%d position=%d", static_cast<int> (i), aCam[i].sName.c_str (), aCam[i].nSource, aCam[i].nPosition);
   }
   else
      Log ("ACameraManager_create failed");
   return bResult;
}

void CAPTURE_PLATFORM::Shutdown ()
{
   {
      std::lock_guard<std::mutex> lock (s_mxThread);
      s_bStop = true;
      if (s_pLooper)
         ALooper_wake (s_pLooper);
   }
   if (s_thCamera.joinable ())
      s_thCamera.join ();

   if (s_pManager)
   {
      ACameraManager_delete (s_pManager);
      s_pManager = nullptr;
   }
}

void CAPTURE_PLATFORM::Enumerate (std::vector<INFO>& aInfo)
{
   aInfo.clear ();
   std::vector<CAM> aCam;
   ListCameras (aCam);
   for (const CAM& cam : aCam)
   {
      INFO info;
      info.sName = cam.sName;
      aInfo.push_back (info);
   }
}

uint32_t CAPTURE_PLATFORM::Open (int nIndex)
{
   JOB job;
   job.nOpenIndex = nIndex;
   Post (job);
   return job.nResult;
}

void CAPTURE_PLATFORM::Close (uint32_t nHandle)
{
   JOB job;
   job.nCloseHandle = nHandle;
   Post (job);
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

bool CAPTURE_PLATFORM::Intrinsics (uint32_t nHandle, CAPTURE::INTRINSICS& Pinhole)
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
      if (CAPTURE_PINHOLE::Valid (pDevice->Intrinsics))
      {
         Pinhole = pDevice->Intrinsics;
         bResult = true;
      }
      else if (CAPTURE_PINHOLE::From_Size (Pinhole, pDevice->nWidth, pDevice->nHeight))
      {
         pDevice->Intrinsics = Pinhole;
         bResult = true;
      }
   }
   return bResult;
}
