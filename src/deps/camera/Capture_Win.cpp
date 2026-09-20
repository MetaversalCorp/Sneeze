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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfreadwrite.lib")
#pragma comment (lib, "mf.lib")
#pragma comment (lib, "mfuuid.lib")

using namespace SNEEZE::DEP;

namespace
{
   template <typename T>
   void SafeRelease (T*& p)
   {
      if (p)
      {
         p->Release ();
         p = nullptr;
      }
   }

   std::string WideToUtf8 (const WCHAR* pWide)
   {
      std::string s;
      if (pWide)
      {
         int nBytes = WideCharToMultiByte (CP_UTF8, 0, pWide, -1, nullptr, 0, nullptr, nullptr);
         if (nBytes > 1)
         {
            s.resize (static_cast<size_t> (nBytes - 1));
            WideCharToMultiByte (CP_UTF8, 0, pWide, -1, &s[0], nBytes, nullptr, nullptr);
         }
      }
      return s;
   }

   bool EnumActivates (IMFActivate*** pppActivate, UINT32* pCount)
   {
      bool bResult = false;
      IMFAttributes* pAttr = nullptr;
      *pppActivate = nullptr;
      *pCount      = 0;
      if (SUCCEEDED (MFCreateAttributes (&pAttr, 1)))
      {
         if (SUCCEEDED (pAttr->SetGUID (MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
         {
            if (SUCCEEDED (MFEnumDeviceSources (pAttr, pppActivate, pCount)))
               bResult = true;
         }
         SafeRelease (pAttr);
      }
      return bResult;
   }

   class DEVICE : public IMFSourceReaderCallback
   {
   public:
      DEVICE ()
         : m_nRef (1)
         , m_pReader (nullptr)
         , m_hFlush (nullptr)
         , m_bStop (false)
         , m_nWidth (0)
         , m_nHeight (0)
         , m_eFormat (kFORMAT_BGRA)
         , m_nFrameIx (0)
      {
         m_hFlush = CreateEventW (nullptr, TRUE, FALSE, nullptr);
      }

      ~DEVICE ()
      {
         Stop ();
         if (m_hFlush)
            CloseHandle (m_hFlush);
      }

      enum eFORMAT { kFORMAT_BGRA, kFORMAT_NV12, kFORMAT_YUY2 };

      HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
      {
         HRESULT hr = E_NOINTERFACE;
         if (ppv)
         {
            *ppv = nullptr;
            if (riid == IID_IUnknown  ||  riid == IID_IMFSourceReaderCallback)
            {
               *ppv = static_cast<IMFSourceReaderCallback*> (this);
               AddRef ();
               hr = S_OK;
            }
         }
         else
            hr = E_POINTER;
         return hr;
      }

      ULONG STDMETHODCALLTYPE AddRef () override
      {
         return static_cast<ULONG> (InterlockedIncrement (&m_nRef));
      }

      ULONG STDMETHODCALLTYPE Release () override
      {
         LONG nRef = InterlockedDecrement (&m_nRef);
         if (nRef == 0)
            delete this;
         return static_cast<ULONG> (nRef);
      }

      HRESULT STDMETHODCALLTYPE OnReadSample (HRESULT hrStatus, DWORD, DWORD, LONGLONG, IMFSample* pSample) override
      {
         std::lock_guard<std::mutex> lock (m_mxIo);
         if (!m_bStop.load ())
         {
            if (SUCCEEDED (hrStatus)  &&  pSample)
               Ingest (pSample);
            if (m_pReader  &&  !m_bStop.load ())
               m_pReader->ReadSample (MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
         }
         return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnFlush (DWORD) override
      {
         if (m_hFlush)
            SetEvent (m_hFlush);
         return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnEvent (DWORD, IMFMediaEvent*) override
      {
         return S_OK;
      }

      bool Start (IMFActivate* pActivate)
      {
         bool bResult = false;
         IMFMediaSource* pSource = nullptr;
         if (SUCCEEDED (pActivate->ActivateObject (IID_PPV_ARGS (&pSource)))  &&  pSource)
         {
            IMFAttributes* pAttr = nullptr;
            if (SUCCEEDED (MFCreateAttributes (&pAttr, 3)))
            {
               pAttr->SetUnknown (MF_SOURCE_READER_ASYNC_CALLBACK, this);
               pAttr->SetUINT32 (MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
               pAttr->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
               if (SUCCEEDED (MFCreateSourceReaderFromMediaSource (pSource, pAttr, &m_pReader))  &&  m_pReader)
               {
                  IMFMediaType* pType = nullptr;
                  if (SUCCEEDED (MFCreateMediaType (&pType)))
                  {
                     pType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
                     pType->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                     if (FAILED (m_pReader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType)))
                     {
                        pType->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_NV12);
                        if (FAILED (m_pReader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType)))
                        {
                           pType->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_YUY2);
                           m_pReader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
                        }
                     }
                     SafeRelease (pType);
                  }

                  IMFMediaType* pCurrent = nullptr;
                  if (SUCCEEDED (m_pReader->GetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent))  &&  pCurrent)
                  {
                     UINT32 nW = 0, nH = 0;
                     MFGetAttributeSize (pCurrent, MF_MT_FRAME_SIZE, &nW, &nH);
                     m_nWidth  = static_cast<int> (nW);
                     m_nHeight = static_cast<int> (nH);
                     GUID subtype = {};
                     pCurrent->GetGUID (MF_MT_SUBTYPE, &subtype);
                     if (subtype == MFVideoFormat_NV12)
                        m_eFormat = kFORMAT_NV12;
                     else if (subtype == MFVideoFormat_YUY2)
                        m_eFormat = kFORMAT_YUY2;
                     else
                        m_eFormat = kFORMAT_BGRA;
                     SafeRelease (pCurrent);
                  }

                  if (m_nWidth > 0  &&  m_nHeight > 0)
                  {
                     if (SUCCEEDED (m_pReader->ReadSample (MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr)))
                        bResult = true;
                  }
               }
               SafeRelease (pAttr);
            }
            SafeRelease (pSource);
         }
         return bResult;
      }

      void Stop ()
      {
         m_bStop.store (true);
         if (m_pReader)
         {
            if (m_hFlush)
               ResetEvent (m_hFlush);
            m_pReader->Flush (MF_SOURCE_READER_ALL_STREAMS);
            if (m_hFlush)
               WaitForSingleObject (m_hFlush, 2000);
            std::lock_guard<std::mutex> lock (m_mxIo);
            SafeRelease (m_pReader);
         }
      }

      bool Latest (CAPTURE_PLATFORM::FRAME& Frame)
      {
         bool bResult = false;
         std::lock_guard<std::mutex> lock (m_mxFrame);
         if (!m_aRgba.empty ())
         {
            Frame.nWidth   = m_nWidth;
            Frame.nHeight  = m_nHeight;
            Frame.nFrameIx = m_nFrameIx;
            Frame.aRgba    = m_aRgba;
            bResult        = true;
         }
         return bResult;
      }

   private:
      void Ingest (IMFSample* pSample)
      {
         IMFMediaBuffer* pBuffer = nullptr;
         if (SUCCEEDED (pSample->ConvertToContiguousBuffer (&pBuffer))  &&  pBuffer)
         {
            BYTE* pData = nullptr;
            DWORD nMax = 0, nCur = 0;
            if (SUCCEEDED (pBuffer->Lock (&pData, &nMax, &nCur))  &&  pData)
            {
               DWORD nNeed = static_cast<DWORD> (m_nWidth) * static_cast<DWORD> (m_nHeight) * 4;
               if (m_eFormat == kFORMAT_NV12)
                  nNeed = static_cast<DWORD> (m_nWidth) * static_cast<DWORD> (m_nHeight) * 3 / 2;
               else if (m_eFormat == kFORMAT_YUY2)
                  nNeed = static_cast<DWORD> (m_nWidth) * static_cast<DWORD> (m_nHeight) * 2;

               std::vector<uint8_t> aRgba;
               if (nCur >= nNeed  &&  m_nWidth > 0  &&  m_nHeight > 0)
               {
                  if (m_eFormat == kFORMAT_NV12)
                  {
                     const uint8_t* pY  = pData;
                     const uint8_t* pUv = pData + static_cast<size_t> (m_nWidth) * static_cast<size_t> (m_nHeight);
                     CAPTURE_CONVERT::Nv12 (pY, m_nWidth, pUv, m_nWidth, m_nWidth, m_nHeight, aRgba);
                  }
                  else if (m_eFormat == kFORMAT_YUY2)
                     CAPTURE_CONVERT::Yuy2 (pData, m_nWidth, m_nHeight, m_nWidth * 2, aRgba);
                  else
                     CAPTURE_CONVERT::Bgra (pData, m_nWidth, m_nHeight, m_nWidth * 4, aRgba);
               }

               pBuffer->Unlock ();

               if (!aRgba.empty ())
               {
                  std::lock_guard<std::mutex> lock (m_mxFrame);
                  m_aRgba.swap (aRgba);
                  m_nFrameIx++;
               }
            }
            SafeRelease (pBuffer);
         }
      }

      LONG                 m_nRef;
      IMFSourceReader*     m_pReader;
      HANDLE               m_hFlush;
      std::atomic<bool>    m_bStop;
      int                  m_nWidth;
      int                  m_nHeight;
      eFORMAT              m_eFormat;
      std::mutex           m_mxIo;
      std::mutex           m_mxFrame;
      std::vector<uint8_t> m_aRgba;
      uint64_t             m_nFrameIx;
   };

   std::mutex                           s_mxMap;
   uint32_t                             s_nNext = 1;
   std::unordered_map<uint32_t, DEVICE*> s_umpDevice;
   bool                                 s_bMf     = false;
   bool                                 s_bCoInit = false;
}

bool CAPTURE_PLATFORM::Startup ()
{
   bool bResult = false;
   HRESULT hrCo = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
   if (SUCCEEDED (hrCo)  ||  hrCo == RPC_E_CHANGED_MODE)
   {
      s_bCoInit = SUCCEEDED (hrCo);
      if (SUCCEEDED (MFStartup (MF_VERSION)))
      {
         s_bMf   = true;
         bResult = true;
      }
   }
   return bResult;
}

void CAPTURE_PLATFORM::Shutdown ()
{
   {
      std::lock_guard<std::mutex> lock (s_mxMap);
      for (auto& pair : s_umpDevice)
      {
         if (pair.second)
         {
            pair.second->Stop ();
            pair.second->Release ();
         }
      }
      s_umpDevice.clear ();
   }
   if (s_bMf)
   {
      MFShutdown ();
      s_bMf = false;
   }
   if (s_bCoInit)
   {
      CoUninitialize ();
      s_bCoInit = false;
   }
}

void CAPTURE_PLATFORM::Enumerate (std::vector<INFO>& aInfo)
{
   aInfo.clear ();
   IMFActivate** ppActivate = nullptr;
   UINT32 nCount = 0;
   if (EnumActivates (&ppActivate, &nCount)  &&  ppActivate)
   {
      for (UINT32 i = 0; i < nCount; i++)
      {
         INFO info;
         WCHAR* pName = nullptr;
         if (ppActivate[i])
         {
            ppActivate[i]->GetAllocatedString (MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &pName, nullptr);
            info.sName = WideToUtf8 (pName);
            if (pName)
               CoTaskMemFree (pName);
            ppActivate[i]->Release ();
         }
         if (info.sName.empty ())
            info.sName = "Camera " + std::to_string (i);
         aInfo.push_back (info);
      }
      CoTaskMemFree (ppActivate);
   }
}

uint32_t CAPTURE_PLATFORM::Open (int nIndex)
{
   uint32_t nHandle = 0;
   IMFActivate** ppActivate = nullptr;
   UINT32 nCount = 0;
   if (nIndex >= 0  &&  EnumActivates (&ppActivate, &nCount)  &&  ppActivate)
   {
      if (static_cast<UINT32> (nIndex) < nCount  &&  ppActivate[nIndex])
      {
         DEVICE* pDevice = new DEVICE ();
         if (pDevice->Start (ppActivate[nIndex]))
         {
            std::lock_guard<std::mutex> lock (s_mxMap);
            nHandle = s_nNext++;
            s_umpDevice.emplace (nHandle, pDevice);
         }
         else
         {
            pDevice->Stop ();
            pDevice->Release ();
         }
      }
      for (UINT32 i = 0; i < nCount; i++)
      {
         if (ppActivate[i])
            ppActivate[i]->Release ();
      }
      CoTaskMemFree (ppActivate);
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
   if (pDevice)
   {
      pDevice->Stop ();
      pDevice->Release ();
   }
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
      bResult = pDevice->Latest (Frame);
   return bResult;
}
