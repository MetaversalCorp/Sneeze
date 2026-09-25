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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/select.h>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace SNEEZE::DEP;

namespace
{
   struct BUFFER
   {
      void*  pStart  = MAP_FAILED;
      size_t nLength = 0;
   };

   struct DEVICE
   {
      int                  nFd      = -1;
      int                  nWidth   = 0;
      int                  nHeight  = 0;
      uint32_t             nFourcc  = 0;
      std::vector<BUFFER>  aBuffer;
      std::atomic<bool>    bStop    { false };
      std::thread          thCapture;
      std::mutex           mxFrame;
      std::vector<uint8_t> aRgba;
      uint64_t             nFrameIx = 0;
      CAPTURE::INTRINSICS     Intrinsics;
   };

   std::mutex                            s_mxMap;
   uint32_t                              s_nNext = 1;
   std::unordered_map<uint32_t, DEVICE*> s_umpDevice;

   std::vector<std::string> VideoPaths ()
   {
      std::vector<std::string> aPath;
      DIR* pDir = opendir ("/dev");
      if (pDir)
      {
         while (dirent* pEnt = readdir (pDir))
         {
            if (std::strncmp (pEnt->d_name, "video", 5) == 0)
               aPath.push_back (std::string ("/dev/") + pEnt->d_name);
         }
         closedir (pDir);
      }
      std::sort (aPath.begin (), aPath.end ());
      return aPath;
   }

   bool IsCaptureDevice (const std::string& sPath, std::string& sName)
   {
      bool bResult = false;
      int nFd = open (sPath.c_str (), O_RDWR | O_NONBLOCK);
      if (nFd >= 0)
      {
         v4l2_capability cap = {};
         if (ioctl (nFd, VIDIOC_QUERYCAP, &cap) == 0)
         {
            unsigned int nCaps = cap.capabilities;
            if (nCaps & V4L2_CAP_DEVICE_CAPS)
               nCaps = cap.device_caps;
            if (nCaps & V4L2_CAP_VIDEO_CAPTURE)
            {
               sName    = reinterpret_cast<const char*> (cap.card);
               bResult  = true;
            }
         }
         close (nFd);
      }
      return bResult;
   }

