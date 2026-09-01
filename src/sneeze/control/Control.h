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

#ifndef SNEEZE_CORE_CONTROL_H
#define SNEEZE_CORE_CONTROL_H

#include "Engine.h"
#include <unordered_map>

namespace SNEEZE
{
   class AGENT;
   class CONTROL;

   class POOL;
   template <typename JOB_PTR> class POOL_QUEUE;
   class POOL_CYCLE;

   class IJOB;
   class JOB_SCRUB;

   // ========================================================================
   // IJOB -- base interface for all jobs submitted to agent pools.
   //
   // Owns: m_mxJob + m_bCancelled (Cancel / IsCancelled / Complete).
   // ========================================================================

   class IJOB
   {
   public:
      virtual ~IJOB () {}

      bool         IsCancelled () const;
      virtual void Cancel      ();
      virtual void Complete    ();

   protected:
      virtual void Complete_Deliver () = 0;

   private:
      mutable std::mutex m_mxJob;
      bool    m_bCancelled { false };
   };

   // ========================================================================
   // FETCH_RESULT -- plain struct returned by a completed fetch.
   // ========================================================================

   struct FETCH_RESULT
   {
      bool                             bSuccess;
      uint64_t                         nSizeBytes;
      long                             nHttpStatus;
      std::string                      sRemoteAddress;

      std::unordered_map<std::string, std::string> mapReqHeaders;
      std::unordered_map<std::string, std::string> mapRspHeaders;
   };

   // ========================================================================
   // JOB_FETCH -- fetch job. Heap-allocated, self-cleaning.
   //
   // Owns: concrete storage of URL, paths, hash, fetch completion callback.
   // Knows about: FETCH_RESULT (plain struct) for OnFetch_Complete only.
   // ========================================================================

   class JOB_FETCH : public IJOB
   {
   public:
      JOB_FETCH (bool bFetch, const std::string& sUrl, const std::string& sPath_Temp, const std::string& sPath_Data, const std::string& sHash, std::unordered_map<std::string, std::string>& mapReqHeaders);
      JOB_FETCH (bool bFetch);

      const std::string&                                    Url ()            const;
      const std::string&                                    Path_Temp ()      const;
      const std::string&                                    Path_Data ()      const;
      const std::string&                                    Hash ()           const;
      const std::unordered_map<std::string, std::string>&   RequestHeaders () const;

      bool IsFetch () const { return m_bFetch; }

      virtual void OnFetch_Complete (const FETCH_RESULT& result) {}

      void Result (const FETCH_RESULT& result);

   private:
      bool m_bFetch;
      std::string m_sUrl;
      std::string m_sPath_Temp;
      std::string m_sPath_Data;
      std::string m_sHash;
      std::unordered_map<std::string, std::string> m_mapReqHeaders;

      FETCH_RESULT m_ResultComplete;

      void Complete_Deliver () override;
   };

   // ========================================================================
   // JOB_SCRUB -- cleanup job. Heap-allocated, self-cleaning.
   //
   // Owns: concrete storage of path, cleanup completion callback.
   // Knows about: nothing else.
   // ========================================================================

   class JOB_SCRUB : public IJOB
   {
   public:
      explicit JOB_SCRUB (const std::string& sPath);

      const std::string& Path () const { return m_sPath; }

      virtual void OnScrub_Complete () {}

   private:
      std::string m_sPath;

      void Complete_Deliver () override;
   };

   // ========================================================================
   // JOB_COMPOSITOR -- perpetual viewport rendering job.
   //
   // Owns: state machine, frame mutex, destroy synchronization.
   // Lifecycle: Create -> Ready -> Setup/Render/Present (loop) -> Destroy.
   // Cancel() blocks until the compositor thread completes destruction.
   // ========================================================================

   class JOB_COMPOSITOR : public IJOB
   {
   public:
      enum eSTATE
      {
         kSTATE_CREATE,
         kSTATE_RENDER,
         kSTATE_PRESENT,
         kSTATE_DESTROY,
      };

      explicit JOB_COMPOSITOR (VIEWPORT* pViewport);

