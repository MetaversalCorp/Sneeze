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
// build where SNEEZE_ENABLE_XR is OFF). Initialize succeeds but the
// runtime reports "no VR/AR runtime detected" through HasRuntime () so
// the rest of the engine can branch on that the same way it does for a
// real XR loader that couldn't find a runtime on the host.

#include <Sneeze.h>
#include "xr/XrRuntime.h"

namespace SNEEZE { namespace DEP {

class XR_RUNTIME::Impl
{
public:
   ENGINE* m_pEngine = nullptr;
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
bool XR_RUNTIME::WantsSession () const                { return false; }
bool XR_RUNTIME::PrepareGraphics ()                   { return false; }
const XR_RUNTIME::XR_VULKAN_CREATE* XR_RUNTIME::VulkanCreateHooks () const { return nullptr; }

bool XR_RUNTIME::BindGraphics (uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t)
{
   return false;
}

void XR_RUNTIME::UnbindGraphics () {}

bool XR_RUNTIME::HasSession () const       { return false; }
int  XR_RUNTIME::RecommendedWidth () const { return 0; }
int  XR_RUNTIME::RecommendedHeight () const{ return 0; }
bool XR_RUNTIME::WaitFrame ()              { return false; }
bool XR_RUNTIME::ShouldRender () const     { return false; }
bool XR_RUNTIME::BeginFrame ()             { return false; }
int  XR_RUNTIME::ViewCount () const        { return 0; }
bool XR_RUNTIME::AcquireView (int, XR_VIEW&) { return false; }
void XR_RUNTIME::ReleaseView (int)         {}
void XR_RUNTIME::EndFrame ()               {}
void XR_RUNTIME::SetChromePixels (const uint8_t*, int, int) {}
bool XR_RUNTIME::ConsumeUrlFocus ()        { return false; }

}} // namespace SNEEZE::DEP
