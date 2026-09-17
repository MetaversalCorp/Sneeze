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
      class WASM_NETWORK;

      // ---------------------------------------------------------------------------
      // Instance lifecycle states
      // ---------------------------------------------------------------------------

      enum INSTANCE_STATE
      {
         INSTANCE_STATE_DORMANT  = 0,
         INSTANCE_STATE_ACTIVE   = 1,
      };

      // ---------------------------------------------------------------------------
      // WASM_INSTANCE - a single compiled module instance within a WASM_STORE.
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
      // WASM_STORE - isolated execution context for one or more WASM instances.
      //
      // One store per CONTAINER - the CONTAINER owns uniqueness. WASM_RUNTIME
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

         // --- Network notification ---

         // Delivers one NETWORK event (a completed request, a socket message)
         // to every active instance. Same contract as Notify_Timer: the store
         // lock is held for the whole call, so a network agent never enters this
         // store's wasmtime context alongside a lifecycle call or a timer fire.
         // Every NETWORK notify payload is four u64 fields, so one builder
         // serves the whole subsystem.
         void           Notify_Network (uint16_t wMethod, uint64_t twFabricIx, uint64_t twHandle, uint64_t qwA, uint64_t qwB);

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
      // WASM_TIMERS - the engine-wide timer service backing the guest TIMER ABI.
      //
      // One instance, owned by WASM_RUNTIME. Guest Calls (on the store's thread)
      // Arm/Clear entries; the Control metronome signals the TIMER agent pool once
      // per wake, and those agents drive the fire path: Claim a due entry, drive
      // its store's Notify (per-store locked), then Complete (reschedule or drop).
      //
      // Concurrency: m_mxTimer guards the entry list only - it is never held while
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
      // WASM_NETWORK
      // ========================================================================

      // ---------------------------------------------------------------------------
      // WASM_NETWORK - the engine-wide service backing the guest NETWORK ABI.
      //
      // One instance, owned by WASM_RUNTIME, structurally parallel to WASM_TIMERS
      // and entirely separate from it. It owns the guest's request handles and
      // the queue of events waiting to be delivered into a guest.
      //
      // A guest request is an ordinary SNEEZE::FILE on the container's CACHE -
      // the same object an engine asset fetch produces, which is what makes it
      // visible to the inspector. WASM_NETWORK is only the guest's side of it:
      // the handle table, and the bridge from a fetch completion back into
      // wasmtime.
      //
      // Why the queue exists: a fetch completes on a FETCH agent, inside the
      // asset lock. Calling into wasmtime from there would enter a store from an
      // arbitrary thread while a lock is held. So a completion only enqueues an
      // event; the NETWORK agent pool drains the queue and does the delivery,
      // taking the store lock properly.
      //
      // Concurrency: m_mxNetwork is a LEAF lock. It is never held while calling
      // into a FILE, a CACHE, or a store's wasmtime context; the fetch machinery
      // in the other direction takes the asset lock and then this one, so the
      // order is always asset-then-network and never the reverse. Guest Calls
      // arrive with the store lock already held (a store is single-entrant),
      // which is what keeps a completion from being delivered to a guest in the
      // middle of that guest's own Send.
      //
      // The leaf rule is why a completed request is SNAPSHOT rather than read
      // through its FILE on demand: status, headers, and body are copied into
      // the entry on the FETCH agent (where the asset lock is legitimately held
      // and the data is known to be there), and every guest read is then a plain
      // table read. This is what XHR does too - responseText is a materialized
      // value, not a live view of a disk file.
      //
      // FILE ownership: WASM_NETWORK never owns a FILE. A CACHE does, and keeps
      // it after Close so the inspector can still read it. The one rule here is
      // that FILE::Close is only ever called once a fetch has landed - either
      // from the listener callback (the request was dropped while in flight) or
      // from the guest's own Close (the normal path). Closing mid-fetch would
      // race the guard ASSET holds across a completion.
      // ---------------------------------------------------------------------------

      class WASM_NETWORK
      {
      public:
         // The claimed snapshot an agent carries from Claim through Notify to
         // Complete. It is a value, not a view into a table entry, so a delivery
         // in flight never touches state the guest may be closing underneath it.
         struct EVENT
         {
            uint16_t                                        wMethod;
            WASM_STORE*                                     pStore;
            uint64_t                                        twFabricIx;
            uint64_t                                        twHandle;
            uint64_t                                        qwA;
            uint64_t                                        qwB;
         };

         // What a guest can read back off a completed request. Assembled under
         // the lock so the caller never holds a FILE pointer of its own.
         struct RESULT
         {
            int32_t                                         eState;
            long                                            nHttpStatus;
            uint64_t                                        nSizeBytes;
            bool                                            bCached;
            std::string                                     sUrl;
            std::string                                     sContentType;
            std::string                                     sError;
         };

         // What a guest can read back off a socket. Everything here is kept
         // current in the table as the socket reports, so reading it never
         // touches the live SOCKET.
         struct SOCKET_RESULT
         {
            int32_t                                         eState;
            std::string                                     sUrl;
            std::string                                     sProtocol;
            std::string                                     sError;
         };

         explicit WASM_NETWORK (ENGINE* pEngine);
         ~WASM_NETWORK ();

         // --- Requests (guest thread, inside a Call) ---

         uint64_t Request_Open        (WASM_STORE* pStore, uint64_t twFabricIx, eREQUEST_VERB eVerb, const std::string& sUrl, const std::string& sHash);
         bool     Request_Header_Set  (WASM_STORE* pStore, uint64_t twRequestIx, const std::string& sName, const std::string& sValue);
         bool     Request_Timeout_Set (WASM_STORE* pStore, uint64_t twRequestIx, int32_t nMilli);
         bool     Request_Send        (WASM_STORE* pStore, uint64_t twRequestIx, CACHE* pCache, const uint8_t* pBody, size_t nBody);
         bool     Request_Abort       (WASM_STORE* pStore, uint64_t twRequestIx);
         bool     Request_Close       (WASM_STORE* pStore, uint64_t twRequestIx);

         // --- Reading a request (guest thread, inside a Call) ---

         bool     Request_Result      (WASM_STORE* pStore, uint64_t twRequestIx, RESULT& Result) const;
         bool     Request_Body        (WASM_STORE* pStore, uint64_t twRequestIx, std::vector<uint8_t>& aBody) const;
         bool     Request_Header      (WASM_STORE* pStore, uint64_t twRequestIx, const std::string& sName, std::string& sValue) const;
         bool     Request_Headers     (WASM_STORE* pStore, uint64_t twRequestIx, std::string& sHeaders) const;

         // --- Sockets (guest thread, inside a Call) ---

         // The URL must be an absolute ws:// or wss:// URL; anything else fails.
         // Unlike a request there is no relative form to resolve, which is the
         // rule the browser's WebSocket constructor follows too.
         uint64_t Socket_Open         (WASM_STORE* pStore, uint64_t twFabricIx, CONTAINER* pContainer, const std::string& sUrl, const std::string& sProtocol);
         bool     Socket_Send         (WASM_STORE* pStore, uint64_t twSocketIx, const uint8_t* pData, size_t nSize, bool bBinary);
         bool     Socket_Close        (WASM_STORE* pStore, uint64_t twSocketIx, uint16_t wCode, const std::string& sReason);

         // The mirror of Open. Closes the connection if it is still up, then
         // retires the handle - Socket_Close alone leaves it readable.
         bool     Socket_Free         (WASM_STORE* pStore, uint64_t twSocketIx);

         // --- Reading a socket (guest thread, inside a Call) ---

         bool     Socket_Result       (WASM_STORE* pStore, uint64_t twSocketIx, SOCKET_RESULT& Result) const;
         uint64_t Socket_Buffered     (WASM_STORE* pStore, uint64_t twSocketIx) const;

         // The head of the receive queue. False means nothing was waiting.
         //
         // It is only popped when nCapacity can hold the whole message, so a
         // guest asking how big the head is (nCapacity 0) can ask again with a
         // buffer that fits, and a message is never half-delivered and lost.
         bool     Socket_Recv         (WASM_STORE* pStore, uint64_t twSocketIx, size_t nCapacity, std::vector<uint8_t>& aData, bool& bBinary);

         // --- Teardown ---

         // Drops every request a fabric opened. A fabric going away takes its
         // requests with it, exactly as a closing store does.
         void     Fabric_Close        (WASM_STORE* pStore, uint64_t twFabricIx);

         // Drops every request in a store and blocks until no delivery is in
         // flight for it, so WASM_RUNTIME can then delete the store. Runs before
         // the container closes its CACHE, so the FILEs are still valid.
         void     Store_Close         (WASM_STORE* pStore);

         // --- Delivery (NETWORK agents) ---

         bool     Claim               (EVENT& event);
         void     Complete            (const EVENT& event);

      private:
         // ---------------------------------------------------------------------
         // LISTENER - one per sent request, the FILE's IFILE. Self-deleting: the
         // FILE calls exactly one of OnFileReady/OnFileFailed per fetch, and that
         // call is where the listener retires. If its request was dropped while
         // in flight (m_bOrphan), it closes the FILE on the way out; otherwise
         // the guest's Close does that later.
         // ---------------------------------------------------------------------

         class LISTENER : public IFILE
         {
         public:
            LISTENER (WASM_NETWORK* pNetwork, uint64_t twRequestIx);

            void Orphan ();

            void OnFileReady  (SNEEZE::FILE* pFile) override;
            void OnFileFailed (SNEEZE::FILE* pFile) override;

         private:
            void Retire (SNEEZE::FILE* pFile, bool bSuccess);

            WASM_NETWORK*                                   m_pNetwork;
            uint64_t                                        m_twRequestIx;
            std::atomic<bool>                               m_bOrphan;
         };

         // ---------------------------------------------------------------------
         // SOCKET_LISTENER - one per socket, the SOCKET's ISOCKET. Unlike the
         // request LISTENER it is not self-deleting: a socket reports many times
         // over its life, so the listener lives as long as its entry does.
         //
         // NETWORK::Socket_Close is what makes deleting it safe - it promises no
         // callback is running or will run once it returns, so the listener is
         // always closed first and deleted second.
         // ---------------------------------------------------------------------

         class SOCKET_LISTENER : public ISOCKET
         {
         public:
            SOCKET_LISTENER (WASM_NETWORK* pNetwork, uint64_t twSocketIx);

            void OnSocketOpened  (SNEEZE::SOCKET* pSocket) override;
            void OnSocketMessage (SNEEZE::SOCKET* pSocket, const uint8_t* pData, size_t nSize, bool bBinary) override;
            void OnSocketFailed  (SNEEZE::SOCKET* pSocket) override;
            void OnSocketClosed  (SNEEZE::SOCKET* pSocket, uint16_t wCode, bool bClean) override;

         private:
            WASM_NETWORK*                                   m_pNetwork;
            uint64_t                                        m_twSocketIx;
         };

         struct ENTRY
         {
            uint64_t                                        twRequestIx;
            WASM_STORE*                                     pStore;
            uint64_t                                        twFabricIx;
            LISTENER*                                       pListener;
            SNEEZE::FILE*                                   pFile;
            REQUEST                                         Request;
            std::string                                     sUrl;
            std::string                                     sHash;
            int32_t                                         eState;
            bool                                            bAbort;

            // The snapshot, filled once by Request_Complete.
            long                                            nHttpStatus;
            uint64_t                                        nSizeBytes;
            bool                                            bCached;
            std::string                                     sUrl_Final;
            std::string                                     sContentType;
            std::string                                     sError;
            std::unordered_map<std::string, std::string>    umsRspHeader;
            std::vector<uint8_t>                            aBody;
         };

         // One message waiting for the guest to take it.
         struct MESSAGE
         {
            std::vector<uint8_t>                            aData;
            bool                                            bBinary;
         };

         // One per guest socket. The SOCKET itself belongs to NETWORK; this is
         // the guest's side of it - the handle, the receive queue, and the state
         // the socket last reported.
         struct SOCKET_ENTRY
         {
            uint64_t                                        twSocketIx;
            WASM_STORE*                                     pStore;
            uint64_t                                        twFabricIx;
            SNEEZE::SOCKET*                                 pSocket;
            SOCKET_LISTENER*                                pListener;
            int32_t                                         eState;
            std::string                                     sUrl;
            std::string                                     sProtocol;
            std::string                                     sError;

            std::vector<MESSAGE>                            aMessage;
            uint64_t                                        nQueued;
         };

         // Called by LISTENER from a FETCH agent. Takes the snapshot off the FILE
         // before locking, then records it and enqueues the guest's event. False
         // means the handle was gone, so the listener owns closing the FILE.
         bool     Request_Complete    (uint64_t twRequestIx, SNEEZE::FILE* pFile, bool bSuccess);

         // Called by SOCKET_LISTENER from the network's io thread. Each one
         // records what changed and enqueues the guest's event; none of them
         // touch a SOCKET, so the leaf rule holds in this direction too.
         void     Socket_Opened       (uint64_t twSocketIx, const std::string& sProtocol);
         void     Socket_Message      (uint64_t twSocketIx, const uint8_t* pData, size_t nSize, bool bBinary);
         void     Socket_Failed       (uint64_t twSocketIx, const std::string& sError);
         void     Socket_Closed       (uint64_t twSocketIx, uint16_t wCode, bool bClean);

         // Both return the index into m_aEntry, or m_aEntry.size() when there is
         // no such handle. Callers hold m_mxNetwork.
         size_t   Entry_Find          (WASM_STORE* pStore, uint64_t twRequestIx) const;
         size_t   Entry_Find          (uint64_t twRequestIx) const;

         // Removes one entry. A FILE that still needs closing is appended to
         // apClose for the caller to close after releasing the lock - closing it
         // here would break the leaf rule. Caller holds m_mxNetwork.
         void     Entry_Drop          (size_t nEntry, std::vector<SNEEZE::FILE*>& apClose);

         // The socket mirrors of the two above. A dropped socket hands its
         // SOCKET and its listener to the caller to retire outside the lock, in
         // that order, because closing the socket is what makes deleting the
         // listener safe. Callers hold m_mxNetwork.
         size_t   Socket_Find         (WASM_STORE* pStore, uint64_t twSocketIx) const;
         size_t   Socket_Find         (uint64_t twSocketIx) const;
         void     Socket_Drop         (size_t nSocket, std::vector<SOCKET_ENTRY>& aRetire);

         // Closes and deletes what Socket_Drop handed back. Runs with no lock
         // held.
         void     Socket_Retire       (std::vector<SOCKET_ENTRY>& aRetire);

         ENGINE*                                            m_pEngine;
         mutable std::mutex                                 m_mxNetwork;
         std::condition_variable                            m_cvNetwork;
         std::vector<ENTRY>                                 m_aEntry;
         std::vector<SOCKET_ENTRY>                          m_aSocket;
         std::vector<EVENT>                                 m_aEvent;
         uint64_t                                           m_twRequest_Next;
         uint64_t                                           m_twSocket_Next;
         int                                                m_nInFlight;

         WASM_NETWORK            (const WASM_NETWORK&) = delete;
         WASM_NETWORK& operator= (const WASM_NETWORK&) = delete;
      };

      // ========================================================================
      // WASM_RUNTIME
      // ========================================================================

      // ---------------------------------------------------------------------------
      // WASM_RUNTIME - top-level manager of the Wasmtime engine and all stores.
      //
      // Owns the shared wasm_engine_t. Stores are created/destroyed by
      // CONTAINER - one store per container, no identity lookup needed here.
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

         // The engine-wide network service (owned here; shared by every store).
         WASM_NETWORK*  Network    () const { return m_pNetwork; }

         // --- Store lifecycle ---

         WASM_STORE* Store_Open ();
         void        Store_Close (WASM_STORE* pStore);

      private:
         ENGINE*                                            m_pEngine;
         wasm_engine_t*                                     m_pWsam_Engine;
         WASM_TIMERS*                                       m_pTimers;
         WASM_NETWORK*                                      m_pNetwork;
         std::vector<WASM_STORE*>                           m_apStore;
         mutable std::mutex                                 m_mxStore;
      };

   } // namespace DEP
}

#endif // SNEEZE_WASM_H
