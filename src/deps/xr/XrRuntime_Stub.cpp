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
//
// No-op XR_RUNTIME for platforms without OpenXR SDK (iOS today; any
// build where SNEEZE_ENABLE_XR is OFF).

#include <Sneeze.h>
#include "xr/XrRuntime.h"

namespace SNEEZE { namespace DEP {

class XR_RUNTIME::Impl
{
public:
   ENGINE* m_pEngine = nullptr;
   XR_FACE_STATE faceCached {};
   XR_BODY_STATE bodyCached {};
   bool bFaceFixture = false;
   bool bBodyFixture = false;
   XR_CAPABILITIES caps {};
   std::string sAvatarBindId;
};

XR_RUNTIME::XR_RUNTIME (ENGINE* pEngine) : m_pImpl (new Impl ()) { m_pImpl->m_pEngine = pEngine; }
XR_RUNTIME::~XR_RUNTIME ()                                       { delete m_pImpl; }

bool XR_RUNTIME::Initialize ()
{
   m_pImpl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
      "OpenXR support disabled at build time (no headset / runtime expected on this platform).");
   return true;
}

bool XR_RUNTIME::HasRuntime () const                  { return false; }
std::string XR_RUNTIME::GetRuntimeName () const       { return {};    }

XR_CAPABILITIES XR_RUNTIME::Capabilities () const
{
   XR_CAPABILITIES c = m_pImpl->caps;
   c.bFixtureMode = m_pImpl->bFaceFixture || m_pImpl->bBodyFixture;
   return c;
}

void XR_RUNTIME::RefreshExtensionProbe () {}

void XR_RUNTIME::InjectFaceFixture (const XR_FACE_STATE& face)
{
   m_pImpl->faceCached = face;
   m_pImpl->bFaceFixture = true;
}

void XR_RUNTIME::InjectBodyFixture (const XR_BODY_STATE& body)
{
   m_pImpl->bodyCached = body;
   m_pImpl->bBodyFixture = true;
}

void XR_RUNTIME::ClearFixtures ()
{
   m_pImpl->bFaceFixture = false;
   m_pImpl->bBodyFixture = false;
   m_pImpl->faceCached = {};
   m_pImpl->bodyCached = {};
}

bool XR_RUNTIME::PollFace (XR_FACE_STATE& outFace) const
{
   outFace = m_pImpl->faceCached;
   return outFace.bValid || outFace.bTracking || m_pImpl->bFaceFixture;
}

bool XR_RUNTIME::PollBody (XR_BODY_STATE& outBody) const
{
   outBody = m_pImpl->bodyCached;
   return outBody.bValid || m_pImpl->bBodyFixture;
}

bool XR_RUNTIME::BeginAndroidSession (void*, void*, void*) { return false; }
void XR_RUNTIME::EndAndroidSession () {}
bool XR_RUNTIME::PumpAndroidTracking () { return false; }

void XR_RUNTIME::SetAvatarBindId (const std::string& sId) { m_pImpl->sAvatarBindId = sId; }
std::string XR_RUNTIME::AvatarBindId () const { return m_pImpl->sAvatarBindId; }

}} // namespace SNEEZE::DEP
