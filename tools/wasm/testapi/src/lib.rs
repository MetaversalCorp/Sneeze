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

// Build the wasm executable (run from this crate's directory):
//    cargo build --target wasm32-unknown-unknown --release --target-dir target
// Output: target/wasm32-unknown-unknown/release/testapi.wasm
// Always pass --target-dir target; otherwise a CARGO_TARGET_DIR set in the shell
// can redirect the output elsewhere.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

// ---------------------------------------------------------------------------
// API test module
//
// This module owns no scene data. On Open it exercises the object API of the
// sneeze SDK - the typed views hanging off the FABRIC handle - and logs every
// method's result to the developer console. Most of it is read-only (it never
// touches the raw snapshot JSON, which the SDK keeps private); the clock and
// timer surfaces additionally arm timers, whose fires arrive asynchronously in
// INSTANCE::Timer. The surfaces it walks:
//
//    FABRIC     - Index
//    LOCATION   - Href / Protocol / Host / Pathname / Origin       (5)
//    RESOURCE   - Id / Name                                        (2)
//    SIGNATURE  - Algorithm / IsValid / IsChainTrusted / IsChainExpired (4)
//    AGENT      - Browser_Name / _Version / Engine_Name / _Version / Platform / Language (6)
//    CONTAINER  - Name / Organization / OrganizationHash / Persona /
//                 PersonaHash / Fingerprint / Trust / DisplayName /
//                 DisplayOrganization                              (9)
//    SERVICE/MODULE lists via Fabric::Services / Modules
//    DATA       - Has / Get, plus a typed Get_As read of "Schema"  (2 + typed)
//    CONSOLE    - all 14 methods
//    CHRONO     - Time / Date / Now, MOMENT getters + format + a setter round-trip
//    PERFORMANCE- Now / Origin
//    TIMER      - Set (one-shot) / Interval (repeat) / Clear, fired via INSTANCE::Timer
//
// NODE's accessors are read-only too, but a NODE is only obtained through a
// scene-mutating call, so it is out of scope for a read-only tester.
// ---------------------------------------------------------------------------

use sneeze::*;
use nanoserde::DeJson;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

struct TESTAPI;

// Timer test state. A wasm guest is single-threaded, but a timer fire arrives
// asynchronously (host -> guest Notify) after Open returns, so the arming site
// (Test_Timer) and the fire site (INSTANCE::Timer) share state through module
// statics. The repeating timer self-clears after a few fires to prove Clear.
static TIMER_INTERVAL_ID:   AtomicU64 = AtomicU64::new (0);
static TIMER_INTERVAL_HITS: AtomicU32 = AtomicU32::new (0);

const TIMER_PARAM_ONESHOT:  u64 = 1;
const TIMER_PARAM_INTERVAL: u64 = 2;
const TIMER_INTERVAL_LIMIT: u32 = 5;

// A typed record read out of the fabric's "Data" block at "Schema", to prove
// DATA::Get_As deserializes a data sub-tree into a guest struct.
#[derive(DeJson, Default)]
struct KEY_OBJECT
{
   #[nserde(default)] a                                     : i64,
   #[nserde(default)] b                                     : i64,
   #[nserde(default)] c                                     : i64,
}

#[derive(DeJson, Default)]
struct SCHEMA
{
   #[nserde(default)] nKey                                  : i64,
   #[nserde(default)] sKey                                  : String,
   #[nserde(default)] Key_Object                            : KEY_OBJECT,
   #[nserde(default)] Key_Vector                            : Vec<i64>,
}

