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

#include "camera/Capture.h"
#include "camera/Capture_Platform.h"
#include "camera/Capture_Pinhole.h"

#include <Sneeze.h>

#include <mutex>
#include <unordered_map>

using namespace SNEEZE::DEP;

class SNEEZE::DEP::CAPTURE::Impl
{
public:
   struct SLOT
   {
      int      nRef    = 0;
      uint32_t nHandle = 0;
   };

   ENGINE*                                      m_pEngine;
   bool                                         m_bStarted;
   mutable std::mutex                           m_mxCapture;
   mutable std::vector<CAPTURE_PLATFORM::INFO>  m_aDevice;
   std::unordered_map<int, SLOT>                m_umpOpen;
};

CAPTURE::CAPTURE (ENGINE* pEngine)
   : m_pImpl (new Impl ())
{
   m_pImpl->m_pEngine  = pEngine;
   m_pImpl->m_bStarted = false;
}

CAPTURE::~CAPTURE ()
{
   if (m_pImpl)
   {
      for (auto& pair : m_pImpl->m_umpOpen)
      {
         if (pair.second.nHandle)
            CAPTURE_PLATFORM::Close (pair.second.nHandle);
      }
      m_pImpl->m_umpOpen.clear ();
      if (m_pImpl->m_bStarted)
         CAPTURE_PLATFORM::Shutdown ();
      delete m_pImpl;
      m_pImpl = nullptr;
   }
}

bool CAPTURE::Initialize ()
{
   bool bResult = false;

   if (m_pImpl)
   {
      bResult = CAPTURE_PLATFORM::Startup ();
      if (bResult)
      {
         m_pImpl->m_bStarted = true;
         std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
         CAPTURE_PLATFORM::Enumerate (m_pImpl->m_aDevice);
         if (m_pImpl->m_pEngine)
         {
            m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "CAPTURE",
               "Initialized (" + std::to_string (m_pImpl->m_aDevice.size ()) + " camera device(s))");
         }
      }
      else if (m_pImpl->m_pEngine)
      {
         m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "CAPTURE", "Platform startup failed");
      }
   }

   return bResult;
}

int CAPTURE::Device_Count () const
{
   int nCount = 0;

   if (m_pImpl)
   {
      std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
      if (m_pImpl->m_umpOpen.empty ())
         CAPTURE_PLATFORM::Enumerate (m_pImpl->m_aDevice);
      nCount = static_cast<int> (m_pImpl->m_aDevice.size ());
   }

   return nCount;
}

std::string CAPTURE::Device_Name (int nIndex) const
{
   std::string sName;

   if (m_pImpl)
   {
      std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
      if (nIndex >= 0  &&  nIndex < static_cast<int> (m_pImpl->m_aDevice.size ()))
         sName = m_pImpl->m_aDevice[nIndex].sName;
   }

   return sName;
}

bool CAPTURE::Device_Open (int nIndex)
{
   bool bResult = false;

   if (m_pImpl  &&  nIndex >= 0)
   {
      std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
      auto it = m_pImpl->m_umpOpen.find (nIndex);
      if (it != m_pImpl->m_umpOpen.end ())
      {
         it->second.nRef++;
         bResult = true;
      }
      else
      {
         uint32_t nHandle = CAPTURE_PLATFORM::Open (nIndex);
         if (nHandle != 0)
         {
            Impl::SLOT slot;
            slot.nRef    = 1;
            slot.nHandle = nHandle;
            m_pImpl->m_umpOpen.emplace (nIndex, slot);
            bResult = true;
            std::string sName;
            if (nIndex < static_cast<int> (m_pImpl->m_aDevice.size ()))
               sName = m_pImpl->m_aDevice[nIndex].sName;
            if (m_pImpl->m_pEngine)
            {
               m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "CAPTURE",
                  "Opened camera " + std::to_string (nIndex) + " (" + sName + ")");
            }
         }
         else if (m_pImpl->m_pEngine)
         {
            m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Warning, "CAPTURE",
               "Failed to open camera " + std::to_string (nIndex));
         }
      }
   }

   return bResult;
}

void CAPTURE::Device_Close (int nIndex)
{
   if (m_pImpl)
   {
      std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
      auto it = m_pImpl->m_umpOpen.find (nIndex);
      if (it != m_pImpl->m_umpOpen.end ())
      {
         it->second.nRef--;
         if (it->second.nRef <= 0)
         {
            CAPTURE_PLATFORM::Close (it->second.nHandle);
            m_pImpl->m_umpOpen.erase (it);
         }
      }
   }
}

bool CAPTURE::Device_IsOpen (int nIndex) const
{
   bool bOpen = false;

   if (m_pImpl)
   {
      std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
      bOpen = m_pImpl->m_umpOpen.find (nIndex) != m_pImpl->m_umpOpen.end ();
   }

   return bOpen;
}

bool CAPTURE::Device_Intrinsics (int nIndex, INTRINSICS& Intrinsics) const
{
   bool bResult = false;

   CAPTURE_PINHOLE::Clear (Intrinsics);

   if (m_pImpl)
   {
      uint32_t nHandle = 0;
      {
         std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
         auto it = m_pImpl->m_umpOpen.find (nIndex);
         if (it != m_pImpl->m_umpOpen.end ())
            nHandle = it->second.nHandle;
      }

      if (nHandle)
         bResult = CAPTURE_PLATFORM::Intrinsics (nHandle, Intrinsics);

      if (!bResult)
         CAPTURE_PINHOLE::Clear (Intrinsics);
   }

   return bResult;
}

bool CAPTURE::Frame_Latest (int nIndex, int& nWidth, int& nHeight, std::vector<uint8_t>& aRgba, uint64_t& nFrameIx)
{
   bool bResult = false;

   nWidth   = 0;
   nHeight  = 0;
   nFrameIx = 0;

   if (m_pImpl)
   {
      uint32_t nHandle = 0;
      {
         std::lock_guard<std::mutex> lock (m_pImpl->m_mxCapture);
         auto it = m_pImpl->m_umpOpen.find (nIndex);
         if (it != m_pImpl->m_umpOpen.end ())
            nHandle = it->second.nHandle;
      }

      if (nHandle)
      {
         CAPTURE_PLATFORM::FRAME frame;
         if (CAPTURE_PLATFORM::Latest (nHandle, frame)  &&  !frame.aRgba.empty ())
         {
            nWidth   = frame.nWidth;
            nHeight  = frame.nHeight;
            nFrameIx = frame.nFrameIx;
            aRgba    = std::move (frame.aRgba);
            bResult  = true;
         }
      }
   }

   return bResult;
}
