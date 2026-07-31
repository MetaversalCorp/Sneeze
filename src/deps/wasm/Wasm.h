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

#ifndef SNEEZE_WASM_H
#define SNEEZE_WASM_H

#include <wasmtime.h>

namespace SNEEZE
{
   namespace DEP
   {

      // ========================================================================
      // WASM_INSTANCE
      // ========================================================================

      class WASM_STORE;
      class WASM_TIMERS;

      // ---------------------------------------------------------------------------
      // Instance lifecycle states
      // ---------------------------------------------------------------------------

      enum INSTANCE_STATE
      {
         INSTANCE_STATE_DORMANT  = 0,
         INSTANCE_STATE_ACTIVE   = 1,
      };

      // ---------------------------------------------------------------------------
      // WASM_INSTANCE — a single compiled module instance within a WASM_STORE.
      //
      // Identity: URL + SHA256 (same bytecode from different URLs = different
      // instances). Lifecycle managed entirely through Open / Close:
      //   - Open:  increments refcount. First call (0->1) fires Initialize, then Open.
      //   - Close: fires Close, then decrements refcount. Last call (1->0)
      //            fires Finalize.
      //
      // Instances cannot be unloaded from a live store. When dormant they simply
      // stop receiving calls. Memory is freed only when the entire store is
      // destroyed.
      // ---------------------------------------------------------------------------

      class WASM_INSTANCE
      {
      public:
         WASM_INSTANCE (ENGINE* pEngine, WASM_STORE* pStore, const std::string& sUrl, const std::string& sHash);
         ~WASM_INSTANCE ();

         // --- Identity ---

         const std::string& Url   () const   { return m_sUrl; }
         const std::string& Hash  () const  { return m_sHash; }
         WASM_STORE*        Store () const  { return m_pStore; }
         INSTANCE_STATE     State () const  { return m_bState; }

         // --- Module compilation ---

         bool Compile (wasm_engine_t* pEngine, const uint8_t* pBytes, size_t nSize);
         bool IsCompiled () const      { return m_pModule != nullptr; }

         // --- Instantiation ---

         bool Instantiate ();
         bool IsInstantiated () const { return m_bInstantiated; }

         // --- Lifecycle ---

         int  RefCount () const { return m_nRefCount; }

         bool Open  (uint64_t twFabricIx, const uint8_t* pParams, size_t nParamsSize);
         bool Close (uint64_t twFabricIx);

         // Host -> guest event. Pushes a self-describing packet (header +
         // payload) into guest memory via the Alloc handshake and calls the
         // guest's Notify export. The caller must already hold the store lock
         // (a store's wasmtime context is single-entrant). No-op if the
         // instance is dormant or exports no Notify.
         bool Notify_Guest (const uint8_t* pPacket, size_t nSize);

      private:
         bool Initialize ();
         bool Finalize   ();

         bool Export_Lookup (const char* sName, wasmtime_func_t* pFunc, bool* pFound);

         // The Open snapshot push handshake. Alloc_Guest calls the guest's Alloc
         // export to reserve nSize bytes of guest linear memory (returns the guest
         // offset, or 0 on failure); Memory_Write copies host bytes into guest
         // memory at that offset; Free_Guest releases the block via the guest's
         // Free export. The engine never hands the guest a host pointer.
         int32_t Alloc_Guest  (int32_t nSize);
         void    Free_Guest   (int32_t nOffset, int32_t nSize);
         bool    Memory_Write (int32_t nOffset, const uint8_t* pBytes, size_t nSize);

         ENGINE*                                            m_pEngine;
         WASM_STORE*                                        m_pStore;
         std::string                                        m_sUrl;
         std::string                                        m_sHash;
         INSTANCE_STATE                                     m_bState;
         int                                                m_nRefCount;

         wasmtime_module_t*                                 m_pModule;
         bool                                               m_bInstantiated;
         wasmtime_instance_t                                m_wasmInstance;

         wasmtime_func_t                                    m_fnInit;
         wasmtime_func_t                                    m_fnShutdown;
         wasmtime_func_t                                    m_fnOpen;
         wasmtime_func_t                                    m_fnClose;