   void CaptureLoop (DEVICE* pDevice)
   {
      while (!pDevice->bStop.load ())
      {
         fd_set fds;
         FD_ZERO (&fds);
         FD_SET (pDevice->nFd, &fds);
         timeval tv;
         tv.tv_sec  = 0;
         tv.tv_usec = 200000;
         int nSel = select (pDevice->nFd + 1, &fds, nullptr, nullptr, &tv);
         if (nSel > 0)
         {
            v4l2_buffer buf = {};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl (pDevice->nFd, VIDIOC_DQBUF, &buf) == 0)
            {
               if (buf.index < pDevice->aBuffer.size ())
               {
                  const uint8_t* pSrc = static_cast<const uint8_t*> (pDevice->aBuffer[buf.index].pStart);
                  std::vector<uint8_t> aRgba;
                  if (pDevice->nFourcc == V4L2_PIX_FMT_YUYV)
                     CAPTURE_CONVERT::Yuy2 (pSrc, pDevice->nWidth, pDevice->nHeight, pDevice->nWidth * 2, aRgba);
                  else if (pDevice->nFourcc == V4L2_PIX_FMT_NV12)
                  {
                     const uint8_t* pUv = pSrc + static_cast<size_t> (pDevice->nWidth) * static_cast<size_t> (pDevice->nHeight);
                     CAPTURE_CONVERT::Nv12 (pSrc, pDevice->nWidth, pUv, pDevice->nWidth, pDevice->nWidth, pDevice->nHeight, aRgba);
                  }
                  else if (pDevice->nFourcc == V4L2_PIX_FMT_RGB24)
                     CAPTURE_CONVERT::Rgb (pSrc, pDevice->nWidth, pDevice->nHeight, pDevice->nWidth * 3, aRgba);
                  else if (pDevice->nFourcc == V4L2_PIX_FMT_BGR32)
                     CAPTURE_CONVERT::Bgra (pSrc, pDevice->nWidth, pDevice->nHeight, pDevice->nWidth * 4, aRgba);

                  if (!aRgba.empty ())
                  {
                     std::lock_guard<std::mutex> lock (pDevice->mxFrame);
                     pDevice->aRgba.swap (aRgba);
                     pDevice->nFrameIx++;
                  }
               }
               ioctl (pDevice->nFd, VIDIOC_QBUF, &buf);
            }
         }
      }
   }

   void DestroyDevice (DEVICE* pDevice)
   {
      if (pDevice)
      {
         pDevice->bStop.store (true);
         if (pDevice->thCapture.joinable ())
            pDevice->thCapture.join ();
         if (pDevice->nFd >= 0)
         {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl (pDevice->nFd, VIDIOC_STREAMOFF, &type);
            for (BUFFER& buf : pDevice->aBuffer)
            {
               if (buf.pStart != MAP_FAILED)
                  munmap (buf.pStart, buf.nLength);
            }
            close (pDevice->nFd);
         }
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
   std::vector<std::string> aPath = VideoPaths ();
   for (const std::string& sPath : aPath)
   {
      std::string sName;
      if (IsCaptureDevice (sPath, sName))
      {
         INFO info;
         info.sName = sName.empty () ? sPath : sName;
         aInfo.push_back (info);
      }
   }
}

uint32_t CAPTURE_PLATFORM::Open (int nIndex)
{
   uint32_t nHandle = 0;
   std::vector<std::string> aPath;
   std::vector<std::string> aAll = VideoPaths ();
   for (const std::string& sPath : aAll)
   {
      std::string sName;
      if (IsCaptureDevice (sPath, sName))
         aPath.push_back (sPath);
   }

   if (nIndex >= 0  &&  nIndex < static_cast<int> (aPath.size ()))
   {
      int nFd = open (aPath[nIndex].c_str (), O_RDWR | O_NONBLOCK);
      if (nFd >= 0)
      {
         v4l2_format fmt = {};
         fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
         ioctl (nFd, VIDIOC_G_FMT, &fmt);
         fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
         fmt.fmt.pix.width  = 640;
         fmt.fmt.pix.height = 480;
         if (ioctl (nFd, VIDIOC_S_FMT, &fmt) != 0)
         {
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
            if (ioctl (nFd, VIDIOC_S_FMT, &fmt) != 0)
            {
               fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
               ioctl (nFd, VIDIOC_S_FMT, &fmt);
            }
         }
         ioctl (nFd, VIDIOC_G_FMT, &fmt);

         const uint32_t nFourcc = fmt.fmt.pix.pixelformat;
         if (nFourcc != V4L2_PIX_FMT_YUYV  &&  nFourcc != V4L2_PIX_FMT_NV12  &&
             nFourcc != V4L2_PIX_FMT_RGB24  &&  nFourcc != V4L2_PIX_FMT_BGR32)
         {
            close (nFd);
            nFd = -1;
         }

         if (nFd >= 0)
         {
            DEVICE* pDevice = new DEVICE ();
            pDevice->nFd     = nFd;
            pDevice->nWidth  = static_cast<int> (fmt.fmt.pix.width);
            pDevice->nHeight = static_cast<int> (fmt.fmt.pix.height);
            pDevice->nFourcc = nFourcc;

            v4l2_requestbuffers req = {};
            req.count  = 4;
            req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            bool bOk = (ioctl (nFd, VIDIOC_REQBUFS, &req) == 0  &&  req.count >= 2);
            if (bOk)
            {
               pDevice->aBuffer.resize (req.count);
               for (unsigned int i = 0; i < req.count  &&  bOk; i++)
               {
                  v4l2_buffer buf = {};
                  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                  buf.memory = V4L2_MEMORY_MMAP;
                  buf.index  = i;
                  if (ioctl (nFd, VIDIOC_QUERYBUF, &buf) == 0)
                  {
                     pDevice->aBuffer[i].nLength = buf.length;
                     pDevice->aBuffer[i].pStart  = mmap (nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, nFd, buf.m.offset);
                     if (pDevice->aBuffer[i].pStart == MAP_FAILED)
                        bOk = false;
                     else
                        ioctl (nFd, VIDIOC_QBUF, &buf);
                  }
                  else
                     bOk = false;
               }
            }

            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (bOk  &&  ioctl (nFd, VIDIOC_STREAMON, &type) == 0)
            {
               pDevice->thCapture = std::thread (CaptureLoop, pDevice);
               CAPTURE_PINHOLE::From_Size (pDevice->Intrinsics, pDevice->nWidth, pDevice->nHeight);
               std::lock_guard<std::mutex> lock (s_mxMap);
               nHandle = s_nNext++;
               s_umpDevice.emplace (nHandle, pDevice);
            }
            else
            {
               pDevice->nFd = -1;
               DestroyDevice (pDevice);
               close (nFd);
            }
         }
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
