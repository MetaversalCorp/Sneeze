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

#include "Wasm.h"
#include "HostFunctions.h"

using namespace SNEEZE::DEP;

WASM_STORE::WASM_STORE (ENGINE* pEngine, wasm_engine_t* pWASM_Engine) : 
   m_pEngine (pEngine), 
   m_pWasmEngine (pWASM_Engine), 
   m_pStore (nullptr), 
   m_pLinker (nullptr), 
   m_pHostData (nullptr), 
   m_nFabricRefCount (0)
{
   m_pStore = wasmtime_store_new (pWASM_Engine, nullptr, nullptr);
   if (!m_pStore)
      m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_STORE", "Failed to create native store");
}

WASM_STORE::~WASM_STORE ()
{
   for (auto* pInstance : m_apInstances)
      delete pInstance;
   m_apInstances.clear ();

   if (m_pLinker)
   {
      wasmtime_linker_delete (m_pLinker);
      m_pLinker = nullptr;
   }

   if (m_pStore)
   {
      wasmtime_store_delete (m_pStore);
      m_pStore = nullptr;
   }
}

wasmtime_context_t* WASM_STORE::Context () const
{
   wasmtime_context_t* pContext = nullptr;

   if (m_pStore)
      pContext = wasmtime_store_context (m_pStore);

   return pContext;
}

int WASM_STORE::Fabric_AddRef ()
{
   std::lock_guard<std::mutex> guard (m_mutex);

   m_nFabricRefCount++;

   return m_nFabricRefCount;
}

int WASM_STORE::Fabric_ReleaseRef ()
{
   std::lock_guard<std::mutex> guard (m_mutex);

   if (m_nFabricRefCount > 0)
      m_nFabricRefCount--;

   return m_nFabricRefCount;
}

bool WASM_STORE::Instance_Open (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash, const uint8_t* pBytes, size_t nSize, const uint8_t* pParams, size_t nParamsSize)
{
   std::lock_guard<std::mutex> guard (m_mutex);

   bool bResult = false;

   WASM_INSTANCE* pInstance = nullptr;

   for (auto* pCandidate : m_apInstances)
   {
      if (pCandidate->Url () == sUrl  &&  pCandidate->Hash () == sHash)
         pInstance = pCandidate;
   }

   if (!pInstance)
   {
      pInstance = new WASM_INSTANCE (m_pEngine, this, sUrl, sHash);

      if (pInstance->Compile (m_pWasmEngine, pBytes, nSize)  &&  pInstance->Instantiate ())
         m_apInstances.push_back (pInstance);
      else
      {
         delete pInstance;
         pInstance = nullptr;
      }
   }

   if (pInstance)
      bResult = pInstance->Open (twFabricIx, pParams, nParamsSize);

   return bResult;
}

void WASM_STORE::Instance_Close (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash)
{
   std::lock_guard<std::mutex> guard (m_mutex);

   for (auto* pInstance : m_apInstances)
   {
      if (pInstance->Url () == sUrl  &&  pInstance->Hash () == sHash)
         pInstance->Close (twFabricIx);
   }
}

WASM_INSTANCE* WASM_STORE::Instance_Find (const std::string& sUrl, const std::string& sHash) const
{
   std::lock_guard<std::mutex> guard (m_mutex);

   WASM_INSTANCE* pResult = nullptr;

   for (auto* pInstance : m_apInstances)
   {
      if (pInstance->Url () == sUrl  &&  pInstance->Hash () == sHash)
         pResult = pInstance;
   }

   return pResult;
}

// ---------------------------------------------------------------------------
// Func_Register — helper to register a single host function with the linker.
// ---------------------------------------------------------------------------

bool WASM_STORE::Func_Register (const char* sModule, const char* sName, wasmtime_func_callback_t fnCallback, const wasm_valkind_t* aParams, size_t nParams, const wasm_valkind_t* aResults, size_t nResults)
{
   wasm_valtype_vec_t vecParams, vecResults;

   wasm_valtype_vec_new_uninitialized (&vecParams, nParams);
   for (size_t i = 0; i < nParams; i++)
      vecParams.data[i] = wasm_valtype_new (aParams[i]);

   wasm_valtype_vec_new_uninitialized (&vecResults, nResults);
   for (size_t i = 0; i < nResults; i++)
      vecResults.data[i] = wasm_valtype_new (aResults[i]);

   wasm_functype_t* pFuncType = wasm_functype_new (&vecParams, &vecResults);

   wasmtime_error_t* pError = wasmtime_linker_define_func (m_pLinker, sModule, strlen (sModule), sName, strlen (sName), pFuncType, fnCallback, this, nullptr);

   wasm_functype_delete (pFuncType);

   bool bResult = true;

   if (pError)
   {
      wasm_message_t msg;
      wasmtime_error_message (pError, &msg);
      m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_STORE", "Failed to register " + std::string (sModule) + "." + sName + ": " + std::string (msg.data, msg.size));
      wasm_byte_vec_delete (&msg);
      wasmtime_error_delete (pError);

      bResult = false;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Linker_Initialize — creates the linker and registers all host functions.
// ---------------------------------------------------------------------------

bool WASM_STORE::Linker_Initialize ()
{
   bool bResult = false;

   if (m_pLinker)
      bResult = true;
   else
   {
      m_pLinker = wasmtime_linker_new (m_pWasmEngine);

      if (m_pLinker)
      {
         int nCount = 0;

         // --- The single guest -> host entry point (module: "Sneeze") ---
         //
         // Every subsystem call the guest can make crosses through this one
         // import: Call (i32 packetOffset, i32 packetSize) -> i64. The engine
         // reads the packet header, routes on (wType, wMethod), and returns an
         // i64. A module compiled once keeps loading as the engine grows new
         // methods (new numbers, never new symbols). See sdk/include/sneeze.h.

         {
            wasm_valkind_t p[] = { WASM_I32, WASM_I32 };
            wasm_valkind_t r[] = { WASM_I64 };

            if (Func_Register ("Sneeze", "Call", SNEEZE::DEP::Call, p, 2, r, 1))
               nCount++;
         }

         m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "WASM_STORE", "Linker initialized (" + std::to_string (nCount) + " host functions registered)");

         bResult = true;
      }
      else
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_STORE", "Failed to create linker");
   }

   return bResult;
}
