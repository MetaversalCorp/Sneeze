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

// ---------------------------------------------------------------------------
// Chrono / Performance / Timer host-side suite. These exercise the engine
// logic behind the guest CHRONO / PERFORMANCE / TIMER facades directly,
// without a WASM store: the civil (calendar) fills that back a MOMENT, the
// monotonic clock, and the WASM_TIMERS scheduler (arm / claim / complete /
// clear / drain). The guest SDKs only marshal to these functions, so proving
// them here proves the substance the Rust SDK depends on.
// ---------------------------------------------------------------------------

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sneeze_abi.h>

// Wasm.h leans on the engine's precompiled header for the standard-library
// includes above and a forward declaration of the engine. In a test TU there
// is no such PCH, so supply the forward declaration before including it.
namespace SNEEZE { class ENGINE; }

#include "wasm/Chrono.h"
#include "wasm/Wasm.h"

using namespace SNEEZE::DEP;

static int nPassed = 0;
static int nFailed = 0;

static void Check (bool bCondition, const char* szName)
{
   if (bCondition)
   {
      std::printf ("  PASS: %s\n", szName);
      nPassed++;
   }
   else
   {
      std::printf ("  FAIL: %s\n", szName);
      nFailed++;
   }
}

static void Sleep_Ms (int nMs)
{
   std::this_thread::sleep_for (std::chrono::milliseconds (nMs));
}

// ---------------------------------------------------------------------------
// Test 1: Wall clock scalars (Chrono_Time / Chrono_Date)
// ---------------------------------------------------------------------------

static void TestChronoClocks ()
{
   std::printf ("\n[Test 1] Wall clock scalars\n");

   int64_t tm = Chrono_Time ();
   int64_t dt = Chrono_Date ();

   Check (tm > 0, "Chrono_Time positive");
   Check (dt > 0, "Chrono_Date positive");

   // dt is Unix ms; any run of this suite is well past 2020 (1.5e12 ms) and
   // comfortably before 2100 (4.1e12 ms). A sane clock lands in that window.
   Check (dt > 1500000000000LL  &&  dt < 4100000000000LL, "Chrono_Date in a plausible year");

   // Reading twice never goes backward (real-time clock, second read is later).
   int64_t dt2 = Chrono_Date ();
   Check (dt2 >= dt, "Chrono_Date non-decreasing");
}

// ---------------------------------------------------------------------------
// Test 2: MOMENT from scalars (Chrono_Moment_Scalar) + the invalid sentinel
// ---------------------------------------------------------------------------

static void TestMomentScalar ()
{
   std::printf ("\n[Test 2] MOMENT from scalars\n");

   // A zeroed MOMENT is the invalid sentinel (bMonth == 0).
   SNEEZE_ABI_MOMENT mNull;
   std::memset (&mNull, 0, sizeof (mNull));
   Check (mNull.Utc.bMonth == 0, "zeroed MOMENT is the invalid sentinel");

   // The Unix epoch: dt = 0 -> 1970-01-01T00:00:00Z, a Thursday (weekday 4).
   SNEEZE_ABI_MOMENT mEpoch;
   Chrono_Moment_Scalar (mEpoch, 0, true);
   Check (mEpoch.dt == 0,             "dt round-trips (epoch)");
   Check (mEpoch.Utc.wYear == 1970,   "epoch year is 1970");
   Check (mEpoch.Utc.bMonth == 1,     "epoch month is January");
   Check (mEpoch.Utc.bDay == 1,       "epoch day is 1");
   Check (mEpoch.Utc.bHour == 0,      "epoch hour is 0");
   Check (mEpoch.Utc.bWeekday == 4,   "epoch weekday is Thursday");

   // Y2K midnight UTC: 946684800000 ms, a Saturday (weekday 6).
   const int64_t Y2K_MS = 946684800000LL;
   SNEEZE_ABI_MOMENT mY2K;
   Chrono_Moment_Scalar (mY2K, Y2K_MS, true);
   Check (mY2K.dt == Y2K_MS,          "dt round-trips (Y2K)");
   Check (mY2K.Utc.wYear == 2000,     "Y2K year is 2000");
   Check (mY2K.Utc.bMonth == 1,       "Y2K month is January");
   Check (mY2K.Utc.bDay == 1,         "Y2K day is 1");
   Check (mY2K.Utc.bWeekday == 6,     "Y2K weekday is Saturday");

   // tm <-> dt: whole seconds are exactly divisible by a 1/64 s tick
   // (1 s = 10,000,000 * 100 ns = 64 * 156250), so building from tm recovers dt.
   SNEEZE_ABI_MOMENT mFromTick;
   Chrono_Moment_Scalar (mFromTick, mY2K.tm, false);
   Check (mFromTick.dt == Y2K_MS,     "tm -> dt round-trip at whole seconds");
}

