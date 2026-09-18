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
// sneeze SDK - the typed views hanging off the HOST handle - and logs every
// method's result to the developer console. Most of it is read-only (it never
// touches the raw snapshot JSON, which the SDK keeps private); the timer and
// network surfaces additionally start work whose results arrive asynchronously
// in INSTANCE::Timer, INSTANCE::Request and the four INSTANCE::Socket_* hooks.
// The surfaces it walks:
//
//    HOST       - Index
//    LOCATION   - Href / Protocol / Host / Pathname / Origin       (5)
//    RESOURCE   - Id / Name                                        (2)
//    SIGNATURE  - Algorithm / IsValid / IsChainTrusted / IsChainExpired (4)
//    AGENT      - Browser_Name / _Version / Engine_Name / _Version / Platform / Language (6)
//    CONTAINER  - Name / Organization / OrganizationHash / Persona /
//                 PersonaHash / Fingerprint / Trust / DisplayName /
//                 DisplayOrganization                              (9)
//    SERVICES   - Has / Get by service name; MODULE list via Host::Modules
//    DATA       - Has / Get, plus a typed Get_As read of "Schema"  (2 + typed)
//    CONSOLE    - all 14 methods
//    CHRONO     - Time / Date / Now, MOMENT getters + format + a setter round-trip
//    PERFORMANCE- Now / Origin
//    TIMER      - Set (one-shot) / Interval (repeat) / Clear, fired via INSTANCE::Timer
//    NETWORK    - Request_Open + REQUEST configure/send/read/Close, completed
//                 via INSTANCE::Request
//    SOCKET     - Socket_Open + send/Recv/Close/Free, driven by the four
//                 INSTANCE::Socket_* callbacks
//
// NODE's accessors are read-only too, but a NODE is only obtained through a
// scene-mutating call, so it is out of scope for a read-only tester. FABRIC (the
// node-tree view) is likewise skipped for the same reason.
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

// Request test state. Same shape as the timer state and for the same reason: the
// completion lands in INSTANCE::Request long after Test_Request returned. A
// REQUEST carries no cookie, so we remember each handle's index to tell the two
// in-flight requests apart when they come back.
static REQUEST_SELF_ID:    AtomicU64 = AtomicU64::new (0);
static REQUEST_MISSING_ID: AtomicU64 = AtomicU64::new (0);

// Socket test state. A socket outlives its opening call by even more than a
// request does - four callbacks can land on one handle - so the same static
// pattern applies. Two sockets are opened: one to an echo server, and one to a
// host that cannot resolve, to show the failure path.
static SOCKET_ECHO_ID: AtomicU64 = AtomicU64::new (0);
static SOCKET_BAD_ID:  AtomicU64 = AtomicU64::new (0);
static SOCKET_ECHO_IN: AtomicU32 = AtomicU32::new (0);

