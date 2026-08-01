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
// Chrono - host-side wall clock, monotonic clock, and civil (calendar) logic
// backing the guest CHRONO / PERFORMANCE ABI. The engine owns every calendar
// computation: a single call fills a SNEEZE_ABI_MOMENT (both scalar forms plus
// the full UTC and local breakdowns) so the guest reads Year/Month/Day/... and
// tick/ms locally without crossing back. The sub-second is stored once as
// dwFraction (100 ns units); tick and ms are derived views. The wall clock is
// global (process-wide); the monotonic (PERFORMANCE) origin is per fabric - the
// FABRIC owns it, so these helpers just take/return the origin scalars.
// ---------------------------------------------------------------------------

#ifndef SNEEZE_DEP_CHRONO_H
#define SNEEZE_DEP_CHRONO_H

#include <cstdint>
#include <string>

#include <sneeze_abi.h>

namespace SNEEZE
{
namespace DEP
{

// Wall clock - UTC scalars off the OS real-time clock.
int64_t Chrono_Time ();                                          // 1/64 s since 1601-01-01 UTC
int64_t Chrono_Date ();                                          // Unix ms since 1970-01-01 UTC

// Monotonic clock - 100 ns units since a per-fabric origin (never wall time).
// The origin mirrors performance.timeOrigin: each FABRIC captures it once at
// load with Performance_Origin_Capture (stashing both scalars) and passes them
// back to Now/Origin. tmSteady is the monotonic count at the origin; ft100 is
// the wall anchor (100 ns since 1601 UTC). Only Now deltas are meaningful.
void    Performance_Origin_Capture (int64_t& tmSteady, int64_t& ft100);
int64_t Performance_Now            (int64_t tmSteadyOrigin);
void    Performance_Origin         (int64_t ft100Origin, SNEEZE_ABI_MOMENT& moment);   // wall MOMENT at the origin

// MOMENT fills - the host computes both scalars, both civil views, and the
// UTC offset in one shot. eZone is a kSNEEZE_ABI_CHRONO_ZONE_* value.
void        Chrono_Moment_Now    (SNEEZE_ABI_MOMENT& moment);
void        Chrono_Moment_Scalar (SNEEZE_ABI_MOMENT& moment, int64_t qwValue, bool bFromDate);
bool        Chrono_Moment_Set    (SNEEZE_ABI_MOMENT& moment, int32_t eZone, int32_t nYear, int32_t nMonth, int32_t nDay, int32_t nHour, int32_t nMinute, int32_t nSecond, int32_t nFraction);
bool        Chrono_Moment_Parse  (SNEEZE_ABI_MOMENT& moment, int32_t eZone, const std::string& sText);
std::string Chrono_Format        (const SNEEZE_ABI_MOMENT& moment, int32_t eZone, const std::string& sSpec);

} // namespace DEP
} // namespace SNEEZE

#endif // SNEEZE_DEP_CHRONO_H