// ---------------------------------------------------------------------------
// Test 3: MOMENT from civil components (Chrono_Moment_Set)
// ---------------------------------------------------------------------------

static void TestMomentSet ()
{
   std::printf ("\n[Test 3] MOMENT from civil components\n");

   // A plain UTC construction round-trips its fields verbatim. 2026-07-30 is a
   // Thursday (weekday 4).
   SNEEZE_ABI_MOMENT m;
   bool bOk = Chrono_Moment_Set (m, kSNEEZE_ABI_CHRONO_ZONE_UTC, 2026, 7, 30, 14, 52, 0, 0);
   Check (bOk,                    "Set (UTC) succeeds");
   Check (m.Utc.wYear == 2026,    "year 2026");
   Check (m.Utc.bMonth == 7,      "month 7 (July)");
   Check (m.Utc.bDay == 30,       "day 30");
   Check (m.Utc.bHour == 14,      "hour 14");
   Check (m.Utc.bMinute == 52,    "minute 52");
   Check (m.Utc.bSecond == 0,     "second 0");
   Check (m.Utc.bWeekday == 4,    "weekday Thursday");

   // JS-style rollover: month 13 normalizes to January of the next year.
   SNEEZE_ABI_MOMENT mRoll;
   Chrono_Moment_Set (mRoll, kSNEEZE_ABI_CHRONO_ZONE_UTC, 2026, 13, 1, 0, 0, 0, 0);
   Check (mRoll.Utc.wYear == 2027,  "month 13 rolls the year to 2027");
   Check (mRoll.Utc.bMonth == 1,    "month 13 rolls to January");

   // dwFraction is the canonical 100 ns sub-second: 5 ms == 50000 units.
   SNEEZE_ABI_MOMENT mMilli;
   Chrono_Moment_Set (mMilli, kSNEEZE_ABI_CHRONO_ZONE_UTC, 2000, 1, 1, 0, 0, 0, 50000);
   Check (mMilli.Utc.dwFraction == 50000, "5 ms stored as 50000 (100 ns units)");

   // 20 ticks (1/64 s) == 20 * 156250 == 3,125,000 units, ~312 ms.
   SNEEZE_ABI_MOMENT mTick;
   Chrono_Moment_Set (mTick, kSNEEZE_ABI_CHRONO_ZONE_UTC, 2000, 1, 1, 0, 0, 0, 3125000);
   Check (mTick.Utc.dwFraction == 3125000, "20 ticks stored as 3125000 (100 ns units)");

   // A fraction >= 1 s carries into seconds: 1.005 s -> +1 s, 50000 remainder.
   SNEEZE_ABI_MOMENT mCarry;
   Chrono_Moment_Set (mCarry, kSNEEZE_ABI_CHRONO_ZONE_UTC, 2000, 1, 1, 0, 0, 0, 10050000);
   Check (mCarry.Utc.bSecond == 1,          "fraction >= 1 s carries into seconds");
   Check (mCarry.Utc.dwFraction == 50000,   "fraction remainder after carry");
}

// ---------------------------------------------------------------------------
// Test 4: Parse and Format
// ---------------------------------------------------------------------------