impl INSTANCE for TESTAPI
{
   fn Open (pFabric: FABRIC)
   {
      let pConsole = pFabric.Console ();

      pConsole.Group ("testapi: object/method API sweep");

      Test_Fabric      (&pFabric);
      Test_Location    (&pFabric);
      Test_Resource    (&pFabric);
      Test_Signature   (&pFabric);
      Test_Agent       (&pFabric);
      Test_Container   (&pFabric);
      Test_Manifest    (&pFabric);
      Test_Data        (&pFabric);
      Test_Console     (&pFabric);
      Test_Chrono      (&pFabric);
      Test_Moment      (&pFabric);
      Test_Performance (&pFabric);
      Test_Timer       (&pFabric);

      pConsole.Group_End ();
   }

   fn Close (pFabric: FABRIC)
   {
      pFabric.Console ().Log ("testapi: Close");
   }

   // Asynchronous timer fire (host -> guest). Armed in Test_Timer; the engine
   // echoes the qwParam we passed, so we tell the one-shot from the repeat by
   // its cookie. The repeat self-clears once it has fired TIMER_INTERVAL_LIMIT
   // times, which also exercises Clear on an in-flight timer.
   fn Timer (pFabric: FABRIC, twTimerIx: u64, qwParam: u64)
   {
      let pConsole = pFabric.Console ();

      if qwParam == TIMER_PARAM_INTERVAL
      {
         let nHits = TIMER_INTERVAL_HITS.fetch_add (1, Ordering::SeqCst) + 1;
         pConsole.Log (&format! ("Timer FIRED (interval) id={} param={} hit {}/{}", twTimerIx, qwParam, nHits, TIMER_INTERVAL_LIMIT));

         if nHits >= TIMER_INTERVAL_LIMIT
         {
            let bCleared = pFabric.Timer ().Clear (twTimerIx);
            pConsole.Log (&format! ("interval reached {} fires; Clear ({}) -> {}", TIMER_INTERVAL_LIMIT, twTimerIx, bCleared));
         }
      }
      else
      {
         pConsole.Log (&format! ("Timer FIRED (one-shot) id={} param={}", twTimerIx, qwParam));
      }
   }
}

// ---------------------------------------------------------------------------
// FABRIC.
// ---------------------------------------------------------------------------

