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

#include "Control.h"

// ---------------------------------------------------------------------------
// Pool / agent configuration table
// ---------------------------------------------------------------------------

enum ePOOL
{
   kPOOL_COMPOSITOR = 0,
   kPOOL_SCRUB      = 1,
   kPOOL_FETCH      = 2,
   kPOOL_TIMER      = 3,
   kPOOL_C          = 4,
};

struct AGENT_INIT
{
   int                                                       nHertz;
   int                                                       nAgents;
   std::function<SNEEZE::POOL*  (SNEEZE::CONTROL*)>          fnCreate_Pool;
   std::function<SNEEZE::AGENT* (SNEEZE::POOL*, int)>        fnCreate_Agent;
};

static const std::vector<AGENT_INIT> aAgent_Init =
{
   {    0,  1, [] (SNEEZE::CONTROL* pControl)->SNEEZE::POOL* { return new SNEEZE::POOL_CYCLE                     (pControl); }, [] (SNEEZE::POOL* pPool, int nAgentIz)->SNEEZE::AGENT* { return new SNEEZE::AGENT::COMPOSITOR (pPool, nAgentIz); } },
   {    0,  2, [] (SNEEZE::CONTROL* pControl)->SNEEZE::POOL* { return new SNEEZE::POOL_QUEUE<SNEEZE::JOB_SCRUB*> (pControl); }, [] (SNEEZE::POOL* pPool, int nAgentIz)->SNEEZE::AGENT* { return new SNEEZE::AGENT::SCRUB      (pPool, nAgentIz); } },
   {    0, 16, [] (SNEEZE::CONTROL* pControl)->SNEEZE::POOL* { return new SNEEZE::POOL_QUEUE<SNEEZE::JOB_FETCH*> (pControl); }, [] (SNEEZE::POOL* pPool, int nAgentIz)->SNEEZE::AGENT* { return new SNEEZE::AGENT::FETCH      (pPool, nAgentIz); } },
   { 1000,  4, [] (SNEEZE::CONTROL* pControl)->SNEEZE::POOL* { return new SNEEZE::POOL                           (pControl); }, [] (SNEEZE::POOL* pPool, int nAgentIz)->SNEEZE::AGENT* { return new SNEEZE::AGENT::TIMER      (pPool, nAgentIz); } },
   {   30,  1, [] (SNEEZE::CONTROL* pControl)->SNEEZE::POOL* { return new SNEEZE::POOL                           (pControl); }, [] (SNEEZE::POOL* pPool, int nAgentIz)->SNEEZE::AGENT* { return new SNEEZE::AGENT::C          (pPool, nAgentIz); } },
};

/***********************************************************************************************************************************
**  CONTROL
***********************************************************************************************************************************/

SNEEZE::CONTROL::CONTROL (::SNEEZE::ENGINE* pEngine) : THREAD (),
   m_pEngine (pEngine)
{
}

SNEEZE::CONTROL::~CONTROL ()
{
   Join ();
}

bool SNEEZE::CONTROL::Initialize (int& nAgentCount)
{
   bool bResult = THREAD::Initialize ();

   nAgentCount = 0;

   for (auto* pPool : m_apPool)
      nAgentCount += static_cast<int> (pPool->m_apAgent.size ());

   return bResult;
}

SNEEZE::ENGINE* SNEEZE::CONTROL::Engine () const
{
   return m_pEngine;
}

// ---------------------------------------------------------------------------
// Public API -- delegates immediately to pool methods
// ---------------------------------------------------------------------------

SNEEZE::POOL_CYCLE& SNEEZE::CONTROL::Pool_Compositor ()
{
   return static_cast<POOL_CYCLE&> (*m_apPool[kPOOL_COMPOSITOR]);
}

SNEEZE::POOL_QUEUE<SNEEZE::JOB_SCRUB*>& SNEEZE::CONTROL::Pool_Scrub ()
{
   return static_cast<POOL_QUEUE<JOB_SCRUB*>&> (*m_apPool[kPOOL_SCRUB]);
}

SNEEZE::POOL_QUEUE<SNEEZE::JOB_FETCH*>& SNEEZE::CONTROL::Pool_Fetch ()
{
   return static_cast<POOL_QUEUE<JOB_FETCH*>&> (*m_apPool[kPOOL_FETCH]);
}

void SNEEZE::CONTROL::Queue_Post_Compositor (JOB_COMPOSITOR* pJob_Compositor)
{
   Pool_Compositor ().Post (pJob_Compositor);
}

void SNEEZE::CONTROL::Queue_Post_Scrub (JOB_SCRUB* pJob_Scrub)
{
   Pool_Scrub ().Post (pJob_Scrub);
}

void SNEEZE::CONTROL::Queue_Post_Fetch (JOB_FETCH* pJob_Fetch)
{
   Pool_Fetch ().Post (pJob_Fetch);
}

// ---------------------------------------------------------------------------
// Thread loop (metronome + pool scheduling)
// ---------------------------------------------------------------------------

void SNEEZE::CONTROL::Main ()
{
#ifdef _WIN32
   timeBeginPeriod (1);
#endif

   // --- Create pools and their agent threads ---

   bool bInitialized = true;

   for (const auto& Agent_Init : aAgent_Init)
   {
      POOL* pPool = Agent_Init.fnCreate_Pool (this);

      m_apPool.push_back (pPool);

      if (!pPool->Initialize (Agent_Init.nHertz, Agent_Init.nAgents, Agent_Init.fnCreate_Agent))
      {
         bInitialized = false;

         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "CONTROL", "Pool failed to initialize");

         break;
      }
   }

   Ready (bInitialized);

   // --- Metronome loop ---

   if (bInitialized)
   {
      auto tpOrigin = std::chrono::steady_clock::now ();
      int64_t nLastReport = 0;

      do
      {
         if (!IsShutdown ())
         {
            auto tpNow = std::chrono::steady_clock::now ();
            double dElapsed = std::chrono::duration<double> (tpNow - tpOrigin).count ();

            for (auto* pPool : m_apPool)
               pPool->Tick (dElapsed);

            Diagnostics (dElapsed, nLastReport);
         }
         else break;

         Wait (std::chrono::milliseconds (1));
      }
      while (true);
   }

   // --- Shut down and destroy pools ---

   for (auto* pPool : m_apPool)
      delete pPool;

   m_apPool.clear ();

#ifdef _WIN32
   timeEndPeriod (1);
#endif
}

void SNEEZE::CONTROL::Diagnostics (double dElapsed, int64_t& nLastReport)
{
   int64_t nCurrentSecond = static_cast<int64_t> (dElapsed);

   if (nCurrentSecond > nLastReport)
   {
      std::string sMetronome;
      for (int nPool = 0; nPool < static_cast<int> (m_apPool.size ()); nPool++)
      {
         POOL* pPool = m_apPool[nPool];

         if (pPool->Hertz () > 0)
         {
            sMetronome += "  [" + std::to_string (nPool) + "] " + std::to_string (pPool->SignalCount ()) + "/" + std::to_string (pPool->Hertz ()) + " Hz";

            pPool->SignalCount_Reset ();
         }
      }

      nLastReport = nCurrentSecond;

      // m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "METRONOME", sMetronome);
   }
}