static void TestParseFormat ()
{
   std::printf ("\n[Test 4] Parse and Format\n");

   // A trailing 'Z' forces UTC regardless of the requested zone.
   SNEEZE_ABI_MOMENT m;
   bool bOk = Chrono_Moment_Parse (m, kSNEEZE_ABI_CHRONO_ZONE_LOCAL, "2000-01-01T00:00:00Z");
   Check (bOk,                    "Parse ISO 'Z' succeeds");
   Check (m.Utc.wYear == 2000,    "parsed year 2000");
   Check (m.Utc.bMonth == 1,      "parsed month 1");
   Check (m.Utc.bDay == 1,        "parsed day 1");
   Check (m.Utc.bHour == 0,       "parsed hour 0");

   // Fractional seconds: '.123' -> 1,230,000 units of 100 ns.
   SNEEZE_ABI_MOMENT mFrac;
   Chrono_Moment_Parse (mFrac, kSNEEZE_ABI_CHRONO_ZONE_UTC, "2000-01-01T00:00:00.123Z");
   Check (mFrac.Utc.dwFraction == 1230000, "parsed '.123' -> 1230000");

   // Garbage does not parse and leaves the invalid sentinel.
   SNEEZE_ABI_MOMENT mBad;
   bool bBad = Chrono_Moment_Parse (mBad, kSNEEZE_ABI_CHRONO_ZONE_UTC, "not a date");
   Check (!bBad,                  "Parse rejects garbage");
   Check (mBad.Utc.bMonth == 0,   "rejected parse leaves invalid sentinel");

   // Default (empty spec) UTC render is ISO-8601 with a 'Z'.
   std::string sIso = Chrono_Format (m, kSNEEZE_ABI_CHRONO_ZONE_UTC, "");
   Check (sIso == "2000-01-01T00:00:00Z", "default UTC format is ISO-8601 Z");

   // Sub-second shows as milliseconds when non-zero.
   std::string sMilli = Chrono_Format (mFrac, kSNEEZE_ABI_CHRONO_ZONE_UTC, "");
   Check (sMilli == "2000-01-01T00:00:00.123Z", "default format renders milliseconds");

   // A strftime spec passes through against the selected civil view.
   std::string sSpec = Chrono_Format (m, kSNEEZE_ABI_CHRONO_ZONE_UTC, "%Y-%m-%d");
   Check (sSpec == "2000-01-01", "strftime spec passthrough");
}

// ---------------------------------------------------------------------------
// Test 5: Monotonic clock (Performance)
// ---------------------------------------------------------------------------

static void TestPerformance ()
{
   std::printf ("\n[Test 5] Monotonic clock\n");

   // The origin is per fabric: capture it once, then measure Now against it.
   int64_t tmOrigin    = 0;
   int64_t ft100Origin = 0;
   Performance_Origin_Capture (tmOrigin, ft100Origin);

   int64_t n1 = Performance_Now (tmOrigin);
   Check (n1 >= 0, "Performance_Now non-negative");

   Sleep_Ms (10);

   int64_t n2 = Performance_Now (tmOrigin);
   Check (n2 >= n1, "Performance_Now non-decreasing");
   Check (n2 > n1,  "Performance_Now advances across a sleep");

   // The origin is a real wall-clock MOMENT captured at t0.
   SNEEZE_ABI_MOMENT mOrigin;
   Performance_Origin (ft100Origin, mOrigin);
   Check (mOrigin.Utc.bMonth != 0, "Performance_Origin fills a valid MOMENT");
}

// ---------------------------------------------------------------------------
// Test 6: Timer service - arm / claim / complete (one-shot)
// ---------------------------------------------------------------------------

// Fake store keys: WASM_TIMERS only ever compares the pointer identity of a
// store, never dereferences it, so opaque non-null values are enough here.
static WASM_STORE* const STORE_A = reinterpret_cast<WASM_STORE*> (0x100);
static WASM_STORE* const STORE_B = reinterpret_cast<WASM_STORE*> (0x200);

