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

#include "compute/ComputeDispatch.h"
#include "compute/EmbeddedKernels.h"

#include <vox/Vox.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace DEP
{

// ---------------------------------------------------------------------------
// CPU fallback: test_proximity kernel
// ---------------------------------------------------------------------------

static void CpuProximityKernel (
   BUFFER_BINDING* aBindings,
   uint32_t        nBindingCount,
   const void*     pPushConstants,
   size_t          nPushConstantSize,
   uint32_t        /*nGroupsX*/,
   uint32_t        /*nGroupsY*/,
   uint32_t        /*nGroupsZ*/
)
{
   struct PC
   {
      float    dX, dY, dZ, dW;
      uint32_t nCount;
   };

   if (nPushConstantSize < sizeof (PC))
      return;

   PC pPC;
   std::memcpy (&pPC, pPushConstants, sizeof (PC));

   const float* pPositions = nullptr;
   float*       pDistances = nullptr;

   for (uint32_t i = 0; i < nBindingCount; i++)
   {
      if (aBindings[i].nBinding == 0)
         pPositions = static_cast<const float*> (aBindings[i].pData);
      if (aBindings[i].nBinding == 1)
         pDistances = static_cast<float*> (aBindings[i].pData);
   }

   if (!pPositions || !pDistances)
      return;

   for (uint32_t i = 0; i < pPC.nCount; i++)
   {
      float dDx = pPositions[i * 4 + 0] - pPC.dX;
      float dDy = pPositions[i * 4 + 1] - pPC.dY;
      float dDz = pPositions[i * 4 + 2] - pPC.dZ;
      pDistances[i] = std::sqrt (dDx * dDx + dDy * dDy + dDz * dDz);
   }
}

// ---------------------------------------------------------------------------
// COMPUTE_DISPATCH
// ---------------------------------------------------------------------------

COMPUTE_DISPATCH::COMPUTE_DISPATCH (ANARIDevice /*pDevice*/)
   : m_pVoxDevice (nullptr)
{
   // Lazily probe for a GPU backend. Failure is fine - we fall back to
   // the CPU kernel registry. Vox picks Vulkan/DX12/Metal in priority
   // order via Backend::Auto and returns null if none are available.
   m_pVoxDevice = vox::DEVICE::Create (vox::Backend::Auto);

   RegisterCpuKernel ("TEST_PROXIMITY", CpuProximityKernel);
}

COMPUTE_DISPATCH::~COMPUTE_DISPATCH ()
{
   delete m_pVoxDevice;
   m_pVoxDevice = nullptr;
}

bool COMPUTE_DISPATCH::SupportsNativeCompute () const
{
   return m_pVoxDevice != nullptr;
}

void COMPUTE_DISPATCH::RegisterCpuKernel (const char* szName, CpuKernelFn fnKernel)
{
   m_aCpuKernels[szName] = fnKernel;
}

bool COMPUTE_DISPATCH::Dispatch (
   const char*     szKernelName,
   uint32_t        nGroupsX,
   uint32_t        nGroupsY,
   uint32_t        nGroupsZ,
   BUFFER_BINDING* aBindings,
   uint32_t        nBindingCount,
   const void*     pPushConstants,
   size_t          nPushConstantSize
)
{
   if (!szKernelName)
      return false;

   if (m_pVoxDevice)
   {
      KERNEL_DATA pKernel = GetEmbeddedKernel (szKernelName);
      if (pKernel.pBytes && pKernel.nSize > 0)
      {
         if (DispatchVox (pKernel.pBytes, pKernel.nSize,
                          nGroupsX, nGroupsY, nGroupsZ,
                          aBindings, nBindingCount,
                          pPushConstants, nPushConstantSize))
         {
            return true;
         }
         // Vox path failed (shader translation, kernel creation, etc.).
         // Fall through to the CPU registry so callers still get a
         // valid result when a CPU mirror exists.
      }
   }

   auto it = m_aCpuKernels.find (szKernelName);
   if (it != m_aCpuKernels.end ())
   {
      it->second (aBindings, nBindingCount,
         pPushConstants, nPushConstantSize,
         nGroupsX, nGroupsY, nGroupsZ);
      return true;
   }

   return false;
}

bool COMPUTE_DISPATCH::DispatchVox (
   const uint8_t*  pSpvBytes,
   size_t          nSpvSize,
   uint32_t        nGroupsX,
   uint32_t        nGroupsY,
   uint32_t        nGroupsZ,
   BUFFER_BINDING* aBindings,
   uint32_t        nBindingCount,
   const void*     pPushConstants,
   size_t          nPushConstantSize
)
{
   vox::KERNEL* pKernel = m_pVoxDevice->CreateKernel (pSpvBytes, nSpvSize, "main");
   if (!pKernel)
      return false;

   // Mirror each Sneeze BUFFER_BINDING into a Vox BUFFER, upload the
   // caller's bytes, then read back after dispatch for non-read-only
   // bindings. Everything is destroyed before we return so the caller's
   // memory remains authoritative.
   std::vector<vox::BUFFER*> aVoxBuffers (nBindingCount, nullptr);

   for (uint32_t i = 0; i < nBindingCount; i++)
   {
      vox::BUFFER_DESC desc;
      desc.nSize        = aBindings[i].nSize;
      desc.bHostVisible = true;

      aVoxBuffers[i] = m_pVoxDevice->CreateBuffer (desc);
      if (!aVoxBuffers[i])
      {
         for (uint32_t j = 0; j < i; j++)
            m_pVoxDevice->DestroyBuffer (aVoxBuffers[j]);
         m_pVoxDevice->DestroyKernel (pKernel);
         return false;
      }

      if (aBindings[i].pData && aBindings[i].nSize > 0)
         aVoxBuffers[i]->SetData (aBindings[i].pData, aBindings[i].nSize);
   }

   m_pVoxDevice->SetKernel (pKernel);

   for (uint32_t i = 0; i < nBindingCount; i++)
   {
      m_pVoxDevice->SetBuffer (aVoxBuffers[i], aBindings[i].nBinding,
                               aBindings[i].bReadOnly);
   }

   if (pPushConstants && nPushConstantSize > 0)
      m_pVoxDevice->SetPushConstants (pPushConstants, nPushConstantSize);

   vox::DISPATCH_ARGS args;
   args.nGroupsX = nGroupsX;
   args.nGroupsY = nGroupsY;
   args.nGroupsZ = nGroupsZ;
   m_pVoxDevice->Dispatch (args);
   m_pVoxDevice->Finish ();

   for (uint32_t i = 0; i < nBindingCount; i++)
   {
      if (!aBindings[i].bReadOnly && aBindings[i].pData && aBindings[i].nSize > 0)
         aVoxBuffers[i]->GetData (aBindings[i].pData, aBindings[i].nSize);
   }

   for (uint32_t i = 0; i < nBindingCount; i++)
      m_pVoxDevice->DestroyBuffer (aVoxBuffers[i]);

   m_pVoxDevice->DestroyKernel (pKernel);

   return true;
}

} // namespace DEP