fn Test_Fabric (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group ("===== FABRIC =====");
   pConsole.Log (&format! ("Index () = {}", pFabric.Index ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// LOCATION - the fabric URL, split web-style.
// ---------------------------------------------------------------------------

fn Test_Location (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pLoc     = pFabric.Location ();

   pConsole.Group ("----- Location () - 5 methods -----");
   pConsole.Log (&format! ("Href     () = {}", pLoc.Href ()));
   pConsole.Log (&format! ("Protocol () = {}", pLoc.Protocol ()));
   pConsole.Log (&format! ("Host     () = {}", pLoc.Host ()));
   pConsole.Log (&format! ("Pathname () = {}", pLoc.Pathname ()));
   pConsole.Log (&format! ("Origin   () = {}", pLoc.Origin ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// RESOURCE - the launching resource's identity.
// ---------------------------------------------------------------------------

fn Test_Resource (pFabric: &FABRIC)
{
   let pConsole  = pFabric.Console ();
   let pResource = pFabric.Resource ();

   pConsole.Group ("----- Resource () - 2 methods -----");
   pConsole.Log (&format! ("Id   () = {}", pResource.Id ()));
   pConsole.Log (&format! ("Name () = {}", pResource.Name ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// SIGNATURE - the MSF verification result.
// ---------------------------------------------------------------------------

fn Test_Signature (pFabric: &FABRIC)
{
   let pConsole   = pFabric.Console ();
   let pSignature = pFabric.Signature ();

   pConsole.Group ("----- Signature () - 4 methods -----");
   pConsole.Log (&format! ("Algorithm      () = {}", pSignature.Algorithm ()));
   pConsole.Log (&format! ("IsValid        () = {}", pSignature.IsValid ()));
   pConsole.Log (&format! ("IsChainTrusted () = {}", pSignature.IsChainTrusted ()));
   pConsole.Log (&format! ("IsChainExpired () = {}", pSignature.IsChainExpired ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// AGENT - host/engine identity (navigator analog).
// ---------------------------------------------------------------------------

fn Test_Agent (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pAgent   = pFabric.Agent ();

   pConsole.Group ("----- Agent () - 6 methods -----");
   pConsole.Log (&format! ("Browser_Name    () = {}", pAgent.Browser_Name ()));
   pConsole.Log (&format! ("Browser_Version () = {}", pAgent.Browser_Version ()));
   pConsole.Log (&format! ("Engine_Name     () = {}", pAgent.Engine_Name ()));
   pConsole.Log (&format! ("Engine_Version  () = {}", pAgent.Engine_Version ()));
   pConsole.Log (&format! ("Platform        () = {}", pAgent.Platform ()));
   pConsole.Log (&format! ("Language        () = {}", pAgent.Language ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// CONTAINER - the container identity, raw fields plus composed display names.
// ---------------------------------------------------------------------------

fn Test_Container (pFabric: &FABRIC)
{
   let pConsole   = pFabric.Console ();
   let pContainer = pFabric.Container ();

   pConsole.Group ("----- Container () - 9 methods -----");
   pConsole.Log (&format! ("Name                () = {}", pContainer.Name ()));
   pConsole.Log (&format! ("Organization        () = {}", pContainer.Organization ()));
   pConsole.Log (&format! ("OrganizationHash    () = {}", pContainer.OrganizationHash ()));
   pConsole.Log (&format! ("Persona             () = {}", pContainer.Persona ()));
   pConsole.Log (&format! ("PersonaHash         () = {}", pContainer.PersonaHash ()));
   pConsole.Log (&format! ("Fingerprint         () = {}", pContainer.Fingerprint ()));
   pConsole.Log (&format! ("Trust               () = {} ({})", pContainer.Trust (), Trust_Name (pContainer.Trust ())));
   pConsole.Log (&format! ("DisplayName         () = {}", pContainer.DisplayName ()));
   pConsole.Log (&format! ("DisplayOrganization () = {}", pContainer.DisplayOrganization ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// SERVICE + MODULE manifest lists.
// ---------------------------------------------------------------------------

fn Test_Manifest (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group (&format! ("----- Services () - {} entries -----", pFabric.Services ().len ()));
   for pService in pFabric.Services ()
   {
      pConsole.Log (&format! ("Name={}  Type={}  Endpoint={}  Modules={:?}", pService.Name (), pService.Type (), pService.Endpoint (), pService.Modules ()));
   }
   pConsole.Group_End ();

   pConsole.Group (&format! ("----- Modules () - {} entries -----", pFabric.Modules ().len ()));
   for pModule in pFabric.Modules ()
   {
      pConsole.Log (&format! ("Url={}  Hash={}", pModule.Url (), pModule.Hash ()));
   }
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// DATA - the read-only "Data" tree: Has/Get, plus a typed Get_As of "Schema".
// ---------------------------------------------------------------------------

fn Test_Data (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pData    = pFabric.Data ();

   pConsole.Group ("----- Data () - Has / Get -----");
   Show_Data (&pConsole, &pData, "");
   Show_Data (&pConsole, &pData, "Scene");
   Show_Data (&pConsole, &pData, "does.not.exist");
   pConsole.Group_End ();

   pConsole.Group ("Data ().Get (\"Schema\") - guest-parsed typed read");
   let pSchema = pData.Get ("Schema").and_then (|sJson| SCHEMA::deserialize_json (&sJson).ok ());
   match pSchema
   {
      Some (pSchema) =>
      {
         pConsole.Log (&format! ("nKey       = {}", pSchema.nKey));
         pConsole.Log (&format! ("sKey       = {}", pSchema.sKey));
         pConsole.Log (&format! ("Key_Object = {{ a:{}, b:{}, c:{} }}", pSchema.Key_Object.a, pSchema.Key_Object.b, pSchema.Key_Object.c));
         pConsole.Log (&format! ("Key_Vector = {:?}", pSchema.Key_Vector));
      }
      None =>
      {
         pConsole.Warn ("Schema absent or failed to parse (add a \"Schema\" object to the fabric's Data to see this populated)");
      }
   }
   pConsole.Group_End ();
}

fn Show_Data (pConsole: &CONSOLE, pData: &DATA, sPath: &str)
{
   let sLabel = if sPath.is_empty () { "(root)" } else { sPath };

   pConsole.Log (&format! ("Has (\"{}\") = {}", sLabel, pData.Has (sPath)));
   pConsole.Log (&format! ("Get (\"{}\") = {}", sLabel, Show_Option (&pData.Get (sPath))));
}

// ---------------------------------------------------------------------------
// CONSOLE - all 14 methods. Log and Group are used throughout the sweep; this
// section exercises the remaining severities and facilities. Assert is called
// with a true condition so it does not fire.
// ---------------------------------------------------------------------------

fn Test_Console (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group ("----- Console () - severities + facilities -----");

   pConsole.Debug ("Debug line");
   pConsole.Info  ("Info line");
   pConsole.Warn  ("Warn line");
   pConsole.Error ("Error line");

   pConsole.Assert (true, "this assert should NOT print (condition is true)");

   pConsole.Count ("tick");
   pConsole.Count ("tick");
   pConsole.Count_Reset ("tick");

   pConsole.Time ("span");
   pConsole.Time_Log ("span");
   pConsole.Time_End ("span");

   pConsole.Group_Collapsed ("collapsed subgroup");
   pConsole.Log ("(inside collapsed group)");
   pConsole.Group_End ();

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// CHRONO - the wall clock and a MOMENT read out of Now (). Both calendar views
// (local and UTC), the scalars, formatting, and a setter round-trip.
// ---------------------------------------------------------------------------

fn Test_Chrono (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pChrono  = pFabric.Chrono ();

   pConsole.Group ("----- Chrono () - clock + MOMENT -----");
   pConsole.Log (&format! ("Time () = {} (1/64 s since 1601 UTC)", pChrono.Time ()));
   pConsole.Log (&format! ("Date () = {} (Unix ms)", pChrono.Date ()));

   let m = pChrono.Now ();
   pConsole.Log (&format! ("Now ().IsValid () = {}", m.IsValid ()));
   pConsole.Log (&format! ("local = {:04}-{:02}-{:02} {:02}:{:02}:{:02} (weekday {})", m.Year (),     m.Month (),     m.Day (),     m.Hour (),     m.Minute (),     m.Second (),     m.Weekday ()));
   pConsole.Log (&format! ("utc   = {:04}-{:02}-{:02} {:02}:{:02}:{:02}",               m.Year_Utc (), m.Month_Utc (), m.Day_Utc (), m.Hour_Utc (), m.Minute_Utc (), m.Second_Utc ()));
   pConsole.Log (&format! ("Zone_Offset () = {} min", m.Zone_Offset ()));
   pConsole.Log (&format! ("String_Iso () = {}", m.String_Iso ()));
   pConsole.Log (&format! ("String ()     = {}", m.String ()));
   pConsole.Log (&format! ("Format (%A %B %d) = {}", m.Format (eSNEEZE_ABI_CHRONO_ZONE::kSNEEZE_ABI_CHRONO_ZONE_LOCAL, "%A %B %d")));

   let mut mEdit = MOMENT::From_Date (0);
   pConsole.Log (&format! ("From_Date (0)      = {}", mEdit.String_Iso ()));
   mEdit.Year_Utc_Set (2000);
   pConsole.Log (&format! ("after Year_Utc_Set = {} (host renormalized)", mEdit.String_Iso ()));

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// MOMENT - the remaining value-object surface not already touched by Chrono:
// every constructor, the sub-second getters, all component setters (local + a
// UTC sample), JS-style month rollover, and the Json / String_Utc formatters.
// UTC-built moments log String_Iso (UTC); local mutations log String () (local)
// so the field just set reads back directly.
// ---------------------------------------------------------------------------

fn Test_Moment (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();

   pConsole.Group ("----- MOMENT - full value surface -----");

   // Constructors.
   let mNull = MOMENT::Null ();
   pConsole.Log (&format! ("Null ().IsValid () = {} (expect false)", mNull.IsValid ()));

   let mTime = MOMENT::From_Time (859519617528);
   pConsole.Log (&format! ("From_Time (tm)  = {}  Time ()={} Date ()={}", mTime.String_Iso (), mTime.Time (), mTime.Date ()));

   let mParts = MOMENT::From_Parts (2026, 7, 4, 12, 30, 15, eSNEEZE_ABI_CHRONO_ZONE::kSNEEZE_ABI_CHRONO_ZONE_UTC);
   pConsole.Log (&format! ("From_Parts      = {}", mParts.String_Iso ()));

   let mParse = MOMENT::Parse ("2026-12-25T08:15:30.250Z", eSNEEZE_ABI_CHRONO_ZONE::kSNEEZE_ABI_CHRONO_ZONE_UTC);
   pConsole.Log (&format! ("Parse           = {}  IsValid ()={}", mParse.String_Iso (), mParse.IsValid ()));
   pConsole.Log (&format! ("  Milli={} Tick={}   Milli_Utc={} Tick_Utc={}", mParse.Milli (), mParse.Tick (), mParse.Milli_Utc (), mParse.Tick_Utc ()));
   pConsole.Log (&format! ("  Weekday_Utc={} (Dec 25 2026 = Friday = 5)", mParse.Weekday_Utc ()));

   // Formatters.
   pConsole.Log (&format! ("Json ()       = {}", mParse.Json ()));
   pConsole.Log (&format! ("String_Utc () = {}", mParse.String_Utc ()));

   // Local component setters (each mutates in place; the host renormalizes).
   let mut mEdit = MOMENT::From_Parts (2026, 1, 1, 0, 0, 0, eSNEEZE_ABI_CHRONO_ZONE::kSNEEZE_ABI_CHRONO_ZONE_LOCAL);
   pConsole.Log (&format! ("base (local)      = {}", mEdit.String ()));
   mEdit.Year_Set   (2030); pConsole.Log (&format! ("Year_Set (2030)   = {}", mEdit.String ()));
   mEdit.Month_Set  (7);    pConsole.Log (&format! ("Month_Set (7)     = {}", mEdit.String ()));
   mEdit.Day_Set    (30);   pConsole.Log (&format! ("Day_Set (30)      = {}", mEdit.String ()));
   mEdit.Hour_Set   (13);   pConsole.Log (&format! ("Hour_Set (13)     = {}", mEdit.String ()));
   mEdit.Minute_Set (45);   pConsole.Log (&format! ("Minute_Set (45)   = {}", mEdit.String ()));
   mEdit.Second_Set (20);   pConsole.Log (&format! ("Second_Set (20)   = {}", mEdit.String ()));
   mEdit.Milli_Set  (250);  pConsole.Log (&format! ("Milli_Set (250)   = {}", mEdit.String ()));
   mEdit.Tick_Set   (32);   pConsole.Log (&format! ("Tick_Set (32)     = {} (32/64 s = .500)", mEdit.String ()));

   // JS-style month overflow: month 13 rolls into the next year.
   let mut mRoll = MOMENT::From_Parts (2026, 11, 15, 0, 0, 0, eSNEEZE_ABI_CHRONO_ZONE::kSNEEZE_ABI_CHRONO_ZONE_UTC);
   mRoll.Month_Set (13);
   pConsole.Log (&format! ("Month_Set (13) rollover = {} (-> 2027-01)", mRoll.String_Iso ()));

   // setTime-style whole-instant reset, and a UTC-family setter sample.
   let mut mReset = MOMENT::Null ();
   mReset.Date_Set (0);
   pConsole.Log (&format! ("Date_Set (0)      = {} (Unix epoch)", mReset.String_Iso ()));

   let mut mUtc = MOMENT::From_Parts (2026, 6, 15, 10, 0, 0, eSNEEZE_ABI_CHRONO_ZONE::kSNEEZE_ABI_CHRONO_ZONE_UTC);
   mUtc.Hour_Utc_Set (23);
   pConsole.Log (&format! ("Hour_Utc_Set (23) = {}", mUtc.String_Iso ()));

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// PERFORMANCE - the monotonic clock. Two reads (the delta is the point) and
// the wall-clock origin.
// ---------------------------------------------------------------------------

fn Test_Performance (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pPerf    = pFabric.Performance ();

   pConsole.Group ("----- Performance () - monotonic clock -----");
   let n1 = pPerf.Now ();
   let n2 = pPerf.Now ();
   pConsole.Log (&format! ("Now () = {} then {} (delta {} x100ns)", n1, n2, n2 - n1));
   pConsole.Log (&format! ("Origin () = {}", pPerf.Origin ().String_Iso ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// TIMER - arm a one-shot and a repeating timer. The fires land later in
// INSTANCE::Timer (above); the repeat self-clears after a few hits.
// ---------------------------------------------------------------------------

fn Test_Timer (pFabric: &FABRIC)
{
   let pConsole = pFabric.Console ();
   let pTimer   = pFabric.Timer ();

   pConsole.Group ("----- Timer () - Set / Interval / Clear -----");

   let twOnce = pTimer.Set (1000, eSNEEZE_ABI_TIMER_UNIT::kSNEEZE_ABI_TIMER_UNIT_MS, TIMER_PARAM_ONESHOT);
   pConsole.Log (&format! ("Set (1000 ms)  -> twTimerIx {} (fires once)", twOnce));

   let twEvery = pTimer.Interval (2, eSNEEZE_ABI_TIMER_UNIT::kSNEEZE_ABI_TIMER_UNIT_HZ, TIMER_PARAM_INTERVAL);
   TIMER_INTERVAL_ID.store (twEvery, Ordering::SeqCst);
   pConsole.Log (&format! ("Interval (2 Hz) -> twTimerIx {} (self-clears after {} fires)", twEvery, TIMER_INTERVAL_LIMIT));

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

fn Trust_Name (nTrust: i32) -> &'static str
{
   let sName = match nTrust
   {
      sneeze::abi::kSNEEZE_ABI_TRUST_NONE       => "NONE",
      sneeze::abi::kSNEEZE_ABI_TRUST_UNTRUSTED  => "UNTRUSTED",
      sneeze::abi::kSNEEZE_ABI_TRUST_UNVERIFIED => "UNVERIFIED",
      sneeze::abi::kSNEEZE_ABI_TRUST_EXPIRED    => "EXPIRED",
      sneeze::abi::kSNEEZE_ABI_TRUST_VERIFIED   => "VERIFIED",
      sneeze::abi::kSNEEZE_ABI_TRUST_ROOT       => "ROOT",
      _                                         => "?",
   };

   sName
}

fn Show_Option (pValue: &Option<String>) -> String
{
   let sOut = match pValue
   {
      Some (sText) => Clip (sText),
      None         => String::from ("(none)"),
   };

   sOut
}

// Caps a long string at 200 chars (on a char boundary) so a big data blob does
// not flood the console; appends the true byte length when clipped.
fn Clip (sText: &str) -> String
{
   let sOut = if sText.chars ().count () > 200
   {
      let sHead: String = sText.chars ().take (200).collect ();
      format! ("{}... ({} bytes)", sHead, sText.len ())
   }
   else
   {
      sText.to_string ()
   };

   sOut
}

sneeze::instance! (TESTAPI);