static void TestTimerOneShot ()
{
   std::printf ("\n[Test 6] Timer one-shot\n");

   WASM_TIMERS timers (nullptr);

   uint64_t twId = timers.Arm (STORE_A, 7, kSNEEZE_ABI_TIMER_UNIT_MS, 40, 0xBEEFULL, false);
   Check (twId != 0, "Arm returns a nonzero timer id");

   WASM_TIMERS::FIRE fire;
   Check (!timers.Claim (fire), "not due before its period elapses");

   Sleep_Ms (70);

   Check (timers.Claim (fire), "due after the period");
   Check (fire.twTimerIx == twId,        "fire carries the timer id");
   Check (fire.pStore == STORE_A,        "fire carries the store");
   Check (fire.twFabricIx == 7,          "fire carries the fabric index");
   Check (fire.qwParam == 0xBEEFULL,     "fire carries qwParam");

   timers.Complete (fire);
   Check (!timers.Claim (fire), "one-shot is gone after Complete");
}

// ---------------------------------------------------------------------------
// Test 7: Timer service - repeat
// ---------------------------------------------------------------------------

static void TestTimerRepeat ()
{
   std::printf ("\n[Test 7] Timer repeat\n");

   WASM_TIMERS timers (nullptr);

   uint64_t twId = timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 20, 0, true);
   Check (twId != 0, "Arm (repeat) returns a nonzero id");

   WASM_TIMERS::FIRE fire;

   Sleep_Ms (35);
   Check (timers.Claim (fire)  &&  fire.twTimerIx == twId, "repeat fires first time");
   timers.Complete (fire);

   Check (!timers.Claim (fire), "repeat is not immediately due again");

   Sleep_Ms (35);
   Check (timers.Claim (fire)  &&  fire.twTimerIx == twId, "repeat fires a second time");
   timers.Complete (fire);

   Check (timers.Clear (STORE_A, twId), "repeat can be cleared");
}

// ---------------------------------------------------------------------------
// Test 8: Timer service - units, rejection, clear, distinct ids
// ---------------------------------------------------------------------------

static void TestTimerUnitsAndClear ()
{
   std::printf ("\n[Test 8] Timer units, rejection, clear\n");

   WASM_TIMERS timers (nullptr);

   Check (timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_HZ,   1000, 0, false) != 0, "HZ arms");
   Check (timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_TICK, 1,    0, false) != 0, "TICK arms");

   Check (timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 0,  0, false) == 0, "non-positive value rejected");
   Check (timers.Arm (nullptr, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 10, 0, false) == 0, "null store rejected");

   uint64_t twA = timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 500, 0, false);
   uint64_t twB = timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 500, 0, false);
   Check (twA != twB, "distinct timers get distinct ids");

   Check (timers.Clear (STORE_A, twA),      "Clear an existing timer");
   Check (!timers.Clear (STORE_A, twA),     "Clear the same timer again fails");
   Check (!timers.Clear (STORE_A, 999999),  "Clear an unknown id fails");
}

// ---------------------------------------------------------------------------
// Test 9: Timer service - Store_Close drains only the target store
// ---------------------------------------------------------------------------

static void TestTimerStoreClose ()
{
   std::printf ("\n[Test 9] Timer store teardown\n");

   WASM_TIMERS timers (nullptr);

   timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 20, 0, true);
   timers.Arm (STORE_A, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 20, 0, true);
   timers.Arm (STORE_B, 0, kSNEEZE_ABI_TIMER_UNIT_MS, 20, 0, true);

   Sleep_Ms (35);

   timers.Store_Close (STORE_A);

   // Only STORE_B's timer survives the close; STORE_A's are gone.
   WASM_TIMERS::FIRE fire;
   bool bClaimed = timers.Claim (fire);
   Check (bClaimed  &&  fire.pStore == STORE_B, "Store_Close removed only the closed store");

   timers.Complete (fire);
}

// ---------------------------------------------------------------------------

int RunChronoTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Chrono / Performance / Timer Test Suite ===\n");

   nPassed = 0;
   nFailed = 0;

   TestChronoClocks ();
   TestMomentScalar ();
   TestMomentSet ();
   TestParseFormat ();
   TestPerformance ();
   TestTimerOneShot ();
   TestTimerRepeat ();
   TestTimerUnitsAndClear ();
   TestTimerStoreClose ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
