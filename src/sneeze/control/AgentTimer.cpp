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

AGENT::TIMER::TIMER (POOL* pPool, int nAgentIz) : AGENT (pPool, nAgentIz)
{
}

AGENT::TIMER::~TIMER ()
{
   Join ();
}

void AGENT::TIMER::Main ()
{
   Ready ();

   Wait ([this] { return Tick (); });
}

// Drains every timer that is currently due, then reports shutdown to end the
// wait until the next metronome signal. Claim hands each agent a distinct
// in-flight entry, so several agents share the drain and never fire the same
// timer twice; the per-store lock inside Notify_Timer serializes same-store
// deliveries while different stores proceed in parallel.
bool AGENT::TIMER::Tick ()
{
   DEP::WASM_TIMERS* pTimers  = Engine ()->Wasm_Runtime ()->Timers ();

   DEP::WASM_TIMERS::FIRE fire;

   while (pTimers->Claim (fire))
   {
      fire.pStore->Notify_Timer (fire.twFabricIx, fire.twTimerIx, fire.qwParam);

      pTimers->Complete (fire);
   }

   return IsShutdown ();
}
