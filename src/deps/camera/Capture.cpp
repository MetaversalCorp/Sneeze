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

#include <Capture/Capture.h>
#include <Sneeze.h>

namespace
{
   void Capture_Log (Capture::eLOG eLevel, const char* szMessage, void* pUser)
   {
      SNEEZE::ENGINE* pEngine = static_cast<SNEEZE::ENGINE*> (pUser);
      if (pEngine  &&  szMessage)
      {
         SNEEZE::IENGINE::eLOGLEVEL eSneeze = SNEEZE::IENGINE::kLOGLEVEL_Info;
         if (eLevel == Capture::kLOG_Warning)
            eSneeze = SNEEZE::IENGINE::kLOGLEVEL_Warning;
         else if (eLevel == Capture::kLOG_Error)
            eSneeze = SNEEZE::IENGINE::kLOGLEVEL_Error;
         pEngine->Log (eSneeze, "CAPTURE", szMessage);
      }
   }

   void Copy_Intrinsics (const ::Capture::CAPTURE::INTRINSICS& Src, SNEEZE::DEP::CAPTURE::INTRINSICS& Dst)
   {
      Dst.eModel       = static_cast<SNEEZE::DEP::CAPTURE::eMODEL> (Src.eModel);
      Dst.nWidth       = Src.nWidth;
      Dst.nHeight      = Src.nHeight;
      Dst.eOrientation = static_cast<SNEEZE::DEP::CAPTURE::eORIENTATION> (Src.eOrientation);
      Dst.dFx          = Src.dFx;
      Dst.dFy          = Src.dFy;
      Dst.dCx          = Src.dCx;
      Dst.dCy          = Src.dCy;
   }
}

class SNEEZE::DEP::CAPTURE::Impl
{
public:
   explicit Impl (ENGINE* pEngine)
      : m_pEngine (pEngine)
      , m_Capture (Capture_Log, pEngine)
   {
   }

   ENGINE*            m_pEngine;
   ::Capture::CAPTURE m_Capture;
};

using namespace SNEEZE::DEP;

CAPTURE::CAPTURE (ENGINE* pEngine)
   : m_pImpl (new Impl (pEngine))
{
}

CAPTURE::~CAPTURE ()
{
   delete m_pImpl;
   m_pImpl = nullptr;
}

bool CAPTURE::Initialize ()
{
   bool bResult = false;

   if (m_pImpl)
      bResult = m_pImpl->m_Capture.Initialize ();

   return bResult;
}

int CAPTURE::Device_Count () const
{
   int nCount = 0;

   if (m_pImpl)
      nCount = m_pImpl->m_Capture.Device_Count ();

   return nCount;
}

std::string CAPTURE::Device_Name (int nIndex) const
{
   std::string sName;

   if (m_pImpl)
      sName = m_pImpl->m_Capture.Device_Name (nIndex);

   return sName;
}

bool CAPTURE::Device_Open (int nIndex)
{
   bool bResult = false;

   if (m_pImpl)
      bResult = m_pImpl->m_Capture.Device_Open (nIndex);

   return bResult;
}

void CAPTURE::Device_Close (int nIndex)
{
   if (m_pImpl)
      m_pImpl->m_Capture.Device_Close (nIndex);
}

bool CAPTURE::Device_IsOpen (int nIndex) const
{
   bool bOpen = false;

   if (m_pImpl)
      bOpen = m_pImpl->m_Capture.Device_IsOpen (nIndex);

   return bOpen;
}

const char* CAPTURE::Orientation_Name (eORIENTATION eOrientation)
{
   return ::Capture::CAPTURE::Orientation_Name (
      static_cast< ::Capture::CAPTURE::eORIENTATION> (eOrientation));
}

const char* CAPTURE::Model_Name (eMODEL eModel)
{
   return ::Capture::CAPTURE::Model_Name (
      static_cast< ::Capture::CAPTURE::eMODEL> (eModel));
}

bool CAPTURE::Device_Intrinsics (int nIndex, INTRINSICS& Intrinsics) const
{
   bool bResult = false;

   Intrinsics = INTRINSICS ();
   if (m_pImpl)
   {
      ::Capture::CAPTURE::INTRINSICS src;
      bResult = m_pImpl->m_Capture.Device_Intrinsics (nIndex, src);
      if (bResult)
         Copy_Intrinsics (src, Intrinsics);
   }

   return bResult;
}

bool CAPTURE::Frame_Latest (int nIndex, int& nWidth, int& nHeight, std::vector<uint8_t>& aRgba, uint64_t& nFrameIx)
{
   bool bResult = false;

   if (m_pImpl)
      bResult = m_pImpl->m_Capture.Frame_Latest (nIndex, nWidth, nHeight, aRgba, nFrameIx);
   else
   {
      nWidth   = 0;
      nHeight  = 0;
      nFrameIx = 0;
   }

   return bResult;
}
