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

#include "Chrono.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ratio>

#ifdef _WIN32
   #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
   #endif
   #ifndef NOMINMAX
      #define NOMINMAX
   #endif
   #include <windows.h>
#endif

namespace SNEEZE
{
namespace DEP
{

static_assert (sizeof (SNEEZE_ABI_MOMENT) == SNEEZE_ABI_MOMENT_SIZE, "SNEEZE_ABI_MOMENT wire size drift");
static_assert (sizeof (SNEEZE_ABI_CIVIL)  == SNEEZE_ABI_CIVIL_SIZE,  "SNEEZE_ABI_CIVIL wire size drift");

// 100 ns intervals between 1601-01-01 and 1970-01-01 (the FILETIME/Unix gap).
static const int64_t EPOCH_DIFF_100NS = 116444736000000000LL;
static const int64_t HNS_PER_SECOND   = 10000000LL;             // 100 ns units per second
static const int64_t HNS_PER_TICK     = 156250LL;               // 100 ns per 1/64 s
static const int64_t HNS_PER_MS       = 10000LL;                // 100 ns per millisecond

// ---------------------------------------------------------------------------
// FileTime100 - the OS real-time clock as 100 ns units since 1601-01-01 UTC
// (Windows FILETIME's native grain). POSIX shifts the Unix epoch by the gap.
// ---------------------------------------------------------------------------

#ifdef _WIN32
static int64_t FileTime100 ()
{
   FILETIME        FileTime;
   ULARGE_INTEGER  u;

   GetSystemTimePreciseAsFileTime (&FileTime);
   u.LowPart  = FileTime.dwLowDateTime;
   u.HighPart = FileTime.dwHighDateTime;

   return static_cast<int64_t> (u.QuadPart);
}
#else
static int64_t FileTime100 ()
{
   struct timespec ts;

   clock_gettime (CLOCK_REALTIME, &ts);

   return static_cast<int64_t> (ts.tv_sec) * HNS_PER_SECOND + static_cast<int64_t> (ts.tv_nsec) / 100 + EPOCH_DIFF_100NS;
}
#endif

// ---------------------------------------------------------------------------
// Broken-down-time wrappers (thread-safe variants; per-OS spelling).
// ---------------------------------------------------------------------------

static void GmTime (time_t t, struct tm& out)
{
#ifdef _WIN32
   gmtime_s (&out, &t);
#else
   gmtime_r (&t, &out);
#endif
}

static void LocalTime (time_t t, struct tm& out)
{
#ifdef _WIN32
   localtime_s (&out, &t);
#else
   localtime_r (&t, &out);
#endif
}

static time_t TimeGm (struct tm* pTm)
{
#ifdef _WIN32
   return _mkgmtime (pTm);
#else
   return timegm (pTm);
#endif
}

// Minutes east of UTC for the local zone at instant t. POSIX reads tm_gmtoff
// directly; Windows reinterprets the UTC wall-clock fields as local time and
// measures the shift (tm_isdst = -1 lets mktime resolve DST for that date).
static int32_t LocalOffsetMinutes (time_t t, const struct tm& gtm, const struct tm& ltm)
{
   int32_t nResult = 0;

#ifdef _WIN32
   struct tm g = gtm;
   g.tm_isdst  = -1;
   time_t tg   = mktime (&g);

   (void) ltm;

   if (tg != static_cast<time_t> (-1))
      nResult = static_cast<int32_t> ((t - tg) / 60);
#else
   (void) t;
   (void) gtm;
   nResult = static_cast<int32_t> (ltm.tm_gmtoff / 60);
#endif

   return nResult;
}

// ---------------------------------------------------------------------------
// Civil fill - one broken-down struct tm -> SNEEZE_ABI_CIVIL (SYSTEMTIME
// conventions: 1-based month, 0-based Sunday weekday). dwFraction is the
// zone-invariant sub-second in 100 ns units.
// ---------------------------------------------------------------------------

static void FillCivil (const struct tm& t, uint32_t dwFraction, SNEEZE_ABI_CIVIL& Civil)
{
   Civil.wYear      = static_cast<int16_t> (t.tm_year + 1900);
   Civil.bMonth     = static_cast<uint8_t> (t.tm_mon + 1);
   Civil.bDay       = static_cast<uint8_t> (t.tm_mday);
   Civil.bWeekday   = static_cast<uint8_t> (t.tm_wday);
   Civil.bHour      = static_cast<uint8_t> (t.tm_hour);
   Civil.bMinute    = static_cast<uint8_t> (t.tm_min);
   Civil.bSecond    = static_cast<uint8_t> (t.tm_sec);
   Civil.dwFraction = dwFraction;
}

// ---------------------------------------------------------------------------
// FillMoment - the single point where a 100 ns instant (since 1601 UTC)
// becomes a fully populated MOMENT: both scalars, both civil views, offset.
// ---------------------------------------------------------------------------

static void FillMoment (int64_t ft100, SNEEZE_ABI_MOMENT& Moment)
{
   memset (&Moment, 0, sizeof (Moment));

   int64_t  unix100    = ft100 - EPOCH_DIFF_100NS;
   uint32_t dwFraction = static_cast<uint32_t> (((ft100 % HNS_PER_SECOND) + HNS_PER_SECOND) % HNS_PER_SECOND);
   time_t   t          = static_cast<time_t> (unix100 / HNS_PER_SECOND);

   struct tm gtm;
   struct tm ltm;

   memset (&gtm, 0, sizeof (gtm));
   memset (&ltm, 0, sizeof (ltm));

   GmTime    (t, gtm);
   LocalTime (t, ltm);

   Moment.tm      = ft100 / HNS_PER_TICK;
   Moment.dt      = unix100 / HNS_PER_MS;
   Moment.nOffset = LocalOffsetMinutes (t, gtm, ltm);

   FillCivil (gtm, dwFraction, Moment.Utc);
   FillCivil (ltm, dwFraction, Moment.Local);
}

// ---------------------------------------------------------------------------
// Public clock reads.
// ---------------------------------------------------------------------------

int64_t Chrono_Time ()
{
   return FileTime100 () / HNS_PER_TICK;
}

int64_t Chrono_Date ()
{
   return (FileTime100 () - EPOCH_DIFF_100NS) / HNS_PER_MS;
}

// The monotonic clock, in 100 ns units off the steady clock's own epoch. The
// origin is per fabric (see below), so this is a bare counter; the caller keeps
// the origin and subtracts.
static int64_t SteadyNow100 ()
{
   std::chrono::steady_clock::duration d = std::chrono::steady_clock::now ().time_since_epoch ();

   return std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>> (d).count ();
}

// The monotonic origin is per fabric (mirrors performance.timeOrigin per
// document): the FABRIC captures both scalars once, at load, and hands them back
// on each Performance call. tmSteady is the monotonic count at the origin; ft100
// is the wall anchor. Read adjacently so Origin's wall time lines up with Now's
// zero point. Only the delta between two Now reads is meaningful.
void Performance_Origin_Capture (int64_t& tmSteady, int64_t& ft100)
{
   tmSteady = SteadyNow100 ();
   ft100    = FileTime100 ();
}

int64_t Performance_Now (int64_t tmSteadyOrigin)
{
   return SteadyNow100 () - tmSteadyOrigin;
}

void Performance_Origin (int64_t ft100Origin, SNEEZE_ABI_MOMENT& Moment)
{
   FillMoment (ft100Origin, Moment);
}

// ---------------------------------------------------------------------------
// MOMENT fills.
// ---------------------------------------------------------------------------

void Chrono_Moment_Now (SNEEZE_ABI_MOMENT& Moment)
{
   FillMoment (FileTime100 (), Moment);
}

void Chrono_Moment_Scalar (SNEEZE_ABI_MOMENT& Moment, int64_t qwValue, bool bFromDate)
{
   int64_t ft100 = bFromDate ? (qwValue * HNS_PER_MS + EPOCH_DIFF_100NS) : (qwValue * HNS_PER_TICK);

   FillMoment (ft100, Moment);
}

bool Chrono_Moment_Set (SNEEZE_ABI_MOMENT& Moment, int32_t eZone, int32_t nYear, int32_t nMonth, int32_t nDay, int32_t nHour, int32_t nMinute, int32_t nSecond, int32_t nFraction)
{
   struct tm t;

   memset (&t, 0, sizeof (t));

   t.tm_year  = nYear - 1900;
   t.tm_mon   = nMonth - 1;
   t.tm_mday  = nDay;
   t.tm_hour  = nHour;
   t.tm_min   = nMinute;
   t.tm_sec   = nSecond;
   t.tm_isdst = -1;

   // mktime normalizes out-of-range fields, so component setters get JS-style
   // rollover (month 15 -> next March) for free. LOCAL interprets the fields
   // in the local zone; UTC interprets them as UTC.
   time_t unixSeconds = (eZone == kSNEEZE_ABI_CHRONO_ZONE_LOCAL) ? mktime (&t) : TimeGm (&t);

   bool bResult = (unixSeconds != static_cast<time_t> (-1));

   if (bResult)
   {
      int64_t frac  = static_cast<int64_t> (nFraction);
      int64_t carry = frac / HNS_PER_SECOND;

      frac -= carry * HNS_PER_SECOND;

      if (frac < 0)
      {
         frac  += HNS_PER_SECOND;
         carry -= 1;
      }

      int64_t ft100 = (static_cast<int64_t> (unixSeconds) + carry) * HNS_PER_SECOND + frac + EPOCH_DIFF_100NS;

      FillMoment (ft100, Moment);
   }
   else
   {
      memset (&Moment, 0, sizeof (Moment));
   }

   return bResult;
}

bool Chrono_Moment_Parse (SNEEZE_ABI_MOMENT& Moment, int32_t eZone, const std::string& sText)
{
   int nYear   = 0;
   int nMonth  = 0;
   int nDay    = 0;
   int nHour   = 0;
   int nMinute = 0;
   int nSecond = 0;

   int nParsed = sscanf (sText.c_str (), "%d-%d-%dT%d:%d:%d", &nYear, &nMonth, &nDay, &nHour, &nMinute, &nSecond);

   if (nParsed < 3)
      nParsed = sscanf (sText.c_str (), "%d-%d-%d %d:%d:%d", &nYear, &nMonth, &nDay, &nHour, &nMinute, &nSecond);

   bool    bResult    = (nParsed >= 3);
   int32_t nFraction  = 0;
   int32_t eZoneParse = eZone;

   if (bResult)
   {
      // Fractional seconds: the digits after '.' scaled to 100 ns (pad/truncate
      // to 7 digits, so ".5" -> 5,000,000 and ".123" -> 1,230,000).
      size_t nDot = sText.find ('.');

      if (nDot != std::string::npos)
      {
         std::string sFraction;

         for (size_t i = nDot + 1; i < sText.size ()  &&  sFraction.size () < 7  &&  sText[i] >= '0'  &&  sText[i] <= '9'; i++)
            sFraction.push_back (sText[i]);

         while (sFraction.size () < 7)
            sFraction.push_back ('0');

         nFraction = atoi (sFraction.c_str ());
      }

      // A trailing 'Z' forces UTC; explicit numeric offsets are not yet parsed.
      if (sText.find ('Z') != std::string::npos)
         eZoneParse = kSNEEZE_ABI_CHRONO_ZONE_UTC;

      bResult = Chrono_Moment_Set (Moment, eZoneParse, nYear, nMonth, nDay, nHour, nMinute, nSecond, nFraction);
   }
   else
   {
      memset (&Moment, 0, sizeof (Moment));
   }

   return bResult;
}

std::string Chrono_Format (const SNEEZE_ABI_MOMENT& Moment, int32_t eZone, const std::string& sSpec)
{
   const SNEEZE_ABI_CIVIL& Civil = (eZone == kSNEEZE_ABI_CHRONO_ZONE_LOCAL) ? Moment.Local : Moment.Utc;

   std::string sResult;

   if (sSpec.empty ())
   {
      // Default: ISO-8601, with milliseconds when the sub-second is non-zero,
      // and a zone suffix ('Z' for UTC, +/-HH:MM for local).
      char szBuffer[64];

      snprintf (szBuffer, sizeof (szBuffer), "%04d-%02u-%02uT%02u:%02u:%02u",
         static_cast<int>      (Civil.wYear),
         static_cast<unsigned> (Civil.bMonth),
         static_cast<unsigned> (Civil.bDay),
         static_cast<unsigned> (Civil.bHour),
         static_cast<unsigned> (Civil.bMinute),
         static_cast<unsigned> (Civil.bSecond));

      sResult = szBuffer;

      if (Civil.dwFraction != 0)
      {
         char szMilli[8];

         snprintf (szMilli, sizeof (szMilli), ".%03u", static_cast<unsigned> (Civil.dwFraction / static_cast<uint32_t> (HNS_PER_MS)));
         sResult += szMilli;
      }

      if (eZone == kSNEEZE_ABI_CHRONO_ZONE_LOCAL)
      {
         char    szZone[8];
         int32_t nOffset = Moment.nOffset;
         char    cSign   = (nOffset < 0) ? '-' : '+';
         int32_t nAbs    = (nOffset < 0) ? -nOffset : nOffset;

         snprintf (szZone, sizeof (szZone), "%c%02d:%02d", cSign, nAbs / 60, nAbs % 60);
         sResult += szZone;
      }
      else
      {
         sResult += "Z";
      }
   }
   else
   {
      // strftime passthrough on the selected civil view.
      struct tm t;
      char      szBuffer[256];

      memset (&t, 0, sizeof (t));

      t.tm_year = Civil.wYear - 1900;
      t.tm_mon  = Civil.bMonth - 1;
      t.tm_mday = Civil.bDay;
      t.tm_wday = Civil.bWeekday;
      t.tm_hour = Civil.bHour;
      t.tm_min  = Civil.bMinute;
      t.tm_sec  = Civil.bSecond;

      size_t nWritten = strftime (szBuffer, sizeof (szBuffer), sSpec.c_str (), &t);

      sResult.assign (szBuffer, nWritten);
   }

   return sResult;
}

} // namespace DEP
} // namespace SNEEZE