// This echo server echoes both text and binary, and greets a new connection with
// a plain-text banner of its own before either echo arrives -- so a frame counts
// only when it carries back what we sent. (ws.postman-echo.com/raw was the
// earlier choice, but it drops the connection on any binary frame.)
const SOCKET_ECHO_URL:    &str    = "wss://echo.websocket.org";
const SOCKET_ECHO_TEXT:   &str    = "{\"testapi\":\"hello\"}";
const SOCKET_ECHO_BYTES:  [u8; 4] = [0xDE, 0xAD, 0xBE, 0xEF];
const SOCKET_BAD_URL:     &str    = "wss://testapi-no-such-host-9999.invalid/socket";
const SOCKET_ECHO_FRAMES: u32     = 2;

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
   fn Open (pHost: &HOST)
   {
      let pConsole = pHost.Console ();

      pConsole.Group ("testapi: object/method API sweep");

      Test_Host        (pHost);
      Test_Location    (pHost);
      Test_Resource    (pHost);
      Test_Signature   (pHost);
      Test_Agent       (pHost);
      Test_Container   (pHost);
      Test_Manifest    (pHost);
      Test_Data        (pHost);
      Test_Console     (pHost);
      Test_Chrono      (pHost);
      Test_Moment      (pHost);
      Test_Performance (pHost);
      Test_Timer       (pHost);
      Test_Request     (pHost);
      Test_Socket      (pHost);

      pConsole.Group_End ();
   }

   fn Close (pHost: &HOST)
   {
      pHost.Console ().Log ("testapi: Close");
   }

   // Asynchronous timer fire (host -> guest). Armed in Test_Timer; the engine
   // echoes the qwParam we passed, so we tell the one-shot from the repeat by
   // its cookie. The repeat self-clears once it has fired TIMER_INTERVAL_LIMIT
   // times, which also exercises Clear on an in-flight timer.
   fn Timer (pHost: &HOST, twTimerIx: u64, qwParam: u64)
   {
      let pConsole = pHost.Console ();

      if qwParam == TIMER_PARAM_INTERVAL
      {
         let nHits = TIMER_INTERVAL_HITS.fetch_add (1, Ordering::SeqCst) + 1;
         pConsole.Log (&format! ("Timer FIRED (interval) id={} param={} hit {}/{}", twTimerIx, qwParam, nHits, TIMER_INTERVAL_LIMIT));

         if nHits >= TIMER_INTERVAL_LIMIT
         {
            let bCleared = pHost.Timer ().Clear (twTimerIx);
            pConsole.Log (&format! ("interval reached {} fires; Clear ({}) -> {}", TIMER_INTERVAL_LIMIT, twTimerIx, bCleared));
         }
      }
      else
      {
         pConsole.Log (&format! ("Timer FIRED (one-shot) id={} param={}", twTimerIx, qwParam));
      }
   }

   // Asynchronous request completion (host -> guest). Sent in Test_Request. Both
   // requests land here; the one aimed at a missing path is the point of the
   // bSuccess/Status split - a 404 is a completed exchange, so it arrives with
   // bSuccess true. The guest owns the handle, so every path ends in Close.
   fn Request (pHost: &HOST, pRequest: REQUEST, bSuccess: bool)
   {
      let pConsole = pHost.Console ();

      let sWhich = if pRequest.Index () == REQUEST_SELF_ID.load (Ordering::SeqCst)
      {
         "self (the fabric's own URL)"
      }
      else if pRequest.Index () == REQUEST_MISSING_ID.load (Ordering::SeqCst)
      {
         "missing (expect 404)"
      }
      else
      {
         "unrecognized handle"
      };

      pConsole.Group (&format! ("Request COMPLETED id={} - {}", pRequest.Index (), sWhich));

      pConsole.Log (&format! ("bSuccess        = {} (transport, NOT the HTTP status)", bSuccess));
      pConsole.Log (&format! ("State ()        = {} ({})", pRequest.State () as i32, State_Name (pRequest.State ())));
      pConsole.Log (&format! ("Status ()       = {} {}", pRequest.Status (), pRequest.Status_Text ()));
      pConsole.Log (&format! ("Url ()          = {}", pRequest.Url ()));
      pConsole.Log (&format! ("Size ()         = {} bytes", pRequest.Size ()));
      pConsole.Log (&format! ("IsCached ()     = {}", pRequest.IsCached ()));
      pConsole.Log (&format! ("Content_Type () = {}", pRequest.Content_Type ()));
      pConsole.Log (&format! ("Header_All ()   = {}", Clip (&pRequest.Header_All ())));
      pConsole.Log (&format! ("Error ()        = {}", Show_Empty (&pRequest.Error ())));
      pConsole.Log (&format! ("Text ()         = {}", Clip (&pRequest.Text ())));
      pConsole.Log (&format! ("Body ().len ()  = {} (same bytes, untyped)", pRequest.Body ().len ()));

      pConsole.Log (&format! ("Header (\"content-length\") = {}", pRequest.Header ("content-length")));
      pConsole.Log (&format! ("Close ()        = {}", pRequest.Close ()));

      pConsole.Group_End ();
   }

   // The handshake finished (host -> guest). This is the earliest a send can
   // succeed, which is why Test_Socket sends nothing itself. Both frames go out
   // here so the echo comes back as one text and one binary message.
   fn Socket_Opened (pHost: &HOST, pSocket: SOCKET)
   {
      let pConsole = pHost.Console ();

      pConsole.Group (&format! ("Socket OPENED id={} - {}", pSocket.Index (), Socket_Which (&pSocket)));

      pConsole.Log (&format! ("State ()    = {} ({})", pSocket.State () as i32, Socket_State_Name (pSocket.State ())));
      pConsole.Log (&format! ("Url ()      = {}", pSocket.Url ()));
      pConsole.Log (&format! ("Protocol () = {} (empty unless the server picked one we offered)", Show_Empty (&pSocket.Protocol ())));

      pConsole.Log (&format! ("Send_Text (\"...\")  = {}", pSocket.Send_Text (SOCKET_ECHO_TEXT)));
      pConsole.Log (&format! ("Send_Bytes (4)     = {}", pSocket.Send_Bytes (&SOCKET_ECHO_BYTES)));
      pConsole.Log (&format! ("Buffered ()        = {} bytes still to go out", pSocket.Buffered ()));

      pConsole.Group_End ();
   }

   // A message is waiting (host -> guest). The callback carries its size and
   // whether it is binary, not its content: the message sits in the socket's
   // queue until this handler takes it, and a queue left unread eventually
   // overflows and starts dropping. So every one of these owes a Recv.
   fn Socket_Received (pHost: &HOST, pSocket: SOCKET, bBinary: bool, nSize: i64)
   {
      let pConsole = pHost.Console ();
      pConsole.Group (&format! ("Socket RECEIVED id={} binary={} nSize={}", pSocket.Index (), bBinary, nSize));

      // This server greets a new connection with a text banner of its own, so a
      // frame counts toward the two echoes only when it carries back what we
      // sent. Either way the message is read: an unread queue eventually drops.
      let bEcho = if bBinary
      {
         let aByte = pSocket.Recv ();
         pConsole.Log (&format! ("Recv () = {} bytes {:02X?}", aByte.len (), aByte));
         aByte[..] == SOCKET_ECHO_BYTES[..]
      }
      else
      {
         let sText = pSocket.Recv_Text ();
         pConsole.Log (&format! ("Recv_Text () = {}", Clip (&sText)));
         sText == SOCKET_ECHO_TEXT
      };

      if bEcho
      {
         let nFrame = SOCKET_ECHO_IN.fetch_add (1, Ordering::SeqCst) + 1;

         pConsole.Log (&format! ("echo {}/{}", nFrame, SOCKET_ECHO_FRAMES));

         // Both echoes are back, so we are done talking. Closing with a code and
         // a reason is the polite half of the pair; Free comes later, in
         // Socket_Closed, because the handle stays readable until then.
         if nFrame >= SOCKET_ECHO_FRAMES
         {
            pConsole.Log (&format! ("both frames echoed; Close_Ex (1000, \"testapi done\") = {}", pSocket.Close_Ex (1000, "testapi done")));
         }
      }
      else
      {
         pConsole.Log ("not an echo - the server's own greeting, which does not count toward the two");
      }

      pConsole.Group_End ();
   }

   // The connection never came up, or dropped without a closing handshake. A
   // Socket_Closed with code 1006 always follows this, and that is where the
   // handle is freed - nothing to clean up here.
   fn Socket_Failed (pHost: &HOST, pSocket: SOCKET)
   {
      let pConsole = pHost.Console ();

      pConsole.Log (&format! ("Socket FAILED id={} - {}: {}", pSocket.Index (), Socket_Which (&pSocket), Show_Empty (&pSocket.Error ())));
   }

   // The connection is finished either way. The handle is still readable here,
   // which is the point of splitting Close from Free; Free is the mirror of
   // Socket_Open and the guest owes it.
   fn Socket_Closed (pHost: &HOST, pSocket: SOCKET, wCode: i32, bClean: bool)
   {
      let pConsole = pHost.Console ();

      pConsole.Group (&format! ("Socket CLOSED id={} - {}", pSocket.Index (), Socket_Which (&pSocket)));

      pConsole.Log (&format! ("wCode   = {} (1000 = normal, 1006 = no closing handshake)", wCode));
      pConsole.Log (&format! ("bClean  = {}", bClean));
      pConsole.Log (&format! ("State () = {} ({})", pSocket.State () as i32, Socket_State_Name (pSocket.State ())));
      pConsole.Log (&format! ("Error () = {}", Show_Empty (&pSocket.Error ())));
      pConsole.Log (&format! ("Free ()  = {}", pSocket.Free ()));

      pConsole.Group_End ();
   }
}

