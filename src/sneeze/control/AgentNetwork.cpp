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

#include "wasm/Wasm.h"

using namespace SNEEZE;

AGENT::NETWORK::NETWORK (POOL* pPool, int nAgentIz) : AGENT (pPool, nAgentIz)
{
}

AGENT::NETWORK::~NETWORK ()
{
   Join ();
}

void AGENT::NETWORK::Main ()
{
   Ready ();

   Wait ([this] { return Tick (); });
}

// Drains every queued network event, then reports shutdown to end the wait
// until the next metronome signal. Claim hands each agent a distinct event, so
// several agents share the drain and never deliver the same event twice; the
// per-store lock inside Notify_Network serializes same-store deliveries while
// different stores proceed in parallel.
bool AGENT::NETWORK::Tick ()
{
   DEP::WASM_NETWORK* pNetwork = Engine ()->Wasm_Runtime ()->Network ();

   DEP::WASM_NETWORK::EVENT event;

   while (pNetwork->Claim (event))
   {
      event.pStore->Notify_Network (event.wMethod, event.twFabricIx, event.twHandle, event.qwA, event.qwB);

      pNetwork->Complete (event);
   }

   return IsShutdown ();
}