      eSTATE    State    () const;
      VIEWPORT* Viewport () const;

      void Return (eSTATE eState);

      bool Busy    ();
      void Idle    ();
      void Unlock  ();
      void Capture ();
      void Release ();

      void Cancel   () override;
      void Complete () override;

      int64_t                                   m_nLastFrame;
      float                                     m_dRenderScale;
      bool                                      m_bXrFramed;
      std::unordered_map<uint64_t, double>      m_mapExtent;

   protected:
      void Complete_Deliver () override;

   private:
      VIEWPORT*                   m_pViewport;
      eSTATE                      m_eState;
      bool                        m_bBusy;

      std::mutex                  m_mxJob;
      std::recursive_mutex        m_mxCancel;
      std::condition_variable_any m_cvCancel;
      bool                        m_bCancelled;
   };

   // ========================================================================
   // POOL -- owns agent lifecycle (create, initialize, shutdown, delete)
   //         and metronome tick scheduling.
   //
   // Owns: m_apAgent vector, metronome state, Tick() (signals all agents on tick).
   // Knows about: AGENT (base class only), CONTROL (pointer).
   // Does NOT know about: queues, jobs, busy flags.
   // ========================================================================

   class POOL
   {
   public:
      explicit POOL (CONTROL* pControl);
      virtual ~POOL ();

      bool Initialize (int nHertz, int nAgents, std::function<AGENT* (POOL*, int)> fnCreate);
      void Shutdown ();
      void Tick (double dElapsed);

      ::SNEEZE::ENGINE*  Engine () const;

      int  Hertz () const;
      int  SignalCount () const;
      void SignalCount_Reset ();

      CONTROL*             m_pControl;
      std::vector<AGENT*>  m_apAgent;

   private:
      int                  m_nHertz;
      int64_t              m_nLastTick;
      int                  m_nSignalCount;

      POOL            (const POOL&) = delete;
      POOL& operator= (const POOL&) = delete;
   };

   // ========================================================================
   // POOL_QUEUE -- adds a typed job queue on top of POOL.
   //
   // Owns: job queue, queue mutex, Post/Grab, targeted-wake logic.
   // Knows about: POOL::m_apAgent (inherited), AGENT (for Signal(), Busy()).
   // Does NOT know about: job contents, curl, paths, ASSET, ENGINE.
   //
   // Definitions in Pool_Queue.cpp; explicit instantiations for JOB_FETCH* and JOB_SCRUB* only.
   // ========================================================================

   template <typename JOB_PTR>
   class POOL_QUEUE : public POOL
   {
   public:
      using POOL::POOL;

      void Post (JOB_PTR pJob);
      bool Grab (JOB_PTR& pJob);

   private:
      mutable std::mutex      m_mxQueue;
      std::vector<JOB_PTR>    m_apJob;
   };

   // ========================================================================
   // POOL_CYCLE -- perpetual job pool for compositor jobs.
   //
   // Jobs remain in the pool until explicitly Remove()'d. Grab() selects
   // the best available job: Create/Destroy routed to agent 0 (Filament
   // thread affinity), Ready jobs round-robin by longest-ago-rendered.
   // ========================================================================

   class POOL_CYCLE : public POOL
   {
   public:
      using POOL::POOL;

      void Post    (JOB_COMPOSITOR*  pJob);
      bool Grab    (JOB_COMPOSITOR*& pJob, int nAgentIz);
      void Remove  (JOB_COMPOSITOR*  pJob);

   private:
      mutable std::mutex              m_mxCycle;
      std::vector<JOB_COMPOSITOR*>    m_apJob;
   };

   // ========================================================================
   // CONTROL -- owns the engine thread, pool lifecycle, and metronome.
   //
   // Owns: m_apPool vector, metronome loop, thin public API.
   // Does NOT: manage agents, queues, or busy flags directly.
   // ========================================================================

   class CONTROL : public THREAD
   {
   public:
      explicit CONTROL (::SNEEZE::ENGINE* pEngine);
      ~CONTROL ();