// ---------------------------------------------------------------------------
// HOST - the root handle every other view hangs off.
// ---------------------------------------------------------------------------

fn Test_Host (pHost: &HOST)
{
   let pConsole = pHost.Console ();

   pConsole.Group ("===== HOST =====");
   pConsole.Log (&format! ("Index () = {}", pHost.Index ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// LOCATION - the fabric URL, split web-style.
// ---------------------------------------------------------------------------

fn Test_Location (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pLoc     = pHost.Location ();

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

fn Test_Resource (pHost: &HOST)
{
   let pConsole  = pHost.Console ();
   let pResource = pHost.Resource ();

   pConsole.Group ("----- Resource () - 2 methods -----");
   pConsole.Log (&format! ("Id   () = {}", pResource.Id ()));
   pConsole.Log (&format! ("Name () = {}", pResource.Name ()));
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// SIGNATURE - the MSF verification result.
// ---------------------------------------------------------------------------

fn Test_Signature (pHost: &HOST)
{
   let pConsole   = pHost.Console ();
   let pSignature = pHost.Signature ();

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

fn Test_Agent (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pAgent   = pHost.Agent ();

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

fn Test_Container (pHost: &HOST)
{
   let pConsole   = pHost.Console ();
   let pContainer = pHost.Container ();

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
// SERVICES + MODULE manifest.
//
// Services carry whatever fields their MSF author chose, so the engine hands
// them over as raw JSON keyed by name and the guest parses what it cares about.
// There is no enumeration: a module asks for the service it knows it needs. The
// names probed here are conventional, so an absent one is not a failure.
// ---------------------------------------------------------------------------

fn Test_Manifest (pHost: &HOST)
{
   let pConsole  = pHost.Console ();
   let pServices = pHost.Services ();

   pConsole.Group ("----- Services () - Has / Get by name -----");
   for sName in ["map", "chat", "does-not-exist"]
   {
      pConsole.Log (&format! ("Has (\"{}\") = {}", sName, pServices.Has (sName)));
      pConsole.Log (&format! ("Get (\"{}\") = {}", sName, Show_Option (&pServices.Get (sName))));
   }
   pConsole.Group_End ();

   pConsole.Group (&format! ("----- Modules () - {} entries -----", pHost.Modules ().len ()));
   for pModule in pHost.Modules ()
   {
      pConsole.Log (&format! ("Url={}  Hash={}", pModule.Url (), pModule.Hash ()));
   }
   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// DATA - the read-only "Data" tree: Has/Get, plus a typed Get_As of "Schema".
// ---------------------------------------------------------------------------

fn Test_Data (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pData    = pHost.Data ();

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

fn Test_Console (pHost: &HOST)
{
   let pConsole = pHost.Console ();

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

fn Test_Chrono (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pChrono  = pHost.Chrono ();

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

fn Test_Moment (pHost: &HOST)
{
   let pConsole = pHost.Console ();

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

fn Test_Performance (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pPerf    = pHost.Performance ();

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

fn Test_Timer (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pTimer   = pHost.Timer ();

   pConsole.Group ("----- Timer () - Set / Interval / Clear -----");

   let twOnce = pTimer.Set (1000, eSNEEZE_ABI_TIMER_UNIT::kSNEEZE_ABI_TIMER_UNIT_MS, TIMER_PARAM_ONESHOT);
   pConsole.Log (&format! ("Set (1000 ms)  -> twTimerIx {} (fires once)", twOnce));

   let twEvery = pTimer.Interval (2, eSNEEZE_ABI_TIMER_UNIT::kSNEEZE_ABI_TIMER_UNIT_HZ, TIMER_PARAM_INTERVAL);
   TIMER_INTERVAL_ID.store (twEvery, Ordering::SeqCst);
   pConsole.Log (&format! ("Interval (2 Hz) -> twTimerIx {} (self-clears after {} fires)", twEvery, TIMER_INTERVAL_LIMIT));

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// NETWORK - open two GETs and send them. Both answers land later in
// INSTANCE::Request (above), which reads the response and closes the handle.
//
// The URLs are relative, so the engine resolves them against this fabric's own
// URL: "" is the fabric document itself (always present, wherever it is served
// from) and the second is a path that cannot exist, to prove a 404 comes back as
// a completed exchange rather than a failure. Only GET is exercised here; the
// verbs that carry a body are a separate exercise.
// ---------------------------------------------------------------------------

fn Test_Request (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pNetwork = pHost.Network ();

   pConsole.Group ("----- Network () - Request_Open / Send -----");

   let pLoc  = pHost.Location ();
   let pSelf = pNetwork.Request_Open (eSNEEZE_ABI_REQUEST_VERB::kSNEEZE_ABI_REQUEST_VERB_GET, pLoc.Href ());
   REQUEST_SELF_ID.store (pSelf.Index (), Ordering::SeqCst);
   pConsole.Log (&format! ("Request_Open (GET, own URL) -> twRequestIx {}  IsValid ()={}", pSelf.Index (), pSelf.IsValid ()));
   pConsole.Log (&format! ("State () before Send = {} ({})", pSelf.State () as i32, State_Name (pSelf.State ())));
   pConsole.Log (&format! ("Header_Set (\"accept\", \"*/*\") = {}", pSelf.Header_Set ("accept", "*/*")));
   pConsole.Log (&format! ("Timeout_Set (15000) = {}", pSelf.Timeout_Set (15000)));
   pConsole.Log (&format! ("Send () = {}", pSelf.Send ()));

   let pMissing = pNetwork.Request_Open (eSNEEZE_ABI_REQUEST_VERB::kSNEEZE_ABI_REQUEST_VERB_GET, "testapi-no-such-path.txt");
   REQUEST_MISSING_ID.store (pMissing.Index (), Ordering::SeqCst);
   pConsole.Log (&format! ("Request_Open (GET, missing path) -> twRequestIx {}", pMissing.Index ()));
   pConsole.Log (&format! ("Send () = {}", pMissing.Send ()));

   pConsole.Group_End ();
}

// ---------------------------------------------------------------------------
// SOCKET - open two WebSockets and let the callbacks drive them. Unlike a
// request URL, a socket URL is never resolved against the fabric: it must be an
// absolute ws:// or wss:// URL, the same rule the browser's WebSocket applies.
//
// The first socket talks to a public echo server, so the frames sent from
// INSTANCE::Socket_Opened come straight back to INSTANCE::Socket_Received. The
// second names a host that cannot resolve, to show the failure path arriving as
// Socket_Failed followed by Socket_Closed with 1006. A third open is refused
// outright, to show that the check is made before anything touches the network.
// ---------------------------------------------------------------------------

fn Test_Socket (pHost: &HOST)
{
   let pConsole = pHost.Console ();
   let pNetwork = pHost.Network ();

   pConsole.Group ("----- Network () - Socket_Open / SOCKET -----");

   let pEcho = pNetwork.Socket_Open_Ex (SOCKET_ECHO_URL, "testapi.v1");
   SOCKET_ECHO_ID.store (pEcho.Index (), Ordering::SeqCst);
   pConsole.Log (&format! ("Socket_Open_Ex (echo, \"testapi.v1\") -> twSocketIx {}  IsValid ()={}", pEcho.Index (), pEcho.IsValid ()));
   pConsole.Log (&format! ("State () right after open = {} ({}) - the handshake is still running", pEcho.State () as i32, Socket_State_Name (pEcho.State ())));
   pConsole.Log (&format! ("Url () = {}", pEcho.Url ()));
   pConsole.Log (&format! ("Send_Text () before OPEN = {} (refused - wait for Socket_Opened)", pEcho.Send_Text ("too early")));

   let pBad = pNetwork.Socket_Open (SOCKET_BAD_URL);
   SOCKET_BAD_ID.store (pBad.Index (), Ordering::SeqCst);
   pConsole.Log (&format! ("Socket_Open (unresolvable host) -> twSocketIx {} (expect Socket_Failed, then 1006)", pBad.Index ()));

   let pRefused = pNetwork.Socket_Open ("https://example.com/not-a-socket");
   pConsole.Log (&format! ("Socket_Open (https:// URL) -> IsValid ()={} (refused: not a WebSocket scheme)", pRefused.IsValid ()));

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

fn State_Name (eState: eSNEEZE_ABI_REQUEST_STATE) -> &'static str
{
   let sName = match eState
   {
      eSNEEZE_ABI_REQUEST_STATE::kSNEEZE_ABI_REQUEST_STATE_IDLE     => "IDLE",
      eSNEEZE_ABI_REQUEST_STATE::kSNEEZE_ABI_REQUEST_STATE_SENDING  => "SENDING",
      eSNEEZE_ABI_REQUEST_STATE::kSNEEZE_ABI_REQUEST_STATE_COMPLETE => "COMPLETE",
      eSNEEZE_ABI_REQUEST_STATE::kSNEEZE_ABI_REQUEST_STATE_FAILED   => "FAILED",
      eSNEEZE_ABI_REQUEST_STATE::kSNEEZE_ABI_REQUEST_STATE_ABORTED  => "ABORTED",
   };

   sName
}

fn Socket_State_Name (eState: eSNEEZE_ABI_SOCKET_STATE) -> &'static str
{
   let sName = match eState
   {
      eSNEEZE_ABI_SOCKET_STATE::kSNEEZE_ABI_SOCKET_STATE_CONNECTING => "CONNECTING",
      eSNEEZE_ABI_SOCKET_STATE::kSNEEZE_ABI_SOCKET_STATE_OPEN       => "OPEN",
      eSNEEZE_ABI_SOCKET_STATE::kSNEEZE_ABI_SOCKET_STATE_CLOSING    => "CLOSING",
      eSNEEZE_ABI_SOCKET_STATE::kSNEEZE_ABI_SOCKET_STATE_CLOSED     => "CLOSED",
   };

   sName
}

// A socket carries no cookie either, so the two we opened are told apart by the
// handle we remembered.
fn Socket_Which (pSocket: &SOCKET) -> &'static str
{
   let sWhich = if pSocket.Index () == SOCKET_ECHO_ID.load (Ordering::SeqCst)
   {
      "echo"
   }
   else if pSocket.Index () == SOCKET_BAD_ID.load (Ordering::SeqCst)
   {
      "unresolvable host"
   }
   else
   {
      "unrecognized handle"
   };

   sWhich
}

// The string getters return empty rather than an Option when a field is absent,
// so say so out loud instead of logging a blank.
fn Show_Empty (sText: &str) -> String
{
   let sOut = if sText.is_empty ()
   {
      String::from ("(empty)")
   }
   else
   {
      Clip (sText)
   };

   sOut
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