         // NOTE: OnTimer is NOT a WASM export we will use going forward. Timer
         // ticks (and every other host -> guest event) are delivered through the
         // single Notify callback instead. Kept only until existing call sites
         // are migrated off it.
         wasmtime_func_t                                    m_fnOnTimer;

         // The guest SDK's memory + event exports. Alloc/Free let the host
         // write into guest memory (the Open snapshot, event packets); Notify is
         // the host -> guest event entry point. Looked up now; exercised as the
         // snapshot push and node events land.
         wasmtime_func_t                                    m_fnAlloc;
         wasmtime_func_t                                    m_fnFree;
         wasmtime_func_t                                    m_fnNotify;

         bool                                               m_bHas_Init;
         bool                                               m_bHas_Shutdown;
         bool                                               m_bHas_Open;
         bool                                               m_bHas_Close;
         bool                                               m_bHas_OnTimer;
         bool                                               m_bHas_Alloc;
         bool                                               m_bHas_Free;
         bool                                               m_bHas_Notify;
      };

      // ========================================================================
      // WASM_STORE
      // ========================================================================

      // ---------------------------------------------------------------------------
      // WASM_STORE — isolated execution context for one or more WASM instances.
      //
      // One store per CONTAINER — the CONTAINER owns uniqueness. WASM_RUNTIME
      // creates stores on demand via Store_Open() and destroys them via
      // Store_Close(). The store owns instances, the linker, and host data.
      // ---------------------------------------------------------------------------

      class WASM_STORE
      {
      public:
         WASM_STORE (ENGINE* pEngine, wasm_engine_t* pWASM_Engine);
         ~WASM_STORE ();

         ENGINE*               Engine      () const   { return m_pEngine; }
         wasmtime_store_t*     NativeStore () const   { return m_pStore; }
         wasmtime_context_t*   Context     () const;

         // --- Fabric reference counting ---

         int  Fabric_AddRef    ();
         int  Fabric_ReleaseRef ();
         int  Fabric_RefCount   () const { return m_nFabricRefCount; }

         // --- Instance management ---

         bool           Instance_Open  (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash, const uint8_t* pBytes, size_t nSize, const uint8_t* pParams, size_t nParamsSize);
         void           Instance_Close (uint64_t twFabricIx, const std::string& sUrl, const std::string& sHash);
         WASM_INSTANCE* Instance_Find  (                     const std::string& sUrl, const std::string& sHash) const;
         const std::vector<WASM_INSTANCE*>& Instances () const { return m_apInstances; }

         // --- Timer notification ---

         // Delivers a TIMER_FIRED event to every active instance in the store.
         // Takes the store lock for the whole call (mutually exclusive with
         // Instance_Open/Close), so a timer agent never enters the store's
         // wasmtime context concurrently with a lifecycle call.
         void           Notify_Timer   (uint64_t twFabricIx, uint64_t twTimerIx, uint64_t qwParam);

         // --- Linker and host data ---

         bool                  Linker_Initialize ();
         wasmtime_linker_t*    Linker () const    { return m_pLinker; }

         void                  HostData (void* pData) { m_pHostData = pData; }
         void*                 HostData () const       { return m_pHostData; }

      private:
         bool                  Func_Register (const char* sModule, const char* sName, wasmtime_func_callback_t fnCallback, const wasm_valkind_t* aParams, size_t nParams, const wasm_valkind_t* aResults, size_t nResults);
         bool                  Wasi_Initialize ();

         ENGINE*                                            m_pEngine;
         wasm_engine_t*                                     m_pWasmEngine;
         wasmtime_store_t*                                  m_pStore;
         wasmtime_linker_t*                                 m_pLinker;
         void*                                              m_pHostData;
         int                                                m_nFabricRefCount;
         std::vector<WASM_INSTANCE*>                        m_apInstances;
         mutable std::mutex                                 m_mutex;
      };

      // ========================================================================
      // WASM_TIMERS
      // ========================================================================

