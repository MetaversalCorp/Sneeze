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

using namespace SNEEZE;

// ===========================================================================
// POOL
// ===========================================================================

POOL::POOL (CONTROL* pControl) :
   m_pControl     (pControl),
   m_nHertz       (0),
   m_nLastTick    (0),
   m_nSignalCount (0)
{
}

bool POOL::Initialize (int nHertz, int nAgents, std::function<AGENT* (POOL*, int)> fnCreate)
{
   m_nHertz = nHertz;

   bool bResult = true;

   for (int nAgentIz = 0; nAgentIz < nAgents; nAgentIz++)
   {
      AGENT* pAgent = fnCreate (this, nAgentIz);

      m_apAgent.push_back (pAgent);

      if (!pAgent->Initialize ())
      {
         bResult = false;

         break;
      }
   }

   return bResult;
}

void POOL::Shutdown ()
{
   for (AGENT* pAgent : m_apAgent)
      pAgent->Signal (true);

   for (AGENT* pAgent : m_apAgent)
      delete pAgent;

   m_apAgent.clear ();
}

POOL::~POOL ()
{
   Shutdown ();
}

::SNEEZE::ENGINE* POOL::Engine () const
{
   return m_pControl->Engine ();
}

int  POOL::Hertz             () const { return m_nHertz; }
int  POOL::SignalCount       () const { return m_nSignalCount; }
void POOL::SignalCount_Reset ()       {        m_nSignalCount = 0; }

void POOL::Tick (double dElapsed)
{
   if (m_nHertz > 0)
   {
      int64_t nCurrentTick = static_cast<int64_t> (dElapsed * m_nHertz);

      if (nCurrentTick > m_nLastTick)
      {
         m_nLastTick = nCurrentTick;
         m_nSignalCount++;

         // we may want an option that says to wake only one agent per tick instead of them all

         for (AGENT* pAgent : m_apAgent)
            pAgent->Signal ();
      }
   }
}
