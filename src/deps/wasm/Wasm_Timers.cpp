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

#include <sneeze_abi.h>

using namespace SNEEZE::DEP;

// 100 ns-free duration math: the smallest useful grain here is the 1/64 s tick.
static const int64_t NANOS_PER_TICK   = 15625000LL;    // 1/64 s
static const int64_t NANOS_PER_MS     = 1000000LL;     // 1 ms
static const int64_t NANOS_PER_SECOND = 1000000000LL;  // 1 s

// ---------------------------------------------------------------------------
// Period — the fire interval for a (unit, value) pair, as a steady_clock
// duration. TICK counts 1/64 s ticks; MS counts milliseconds; HZ is a
// frequency (period = 1/value s). A non-positive value is invalid and yields
// a zero duration (Arm rejects it).
// ---------------------------------------------------------------------------

static std::chrono::steady_clock::duration Period (int32_t eUnit, int32_t nValue)
{
   std::chrono::nanoseconds ns (0);

   if (nValue > 0)
   {
      switch (eUnit)
      {
         case kSNEEZE_ABI_TIMER_UNIT_TICK: ns = std::chrono::nanoseconds (static_cast<int64_t> (nValue) * NANOS_PER_TICK); break;
         case kSNEEZE_ABI_TIMER_UNIT_MS:   ns = std::chrono::nanoseconds (static_cast<int64_t> (nValue) * NANOS_PER_MS);   break;
         case kSNEEZE_ABI_TIMER_UNIT_HZ:   ns = std::chrono::nanoseconds (NANOS_PER_SECOND / static_cast<int64_t> (nValue)); break;
         default:                                                                                                          break;
      }
   }

   return std::chrono::duration_cast<std::chrono::steady_clock::duration> (ns);
}

// ===========================================================================
// WASM_TIMERS
// ===========================================================================

WASM_TIMERS::WASM_TIMERS (ENGINE* pEngine) :
   m_pEngine      (pEngine),
   m_twTimer_Next (1)
{
}

// ---------------------------------------------------------------------------
// Arm — insert a new timer entry, first fire one period from now. Returns a
// nonzero twTimerIx, or 0 on an invalid unit/value.
// ---------------------------------------------------------------------------

uint64_t WASM_TIMERS::Arm (WASM_STORE* pStore, uint64_t twFabricIx, int32_t eUnit, int32_t nValue, uint64_t qwParam, bool bRepeat)
{
   uint64_t twTimerIx = 0;

   std::chrono::steady_clock::duration dPeriod = Period (eUnit, nValue);

   if (pStore  &&  dPeriod.count () > 0)
   {
      std::lock_guard<std::mutex> guard (m_mxTimer);

      ENTRY Entry;
      Entry.twTimerIx  = m_twTimer_Next++;
      Entry.pStore     = pStore;
      Entry.twFabricIx = twFabricIx;
      Entry.qwParam    = qwParam;
      Entry.tpDue      = std::chrono::steady_clock::now () + dPeriod;
      Entry.dPeriod    = dPeriod;
      Entry.bRepeat    = bRepeat;
      Entry.bInFlight  = false;
      Entry.bCancel    = false;

      m_aEntry.push_back (Entry);

      twTimerIx = Entry.twTimerIx;
   }

   return twTimerIx;
}

// ---------------------------------------------------------------------------
// Clear — disarm a timer by (store, id). A not-in-flight entry is erased
// immediately; an in-flight one is flagged (bCancel) so Complete drops it
// instead of rescheduling.
// ---------------------------------------------------------------------------