      // ---------------------------------------------------------------------------
      // WASM_TIMERS — the engine-wide timer service backing the guest TIMER ABI.
      //
      // One instance, owned by WASM_RUNTIME. Guest Calls (on the store's thread)
      // Arm/Clear entries; the Control metronome signals the TIMER agent pool once
      // per wake, and those agents drive the fire path: Claim a due entry, drive
      // its store's Notify (per-store locked), then Complete (reschedule or drop).
      //
      // Concurrency: m_mxTimer guards the entry list only — it is never held while
      // a store's wasmtime context is entered (that is the store lock's job, taken
      // inside WASM_STORE::Notify_Timer). Claim marks an entry in-flight; Complete
      // clears/reschedules it. Store_Close cancels a store's entries and blocks
      // until any in-flight fire finishes, so WASM_RUNTIME can safely delete the
      // store afterwards with no timer agent still touching it.
      // ---------------------------------------------------------------------------

      class WASM_TIMERS
      {
      public:
         // The claimed snapshot an agent carries from Claim to Notify to Complete.
         struct FIRE
         {
            uint64_t                                        twTimerIx;
            WASM_STORE*                                     pStore;
            uint64_t                                        twFabricIx;
            uint64_t                                        qwParam;
         };

         explicit WASM_TIMERS (ENGINE* pEngine);

         // --- Arming (guest thread, inside a Call) ---

         uint64_t Arm         (WASM_STORE* pStore, uint64_t twFabricIx, int32_t eUnit, int32_t nValue, uint64_t qwParam, bool bRepeat);
         bool     Clear       (WASM_STORE* pStore, uint64_t twTimerIx);

         // --- Teardown (container/store close) ---

         void     Store_Close (WASM_STORE* pStore);

         // --- Fire path (TIMER agents) ---

         bool     Claim       (FIRE& fire);
         void     Complete    (const FIRE& fire);

      private:
         struct ENTRY
         {
            uint64_t                                        twTimerIx;
            WASM_STORE*                                     pStore;
            uint64_t                                        twFabricIx;
            uint64_t                                        qwParam;
            std::chrono::steady_clock::time_point           tpDue;
            std::chrono::steady_clock::duration             dPeriod;
            bool                                            bRepeat;
            bool                                            bInFlight;
            bool                                            bCancel;
         };

         ENGINE*                                            m_pEngine;
         std::mutex                                         m_mxTimer;
         std::condition_variable                            m_cvTimer;
         std::vector<ENTRY>                                 m_aEntry;
         uint64_t                                           m_twTimer_Next;

         WASM_TIMERS            (const WASM_TIMERS&) = delete;
         WASM_TIMERS& operator= (const WASM_TIMERS&) = delete;
      };

      // ========================================================================
      // WASM_RUNTIME
      // ========================================================================

      // ---------------------------------------------------------------------------
      // WASM_RUNTIME — top-level manager of the Wasmtime engine and all stores.
      //
      // Owns the shared wasm_engine_t. Stores are created/destroyed by
      // CONTAINER — one store per container, no identity lookup needed here.
      // ---------------------------------------------------------------------------

      class WASM_RUNTIME
      {
      public:
         WASM_RUNTIME (SNEEZE::ENGINE* pEngine);
         ~WASM_RUNTIME ();

         bool Initialize ();

         wasm_engine_t* WasmEngine () const { return m_pWsam_Engine; }

         // The engine-wide timer service (owned here; shared by every store).
         WASM_TIMERS*   Timers     () const { return m_pTimers; }

         // --- Store lifecycle ---

         WASM_STORE* Store_Open ();
         void        Store_Close (WASM_STORE* pStore);

      private:
         ENGINE*                                            m_pEngine;
         wasm_engine_t*                                     m_pWsam_Engine;
         WASM_TIMERS*                                       m_pTimers;
         std::vector<WASM_STORE*>                           m_apStore;
         mutable std::mutex                                 m_mxStore;
      };

   } // namespace DEP
}

#endif // SNEEZE_WASM_H
