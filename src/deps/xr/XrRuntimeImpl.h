// Copyright 2026 Metaversal Corporation
// Internal — not part of the public Sneeze embedding API.

#ifndef SNEEZE_XR_RUNTIME_IMPL_H
#define SNEEZE_XR_RUNTIME_IMPL_H

#include <Sneeze.h>
#include "xr/XrRuntime.h"

#include <openxr/openxr.h>

#include <mutex>
#include <string>

namespace SNEEZE { namespace DEP {

class XR_RUNTIME::Impl
{
public:
   ENGINE*     m_pEngine    = nullptr;
   XrInstance  hInstance    = XR_NULL_HANDLE;
   bool        bHasRuntime  = false;
   std::string sRuntimeName;
   std::string sAvatarBindId;
   std::string sBoundMorphWeightsJson;

   mutable std::mutex mutex;
   XR_FACE_STATE faceCached {};
   XR_BODY_STATE bodyCached {};
   bool bFaceFixture = false;
   bool bBodyFixture = false;

   XR_CAPABILITIES caps {};

   // Opaque Android session state (XrRuntime_Android.cpp).
   void* pAndroidSession = nullptr;
};

bool XrAndroid_BeginSession (XR_RUNTIME::Impl* impl, void* pJavaVM, void* pActivity, void* pNativeWindow);
void XrAndroid_EndSession (XR_RUNTIME::Impl* impl);
bool XrAndroid_PumpTracking (XR_RUNTIME::Impl* impl);

}} // namespace SNEEZE::DEP

#endif