bool WASM_TIMERS::Clear (WASM_STORE* pStore, uint64_t twTimerIx)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxTimer);

   for (size_t i = 0; i < m_aEntry.size ()  &&  !bResult; i++)
   {
      ENTRY& Entry = m_aEntry[i];

      if (Entry.pStore == pStore  &&  Entry.twTimerIx == twTimerIx)
      {
         if (Entry.bInFlight)
            Entry.bCancel = true;
         else
            m_aEntry.erase (m_aEntry.begin () + i);

         bResult = true;
      }
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Store_Close — cancel every timer for a store being torn down and block
// until no in-flight fire remains for it. After this returns, no timer agent
// holds or will claim any entry for pStore, so the store can be deleted.
// ---------------------------------------------------------------------------

void WASM_TIMERS::Store_Close (WASM_STORE* pStore)
{
   std::unique_lock<std::mutex> lock (m_mxTimer);

   bool bDrained = false;

   while (!bDrained)
   {
      bool bInFlight = false;

      for (size_t i = 0; i < m_aEntry.size (); )
      {
         ENTRY& Entry = m_aEntry[i];

         if (Entry.pStore == pStore)
         {
            Entry.bCancel = true;

            if (Entry.bInFlight)
            {
               bInFlight = true;
               i++;
            }
            else m_aEntry.erase (m_aEntry.begin () + i);
         }
         else i++;
      }

      if (bInFlight)
         m_cvTimer.wait (lock);
      else
         bDrained = true;
   }
}

// ---------------------------------------------------------------------------
// Claim — hand one due, not-in-flight, not-cancelled entry to an agent,
// marking it in-flight. Picks the earliest-due candidate so a backlog fires
// in order. Returns false when nothing is due.
//
// TODO(perf): per-store single-flight. This picks the earliest-due entry
// regardless of store, so two due timers of the SAME store can be claimed by
// two agents; since Notify_Timer serializes on the store lock, the second agent
// parks behind the first (committed to its claimed fire, unable to help other
// stores) until the first delivery completes. With 4 agents that is ~25% of
// drain capacity per blocked agent (up to ~75% if three pile on one store).
// Perf only — transient, same-store only, never a correctness bug. Fix: skip any
// store that already has an in-flight entry, so a second agent finds other work
// instead of blocking. No lock-side change needed. See src/deps/wasm/Wasm.md.
// ---------------------------------------------------------------------------

bool WASM_TIMERS::Claim (FIRE& fire)
{
   bool bResult = false;

   std::lock_guard<std::mutex> guard (m_mxTimer);

   std::chrono::steady_clock::time_point tpNow = std::chrono::steady_clock::now ();

   size_t nBest  = m_aEntry.size ();

   for (size_t i = 0; i < m_aEntry.size (); i++)
   {
      ENTRY& Entry = m_aEntry[i];

      if (!Entry.bInFlight  &&  !Entry.bCancel  &&  Entry.tpDue <= tpNow)
      {
         if (nBest == m_aEntry.size ()  ||  Entry.tpDue < m_aEntry[nBest].tpDue)
            nBest = i;
      }
   }

   if (nBest < m_aEntry.size ())
   {
      ENTRY& Entry = m_aEntry[nBest];

      Entry.bInFlight = true;

      fire.twTimerIx  = Entry.twTimerIx;
      fire.pStore     = Entry.pStore;
      fire.twFabricIx = Entry.twFabricIx;
      fire.qwParam    = Entry.qwParam;

      bResult = true;
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Complete — after an agent has fired an entry: drop it (one-shot, cleared, or
// its store closing) or reschedule it one period on (repeat). Clamps a lagged
// due forward so a slow fire never bursts a backlog. Wakes Store_Close, which
// may be draining this store.
// ---------------------------------------------------------------------------

void WASM_TIMERS::Complete (const FIRE& fire)
{
   std::lock_guard<std::mutex> guard (m_mxTimer);

   for (size_t i = 0; i < m_aEntry.size (); i++)
   {
      ENTRY& Entry = m_aEntry[i];

      if (Entry.pStore == fire.pStore  &&  Entry.twTimerIx == fire.twTimerIx)
      {
         if (Entry.bRepeat  &&  !Entry.bCancel)
         {
            std::chrono::steady_clock::time_point tpNow = std::chrono::steady_clock::now ();

            Entry.tpDue += Entry.dPeriod;

            if (Entry.tpDue <= tpNow)
               Entry.tpDue = tpNow + Entry.dPeriod;

            Entry.bInFlight = false;
         }
         else m_aEntry.erase (m_aEntry.begin () + i);

         break;
      }
   }

   m_cvTimer.notify_all ();
}
