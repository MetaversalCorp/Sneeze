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

using namespace SNEEZE::DEP;

WASM_RUNTIME::WASM_RUNTIME (SNEEZE::ENGINE* pEngine) : 
   m_pEngine (pEngine),
   m_pWsam_Engine (nullptr),
   m_pTimers (nullptr)
{
}

bool WASM_RUNTIME::Initialize ()
{
   bool bResult = false;

   m_pWsam_Engine = wasm_engine_new ();

   if (m_pWsam_Engine)
   {
      m_pTimers = new WASM_TIMERS (m_pEngine);

      m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "WASM_RUNTIME", "Wasmtime " + std::string (WASMTIME_VERSION) + " initialized");
      bResult = true;
   }
   else
      m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_RUNTIME", "Failed to create Wasmtime engine");

   return bResult;
}

WASM_RUNTIME::~WASM_RUNTIME ()
{
   // The TIMER agents (in CONTROL) are already torn down by this point, so no
   // fire is in flight; any stores left here are dropped without a drain.

   for (auto* pStore : m_apStore)
      delete pStore;
   m_apStore.clear ();

   delete m_pTimers;
   m_pTimers = nullptr;

   if (m_pWsam_Engine)
   {
      wasm_engine_delete (m_pWsam_Engine);
      m_pWsam_Engine = nullptr;
   }
}

// ---------------------------------------------------------------------------
// Store lifecycle
// ---------------------------------------------------------------------------

WASM_STORE* WASM_RUNTIME::Store_Open ()
{
   std::lock_guard<std::mutex> guard (m_mxStore);

   WASM_STORE* pStore = new WASM_STORE (m_pEngine, m_pWsam_Engine);

   m_apStore.push_back (pStore);

   return pStore;
}

void WASM_RUNTIME::Store_Close (WASM_STORE* pStore)
{
   std::lock_guard<std::mutex> guard (m_mxStore);

   // Cancel and drain this store's timers first: after this returns no TIMER
   // agent holds or will claim an entry for it, so the delete below is safe.
   if (m_pTimers)
      m_pTimers->Store_Close (pStore);

   for (auto it = m_apStore.begin (); it != m_apStore.end (); ++it)
   {
      if (*it == pStore)
      {
         m_apStore.erase (it);
         break;
      }
   }

   delete pStore;
}