      bool Initialize (int& nAgentCount);

      void Queue_Post_Fetch      (JOB_FETCH* pJob_Fetch);
      void Queue_Post_Scrub      (JOB_SCRUB* pJob_Scrub);
      void Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor);

      ::SNEEZE::ENGINE* Engine () const;

      POOL_QUEUE<JOB_SCRUB*>& Pool_Scrub      ();
      POOL_QUEUE<JOB_FETCH*>& Pool_Fetch      ();
      POOL_CYCLE            & Pool_Compositor ();

      CONTROL (const CONTROL&) = delete;
      CONTROL& operator= (const CONTROL&) = delete;

   protected:
      void Main () override;

   private:
      void Diagnostics (double dElapsed, int64_t& nLastReport);

      ::SNEEZE::ENGINE*              m_pEngine;
      std::vector<POOL*>   m_apPool;
   };

   // ========================================================================
   // AGENT -- abstract base for engine agent threads
   // ========================================================================

   class AGENT : public THREAD
   {
   public:
      class COMPOSITOR;
      class SCRUB;
      class FETCH;
      class TIMER;
      class C;

      AGENT (POOL* pPool, int nAgentIz);
      virtual ~AGENT ();

      AGENT            (const AGENT&) = delete;
      AGENT& operator= (const AGENT&) = delete;

      bool Busy ();

   protected:
      void Main () override = 0;

      ::SNEEZE::ENGINE* Engine () const;

      POOL*                   m_pPool;
      int                     m_nAgentIz;
      std::atomic<bool>       m_bBusy;
   };

   // ========================================================================
   // COMPOSITOR -- drives the render loop (frame timing, camera, scene submit)
   // ========================================================================

   class AGENT::COMPOSITOR : public AGENT
   {
   public:
      COMPOSITOR (POOL* pPool, int nAgentIz);
      ~COMPOSITOR ();

   protected:
      void Main () override;

   private:
      bool Job ();
      void Execute_Create  (JOB_COMPOSITOR* pJob);
      void Execute_Render  (JOB_COMPOSITOR* pJob);
      void Execute_Present (JOB_COMPOSITOR* pJob);
      void Execute_Destroy (JOB_COMPOSITOR* pJob);
   };

   // ========================================================================
   // SCRUB -- disk cleanup (folder deletion, future cache pruning)
   // ========================================================================

   class AGENT::SCRUB : public AGENT
   {
   public:
      SCRUB (POOL* pPool, int nAgentIz);
      ~SCRUB ();

   protected:
      void Main () override;

   private:
      bool Job ();
   };

   // ========================================================================
   // FETCH -- network download agent (curl)
   //
   // Owns: curl download logic, temp file management, IsCancelled checks,
   //       Result(), Complete() (completion + delete).
   // Does NOT know about: NETWORK::ASSET, queue state, pool membership.
   // ========================================================================

   class AGENT::FETCH : public AGENT
   {
   public:
      FETCH (POOL* pPool, int nAgentIz);
      ~FETCH ();

   protected:
      void Main () override;

   private:
      bool Job ();
      void Execute (JOB_FETCH* pJob);
   };

   // ========================================================================
   // TIMER -- fires due guest timers via the WASM timer service
   //
   // Signal-driven: the metronome ticks the TIMER pool once per wake, waking
   // every agent. Each agent drains due entries (Claim -> Notify the store ->
   // Complete). Multiple agents run in parallel across stores; the per-store
   // lock inside Notify serializes same-store fires.
   // ========================================================================

   class AGENT::TIMER : public AGENT
   {
   public:
      TIMER (POOL* pPool, int nAgentIz);
      ~TIMER ();

   protected:
      void Main () override;

   private:
      bool Tick ();
   };

   // ========================================================================
   // C -- placeholder metronome agent
   // ========================================================================

   class AGENT::C : public AGENT
   {
   public:
      C (POOL* pPool, int nAgentIz);
      ~C ();

   protected:
      void Main () override;

   private:
      bool Tick ();
   };
}
#endif // SNEEZE_CORE_CONTROL_H
